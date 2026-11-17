#!/usr/bin/env python3
"""Compares the agentd link and the root disk across transports (QEMU TCG).

  COAL_TEST_BUILD=<build dir> virtio_bench.py [--runs N]

Link: median `exec true` round trip and 1 MiB fs.write / fs.read throughput over COM2, a virtio console and
virtio-vsock (the last needs vhost-device-vsock, see tests/support.py). Disk: coalbench file_read_8m and
file_write_8m on the ATA disk and on virtio-blk.
"""
import argparse
import base64
import os
import statistics
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "coal-gateway", "tests"))
from support import QemuAgent, VirtioQemuAgent, VsockQemuAgent  # noqa: E402

CHUNK = 64 * 1024
MIB = 1 << 20


class BlkOnly(VirtioQemuAgent):
    features = "blk"


class Console(VirtioQemuAgent):
    features = "console"


def link_numbers(fx, runs):
    c = fx.client()
    lat = []
    for _ in range(runs):
        t = time.monotonic()
        c.request("exec", timeout=120, cmd="true")
        lat.append((time.monotonic() - t) * 1000)
    path = "/tmp/bench.bin"
    data = base64.b64encode(os.urandom(CHUNK)).decode()
    t = time.monotonic()
    for i in range(MIB // CHUNK):
        c.request("fs.write", timeout=60, path=path, data=data, more=i < MIB // CHUNK - 1)
    w = MIB / 1024 / (time.monotonic() - t)
    t = time.monotonic()
    for off in range(0, MIB, CHUNK):
        c.request("fs.read", timeout=60, path=path, offset=off, limit=CHUNK)
    r = MIB / 1024 / (time.monotonic() - t)
    c.request("fs.remove", path=path)
    return statistics.median(lat), w, r, c


def disk_numbers(c):
    out = {}
    for name in ("file_read_8m", "file_write_8m"):
        r = c.request("exec", timeout=120, cmd="coalbench -n 5 %s" % name)
        out[name] = r["stdout"]["text"].strip().split("\n")[-1]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=50)
    args = ap.parse_args()
    for label, cls in (("COM2", QemuAgent), ("virtio-console", Console), ("virtio-vsock", VsockQemuAgent)):
        try:
            fx = cls()
        except Exception as e:                      # noqa: BLE001 - skipped fixtures raise SkipTest
            print("%-15s skipped: %s" % (label, e))
            continue
        try:
            lat, w, r, c = link_numbers(fx, args.runs)
            print("%-15s exec true %.1f ms (median of %d)   write %.0f KiB/s   read %.0f KiB/s" % (label, lat, args.runs, w, r))
        finally:
            fx.close()
    for label, cls in (("ATA", QemuAgent), ("virtio-blk", BlkOnly)):
        fx = cls()
        try:
            c = fx.client()
            for k, v in disk_numbers(c).items():
                print("%-10s %s" % (label, v))
        finally:
            fx.close()


if __name__ == "__main__":
    main()
