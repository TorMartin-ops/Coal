#!/usr/bin/env python3
"""Coverage-guided system call fuzzer for the Coal OS debug kernel.

  flock /tmp/coal-testkit/slots/0 fuzz.py --build BUILD_DIR --out OUT_DIR --seconds 540

BUILD_DIR is a build made with COAL_PROFILE=test (it holds kernel.bin, disk.img and fuzz_executor.elf). OUT_DIR keeps
state between runs: the qcow2 image with the clean snapshot, the corpus, the crashes. One run is meant to fit in one
host slot (at most ten minutes); campaign.py chains runs and releases the slot in between.

Each program runs in a fresh child inside the guest. After every program the edge bitmap that the QEMU plugin filled
is compared with everything seen so far; programs that reach new kernel code join the corpus. The kernel log is
scanned for sanitizer reports and panics; a finding is deduplicated by a hash of its message and top stack frames,
minimised, and written out with a C reproducer.
"""
import argparse
import hashlib
import json
import os
import random
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prog as P  # noqa: E402
import vm as V    # noqa: E402

REPORT_RE = re.compile(r"\] \[\w+\s*\] ksan: (KSAN|UBSAN|LOCKDEP): (.*)|KERNEL PANIC: (.*)|(stack smashing detected)")
FRAME_RE = re.compile(r"ksan:\s+#\d+ \[<[0-9a-f]+>\] ([\w.]+)\+")
INTERNAL = re.compile(r"^(ksan_|report$|hd_|kfence_|lockdep_|__ubsan|arith$|mismatch$|kernel_panic$|__stack_chk|"
                      r"kernel_fault_panic$|isr_|interrupt_return$|syscall_entry$|dump_frame$)")
BUCKET = bytes([0] + [1, 2, 4, 8, 8, 16, 16, 16, 16] + [32] * 24 + [64] * 96 + [128] * 128)   # AFL count classes
BUCKET = bytes(BUCKET[min(i, len(BUCKET) - 1)] if i < len(BUCKET) else 128 for i in range(256))
NONZERO = re.compile(rb"[^\x00]")


def findings(text):
    """Every distinct finding in a stretch of kernel log: [(signature, headline, text)]."""
    lines = text.splitlines()
    out = []
    i = 0
    while i < len(lines):
        m = REPORT_RE.search(lines[i])
        if not m:
            i += 1
            continue
        head = (m.group(2) or m.group(3) or m.group(4) or "").strip()
        j = i + 1
        frames = []
        while j < len(lines) and ("ksan:" in lines[j] and (FRAME_RE.search(lines[j]) or "by:" in lines[j] or "ksan: allocated" in lines[j] or "ksan: freed" in lines[j] or "ksan: last freed" in lines[j])):
            f = FRAME_RE.search(lines[j])
            if f and not frames_done(frames, lines[j]):
                frames.append(f.group(1))
            j += 1
        stack = [f for f in frames if not INTERNAL.match(f)][:4]
        kind = (m.group(1) or ("PANIC" if m.group(3) else "STACK"))
        norm = re.sub(r"0x[0-9a-f]+|\d+", "N", head)[:70]
        sig = hashlib.sha1(("|".join([kind, norm] + stack)).encode()).hexdigest()[:12]
        out.append((sig, "%s: %s" % (kind, head[:140]), "\n".join(lines[i:j])))
        i = j
    return out


def frames_done(frames, line):
    return False


