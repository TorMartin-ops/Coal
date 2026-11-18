"""The registry: every suite and every test of Coal OS, in one place.

Guest programs are discovered from CMakeLists.txt and checked against the table below (see `orphans`), so a new
user-space test program cannot be forgotten. Host tests under tools/*/tests are discovered by file name.
"""
import glob
import os
import re

from .model import Suite, Test
from .qemu import GuestSpec

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
SRC = "src/CoalOS"
CHECKS = re.compile(r"(\d+) checks, \d+ failed")
UNITTEST = re.compile(r"^Ran (\d+) tests?", re.M)
OK_LINES = re.compile(r"^ok  ", re.M)
SH = "/bin/sh"

# guest program name (as in add_user_program) -> why it is not a test of its own
NOT_TESTS = {"coalbox": "applet multi-call binary", "init": "PID 1", "shell": "installed as /bin/sh", "snake": "game",
             "agentd": "exercised by the gateway suites", "coalbench": "bench suite", "coalbench_null": "bench helper"}


def oom_rounds(text):
    """The OOM killer logs one line per round, 20 rounds (the kernel log goes to COM1 with the test output)."""
    n = text.count("killing pid")
    return None if n == 20 else "the OOM killer logged %d lines, expected 20" % n


def guest(name, argv, expect=None, memory=128, profile="ide", budget=120, cost=1.0, tags=(), **kw):
    return Test("guest", name, guest=GuestSpec(argv=argv, memory_mb=memory, profile=profile, budget_s=budget),
                deterministic=kw.pop("deterministic", True), cost_s=cost, tags=("guest",) + tuple(tags), needs=("qemu",),
                expect=expect, count=kw.pop("count", CHECKS), **kw)


def host(suite, name, cmd, cost=5.0, tags=(), count=UNITTEST, qemu=False, deterministic=False, needs=(), env=None, **kw):
    return Test(suite, name, cmd=cmd if cmd[0] == "{sh}" else ["{py}"] + cmd, cost_s=cost, tags=tuple(tags), count=count, qemu=qemu,
                deterministic=deterministic, needs=(("qemu", "cmake") if qemu else ()) + tuple(needs),
                env=env or {}, **kw)


def guest_tests():
    return [
        guest("hello", ["/usr/bin/hello"], expect=r"Hello, Coal OS!", count=None, tags=("smoke",)),
        guest("abi_test", ["/usr/bin/abi_test"], expect=r"abi_test: all passed", count=OK_LINES),
        guest("syscall_test", ["/usr/bin/syscall_test"], expect=r"48 checks, 0 failed", cost=1),
        guest("libc_test", ["/usr/bin/libc_test"], expect=r"libc_test: .* 0 failed|atexit handler ran", count=None),
        guest("fault_test", ["/usr/bin/fault_test"], expect=r"fault_test: \d+ checks, 0 failed"),
        guest("fs_stress", ["/usr/bin/fs_stress", "quick"], expect=r"fs_stress: \d+ checks, 0 failed", cost=2),
        guest("kernel_test", ["/usr/bin/kernel_test"], expect=r"kernel_test: \d+ checks, 0 failed", cost=10, budget=600),
        guest("kernel_test_oom", ["/usr/bin/kernel_test", "oom"], expect=r"kernel_test: \d+ checks, 0 failed",
              cost=5, budget=600, check=oom_rounds,
              volatile=(r"\d+ KiB resident",)),    # how far a hog got before it was killed depends on I/O timing
        guest("memtest", ["/usr/bin/memtest"], expect=r"memtest: PASSED", count=None, cost=1),
        guest("sh_test", [SH, "/usr/bin/sh_test.sh"], expect=r"sh_test: \d+ checks, 0 failed", cost=3, budget=300),
        guest("applet_test", [SH, "/usr/bin/applet_test.sh"], expect=r"applet_test: \d+ checks, 0 failed", cost=2),
        Test("guest", "seed", guest=GuestSpec(argv=["/usr/bin/seed_test"]), deterministic=True, cost_s=0.5,
             tags=("guest", "seed"), needs=("qemu",), expect=r"random:[0-9a-f]{32}", seed_sensitive=True),
    ]


