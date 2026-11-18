"""Syscall programs: generation, mutation, serialisation (executor wire format and JSON), C reproducer."""
import json
import random
import struct

import syscalls as sc

MAGIC_REQ = 0x315A4643
MAX_CALLS = 24
RELOCATE = 0x40000                 # offset added to spliced-in data so it cannot overlap the receiving program's

CONST, RESULT, ARENA, ARENA32, ARENA64 = 0, 1, 2, 3, 4


class Prog:
    def __init__(self):
        self.calls = []            # {"nr", "name", "args": [[kind, value], ...]}
        self.blobs = {}            # arena offset -> bytes
        self.next_off = sc.DATA_START

    # -- arena -------------------------------------------------------------------------------------------------
    def alloc(self, n):
        off = self.next_off
        self.next_off = (off + n + 15) & ~15
        if self.next_off >= RELOCATE - 0x2000:
            self.next_off = sc.DATA_START
        return off

    def blob(self, data):
        off = self.alloc(max(len(data), 1))
        self.blobs[off] = data
        return off

    # -- resources -----------------------------------------------------------------------------------------------
    def resources(self, kind, before):
        out = []
        for i, c in enumerate(self.calls[:before]):
            d = SYS_BY_NAME.get(c["name"])
            if not d:
                continue
            if d.returns == kind:
                out.append((RESULT, i))
            if kind == "fd" and d.stores:
                arg, n = d.stores
                a = c["args"][arg] if arg < len(c["args"]) else None
                if a and a[0] == ARENA:
                    out.extend((ARENA32, a[1] + 4 * k) for k in range(n))
        return out

    # -- (de)serialisation ---------------------------------------------------------------------------------------
    def to_json(self):
        return json.dumps({"calls": self.calls, "blobs": {str(k): v.hex() for k, v in self.blobs.items()}})

    @staticmethod
    def from_json(text):
        d = json.loads(text) if isinstance(text, str) else text
        p = Prog()
        p.calls = d["calls"]
        p.blobs = {int(k): bytes.fromhex(v) for k, v in d["blobs"].items()}
        p.next_off = max([sc.DATA_START] + [k + len(v) for k, v in p.blobs.items()
                                           if k < RELOCATE]) + 64
        return p

    def copy(self):
        return Prog.from_json(self.to_json())

    def to_wire(self):
        body = b""
        for off, data in sorted(self.blobs.items()):
            if off + len(data) > sc.ARENA_SIZE:
                continue
            body += struct.pack("<II", off, len(data)) + data + bytes(-len(data) % 8)
        for c in self.calls:
            args = c["args"]
            body += struct.pack("<HBB", c["nr"], len(args), 0)
            for kind, val in args:
                body += struct.pack("<BxxxxxxxQ", kind, val & ((1 << 64) - 1))
        return struct.pack("<IIII", MAGIC_REQ, len(self.calls), len(self.blobs), len(body)) + body

    def describe(self):
        lines = []
        for i, c in enumerate(self.calls):
            args = []
            for kind, v in c["args"]:
                args.append({CONST: "%#x", RESULT: "r%d", ARENA: "&%#x", ARENA32: "*u32(%#x)", ARENA64: "*u64(%#x)"}[kind] % v)
            lines.append("r%d = %s(%s)" % (i, c["name"], ", ".join(args)))
        return "\n".join(lines)

    # -- C reproducer --------------------------------------------------------------------------------------------
    def to_c(self, title=""):
        out = ["/* %s */" % title if title else "/* syscall reproducer written by tools/fuzz */",
               "#include <libc/syscall.h>", "#include <libc/unistd.h>", "#include <libc/string.h>", "",
               "#define ARENA 0x%xul" % sc.ARENA_BASE, "#define ARENA_SIZE 0x%xul" % sc.ARENA_SIZE, "",
               "int main(void) {", "    long r[%d] = { 0 };" % max(len(self.calls), 1),
               "    if (__sc6(SYS_MMAP, ARENA, ARENA_SIZE, 3, 0x32, -1, 0) != (long)ARENA) return 2;"]
        for off, data in sorted(self.blobs.items()):
            if len(data) <= 64:
                for k in range(0, len(data), 16):
                    chunk = data[k:k + 16]
                    out.append("    memcpy((void *)(ARENA + 0x%x), \"%s\", %d);" % (
                        off + k, "".join("\\x%02x" % b for b in chunk), len(chunk)))
            else:
                out.append("    { static const unsigned char d[] = {%s}; memcpy((void *)(ARENA + 0x%x), d, %d); }" % (
                    ",".join(str(b) for b in data), off, len(data)))
        for i, c in enumerate(self.calls):
            vals = []
            for kind, v in c["args"]:
                vals.append({CONST: "%dul" % v, RESULT: "(unsigned long)r[%d]" % v if v < i else "~0ul",
                             ARENA: "ARENA + 0x%x" % v, ARENA32: "*(volatile unsigned *)(ARENA + 0x%x)" % v,
                             ARENA64: "*(volatile unsigned long *)(ARENA + 0x%x)" % v}[kind])
            vals += ["0"] * (6 - len(vals))
            if c["name"] in ("fork", "vfork", "clone"):
                out.append("    r[%d] = fork(); if (r[%d] == 0) { *(volatile char *)ARENA = 1; _exit(0); } "
                           "if (r[%d] > 0) waitpid(r[%d], 0, 0);" % (i, i, i, i))
            else:
                out.append("    r[%d] = __sc6(%d, %s); /* %s */" % (i, c["nr"], ", ".join("(long)(%s)" % v for v in vals), c["name"]))
        out += ["    return 0;", "}", ""]
        return "\n".join(out)


