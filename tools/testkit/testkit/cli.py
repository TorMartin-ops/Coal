"""coaltest command line."""
import argparse
import concurrent.futures
import fnmatch
import os
import subprocess
import sys
import threading
import time

from . import build as buildmod
from . import changed, load, qemu, report, repro, runner, slots, suites
from .model import BAD

CI_TAGS_OFF = ("host-io", "network", "bench", "vsock")


def parse(argv):
    ap = argparse.ArgumentParser(prog="coaltest", description="Coal OS tests: deterministic, isolated, no retries.")
    ap.add_argument("select", nargs="*", help="suite names, suite/test ids or test name globs")
    ap.add_argument("--all", action="store_true", help="every suite")
    ap.add_argument("--tag", action="append", default=[], help="only tests with this tag")
    ap.add_argument("--exclude-tag", action="append", default=[], help="skip tests with this tag")
    ap.add_argument("--changed", nargs="?", const="main", metavar="BASE",
                    help="suites for the paths touched in the working tree and since the merge base with BASE")
    ap.add_argument("--ci", action="store_true", help="CI profile: hermetic suites, --repeat 2, JSON and JUnit output")
    ap.add_argument("--list", action="store_true", help="list suites and tests with tags, devices and cost")
    ap.add_argument("--lint", action="store_true", help="check the registry against CMakeLists.txt")
    ap.add_argument("--slots", action="store_true", help="show the admission slots of this host")
    ap.add_argument("--repeat", type=int, default=1, metavar="N", help="run each test N times with the same seed and "
                    "require identical verdicts and outputs")
    ap.add_argument("--seeds", type=int, default=1, metavar="K", help="run each test with K different seeds")
    ap.add_argument("--seed", type=int, default=1, help="first seed (default 1)")
    ap.add_argument("-j", "--jobs", type=int, default=None, help="tests in parallel (default: admission slots)")
    ap.add_argument("--accel", choices=("tcg", "kvm"), default="tcg",
                    help="kvm is fast but not repeatable: determinism checks and replay are off")
    ap.add_argument("--build-dir", default=None, help="CMake build directory (default build/testkit)")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--out", default=None, help="results directory (default build/testkit-results)")
    ap.add_argument("--json", metavar="FILE")
    ap.add_argument("--junit", metavar="FILE")
    ap.add_argument("--no-repro", action="store_true", help="do not record and replay failures")
    ap.add_argument("--replay", metavar="BUNDLE", help="replay a repro bundle (add --gdb to attach gdb)")
    ap.add_argument("--gdb", action="store_true")
    return ap.parse_args(argv)


def select(args, all_suites):
    chosen = []
    pats = args.select
    if args.changed is not None:
        paths = changed.touched(suites.REPO, args.changed)
        hit = changed.suites_for(paths, all_suites)
        print("changed paths: %d; suites: %s" % (len(paths), ", ".join(s.name for s in hit) or "none"), file=sys.stderr)
        pats = pats + [s.name for s in hit]
    ci = args.ci
    for s in all_suites:
        for t in s.tests:
            tags = set(s.tags) | set(t.tags)
            take = args.all or ci
            for p in pats:
                if p == s.name or p == t.id or fnmatch.fnmatch(t.name, p) or fnmatch.fnmatch(t.id, p):
                    take = True
            if not take:
                continue
            if ci and not args.select and tags & set(CI_TAGS_OFF):
                continue
            if args.tag and not tags & set(args.tag):
                continue
            if tags & set(args.exclude_tag):
                continue
            chosen.append(t)
    return chosen


def listing(all_suites):
    for s in all_suites:
        print("%-12s %2d tests  ~%4.0f s  %s" % (s.name, len(s.tests), s.cost_s, s.doc))
        for t in s.tests:
            kind = "guest" if t.guest else ("host+qemu" if t.qemu else "host")
            det = "deterministic" if t.deterministic else "verdict-only"
            extra = "" if not t.guest else "  %s %dM" % (t.guest.profile, t.guest.memory_mb)
            print("    %-26s %-10s %-13s ~%3.0f s  tags=%s needs=%s%s" % (
                t.name, kind, det, t.cost_s, ",".join(t.tags) or "-", ",".join(t.needs) or "-", extra))