def virtio_tests():
    out = []
    for prof, tag, mem in (("virtio", "virtio", 128), ("microvm", "microvm", 256)):
        for name, argv, expect in (("hello", ["/usr/bin/hello"], r"Hello, Coal OS!"),
                                   ("syscall_test", ["/usr/bin/syscall_test"], r"48 checks, 0 failed"),
                                   ("fs_stress", ["/usr/bin/fs_stress", "quick"], r"fs_stress: \d+ checks, 0 failed")):
            out.append(guest(name, argv, expect=expect, memory=mem, profile=prof, cost=3 if prof == "microvm" else 1.5,
                             tags=(tag,), count=None if name == "hello" else CHECKS))
    return out


def unit_tests():
    gw, mcp = "tools/coal-gateway/tests/", "tools/coal-mcp/tests/"
    no_qemu = {"COAL_TEST_NO_QEMU": "1"}
    tests = [
        host("unit", "gateway_frames", [gw + "test_frames.py"], 1, deterministic=True),
        host("unit", "gateway_http", [gw + "test_http.py"], 10),
        host("unit", "gateway_protocol_pty", [gw + "test_protocol.py", "HostProtocol"], 10, env=no_qemu, needs=("gcc",)),
        host("unit", "agentd_fuzz", [mcp + "test_agentd_fuzz.py"], 5, needs=("gcc",)),
        host("unit", "mcp_server", [mcp + "test_server.py"], 6, needs=("gcc",)),
        host("unit", "conformance_corpus", ["tools/harness/conformance/run.py", "--target", "host"], 5,
             count=re.compile(r"^total\s+(\d+)", re.M)),
        host("unit", "syscall_numbers", ["{sh}", SRC + "/scripts/check_syscall_numbers.sh"], 1, count=None),
    ]
    for path in sorted(glob.glob(os.path.join(REPO, "tools/*/tests/test_*.py"))):
        rel = os.path.relpath(path, REPO)
        if any(rel in " ".join(t.cmd) for t in tests):
            continue
        tests.append(host("unit", os.path.basename(path)[5:-3], [rel], 2, deterministic=False))   # discovered
    return tests


def gateway_tests():
    gw = "tools/coal-gateway/tests/test_protocol.py"
    return [
        host("gateway", "protocol_qemu", [gw, "QemuProtocol"], 60, tags=("host-io",), needs=("qemu", "cmake")),
        host("gateway", "protocol_virtio", [gw, "QemuVirtioProtocol"], 60, tags=("host-io", "virtio"),
             needs=("qemu", "cmake")),
        host("gateway", "protocol_vsock", [gw, "QemuVsockProtocol"], 60, tags=("host-io", "virtio", "vsock"),
             needs=("qemu", "cmake", "vhost-device-vsock")),
    ]


def smoke_tests():
    return [host("smoke", "gateway_end_to_end", ["tools/coal-mcp/tests/smoke_test.py", "{build}"], 120,
                 tags=("host-io", "network"), qemu=True, count=re.compile(r"^(ok)", re.M))]


def net_regressions(text):
    """run.py reports Coal's failures and exits 0 either way; compare them with the baseline like the guest categories."""
    from .conformance import known_failures
    if not re.search(r"^net\s+\d+", text, re.M):
        return "no net row in the corpus report"
    m = re.search(r"net, coal fails: (.*)", text)
    failed = {"net/" + n.strip() for n in m.group(1).split(",")} if m else set()
    known = {k for k in known_failures() if k.startswith("net/")}
    if failed - known:
        return "regressions: %s" % ", ".join(sorted(failed - known))
    if known - failed:
        return "baseline is stale, these now pass: %s" % ", ".join(sorted(known - failed))
    return None