SYSCALLS = sc.build()
SYS_BY_NAME = {s.name: s for s in SYSCALLS}
PRODUCERS = [s for s in SYSCALLS if s.returns or s.stores]
# Calls that exercise the most kernel code get a larger share.
WEIGHT = {"open": 4, "openat": 3, "read": 3, "write": 4, "close": 2, "mmap": 4, "munmap": 2, "mprotect": 3, "socket": 3,
          "socketpair": 2, "pipe": 3, "pipe2": 2, "fork": 1, "clone": 1, "ioctl": 3, "fcntl": 3, "poll": 2, "select": 2,
          "rt_sigaction": 3, "sendto": 2, "recvfrom": 2, "connect": 2, "bind": 2, "listen": 1, "accept": 1, "dup": 1,
          "dup2": 2, "getdents64": 2, "rename": 2, "link": 2, "unlink": 2, "mkdir": 2, "rmdir": 2, "symlink": 2, "stat": 2,
          "lseek": 2, "pread64": 2, "pwrite64": 2, "writev": 2, "readv": 2, "kill": 2, "wait4": 2, "execve": 1,
          "setsockopt": 2, "getsockopt": 2, "brk": 2, "madvise": 1, "sigaltstack": 2, "rt_sigprocmask": 2}


def _pick(rng, pool):
    weights = [WEIGHT.get(s.name, 1) for s in pool]
    return rng.choices(pool, weights)[0]


def gen_call(rng, prog, index, desc=None):
    d = desc or _pick(rng, SYSCALLS)
    ctx = sc.Ctx(rng, prog, index)
    args = [list(t.gen(ctx, i)) for i, t in enumerate(d.args)]
    return {"nr": d.nr, "name": d.name, "args": args}


def generate(rng, ncalls=None):
    p = Prog()
    n = ncalls or rng.randint(2, 10)
    for i in range(n):
        d = _pick(rng, PRODUCERS) if i < 2 and rng.random() < 0.7 else None
        p.calls.append(gen_call(rng, p, i, d))
    return p


# ---- mutation ----------------------------------------------------------------------------------------------------

def _mutate_value(rng, v):
    r = rng.random()
    if r < 0.2:
        return v ^ (1 << rng.randrange(64))
    if r < 0.4:
        return (v + rng.choice([-1, 1, -8, 8, 4096, -4096])) & ((1 << 64) - 1)
    if r < 0.7:
        return rng.choice(sc.SPECIAL_INTS)
    if r < 0.85:
        return rng.getrandbits(rng.choice([8, 16, 32]))
    return 0


