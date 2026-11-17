"""One guest run: QEMU command line, isolation, the guest-time budget and the host watchdog.

Every run gets its own directory (disk overlay, serial log, pidfile), its own process group and exactly one slot of
the admission controller. QEMU is only ever stopped through its own pid; nothing here looks processes up by name.
"""
import atexit
import hashlib
import os
import re
import shutil
import signal
import subprocess
import tempfile
import threading
import time
from dataclasses import dataclass, field

from . import load, slots

DEFAULT_SHIFT = 6                  # 64 ns per instruction: the highest resolution that QEMU can record and replay
BASE_ICOUNT = "shift=%d,sleep=off,align=off"
RTC = "base=2026-01-01T00:00:00,clock=vm"
MARKER = re.compile(r"@@COALTEST exit=(-?\d+) signal=(\d+) timeout=(\d) ns=(\d+)")
PANIC = re.compile(r"KERNEL PANIC")
STAMP = re.compile(r"^\[\s*\d+\.\d{3}\] ", re.M)
MARKER_NS = re.compile(r"ns=\d+")

_live = {}                         # pid -> workdir of every QEMU this process started
_live_lock = threading.Lock()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def normalize(text):
    """The serial log without what legitimately differs between runs: kernel log time stamps and the guest clock."""
    return MARKER_NS.sub("ns=_", STAMP.sub("[ts] ", text.replace("\r", "")))


def qemu_binary():
    return shutil.which("qemu-system-x86_64")


def qemu_version():
    try:
        out = subprocess.run([qemu_binary(), "--version"], capture_output=True, text=True).stdout
        return out.splitlines()[0]
    except (OSError, TypeError, IndexError):
        return "unknown"


@dataclass
class GuestSpec:
    """What to boot and what to run in it."""
    argv: list                     # program and arguments run by init (coal.test=)
    memory_mb: int = 128
    profile: str = "ide"           # ide | virtio | microvm
    net: bool = False
    budget_s: int = 120            # guest seconds
    cmdline: list = field(default_factory=list)
    accel: str = "tcg"             # tcg (deterministic) | kvm (fast, not repeatable)
    files: dict = field(default_factory=dict)    # guest path -> bytes, copied onto a private copy of the disk image


@dataclass
class GuestResult:
    verdict: str                   # PASS | FAIL | TIMEOUT | CRASH | PANIC | WATCHDOG
    detail: str = ""
    exit_code: int = None
    serial: str = ""
    guest_ns: int = 0
    host_s: float = 0.0
    qemu_argv: list = field(default_factory=list)
    workdir: str = ""
    shift: int = DEFAULT_SHIFT
    disk: str = ""                 # the image QEMU was given (a private copy when the spec injects files)

    @property
    def guest_insn(self):
        """Guest instructions the run was long enough for: guest time over the clock's ns per instruction."""
        return self.guest_ns >> self.shift

    @property
    def output_hash(self):
        return hashlib.sha256(normalize(self.serial).encode("utf-8", "replace")).hexdigest()[:16]


def with_files(disk, files, out):
    """A private copy of the disk image with `files` (guest path -> bytes) added; the original is never touched."""
    subprocess.run(["cp", "--sparse=always", disk, out], check=True)
    dirs = sorted({os.path.dirname(p) for p in files if os.path.dirname(p) not in ("", "/")})
    for d in dirs:
        subprocess.run(["mmd", "-i", out, "::" + d], stderr=subprocess.DEVNULL)      # exists already: fine
    for path, data in files.items():
        src = os.path.join(os.path.dirname(out), "inject.tmp")
        with open(src, "wb") as f:
            f.write(data)
        subprocess.run(["mcopy", "-o", "-i", out, src, "::" + path], check=True)
        os.unlink(src)
    return out


def kernel_cmdline(spec, seed):
    words = ["root=%s" % ("hda" if spec.profile == "ide" else "vda"), "coal.seed=%d" % seed,
             "coal.budget=%d" % spec.budget_s, "coal.test=" + ",".join(spec.argv)] + list(spec.cmdline)
    if spec.profile == "microvm":
        words.append("virtio_mmio.device=512@0xfeb02e00:28")
    return words


def build_args(spec, kernel, disk, workdir, seed, mode="run", rrfile=None, shift=DEFAULT_SHIFT, gdb=False):
    """QEMU argument vector (without the binary). `mode` is run, record or replay.

    The disk is always attached through blkreplay on a snapshot overlay, in every mode, so the device topology of a
    recorded run is the topology of the run it reproduces. record/replay needs sleep=on; QEMU hangs with sleep=off."""
    cmdline = kernel_cmdline(spec, seed)
    machine = ["-machine", "pc"]
    drive = ["-drive", "file=%s,if=none,snapshot=on,id=img-direct,format=raw" % disk,
             "-drive", "driver=blkreplay,if=none,image=img-direct,id=img"]
    if spec.profile == "ide":
        drive += ["-device", "ide-hd,drive=img,bus=ide.0,unit=0"]
    elif spec.profile == "virtio":
        drive += ["-device", "virtio-blk-pci,drive=img,disable-legacy=on"]
    elif spec.profile == "microvm":
        machine = ["-machine", "microvm", "-global", "virtio-mmio.force-legacy=false"]
        drive += ["-device", "virtio-blk-device,drive=img"]
    else:
        raise ValueError("unknown profile %r" % spec.profile)
    args = ["-display", "none", "-monitor", "none", "-m", str(spec.memory_mb), "-smp", "1", "-no-reboot"] + machine
    args += ["-kernel", kernel, "-append", " ".join(cmdline), "-rtc", RTC,
             "-serial", "file:" + os.path.join(workdir, "serial.log"), "-pidfile", os.path.join(workdir, "qemu.pid")]
    args += drive
    args += ["-nic", "user,model=e1000,restrict=on"] if spec.net else ["-nic", "none"]
    if spec.accel == "kvm":
        return args + ["-accel", "kvm", "-cpu", "host"]
    args += ["-accel", "tcg,thread=single"]
    if mode == "run":
        args += ["-icount", BASE_ICOUNT % shift]
    else:
        args += ["-icount", "shift=%d,rr=%s,rrfile=%s" % (shift, mode, rrfile)]
    if gdb:
        args += ["-s", "-S"]
    return args


