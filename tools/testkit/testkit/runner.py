"""Runs tests: one execution, then --repeat and --seeds with the determinism rules. No retries, ever."""
import contextlib
import difflib
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass

from . import load, qemu, slots
from .model import BAD, FAIL, NONDET, PASS, SKIP, Result, Run

DEVICES = {
    "qemu": lambda: qemu.qemu_binary() is not None,
    "cmake": lambda: shutil.which("cmake") is not None,
    "gcc": lambda: shutil.which("gcc") is not None,
    "mtools": lambda: shutil.which("mcopy") is not None,
    "vhost-device-vsock": lambda: bool(os.environ.get("COAL_VHOST_DEVICE_VSOCK") or shutil.which("vhost-device-vsock")),
}
HOST_VOLATILE = re.compile(r"\bin \d+\.\d+s\b|/tmp/\S+")


@dataclass
class Context:
    repo: str
    build: object
    out_dir: str
    seeds: list
    repeat: int = 1
    accel: str = "tcg"
    shift: int = qemu.DEFAULT_SHIFT
    repro: bool = True


def missing(test):
    return [n for n in test.needs if n in DEVICES and not DEVICES[n]()]


def _format(arg, ctx):
    return arg.replace("{py}", sys.executable).replace("{sh}", "sh").replace("{build}", ctx.build.dir)


def fingerprint(test, text):
    """Hash of the normalised output: kernel time stamps, the guest clock and the test's volatile numbers removed."""
    text = qemu.normalize(text)
    for pattern in test.volatile:
        text = re.sub(pattern, "#", text)
    return qemu.hashlib.sha256(text.encode("utf-8", "replace")).hexdigest()[:16]


def _count(test, text):
    if test.count is None:
        return None
    found = test.count.findall(text)
    if not found:
        return None
    last = found[-1] if isinstance(found[-1], str) else found[-1][0]
    return int(last) if str(last).isdigit() else len(found)


def _judge(test, passed, text):
    """Applies the test's own expectations to the output of a run whose process said it passed."""
    if passed and test.expect and not re.search(test.expect, text):
        return FAIL, "expected output not found: %s" % test.expect
    if passed and test.check:
        msg = test.check(text)
        if msg:
            return FAIL, msg
    return (PASS, "") if passed else (FAIL, "")


def execute_guest(test, ctx, seed):
    spec = test.guest
    if ctx.accel != "tcg":
        spec = type(spec)(**{**spec.__dict__, "accel": ctx.accel})
    res = qemu.run_guest(spec, ctx.build.kernel, ctx.build.disk, seed, shift=ctx.shift)
    verdict, detail = res.verdict, res.detail
    if verdict == PASS:
        verdict, detail = _judge(test, True, res.serial)
        failed = re.findall(r"(\d+) checks, (\d+) failed", res.serial)
        if verdict == PASS and failed and any(int(f) for _, f in failed):
            verdict, detail = FAIL, "%s checks failed" % failed[-1][1]
    return Run(verdict, detail, res.serial, fingerprint(test, res.serial), _count(test, res.serial), res.guest_ns,
               res.guest_insn, res.host_s, seed)


def _kill_group(proc):
    with contextlib.suppress(ProcessLookupError, PermissionError):
        os.killpg(proc.pid, signal.SIGKILL)         # our own child's group, never anything found by name