def _drop_refs(prog, k):
    """Call k is going away: later references to it become -1, later indices shift down."""
    for c in prog.calls[k:]:
        for a in c["args"]:
            if a[0] == RESULT:
                if a[1] == k:
                    a[0], a[1] = CONST, (1 << 64) - 1
                elif a[1] > k:
                    a[1] -= 1


def remove_call(prog, k):
    del prog.calls[k]
    _drop_refs(prog, k)
    for c in prog.calls[:k]:
        pass


def _mutate_blob(rng, data):
    if not data:
        return bytes([rng.getrandbits(8)])
    b = bytearray(data)
    r = rng.random()
    if r < 0.5:
        b[rng.randrange(len(b))] = rng.choice([0, 0xff, 0x7f, 0x80, rng.getrandbits(8)])
    elif r < 0.7:
        b[rng.randrange(len(b))] ^= 1 << rng.randrange(8)
    elif r < 0.85 and len(b) > 1:
        del b[rng.randrange(len(b)):]
    else:
        b += bytes(rng.getrandbits(8) for _ in range(rng.randint(1, 16)))
    return bytes(b[:8192])


def splice(rng, prog, other):
    if any(off >= RELOCATE for off in other.blobs) or any(off >= RELOCATE for off in prog.blobs):
        return
    take = other.calls[:rng.randint(1, min(4, len(other.calls)))]
    base = len(prog.calls)
    for c in take:
        c = json.loads(json.dumps(c))
        for a in c["args"]:
            if a[0] == RESULT:
                a[1] = a[1] + base if a[1] < len(take) else (1 << 64) - 1
                a[0] = RESULT if a[1] != (1 << 64) - 1 else CONST
            elif a[0] in (ARENA, ARENA32, ARENA64) and a[1] >= sc.DATA_START:
                a[1] += RELOCATE
        prog.calls.append(c)
    for off, data in other.blobs.items():
        prog.blobs[off + RELOCATE] = data


def mutate(rng, prog, corpus=None):
    p = prog.copy()
    for _ in range(rng.choice([1, 1, 2, 3, 5])):
        r = rng.random()
        if r < 0.35 and p.calls:                                  # change an argument
            c = rng.choice(p.calls)
            if c["args"]:
                i = rng.randrange(len(c["args"]))
                d = SYS_BY_NAME.get(c["name"])
                if d and i < len(d.args) and rng.random() < 0.5:
                    ctx = sc.Ctx(rng, p, p.calls.index(c))
                    c["args"][i] = list(d.args[i].gen(ctx, i))
                elif c["args"][i][0] in (CONST, ARENA):
                    c["args"][i][1] = _mutate_value(rng, c["args"][i][1]) if c["args"][i][0] == CONST else \
                        sc.DATA_START + (rng.randrange(0, 0x3000) & ~7)
        elif r < 0.5 and p.blobs:                                 # change data
            off = rng.choice(list(p.blobs))
            p.blobs[off] = _mutate_blob(rng, p.blobs[off])
        elif r < 0.7 and len(p.calls) < MAX_CALLS:                # insert a call
            k = rng.randint(0, len(p.calls))
            for c in p.calls[k:]:
                for a in c["args"]:
                    if a[0] == RESULT and a[1] >= k:
                        a[1] += 1
            p.calls.insert(k, gen_call(rng, p, k))
        elif r < 0.8 and len(p.calls) > 1:                        # remove a call
            remove_call(p, rng.randrange(len(p.calls)))
        elif r < 0.88 and p.calls and len(p.calls) < MAX_CALLS:   # repeat a call
            k = rng.randrange(len(p.calls))
            c = json.loads(json.dumps(p.calls[k]))
            p.calls.insert(k + 1, c)
            for c2 in p.calls[k + 2:]:
                for a in c2["args"]:
                    if a[0] == RESULT and a[1] > k:
                        a[1] += 1
        elif corpus and len(p.calls) < MAX_CALLS - 4:             # splice from another program
            splice(rng, p, rng.choice(corpus))
        elif p.calls:
            c = rng.choice(p.calls)
            for a in c["args"]:
                if rng.random() < 0.3 and a[0] == CONST:
                    a[1] = _mutate_value(rng, a[1])
    if not p.calls:
        return generate(rng)
    return p
