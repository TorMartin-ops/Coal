#!/usr/bin/env python3
"""Boot timeline of Coal OS: milestones in milliseconds since the QEMU process was spawned.

    boottime.py [--runs N] [--json] [--accel tcg|kvm|auto] [--icount] [--build-dir DIR]

Milestones: QEMU's COM1/COM2 sockets appear, first serial byte, kernel entry (the
banner), every kernel log line (one per init stage, including "starting /sbin/init"),
mcpd's ready event and the first ping reply over COM2. Runs are repeatable: the disk
is attached with -snapshot, so nothing a run writes survives it.

--icount makes QEMU's virtual clock follow executed instructions (TCG only). Host
timing is then meaningless, so the report uses guest time: the kernel's own log
timestamps and the up_ms field of mcpd's ready event. Those are identical from run
to run, which makes the numbers comparable across commits. Standard library only.
"""
import argparse
import json
import os
import re
import selectors
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC_DIR = REPO / "src" / "CoalOS"
KLOG = re.compile(rb"^\[\s*(\d+)\.(\d{3})\] \[\w+\s*\] ([\w-]+): (.*)$")
PING = b'{"id":1,"op":"ping"}\n'
LABEL_MAX = 44


class BootError(Exception):
    pass


def build(build_dir, jobs):
    """Configures and builds the kernel ISO and disk image; returns their paths."""
    steps = (["cmake", "-S", str(SRC_DIR), "-B", str(build_dir)],
             ["cmake", "--build", str(build_dir), "-j", str(jobs)])
    for cmd in steps:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode:
            raise BootError("%s failed:\n%s" % (cmd[0], r.stdout[-2000:]))
    iso, disk = Path(build_dir) / "kernel.iso", Path(build_dir) / "disk.img"
    if not (iso.exists() and disk.exists()):
        raise BootError("build produced no kernel.iso/disk.img in %s" % build_dir)
    return iso, disk


def qemu_cmd(args, iso, disk, com1, com2):
    cmd = [args.qemu, "-display", "none", "-m", str(args.memory), "-cdrom", str(iso), "-hda", str(disk),
           "-boot", "d", "-snapshot", "-no-reboot", "-nic", "user,model=e1000",
           "-serial", "unix:%s,server=on,wait=off" % com1, "-serial", "unix:%s,server=on,wait=off" % com2,
           "-monitor", "none"]
    if args.accel == "kvm":
        cmd += ["-accel", "kvm"]
    else:
        cmd += ["-accel", "tcg,thread=single"]
        if args.icount:
            cmd += ["-icount", "shift=0,sleep=off"]
    return cmd + args.qemu_args


class Timeline:
    """Collects milestones as (key, label, host_ms, guest_ms); guest_ms is None when unknown."""

    def __init__(self, t0):
        self.t0 = t0
        self.items = []
        self.seen = {}

    def host_ms(self):
        return (time.monotonic() - self.t0) * 1000.0

    def add(self, key, label, guest_ms=None, host_ms=None):
        n = self.seen[key] = self.seen.get(key, 0) + 1     # repeated modules get #2, #3, ...
        key = key if n == 1 else "%s#%d" % (key, n)
        self.items.append((key, label[:LABEL_MAX], self.host_ms() if host_ms is None else host_ms, guest_ms))


def connect_early(path, deadline, alive):
    """Connects as soon as QEMU creates the socket, so no byte the guest sends is missed."""
    while time.monotonic() < deadline and alive():
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.connect(path)
            return s
        except OSError:
            s.close()
            time.sleep(0.0005)
    raise BootError("QEMU did not open %s" % os.path.basename(path))