class Fuzzer:
    def __init__(self, a):
        self.a = a
        self.rng = random.Random(a.seed)
        self.out = a.out
        for d in ("corpus", "crashes"):
            os.makedirs(os.path.join(self.out, d), exist_ok=True)
        self.vm = V.VM(a.build, self.out, memory_mb=a.memory, coverage=not a.no_coverage)
        self.virgin = bytearray(65536)         # bucketed counts seen so far, per edge
        self.corpus = []
        self.execs = 0
        self.crashes = {}
        self.t_start = time.monotonic()
        self.edges = 0
        self.stats_log = os.path.join(self.out, "coverage.csv")
        self.last_stat = 0
        self.timeouts = 0

    # -- state on disk -------------------------------------------------------------------------------------------------
    def load_state(self):
        d = os.path.join(self.out, "corpus")
        for name in sorted(os.listdir(d)):
            if name.endswith(".json"):
                with open(os.path.join(d, name)) as f:
                    self.corpus.append(P.Prog.from_json(f.read()))
        vp = os.path.join(self.out, "virgin.bin")
        if os.path.exists(vp):
            with open(vp, "rb") as f:
                data = f.read()
            self.virgin[:len(data)] = data
        cp = os.path.join(self.out, "crashes")
        for sig in os.listdir(cp):
            self.crashes[sig] = {"count": 0}
        self.edges = sum(1 for b in self.virgin if b)
        sp = os.path.join(self.out, "stats.json")
        self.prev = json.load(open(sp)) if os.path.exists(sp) else {"execs": 0, "seconds": 0.0, "chunks": 0}

    def save_state(self, final=False):
        with open(os.path.join(self.out, "virgin.bin"), "wb") as f:
            f.write(self.virgin)
        elapsed = time.monotonic() - self.t_start
        stats = {"execs": self.prev["execs"] + self.execs, "seconds": self.prev["seconds"] + elapsed,
                 "chunks": self.prev["chunks"] + (1 if final else 0), "edges": self.edges, "corpus": len(self.corpus),
                 "crashes": len(self.crashes), "timeouts": self.timeouts,
                 "restore_seconds_avg": (sum(self.vm.restore_seconds) / len(self.vm.restore_seconds)) if self.vm.restore_seconds else 0}
        with open(os.path.join(self.out, "stats.json"), "w") as f:
            json.dump(stats, f, indent=1)
        return stats

    def stat_line(self):
        now = time.monotonic()
        if now - self.last_stat < 30:
            return
        self.last_stat = now
        total = self.prev["seconds"] + now - self.t_start
        with open(self.stats_log, "a") as f:
            f.write("%d,%d,%d,%d,%d\n" % (total, self.prev["execs"] + self.execs, self.edges, len(self.corpus), len(self.crashes)))
        print("[%5ds] execs %d  edges %d  corpus %d  crashes %d" % (now - self.t_start, self.execs, self.edges,
                                                                    len(self.corpus), len(self.crashes)), flush=True)

    # -- coverage --------------------------------------------------------------------------------------------------------
    def new_bits(self, cov):
        """True if the run hit an edge, or an edge count class, that no earlier run did. Updates the map."""
        new = False
        classed = cov.translate(BUCKET)
        for m in NONZERO.finditer(classed):
            i = m.start()
            b = classed[i]
            if b & ~self.virgin[i]:
                if not self.virgin[i]:
                    self.edges += 1
                self.virgin[i] |= b
                new = True
        return new

    # -- running ---------------------------------------------------------------------------------------------------------
    def run_prog(self, p):
        """Execute p. Returns (result or None, findings, hung, died)."""
        self.vm.clear_coverage()
        res, hung, died = None, False, False
        try:
            res = self.vm.execute(p.to_wire(), timeout=self.a.exec_timeout)
        except TimeoutError:
            hung = True
        except (V.VMError, OSError):
            died = True
        self.execs += 1
        time.sleep(0.0)
        return res, findings(self.vm.new_log()), hung, died

    def restore(self):
        for attempt in range(3):
            try:
                if self.vm.alive():
                    self.vm.loadvm()
                else:
                    self.vm.start(load=True)
                    self.vm.drain(0.2)
                return
            except (V.VMError, OSError, TimeoutError):
                self.vm.stop()
        raise SystemExit("cannot restore the guest")

    def signature_of_hang(self, p):
        rip = self.vm.rip() or "unknown"
        return hashlib.sha1(("hang|" + rip).encode()).hexdigest()[:12], "HANG in %s" % rip, "no answer from the executor; guest in %s" % rip

    def handle_findings(self, p, fnd, hung, died):
        items = list(fnd)
        if hung:
            items.append(self.signature_of_hang(p))
        elif died and not items:
            items.append((hashlib.sha1(b"qemu-exit").hexdigest()[:12], "QEMU exited", "the guest stopped without a report"))
        new = []
        for sig, head, text in items:
            c = self.crashes.setdefault(sig, {"count": 0})
            c["count"] += 1
            if c["count"] == 1 and not os.path.isdir(os.path.join(self.out, "crashes", sig)):
                new.append((sig, head, text))
        for sig, head, text in new:
            print("NEW FINDING %s %s" % (sig, head), flush=True)
            self.record(sig, head, text, p)
        return bool(items)

    def record(self, sig, head, text, p):
        d = os.path.join(self.out, "crashes", sig)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "report.txt"), "w") as f:
            f.write(head + "\n\n" + text + "\n")
        with open(os.path.join(d, "prog.json"), "w") as f:
            f.write(p.to_json())
        with open(os.path.join(d, "prog.txt"), "w") as f:
            f.write(p.describe() + "\n")

    def reproduces(self, p, sig, tries=1):
        for _ in range(tries):
            self.restore()
            res, fnd, hung, died = self.run_prog(p)
            sigs = [s for s, _, _ in fnd]
            if hung:
                sigs.append(self.signature_of_hang(p)[0])
            if died and not sigs:
                sigs.append(hashlib.sha1(b"qemu-exit").hexdigest()[:12])
            if sig in sigs:
                return True
        return False

    def minimise(self, sig):
        d = os.path.join(self.out, "crashes", sig)
        p = P.Prog.from_json(open(os.path.join(d, "prog.json")).read())
        if not self.reproduces(p, sig, tries=2):
            with open(os.path.join(d, "NOTE"), "w") as f:
                f.write("did not reproduce from the clean snapshot; the program in prog.json is the one that triggered it\n")
            return
        budget = self.a.minimise_runs
        changed = True
        while changed and budget > 0:
            changed = False
            for k in reversed(range(len(p.calls))):                         # drop calls
                if len(p.calls) == 1 or budget <= 0:
                    break
                q = p.copy()
                P.remove_call(q, k)
                budget -= 1
                if self.reproduces(q, sig):
                    p, changed = q, True
            for k, c in enumerate(p.calls):                                  # simplify arguments
                for i, a in enumerate(c["args"]):
                    if budget <= 0 or (a[0] == P.CONST and a[1] == 0):
                        continue
                    if a[0] in (P.CONST, P.RESULT, P.ARENA32, P.ARENA64):
                        q = p.copy()
                        q.calls[k]["args"][i] = [P.CONST, 0]
                        budget -= 1
                        if self.reproduces(q, sig):
                            p, changed = q, True
            for off in list(p.blobs):                                        # shrink data
                data = p.blobs.get(off)
                if budget <= 0 or not data or len(data) <= 1:
                    continue
                for cand in (b"\0" * len(data), data[:len(data) // 2]):
                    q = p.copy()
                    q.blobs[off] = cand
                    budget -= 1
                    if self.reproduces(q, sig):
                        p, changed = q, True
                        break
        used = set()
        for c in p.calls:
            for kind, v in c["args"]:
                if kind in (P.ARENA, P.ARENA32, P.ARENA64):
                    used.add(v)
        p.blobs = {o: b for o, b in p.blobs.items() if any(o <= u < o + max(len(b), 1) + 4096 for u in used)}
        with open(os.path.join(d, "min.json"), "w") as f:
            f.write(p.to_json())
        with open(os.path.join(d, "min.txt"), "w") as f:
            f.write(p.describe() + "\n")
        head = open(os.path.join(d, "report.txt")).readline().strip()
        with open(os.path.join(d, "repro.c"), "w") as f:
            f.write(p.to_c("%s  signature %s" % (head, sig)))
        print("minimised %s to %d calls" % (sig, len(p.calls)), flush=True)

    # -- main loop -----------------------------------------------------------------------------------------------------------
    def next_prog(self):
        if not self.corpus or self.rng.random() < 0.12:
            return P.generate(self.rng)
        base = self.corpus[-1 - int(self.rng.random() ** 2 * len(self.corpus))]       # favour recent entries
        return P.mutate(self.rng, base, self.corpus)

    def add_corpus(self, p):
        self.corpus.append(p)
        name = "%06d" % len(self.corpus)
        with open(os.path.join(self.out, "corpus", name + ".json"), "w") as f:
            f.write(p.to_json())

    def start_vm(self):
        if self.vm.has_snapshot():
            self.vm.start(load=True)
            self.vm.drain(0.3)
        else:
            self.vm.boot_snapshot()

    def run(self):
        self.load_state()
        V.prepare_done = os.path.exists(self.vm.qcow)
        if not os.path.exists(self.vm.qcow):
            V.prepare_image(self.a.build, self.out)
        self.start_vm()
        deadline = self.t_start + self.a.seconds
        since_reload = 0
        pending_min = []
        while time.monotonic() < deadline:
            p = self.next_prog()
            res, fnd, hung, died = self.run_prog(p)
            if hung:
                self.timeouts += 1
            bad = self.handle_findings(p, fnd, hung, died)
            for sig in list(self.crashes):
                d = os.path.join(self.out, "crashes", sig)
                if os.path.isdir(d) and not os.path.exists(os.path.join(d, "min.json")) and not os.path.exists(os.path.join(d, "NOTE")) \
                        and sig not in pending_min:
                    pending_min.append(sig)
            if bad or hung or died:
                self.restore()
                since_reload = 0
                for sig in pending_min:
                    if time.monotonic() < deadline + 120:
                        self.minimise(sig)
                pending_min = []
                self.restore()
                continue
            cov = self.vm.read_coverage()
            if self.new_bits(cov):
                self.add_corpus(p)
            since_reload += 1
            if since_reload >= self.a.reload_every:
                self.restore()
                since_reload = 0
            self.stat_line()
        stats = self.save_state(final=True)
        self.vm.stop()
        print(json.dumps(stats))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=int, default=540)
    ap.add_argument("--seed", type=int, default=int(time.time()))
    ap.add_argument("--memory", type=int, default=96)
    ap.add_argument("--reload-every", type=int, default=400, help="restore the clean snapshot after this many programs")
    ap.add_argument("--exec-timeout", type=float, default=6.0)
    ap.add_argument("--minimise-runs", type=int, default=80)
    ap.add_argument("--no-coverage", action="store_true")
    f = Fuzzer(ap.parse_args())
    try:
        f.run()
    except KeyboardInterrupt:
        f.save_state()
        f.vm.stop()
    finally:
        f.vm.stop()


if __name__ == "__main__":
    main()