def main(argv=None):
    args = parse(argv)
    repo = suites.REPO
    if args.slots:
        print("slot dir %s, %d slots" % (slots.slot_dir(), slots.slot_count()))
        for i, busy in slots.status():
            print("  slot %d: %s" % (i, "busy" if busy else "free"))
        return 0
    all_suites = suites.all_suites()
    if args.list:
        listing(all_suites)
        return 0
    if args.lint:
        unused, unknown = suites.orphans()
        for p in unused:
            print("guest program not covered by any test: %s" % p)
        for p in unknown:
            print("test refers to a program that is not built: %s" % p)
        return 1 if unused or unknown else 0
    if args.replay:
        script = os.path.join(args.replay, "replay.sh")
        return subprocess.call(["sh", script] + (["gdb"] if args.gdb else []))
    if args.ci:
        args.repeat = max(args.repeat, 2) if args.accel == "tcg" else 1
    tests = select(args, all_suites)
    if not tests:
        print("nothing selected; try --all, a suite name or --list", file=sys.stderr)
        return 2
    out_dir = os.path.abspath(args.out or os.path.join(repo, "build", "testkit-results"))
    os.makedirs(out_dir, exist_ok=True)
    build_dir = os.path.abspath(args.build_dir or os.path.join(repo, "build", "testkit"))
    needs_build = any(t.guest or t.qemu or t.cmd and any("{build}" in a for a in t.cmd) for t in tests)
    b = None
    t_start = time.monotonic()
    if needs_build:
        try:
            if args.no_build:
                k, d = (os.path.join(build_dir, n) for n in ("kernel.bin", "disk.pristine.img"))
                b = buildmod.Build(build_dir, k, d, os.path.join(build_dir, "disk.img"), qemu.sha256_file(k), qemu.sha256_file(d))
            else:
                print("building into %s ..." % build_dir, file=sys.stderr)
                b = buildmod.build(repo, build_dir, os.path.join(out_dir, "build.log"))
        except (buildmod.BuildError, OSError) as e:
            print("build failed: %s" % e, file=sys.stderr)
            return 3
    else:
        b = buildmod.Build(build_dir, "", "", "", "", "")
    ctx = runner.Context(repo, b, out_dir, [args.seed + i for i in range(args.seeds)], args.repeat, args.accel,
                         repro=not args.no_repro and args.accel == "tcg")
    jobs = args.jobs or slots.slot_count()
    print("%d tests, repeat %d, seeds %s, jobs %d, slots %d, host load scale %.1f, qemu icount %s" % (
        len(tests), ctx.repeat, ctx.seeds, jobs, slots.slot_count(), load.scale(), qemu.BASE_ICOUNT % ctx.shift),
        file=sys.stderr)
    lock = threading.Lock()
    started = time.time()

    def one(t):
        r = runner.run_test(t, ctx)
        if runner.is_bad(r) and ctx.repro:
            try:
                r.bundle = repro.make_bundle(t, ctx, r, os.path.join(out_dir, "repro"), started)
            except Exception as e:                      # a broken bundle must not hide the failure itself
                r.detail += "  (repro bundle failed: %s)" % e
        with lock:
            print("%-14s %-34s %s" % (r.verdict, t.id, r.detail), file=sys.stderr, flush=True)
        return r

    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            results = list(pool.map(one, tests))
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130
    wall = time.monotonic() - t_start
    print()
    print(report.table(results))
    print("total wall time %.1f s (%.1f min)" % (wall, wall / 60))
    meta = {"qemu": qemu.qemu_version(), "kernel_sha256": b.kernel_sha, "disk_sha256": b.disk_sha, "repeat": ctx.repeat,
            "seeds": ctx.seeds, "accel": ctx.accel, "icount": qemu.BASE_ICOUNT % ctx.shift, "wall_s": round(wall, 1),
            "slots": slots.slot_count(), "load_scale": load.scale()}
    json_path = args.json or (os.path.join(out_dir, "results.json") if args.ci else None)
    junit_path = args.junit or (os.path.join(out_dir, "junit.xml") if args.ci else None)
    if json_path:
        with open(json_path, "w") as f:
            f.write(report.to_json(results, meta))
    if junit_path:
        with open(junit_path, "w") as f:
            f.write(report.to_junit(results))
    return 1 if any(r.verdict in BAD for r in results) else 0