def _kill_group(pid, sig):
    try:
        os.killpg(pid, sig)                  # the group QEMU leads (start_new_session): only our own processes
    except (ProcessLookupError, PermissionError):
        pass


def _cleanup_all():
    with _live_lock:
        pids = list(_live)
    for pid in pids:
        _kill_group(pid, signal.SIGKILL)


atexit.register(_cleanup_all)


def _stop(proc):
    """Ends QEMU through its own pid: SIGTERM, then SIGKILL after two seconds."""
    if proc.poll() is None:
        _kill_group(proc.pid, signal.SIGTERM)
        try:
            proc.wait(2)
        except subprocess.TimeoutExpired:
            _kill_group(proc.pid, signal.SIGKILL)
            proc.wait()


def run_guest(spec, kernel, disk, seed, mode="run", rrfile=None, shift=DEFAULT_SHIFT, workdir=None,
              keep=False, slot=None, wall_s=None):
    """Boots one guest, waits for its result line and returns a GuestResult.

    The guest enforces spec.budget_s in guest time. The host watchdog (wall_s, default generous and scaled by host
    load) only catches a guest that stopped making progress at all, such as a hung kernel without timers."""
    own_slot = slot is None
    slot = slot or slots.acquire("guest")
    created = workdir is None
    workdir = workdir or tempfile.mkdtemp(prefix="coal-tk-")
    os.makedirs(workdir, exist_ok=True)
    if spec.files:
        disk = with_files(disk, spec.files, os.path.join(workdir, "root.img"))
    args = build_args(spec, kernel, disk, workdir, seed, mode, rrfile, shift)
    binary = qemu_binary()
    if binary is None:
        slot.release()
        return GuestResult("CRASH", "qemu-system-x86_64 is not installed", workdir=workdir)
    prefix = ["setpriv", "--pdeathsig", "KILL"] if shutil.which("setpriv") else []
    env = dict(os.environ, TMPDIR=workdir)         # the snapshot overlay lives and dies in the run directory
    serial = os.path.join(workdir, "serial.log")
    open(serial, "w").close()
    t0 = time.monotonic()
    deadline = t0 + (wall_s or max(120.0, spec.budget_s * 8.0)) * load.scale() * (1 if spec.accel == "tcg" else 0.5)
    errlog = open(os.path.join(workdir, "qemu.stderr"), "w")
    proc = subprocess.Popen(prefix + [binary] + args, stdin=subprocess.DEVNULL, stdout=errlog, stderr=errlog,
                            env=env, start_new_session=True)
    with _live_lock:
        _live[proc.pid] = workdir
    verdict, detail, text, grace_end = None, "", "", None
    try:
        while True:
            try:
                proc.wait(0.1)
                alive = False
            except subprocess.TimeoutExpired:
                alive = True
            with open(serial, "rb") as f:
                text = f.read().decode("utf-8", "replace")
            if MARKER.search(text) and alive and grace_end is None:
                grace_end = time.monotonic() + 3.0      # powered off by the guest, or stopped here (microvm)
            if PANIC.search(text):
                verdict, detail = "PANIC", "kernel panic"
                break
            if not alive or (grace_end and time.monotonic() > grace_end):
                break
            if time.monotonic() > deadline:
                verdict, detail = "WATCHDOG", "no result within %.0f s of host time (watchdog, load scale %.1f)" % (
                    deadline - t0, load.scale())
                break
    finally:
        _stop(proc)
        errlog.close()
        with _live_lock:
            _live.pop(proc.pid, None)
        if own_slot:
            slot.release()
    host_s = time.monotonic() - t0
    with open(serial, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    m = MARKER.search(text)
    guest_ns = int(m.group(4)) if m else 0
    if verdict is None:
        if not m:
            with open(os.path.join(workdir, "qemu.stderr"), errors="replace") as f:
                err = f.read().strip().splitlines()
            verdict, detail = "CRASH", "no result line (QEMU %s)%s" % (
                "exited %s" % proc.returncode, "; " + err[-1] if err else "")
        elif m.group(3) == "1":
            verdict, detail = "TIMEOUT", "guest budget of %d s exceeded" % spec.budget_s
        elif m.group(1) == "0":
            verdict = "PASS"
        else:
            verdict = "FAIL"
            detail = "exit %s signal %s" % (m.group(1), m.group(2))
    res = GuestResult(verdict, detail, int(m.group(1)) if m else None, text, guest_ns, host_s, args, workdir, shift, disk)
    if created and not keep:
        res.workdir = ""
        shutil.rmtree(workdir, ignore_errors=True)
    return res
