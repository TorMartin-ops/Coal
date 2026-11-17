"""Repro bundles: a failing guest test is re-run under QEMU record/replay and everything needed to replay it is kept.

A bundle directory holds the kernel and disk image the test ran on (with their hashes), the command line and seed,
the record/replay log (rr.bin), the serial logs of the failing run, the recorded run and the replay, the gateway run log
for host-driven tests, and replay.sh, which replays the recording with or without gdb attached. The replay is verified
here: its serial output must equal the recording byte for byte.
"""
import glob
import json
import os
import shlex
import shutil
import subprocess

from . import qemu
from .model import FAIL, NONDET, PASS

GDB_PORT = 1234


def _copy(src, dst):
    subprocess.run(["cp", "--sparse=always", src, dst], check=True)


def replay_script(argv, shift):
    """replay.sh: paths are relative to the bundle, so the bundle can be moved to another machine."""
    q = " ".join(shlex.quote(a) for a in argv)
    return f"""#!/bin/sh
# Replays the recorded run. `sh replay.sh` runs it to the end and checks the serial output;
# `sh replay.sh gdb` stops at the first instruction and attaches gdb (reverse-step and reverse-continue work).
cd "$(dirname "$0")" || exit 1
rm -rf replay && mkdir replay
export TMPDIR="$PWD/replay"
if [ "$1" = gdb ]; then
    qemu-system-x86_64 {q} -s -S &
    qemu_pid=$!
    trap 'kill $qemu_pid 2>/dev/null' EXIT
    gdb kernel.bin -ex 'target remote :{GDB_PORT}' -ex 'break kernel_main_init' -ex 'continue'
else
    qemu-system-x86_64 {q} && cmp replay/serial.log record/serial.log && echo "replay identical to the recording"
fi
"""


def make_bundle(test, ctx, result, bundle_root, started_at=0):
    """Returns the bundle directory (str). `result` is the failing Result; guest tests get the record/replay run."""
    first = result.runs[-1]
    seed = first.seed
    name = "%s-%s-seed%d" % (test.suite, test.name, seed)
    d = os.path.join(bundle_root, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    manifest = {"test": test.id, "seed": seed, "verdict": result.verdict, "detail": result.detail,
                "qemu": qemu.qemu_version()}
    for i, r in enumerate(result.runs):
        with open(os.path.join(d, "output_run%d_seed%d.log" % (i + 1, r.seed)), "w") as f:
            f.write(r.output)
    if test.guest:
        _guest_part(test, ctx, seed, d, manifest)
    else:
        logs = [p for p in glob.glob(os.path.join(ctx.build.dir, "runs", "*.jsonl"))
                if os.path.getmtime(p) >= started_at]
        for p in logs:
            os.makedirs(os.path.join(d, "gateway_runlog"), exist_ok=True)
            shutil.copy(p, os.path.join(d, "gateway_runlog"))
        manifest["gateway_runlog"] = [os.path.basename(p) for p in logs]
        manifest["record_replay"] = "not available: the test drives QEMU from the host; see its logs"
    with open(os.path.join(d, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
    return d


def _guest_part(test, ctx, seed, d, manifest):
    spec = test.guest
    spec = type(spec)(**{**spec.__dict__, "accel": "tcg"})
    shutil.copy(ctx.build.kernel, os.path.join(d, "kernel.bin"))
    _copy(ctx.build.disk, os.path.join(d, "disk.img"))
    manifest.update(kernel_sha256=ctx.build.kernel_sha, disk_sha256=ctx.build.disk_sha, shift=ctx.shift,
                    cmdline=" ".join(qemu.kernel_cmdline(spec, seed)))
    rec = os.path.join(d, "record")
    res = qemu.run_guest(spec, os.path.join(d, "kernel.bin"), os.path.join(d, "disk.img"), seed, mode="record",
                         rrfile=os.path.join(d, "rr.bin"), shift=ctx.shift, workdir=rec, wall_s=900)
    manifest["record_verdict"] = res.verdict
    manifest["reproduced_under_record"] = res.verdict != PASS
    manifest["rr_bytes"] = os.path.getsize(os.path.join(d, "rr.bin")) if os.path.exists(os.path.join(d, "rr.bin")) else 0
    disk_rel = "disk.img"
    if spec.files:                                      # the recording used a private copy with files added
        disk_rel = os.path.join("record", "root.img")
    args = qemu.build_args(spec, "kernel.bin", disk_rel, "replay", seed, "replay", "rr.bin", ctx.shift)
    with open(os.path.join(d, "replay.sh"), "w") as f:
        f.write(replay_script(args, ctx.shift))
    os.chmod(os.path.join(d, "replay.sh"), 0o755)
    manifest["replay_command"] = "sh %s" % os.path.join(d, "replay.sh")
    manifest["replay_gdb_command"] = "sh %s gdb" % os.path.join(d, "replay.sh")
    # verify the replay: same serial bytes as the recording
    os.makedirs(os.path.join(d, "replay"), exist_ok=True)
    rp = subprocess.run(["sh", os.path.join(d, "replay.sh")], capture_output=True, text=True, timeout=900)
    manifest["replay_identical"] = "replay identical to the recording" in rp.stdout
    manifest["replay_stderr"] = rp.stderr.strip()[-300:]
