"""The data the registry, the runner and the reports share."""
import re
from dataclasses import dataclass, field

from .qemu import GuestSpec

PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"
NONDET = "NONDETERMINISM"
BAD = {FAIL, "TIMEOUT", "CRASH", "PANIC", "WATCHDOG", NONDET}


@dataclass
class Test:
    suite: str
    name: str
    guest: GuestSpec = None            # a program run by init inside one QEMU
    cmd: list = None                   # or a host command, relative to the repository root ({py} = this python)
    env: dict = field(default_factory=dict)
    qemu: bool = False                 # a host command that starts QEMU itself (it inherits our admission slot)
    deterministic: bool = False        # the normalised output must be identical on every run with the same seed
    cost_s: float = 5.0                # estimated host seconds, for ordering and `--list`
    tags: tuple = ()
    needs: tuple = ()                  # devices/tools: qemu, cmake, gcc, vhost-device-vsock ...
    expect: str = None                 # regex that must match the output of a passing run
    count: re.Pattern = None           # regex whose first group is the number of cases/checks in the output
    check: object = None               # callable(text) -> None or a failure message (guest tests with a verdict of their own)
    volatile: tuple = ()               # regexes for numbers that legitimately vary with device timing (blanked before hashing)
    seed_sensitive: bool = False       # output must differ between seeds (it prints random data)

    @property
    def id(self):
        return "%s/%s" % (self.suite, self.name)


@dataclass
class Suite:
    name: str
    doc: str
    tags: tuple = ()
    paths: tuple = ()                  # repository paths whose change makes this suite worth running (--changed)
    tests: list = field(default_factory=list)

    @property
    def cost_s(self):
        return sum(t.cost_s for t in self.tests)

    @property
    def deterministic(self):
        return all(t.deterministic for t in self.tests)


@dataclass
class Run:
    """The outcome of one execution of one test."""
    verdict: str
    detail: str = ""
    output: str = ""
    output_hash: str = ""
    cases: int = None
    guest_ns: int = 0
    guest_insn: int = 0
    host_s: float = 0.0
    seed: int = 0
    workdir: str = ""


@dataclass
class Result:
    """All executions of one test: --repeat N and --seeds K give several."""
    test: Test
    verdict: str
    runs: list = field(default_factory=list)
    detail: str = ""
    diff: str = ""
    bundle: str = ""
