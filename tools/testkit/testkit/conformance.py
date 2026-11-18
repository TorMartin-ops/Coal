"""The shell conformance corpus (tools/harness/conformance/cases) as deterministic guest tests.

Each case is a script with the stdout and exit status it must produce on Linux. Here every case of a category runs
inside one guest, started by init, with the scripts copied onto a private copy of the disk image; the host compares what
came back on the serial line. No sockets or servers are involved. The network category needs servers on the host and
stays with the host-driven `conformance` suite.

Coal does not pass every case yet. baselines/conformance_known_failures.txt lists the ones that are expected to fail;
a case outside that list that fails is a regression, and a listed case that passes is reported so the list can shrink.
"""
import importlib.util
import os
import re

from .model import Test
from .qemu import GuestSpec

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(HERE))), "tools", "harness", "conformance")
BASELINE = os.path.join(os.path.dirname(HERE), "baselines", "conformance_known_failures.txt")
CATEGORIES = ("files", "pipes", "processes", "signals", "text")
KLOG = re.compile(r"^\[\s*\d+\.\d{3}\] \[\w+\s*\] .*\n", re.M)
DRIVER = """cd /
n=0
while [ -f /tmp/conf/$n.sh ]; do
    echo "@@CASE $n"
    rm -rf /tmp/work; mkdir /tmp/work; cd /tmp/work
    /bin/sh /tmp/conf/$n.sh < /dev/null 2> /dev/null
    printf '\\n@@EXIT %d\\n' $?
    cd /
    n=$((n + 1))
done
"""


def _load():
    spec = importlib.util.spec_from_file_location("conformance_run", os.path.join(CASES, "run.py"))
    mod = importlib.util.module_from_spec(spec)
    import sys
    sys.path.insert(0, os.path.join(CASES, ".."))
    spec.loader.exec_module(mod)
    return mod


def known_failures():
    try:
        with open(BASELINE) as f:
            return {l.strip() for l in f if l.strip() and not l.startswith("#")}
    except OSError:
        return set()


def judge(cases, text):
    """(failed ids, unparsed message). The stdout of case n sits between @@CASE n and @@EXIT code."""
    text = KLOG.sub("", text.replace("\r", ""))
    seen, failed = {}, set()
    for m in re.finditer(r"@@CASE (\d+)\n(.*?)\n@@EXIT (\d+)\n", text, re.S):
        seen[int(m.group(1))] = (m.group(2), int(m.group(3)))
    for i, c in enumerate(cases):
        got = seen.get(i)
        if got is None or got[1] != c.exit_code or got[0] != c.stdout:
            failed.add("%s/%s" % (c.category, c.name))
    return failed, len(seen)


def tests():
    mod = _load()
    out = []
    for cat in CATEGORIES:
        cases = mod.load_cases(mod.CASES, cat)
        files = {"/tmp/conf/%d.sh" % i: c.script.encode() for i, c in enumerate(cases)}
        files["/tmp/conf/run.sh"] = DRIVER.encode()

        def check(text, cases=cases):
            failed, ran = judge(cases, text)
            known = known_failures()
            new = sorted(failed - known)
            fixed = sorted(k for k in known if k.split("/")[0] == cases[0].category and k not in failed)
            if ran < len(cases):
                return "only %d of %d cases ran to completion" % (ran, len(cases))
            if new:
                return "%d regressions: %s" % (len(new), ", ".join(new))
            if fixed:
                return "baseline is stale, these now pass: %s" % ", ".join(fixed)
            return None

        out.append(Test("conformance", cat, guest=GuestSpec(argv=["/bin/sh", "/tmp/conf/run.sh"], files=files, budget_s=600),
                        deterministic=True, cost_s=3 + len(cases) * 0.1, tags=("guest", "conformance"), needs=("qemu", "mtools"),
                        count=re.compile(r"(@@CASE) \d+"), check=check))
    return out
