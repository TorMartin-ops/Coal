#!/usr/bin/env python3
"""Time a guest wget of a host file over a link that drops and reorders frames.

    netbench.py BUILD [BUILD ...] [--loss 0,1,5] [--reorder PCT] [--size-mib 2] [--runs 3] [--out FILE.json]

BUILD is a CMake build directory, or REPO:BUILD to boot it with the coal-mcp server of
another checkout. With several builds the runs alternate between them, so a busy host
affects all of them alike.

QEMU user networking carries the traffic; a filter-redirector hands every Ethernet
frame of each direction to this script, which forwards, drops or delays it (ARP and
UDP are never touched, so DHCP and ARP always work). Loss is random but seeded, so
a run is repeatable. Reports the wall-clock time of `wget` in the guest. Standard
library only.
"""
import argparse
import json
import os
import random
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run as bench                                    # Mcp client and QEMU defaults

PORT = 18080


class Relay(threading.Thread):
    """One direction: frames in on a unix socket, frames out on the same socket."""

    def __init__(self, path, link):
        super().__init__(daemon=True)
        self.srv = []
        for suffix in ("o", "i"):                       # QEMU writes frames to "o" and reads them back from "i"
            sock = socket.socket(socket.AF_UNIX)
            sock.bind(path + suffix)
            sock.listen(1)
            self.srv.append(sock)
        self.link = link
        self.dropped = self.passed = 0
        self.start()

    def run(self):
        conn, _ = self.srv[0].accept()
        back, _ = self.srv[1].accept()
        held = []                                       # [frames left to wait, frame]
        buf = b""
        while True:
            chunk = conn.recv(65536)
            if not chunk:
                return
            buf += chunk
            while len(buf) >= 4:
                n = struct.unpack(">I", buf[:4])[0]
                if len(buf) < 4 + n:
                    break
                frame, buf = buf[4:4 + n], buf[4 + n:]
                out = self.decide(frame, held)
                for f in out:
                    back.sendall(struct.pack(">I", len(f)) + f)

    def decide(self, frame, held):
        link = self.link
        tcp = len(frame) > 23 and frame[12:14] == b"\x08\x00" and frame[23] == 6
        out = []
        for h in held:
            h[0] -= 1
        out += [h[1] for h in held if h[0] <= 0]
        held[:] = [h for h in held if h[0] > 0]
        if tcp and link.rng.random() < link.loss:
            self.dropped += 1
        elif tcp and link.rng.random() < link.reorder:
            held.append([link.rng.randint(1, 3), frame])
        else:
            self.passed += 1
            out.append(frame)
        return out


class Link:
    def __init__(self, loss, reorder, seed):
        self.loss, self.reorder = loss, reorder
        self.rng = random.Random(seed)


def one_run(build, loss, reorder, seed, size_mib, memory):
    repo, _, build_dir = build.rpartition(":")
    bench.SERVER = Path(repo or bench.REPO) / "tools" / "coal-mcp" / "server.py"
    tmp = tempfile.mkdtemp(prefix="netbench-")
    with open(os.path.join(tmp, "f"), "wb") as f:
        f.write(random.Random(1).randbytes(size_mib << 20))
    http = subprocess.Popen([sys.executable, "-m", "http.server", str(PORT), "--bind", "127.0.0.1", "-d", tmp],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    link = Link(loss, reorder, seed)
    relays = [Relay(os.path.join(tmp, d), link) for d in ("tx", "rx")]
    qargs = " ".join("-chardev socket,id=%so,path=%s/%so -chardev socket,id=%si,path=%s/%si "
                     "-object filter-redirector,id=f%s,netdev=u0,queue=%s,indev=%si,outdev=%so" % (d, tmp, d, d, tmp, d, d, d, d, d)
                     for d in ("tx", "rx"))
    os.environ["COAL_NIC"] = "user,model=e1000,id=u0"
    os.environ["COAL_QEMU_ARGS"] = qargs
    time.sleep(0.5)
    mcp = bench.Mcp(os.path.abspath(build_dir), memory, qargs)
    try:
        mcp.tool("coal_boot", timeout_s=120)
        time.sleep(1.5)                                 # DHCP
        t = time.monotonic()
        text = mcp.tool("coal_run", timeout=900, command="wget -O /dev/null http://10.0.2.2:%d/f" % PORT, timeout_ms=600000)
        dt = time.monotonic() - t
        ok = "exit code: 0" in text
        if os.environ.get("NETBENCH_DEBUG"): print(text)
        return dt if ok else None, relays[0].dropped + relays[1].dropped
    finally:
        mcp.close()
        http.kill()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("builds", nargs="+")
    ap.add_argument("--loss", default="0,1,5", help="comma separated loss percentages")
    ap.add_argument("--reorder", type=float, default=0, help="reorder percentage")
    ap.add_argument("--size-mib", type=int, default=2)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--memory", type=int, default=128)
    ap.add_argument("--out")
    a = ap.parse_args()
    res = {}
    for loss in (float(x) for x in a.loss.split(",")):
        times = {b: [] for b in a.builds}
        for r in range(a.runs):
            for b in a.builds:
                dt, dropped = one_run(b, loss / 100, a.reorder / 100, 1000 + r, a.size_mib, a.memory)
                print("loss %.1f%% run %d %s: %s (dropped %d frames)" % (loss, r, b, "FAILED" if dt is None else "%.2f s" % dt, dropped), flush=True)
                times[b].append(dt)
        for b, t in times.items():
            good = [x for x in t if x is not None]
            res["%s loss_%g" % (b, loss)] = {"median_s": statistics.median(good) if good else None, "runs": t}
        print("loss %.1f%% medians: %s" % (loss, "  ".join("%s %.2f s" % (b, statistics.median([x for x in t if x is not None] or [float("nan")])) for b, t in times.items())), flush=True)
    print(json.dumps(res, indent=1))
    if a.out:
        Path(a.out).write_text(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