def boot_once(args, iso, disk, workdir):
    com1, com2 = os.path.join(workdir, "com1"), os.path.join(workdir, "com2")
    t0 = time.monotonic()
    proc = subprocess.Popen(qemu_cmd(args, iso, disk, com1, com2), stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    tl = Timeline(t0)
    deadline = t0 + args.timeout
    try:
        s1 = connect_early(com1, deadline, lambda: proc.poll() is None)
        s2 = connect_early(com2, deadline, lambda: proc.poll() is None)
        tl.add("qemu_sockets", "QEMU serial sockets ready")
        collect(args, proc, tl, s1, s2, deadline)
    except BootError as e:
        if proc.poll() is not None:
            e = BootError("%s: %s" % (e, proc.stderr.read().decode("utf-8", "replace").strip()))
        raise e
    finally:
        proc.kill()
        proc.wait()
        proc.stderr.close()
    return tl.items


def collect(args, proc, tl, s1, s2, deadline):
    sel = selectors.DefaultSelector()
    for s, name in ((s1, "com1"), (s2, "com2")):
        s.setblocking(False)
        sel.register(s, selectors.EVENT_READ, name)
    bufs = {"com1": b"", "com2": b""}
    started = ready = pinged = None        # init-start, mcpd ready, ping sent: host ms
    got_byte = replied = False
    done_at = None
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise BootError("QEMU exited during boot")
        if done_at is not None and time.monotonic() >= done_at:
            return
        for key, _ in sel.select(0.002):
            data = key.fileobj.recv(65536)
            if not data:
                raise BootError("serial link closed")
            now = tl.host_ms()
            name = key.data
            if name == "com1" and not got_byte:
                got_byte = True
                tl.add("first_byte", "first serial byte", host_ms=now)
            bufs[name] += data
            *lines, bufs[name] = bufs[name].split(b"\n")
            for raw in lines:
                line = raw.strip(b"\r ")
                if name == "com1":
                    started = on_kernel_line(tl, line, now, started)
                elif b'"event":"ready"' in line:
                    ready = now
                    up = re.search(rb'"up_ms":(\d+)', line)
                    tl.add("mcpd_ready", "mcpd ready event", int(up.group(1)) if up else None, now)
                elif b'"pong"' in line and not replied:
                    replied = True
                    tl.add("mcpd_ping", "first mcpd ping reply", host_ms=now)
                    done_at = time.monotonic() + args.linger / 1000.0
        # Ping once mcpd announced itself; if the announcement was missed, 2 s after init started.
        if (not replied and (ready is not None or (started is not None and tl.host_ms() - started > 2000))
                and (pinged is None or tl.host_ms() - pinged > 50)):
            pinged = tl.host_ms()
            s2.send(PING)
    raise BootError("boot did not complete within %d s" % args.timeout)


def on_kernel_line(tl, line, now, started):
    if line.startswith(b"Coal OS"):
        tl.add("kernel_entry", "kernel entry (banner)", host_ms=now)
        return started
    m = KLOG.match(line)
    if not m:
        return started
    guest = int(m.group(1)) * 1000 + int(m.group(2))
    module, text = m.group(3).decode(), m.group(4).decode("utf-8", "replace")
    tl.add("log:" + module, "%s: %s" % (module, text), guest, now)
    return now if text.startswith("starting ") else started


def pct(values, p):
    """Nearest-rank percentile."""
    v = sorted(values)
    return v[max(0, -(-len(v) * p // 100) - 1)]


def summarize(runs, use_guest):
    order, labels, samples = [], {}, {}
    for items in runs:
        for key, label, host, guest in items:
            value = guest if use_guest else host
            if value is None:
                continue
            if key not in samples:
                order.append(key)
                labels[key] = label
            samples.setdefault(key, []).append(value)
    return [{"key": k, "label": labels[k], "n": len(samples[k]),
             "min": min(samples[k]), "median": statistics.median(samples[k]), "p90": pct(samples[k], 90)}
            for k in order]


def render(rows, clock):
    out = ["%-44s %9s %9s %9s" % ("milestone (%s ms)" % clock, "min", "median", "p90")]
    out += ["%-44s %9.1f %9.1f %9.1f" % (r["label"], r["min"], r["median"], r["p90"]) for r in rows]
    return "\n".join(out)


def parse_args(argv):
    ap = argparse.ArgumentParser(description="Coal OS boot timeline", epilog="Extra QEMU arguments go after --.")
    ap.add_argument("--runs", type=int, default=1, help="boots to average (default 1)")
    ap.add_argument("--json", action="store_true", help="print machine-readable results")
    ap.add_argument("--accel", choices=("tcg", "kvm", "auto"), default="tcg",
                    help="tcg (default), kvm (needs /dev/kvm) or auto (kvm when available)")
    ap.add_argument("--icount", action="store_true", help="deterministic virtual clock (TCG); report guest time")
    ap.add_argument("--build-dir", help="CMake build directory to reuse; default is a fresh temporary one")
    ap.add_argument("--iso", help="boot this ISO instead of building (needs --disk)")
    ap.add_argument("--disk", help="disk image used with --iso")
    ap.add_argument("--memory", type=int, default=128, help="guest RAM in MiB (default 128)")
    ap.add_argument("--timeout", type=int, default=60, help="seconds per boot (default 60)")
    ap.add_argument("--linger", type=int, default=1500, help="ms to keep logging after the ping reply (default 1500)")
    ap.add_argument("--qemu", default="qemu-system-i386")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args, extra = ap.parse_known_args(argv)
    args.qemu_args = [a for a in extra if a != "--"]
    if args.accel == "auto":
        args.accel = "kvm" if os.access("/dev/kvm", os.R_OK | os.W_OK) else "tcg"
    if args.accel == "kvm" and not os.access("/dev/kvm", os.R_OK | os.W_OK):
        ap.error("--accel kvm needs a usable /dev/kvm")
    if args.accel == "kvm" and args.icount:
        ap.error("--icount needs TCG")
    if bool(args.iso) != bool(args.disk):
        ap.error("--iso and --disk go together")
    if args.runs < 1:
        ap.error("--runs must be at least 1")
    if shutil.which(args.qemu) is None:
        ap.error("%s is not installed" % args.qemu)
    return args


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    tmp = tempfile.mkdtemp(prefix="coal-boottime-")
    try:
        if args.iso:
            iso, disk = Path(args.iso), Path(args.disk)
        else:
            iso, disk = build(args.build_dir or os.path.join(tmp, "build"), args.jobs)
        runs = []
        for i in range(args.runs):
            sockdir = os.path.join(tmp, "run%d" % i)
            os.mkdir(sockdir)
            runs.append(boot_once(args, iso, disk, sockdir))
            print("run %d/%d done" % (i + 1, args.runs), file=sys.stderr)
    except BootError as e:
        print("boottime: %s" % e, file=sys.stderr)
        return 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    clock = "guest" if args.icount else "host"
    rows = summarize(runs, args.icount)
    if args.json:
        print(json.dumps({"accel": args.accel, "icount": args.icount, "clock": clock, "runs": args.runs,
                          "milestones": rows}, indent=2))
    else:
        unit = "guest virtual ms" if args.icount else "ms since QEMU spawn"
        print("accel=%s%s runs=%d (%s)" % (args.accel, " icount" if args.icount else "", args.runs, unit))
        print(render(rows, clock))
    return 0


if __name__ == "__main__":
    sys.exit(main())