def conformance_tests():
    from . import conformance
    net = host("conformance", "net_vs_host", ["tools/harness/conformance/run.py", "--target", "both", "--category", "net",
                                               "--build-dir", "{build}"], 60, tags=("host-io", "network"), qemu=True,
               count=re.compile(r"^net\s+(\d+)", re.M), check=net_regressions)
    return conformance.tests() + [net]


def bench_tests():
    return [host("bench", "boottime", ["tools/harness/boottime.py", "--runs", "2", "--icount", "--build-dir", "{build}"],
                 20, tags=("bench", "host-io"), qemu=True, count=None),
            guest("coalbench", ["/usr/bin/coalbench"], count=None, budget=600, cost=6, tags=("bench",),
                  expect=r"idle_irq", deterministic=False)]     # the last digits of every timing follow device timing


def all_suites():
    s = [Suite("unit", "host-only tests: codecs, HTTP, agentd on a pty, the conformance corpus on this host",
               ("unit", "host"), ("tools/coal-gateway", "tools/coal-mcp", "tools/harness", "tools/testkit"), unit_tests()),
         Suite("guest", "user-space test programs run by init under QEMU, deterministic (icount, fixed seed)",
               ("guest", "deterministic"), (SRC + "/kernel", SRC + "/userspace", SRC + "/include", SRC + "/boot",
                                            SRC + "/CMakeLists.txt"), guest_tests()),
         Suite("virtio", "the same programs on a virtio-blk root", ("guest", "virtio", "deterministic"),
               (SRC + "/kernel/drivers/virtio", SRC + "/kernel/drivers/pci.c"), [t for t in virtio_tests() if t.guest.profile == "virtio"]),
         Suite("microvm", "the same programs on QEMU's microvm machine with virtio-mmio",
               ("guest", "microvm", "deterministic"), (SRC + "/kernel/drivers/virtio", SRC + "/kernel/drivers/timer"),
               [t for t in virtio_tests() if t.guest.profile == "microvm"]),
         Suite("gateway", "the agent protocol and agentd against the real kernel (host-driven)", ("host-io", "gateway"),
               ("tools/coal-gateway", SRC + "/userspace/agentd", "docs/agent-vm"), gateway_tests()),
         Suite("smoke", "coal-mcp end to end: build, boot, commands, files, jobs, snapshots, network (host-driven)",
               ("host-io", "network", "smoke"), ("tools/coal-mcp", "tools/coal-gateway", SRC + "/userspace/agentd"),
               smoke_tests()),
         Suite("conformance", "shell-level corpus: every category in a guest, compared with the Linux expectations; network cases host-driven",
               ("conformance",), ("tools/harness", SRC + "/userspace/coalbox", SRC + "/userspace/sh", "tools/testkit/baselines"),
               conformance_tests()),
         Suite("bench", "boot time and micro-benchmarks", ("bench",), ("tools/bench", "tools/harness/boottime.py"),
               bench_tests())]
    for suite in s:
        for t in suite.tests:
            t.suite = suite.name
    return s


def orphans():
    """User-space test programs in CMakeLists.txt that no test runs, and the opposite."""
    with open(os.path.join(REPO, SRC, "CMakeLists.txt")) as f:
        programs = set(re.findall(r"add_user_program\((\w+)", f.read())) | {"abi_test"}
    used = set()
    for suite in all_suites():
        for t in suite.tests:
            if t.guest:
                used |= {os.path.basename(a)[:-3] if a.endswith(".sh") else os.path.basename(a)
                         for a in t.guest.argv if a.startswith("/usr/bin/")}
    scripts = {"sh_test", "applet_test"}
    return sorted(p for p in programs - used - set(NOT_TESTS) if p not in scripts), sorted(used - programs - scripts - {"sh"})