def execute_host(test, ctx, seed):
    cmd = [_format(a, ctx) for a in test.cmd]
    env = dict(os.environ, **test.env)
    env.update(COAL_TEST_BUILD=ctx.build.dir, COAL_TESTKIT_SEED=str(seed), PYTHONHASHSEED=str(seed),
               PYTHONDONTWRITEBYTECODE="1", COAL_TIMEOUT_SCALE="%.1f" % max(2.0, 2 * load.scale()))
    slot = None
    t0 = time.monotonic()
    try:
        if test.qemu:
            slot = slots.acquire(test.id)
            env.update(slots.held_env(slot))
            ctx.build.reset_disk()
        t0 = time.monotonic()
        limit = max(300.0, test.cost_s * 10) * load.scale()
        proc = subprocess.Popen(cmd, cwd=ctx.repo, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, start_new_session=True)
        try:
            out, _ = proc.communicate(timeout=limit)
            verdict_rc = proc.returncode
        except subprocess.TimeoutExpired:
            _kill_group(proc)
            out, _ = proc.communicate()
            text = out.decode("utf-8", "replace")
            return Run("WATCHDOG", "no result within %.0f s of host time (load scale %.1f)" % (limit, load.scale()),
                       text, "", None, 0, 0, time.monotonic() - t0, seed)
        finally:
            _kill_group(proc)
    finally:
        if slot:
            slot.release()
    text = out.decode("utf-8", "replace")
    host_s = time.monotonic() - t0
    n = _count(test, text)
    verdict, detail = _judge(test, verdict_rc == 0, text)
    if verdict == FAIL and not detail:
        detail = "exit status %d" % verdict_rc
    skipped = re.search(r"skipped=(\d+)", text)
    if verdict == PASS and skipped and n is not None and int(skipped.group(1)) >= n:
        verdict, detail = SKIP, "every case was skipped"
    h = qemu.hashlib.sha256(HOST_VOLATILE.sub("_", text).encode()).hexdigest()[:16]
    return Run(verdict, detail, text, h, n, 0, 0, host_s, seed)


def execute(test, ctx, seed):
    absent = missing(test)
    if absent:
        return Run(SKIP, "needs %s" % ", ".join(absent), seed=seed)
    return execute_guest(test, ctx, seed) if test.guest else execute_host(test, ctx, seed)


def _diff(test, a, b):
    def norm(text):
        text = qemu.normalize(text)
        for pattern in test.volatile:
            text = re.sub(pattern, "#", text)
        return text
    na, nb = norm(a.output), norm(b.output)
    return "".join(difflib.unified_diff(na.splitlines(True), nb.splitlines(True), "run seed %d" % a.seed,
                                        "run seed %d" % b.seed, n=2))[:4000]


def run_test(test, ctx):
    """Executes the test repeat times for each seed and applies the determinism rules to the runs."""
    runs = [execute(test, ctx, seed) for seed in ctx.seeds for _ in range(ctx.repeat)]
    if test.seed_sensitive and len(ctx.seeds) == 1 and runs[0].verdict == PASS:
        runs.append(execute(test, ctx, ctx.seeds[0] + 1))      # the seed must be what makes the output differ
    seeds = sorted({r.seed for r in runs})
    if all(r.verdict == SKIP for r in runs):
        return Result(test, SKIP, runs, runs[0].detail)
    verdicts = {r.verdict for r in runs}
    res = Result(test, runs[0].verdict, runs, runs[0].detail)
    if len(verdicts) > 1:
        bad = next(r for r in runs if r.verdict != runs[0].verdict)
        res.verdict = NONDET
        res.detail = "verdicts differ between runs: %s" % ", ".join("%s(seed %d)" % (r.verdict, r.seed) for r in runs)
        res.diff = _diff(test, runs[0], bad)
        return res
    if len(verdicts) == 1 and runs[0].verdict != PASS:
        return res
    if test.deterministic and ctx.accel == "tcg":
        for seed in seeds:
            same = [r for r in runs if r.seed == seed]
            odd = next((r for r in same if r.output_hash != same[0].output_hash), None)
            if odd:
                res.verdict, res.detail = NONDET, "output differs between runs with seed %d" % seed
                res.diff = _diff(test, same[0], odd)
                return res
        if test.seed_sensitive and len(seeds) > 1 and len({r.output_hash for r in runs}) == 1:
            res.verdict, res.detail = FAIL, "output does not depend on the seed"
    return res


def is_bad(result):
    return result.verdict in BAD
