"""System call descriptions for the fuzzer.

The list of calls and their argument counts and widths come from the kernel sources (the syscall table in
kernel/cpu/syscall.c, the handler prototypes in kernel/cpu/syscall_internal.h, the numbers in
include/libc/syscall_numbers.h). Hand-written type information (file descriptors, paths, flag sets, sizes, structures)
refines the calls that matter most; everything else gets generic integer and pointer arguments.
"""
import os
import re
import struct

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "CoalOS")

# Never fuzzed: they would end the executor, reboot or unmount what it runs from.
EXCLUDED = {"reboot", "mount", "umount2", "exit", "exit_group", "rt_sigreturn_", "sync"}

ARENA_BASE = 0x20000000
ARENA_SIZE = 1 << 20
DATA_START = 0x1000            # blobs and output buffers live above this arena offset

SPECIAL_INTS = [0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 31, 32, 63, 64, 100, 127, 128, 255, 256, 511, 512, 1000, 1023, 1024,
                4095, 4096, 4097, 0x7fff, 0x8000, 0xffff, 0x10000, 1 << 20, 0x7fffffff, 0x80000000, 0xffffffff,
                1 << 32, (1 << 62), (1 << 63) - 1, 1 << 63, (1 << 64) - 1, (1 << 64) - 2, (1 << 64) - 4096]

PATHS = ["/tmp/fz/a", "/tmp/fz/b", "/tmp/fz/c", "/tmp/fz/d", "/tmp/fz/d/e", "/tmp/fz/d/f", "/tmp/fz/d/../a",
         "/tmp/fz/link", "/tmp/fz/l2", "/tmp/fz", "/tmp/fz/", "/tmp", ".", "..", "/", "", "a", "d/a", "./a",
         "/dev/null", "/dev/zero", "/dev/console", "/dev/tty", "/dev/ttyS0", "/proc/self/status", "/proc/meminfo",
         "/proc/uptime", "/proc/mounts", "/proc/self", "/proc/1/status", "/bin/coalbox", "/bin/sh",
         "/usr/bin/hello", "/etc/passwd", "/etc/hosts", "/nonexistent", "/tmp/fz/" + "x" * 255,
         "/tmp/fz/" + "y" * 300, "/" + "/".join(["d"] * 40), "/tmp/fz/d/" * 12 + "z", "//tmp//fz//a", "/tmp/fz/a/b"]


class Ctx:
    """What an argument generator may use: randomness, the program so far, arena space."""

    def __init__(self, rng, prog, call_index):
        self.rng = rng
        self.prog = prog
        self.index = call_index
        self.lens = {}          # argument index -> length of the buffer generated for it


# ---- argument types ------------------------------------------------------------------------------------------

def _special(rng):
    return rng.choice(SPECIAL_INTS)


class Int:
    def __init__(self, lo=0, hi=1 << 16, specials=(), width=64, signed=False):
        self.lo, self.hi, self.specials, self.width, self.signed = lo, hi, tuple(specials), width, signed

    def gen(self, c, i):
        r = c.rng.random()
        if r < 0.15:
            v = _special(c.rng)
        elif r < 0.45 and self.specials:
            v = c.rng.choice(self.specials)
        else:
            v = c.rng.randint(self.lo, self.hi)
        return (0, v & ((1 << 64) - 1))


class Flags:
    def __init__(self, values, zero_ok=True):
        self.values, self.zero_ok = tuple(values), zero_ok

    def gen(self, c, i):
        rng = c.rng
        if rng.random() < 0.05:
            return (0, _special(rng) & ((1 << 64) - 1))
        v = 0
        for f in self.values:
            if rng.random() < 0.3:
                v |= f
        return (0, v)


class Enum:
    def __init__(self, values):
        self.values = tuple(values)

    def gen(self, c, i):
        return (0, c.rng.choice(self.values) if c.rng.random() > 0.05 else _special(c.rng))


class Fd:
    """A descriptor: usually a result of an earlier call or one the pipe/socketpair stored in memory."""

    def gen(self, c, i):
        rng = c.rng
        res = c.prog.resources("fd", c.index)
        r = rng.random()
        if res and r < 0.8:
            kind, v = rng.choice(res)
            return (kind, v)
        if r < 0.9:
            return (0, rng.choice([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 100, 1023]))
        return (0, rng.choice([(1 << 64) - 1, 1 << 31, 0x7fffffff, 0xffffffff, 1 << 32]))


class Path:
    def gen(self, c, i):
        rng = c.rng
        p = rng.choice(PATHS)
        if rng.random() < 0.1:
            p = p + rng.choice(["a", "/", "\xff", "\x01", ".."])
        data = p.encode("latin-1") + b"\0"
        c.lens[i] = len(data)
        return (2, c.prog.blob(data))


class Buf:
    """A pointer to input data (blob) or to room for output; `length` of it is reported to a following Len."""

    def __init__(self, size=(0, 256), data=True, text=False):
        self.size, self.data, self.text = size, data, text

    def gen(self, c, i):
        rng = c.rng
        n = rng.choice([0, 1, 2, 3, 7, 8, 16, 31, 64, 100, 255, 256, 512, 1000, 4096]) if rng.random() < 0.7 \
            else rng.randint(*self.size)
        n = min(n, 8192)
        c.lens[i] = n
        if rng.random() < 0.03:
            return (0, rng.choice([0, 1, 0xffffffff80000000, 0x7fffffffffff, 8, 0xfffffffffffff000]))
        if not self.data:
            return (2, c.prog.alloc(max(n, 16)))
        if self.text:
            blob = bytes(rng.choice(b"abcdefghijklmnopqrstuvwxyz \n0123456789") for _ in range(n))
        else:
            blob = bytes(rng.getrandbits(8) for _ in range(n)) if rng.random() < 0.5 else bytes([rng.getrandbits(8)]) * n
        return (2, c.prog.blob(blob))


class Len:
    """Size of the buffer given as argument `of`, now and then wrong."""

    def __init__(self, of, mult=1):
        self.of, self.mult = of, mult

    def gen(self, c, i):
        n = c.lens.get(self.of, 16) * self.mult
        r = c.rng.random()
        if r < 0.08:
            n += c.rng.choice([1, -1, 4096, -4096, 1 << 20])
        elif r < 0.12:
            return (0, _special(c.rng))
        return (0, n & ((1 << 64) - 1))


class Out:
    """Pointer to zeroed room in the arena."""

    def __init__(self, size=64):
        self.size = size

    def gen(self, c, i):
        if c.rng.random() < 0.04:
            return (0, c.rng.choice([0, 1, 0xffffffff80000000, 0x7fffffffffff, 8, 0xfffffffffffff000]))
        c.lens[i] = self.size
        return (2, c.prog.alloc(self.size))


class Struct:
    """A pointer to a structure built by fn(rng) -> bytes (or None for a NULL pointer)."""

    def __init__(self, fn, null=0.05):
        self.fn, self.null = fn, null

    def gen(self, c, i):
        if c.rng.random() < self.null:
            return (0, 0)
        blob = self.fn(c.rng)
        c.lens[i] = len(blob)
        return (2, c.prog.blob(blob))


class Addr:
    """A user address for mmap, munmap, mprotect, brk: near the arena, in the mmap window, or wild."""

    def gen(self, c, i):
        rng = c.rng
        r = rng.random()
        if r < 0.35:
            return (0, ARENA_BASE + rng.randrange(0, ARENA_SIZE, 4096))
        if r < 0.55:
            return (0, ARENA_BASE + rng.randrange(0, ARENA_SIZE))
        if r < 0.7:
            return (0, 0x7f0000000000 + rng.randrange(0, 1 << 24, 4096))
        if r < 0.78:
            return (0, 0x400000 + rng.randrange(0, 1 << 20, 4096))
        mm = c.prog.resources("addr", c.index)
        if mm and r < 0.9:
            return rng.choice(mm)
        return (0, rng.choice([0, 0x1000, 0xffff, 0x10000, 0x7ffffffff000, 0x7fffffffe000, 0x800000000000,
                               0xffffffff80000000, 0xffff800000000000, (1 << 64) - 4096]))


class Pid:
    """Never the executor or init: the own process group, the own pid, or one that does not exist."""

    def gen(self, c, i):
        r = c.rng.random()
        res = c.prog.resources("pid", c.index)
        if res and r < 0.4:
            return c.rng.choice(res)
        return (0, c.rng.choice([0, 0, 99999, 300, (1 << 32) - 2]))


# ---- structures ------------------------------------------------------------------------------------------------

def _sockaddr(rng):
    r = rng.random()
    if r < 0.35:                                        # AF_UNIX
        path = rng.choice([b"/tmp/fz/sock", b"/tmp/fz/sock2", b"/tmp/fz/a", b"\0abstract", b"", b"x" * 107])
        body = struct.pack("<H", 1) + path + b"\0"
        return body + bytes(rng.randrange(0, 6))
    if r < 0.8:                                         # AF_INET
        ip = rng.choice([b"\x7f\0\0\x01", b"\x0a\0\x02\x0f", b"\x0a\0\x02\x02", b"\0\0\0\0", b"\xff\xff\xff\xff"])
        port = rng.choice([7, 22, 80, 1234, 5000, 0, 65535, 8080])
        return struct.pack("<H", 2) + struct.pack(">H", port) + ip + bytes(8)
    if r < 0.9:                                         # AF_VSOCK
        return struct.pack("<HHII", 40, 0, rng.choice([1024, 5, 0]), rng.choice([2, 3, 1, 0xffffffff])) + bytes(4)
    return bytes(rng.getrandbits(8) for _ in range(rng.choice([2, 8, 16, 32, 110])))


def _timespec(rng):
    return struct.pack("<qq", rng.choice([0, 0, 0, 1]), rng.choice([0, 1000, 1000000, 999999999, 1000000000, 5000000]))


def _timeval(rng):
    return struct.pack("<qq", rng.choice([0, 1, 1700000000]), rng.choice([0, 1, 999999, 1000000, 5000]))


def _pollfds(rng):
    out = b""
    for _ in range(rng.randint(1, 4)):
        out += struct.pack("<iHH", rng.choice([0, 1, 2, 3, 4, 5, 6, 7, -1, 100]), rng.choice([1, 4, 5, 0x19, 0x7fff, 0]), 0)
    return out


def _iovecs(rng):
    from_arena = lambda: ARENA_BASE + DATA_START + rng.randrange(0, 4096, 8)  # noqa: E731
    out = b""
    for _ in range(rng.randint(0, 4)):
        base = from_arena() if rng.random() < 0.9 else rng.choice([0, 1, 0xffffffff80000000])
        out += struct.pack("<QQ", base, rng.choice([0, 1, 16, 100, 4096, 1 << 20, (1 << 63) - 1]))
    return out or bytes(16)


def _msghdr(rng):
    ptr = lambda: ARENA_BASE + DATA_START + rng.randrange(0, 8192, 8)  # noqa: E731
    return struct.pack("<QIxxxxQQQQIxxxx", ptr() if rng.random() < 0.6 else 0, rng.choice([0, 16, 110]), ptr(),
                       rng.choice([0, 1, 2]), ptr(), rng.choice([0, 16, 32]), rng.choice([0, 0x40, 0x4000]))


def _sigaction(rng):
    handler = rng.choice([0, 1, ARENA_BASE + 0x1000, 0xffffffff80000000, 0x400000])
    flags = rng.choice([0, 0x04000000, 0x04000004, 0x14000000, 0x0c000000, 0x80000000, 0x04000001])
    return struct.pack("<QQQQ", handler, flags, rng.choice([0, ARENA_BASE + 0x2000]), rng.getrandbits(64))


def _sigset(rng):
    return struct.pack("<Q", rng.choice([0, (1 << 64) - 1, 1 << 8, 1 << 14, 0x7fffffff, rng.getrandbits(64)]))


def _rlimit(rng):
    return struct.pack("<QQ", rng.choice([0, 1, 1 << 20, 1 << 30, (1 << 64) - 1]), rng.choice([0, 1 << 20, (1 << 64) - 1]))


def _sigaltstack(rng):
    return struct.pack("<QiIQ", ARENA_BASE + rng.choice([0x8000, 0x9000, 0]), rng.choice([0, 1, 2, 4]),
                       0, rng.choice([0, 100, 2048, 8192, 1 << 16]))


def _argv(rng):
    return struct.pack("<QQQQ", ARENA_BASE + DATA_START + 0x400, ARENA_BASE + DATA_START + 0x440, 0, 0) + b"\0" * 8


# ---- hand-written descriptions ---------------------------------------------------------------------------------

O_FLAGS = [1, 2, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800, 0x10000, 0x20000, 0x80000, 0x101000]
PROT = [1, 2, 4]
MAP_FLAGS = [1, 2, 0x10, 0x20, 0x100, 0x4000, 0x8000, 0x40, 0x1000000]
MODE = Enum([0, 0o644, 0o755, 0o777, 0o600, 0o4755, 0o1777, 0o170000, 0xffff])
SIGNALS = Enum(list(range(0, 33)) + [64, 65, 100])
CLOCKS = Enum([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 100])
SOCK_DOMAIN = Enum([1, 2, 40, 10, 0, 16, 17, 3])
SOCK_TYPE = Flags([0], zero_ok=False)

DESC = {
    "read": [Fd(), Buf(data=False), Len(1)],
    "write": [Fd(), Buf(), Len(1)],
    "open": [Path(), Flags(O_FLAGS), MODE],
    "openat": [Enum([(1 << 64) - 100, 0, 1, 3, 4]), Path(), Flags(O_FLAGS), MODE],
    "close": [Fd()],
    "stat": [Path(), Out(144)],
    "lstat": [Path(), Out(144)],
    "fstat": [Fd(), Out(144)],
    "newfstatat": [Fd(), Path(), Out(144), Flags([0x100, 0x1000, 0x800])],
    "poll": [Struct(_pollfds), Int(1, 4, [0, 1, 2]), Enum([0, 0, 1, 5, 10, (1 << 32) - 1])],
    "lseek": [Fd(), Int(0, 4096, [(1 << 64) - 1, 1 << 40]), Enum([0, 1, 2, 3, 4, 5])],
    "mmap": [Addr(), Int(0, 1 << 16, [4096, 8192, 1 << 20, 1 << 30, 1 << 40]), Flags(PROT), Flags(MAP_FLAGS), Fd(),
             Int(0, 8192, [0, 4096])],
    "mprotect": [Addr(), Int(0, 1 << 16, [4096, 8192, 1 << 20]), Flags(PROT)],
    "munmap": [Addr(), Int(0, 1 << 16, [4096, 8192, 1 << 20])],
    "brk": [Addr()],
    "madvise": [Addr(), Int(0, 1 << 16, [4096]), Int(0, 20, [4, 8, 14])],
    "rt_sigaction": [SIGNALS, Struct(_sigaction), Out(32), Enum([8, 8, 8, 0, 4, 16])],
    "rt_sigprocmask": [Enum([0, 1, 2, 3]), Struct(_sigset), Out(8), Enum([8, 8, 8, 0, 4])],
    "ioctl": [Fd(), Enum([0x5401, 0x5402, 0x5403, 0x5404, 0x540e, 0x540f, 0x5410, 0x5413, 0x5414, 0x541b, 0x5421,
                          0x5450, 0x5451, 0x8933, 0x8912, 0x5422, 0x5409]), Out(64)],
    "pread64": [Fd(), Buf(data=False), Len(1), Int(0, 4096, [(1 << 64) - 1, 1 << 40])],
    "pwrite64": [Fd(), Buf(), Len(1), Int(0, 4096, [(1 << 64) - 1, 1 << 40])],
    "readv": [Fd(), Struct(_iovecs), Int(0, 4, [0, 1, 2, 3, 4, 1024, 1025])],
    "writev": [Fd(), Struct(_iovecs), Int(0, 4, [0, 1, 2, 3, 4, 1024, 1025])],
    "access": [Path(), Flags([1, 2, 4])],
    "pipe": [Out(8)],
    "pipe2": [Out(8), Flags([0x800, 0x80000, 0x4000])],
    "select": [Int(0, 8, [0, 1, 1024, 1025]), Out(128), Out(128), Out(128), Struct(_timeval)],
    "pselect6": [Int(0, 8, [0, 1, 1024, 1025]), Out(128), Out(128), Out(128), Struct(_timespec), Out(16)],
    "sched_yield": [],
    "dup": [Fd()],
    "dup2": [Fd(), Int(0, 40, [(1 << 31)])],
    "nanosleep": [Struct(_timespec, null=0.1), Out(16)],
    "clock_nanosleep": [CLOCKS, Flags([1]), Struct(_timespec, null=0.1), Out(16)],
    "getpid": [],
    "socket": [SOCK_DOMAIN, Flags([1, 2, 3, 0x800, 0x80000]), Enum([0, 0, 0, 1, 6, 17, 255])],
    "socketpair": [SOCK_DOMAIN, Flags([1, 2, 0x800, 0x80000]), Enum([0, 0]), Out(8)],
    "connect": [Fd(), Struct(_sockaddr, null=0.02), Len(1)],
    "bind": [Fd(), Struct(_sockaddr, null=0.02), Len(1)],
    "accept": [Fd(), Out(128), Out(4)],
    "accept4": [Fd(), Out(128), Out(4), Flags([0x800, 0x80000])],
    "listen": [Fd(), Int(0, 40, [0, 1, 128, 1 << 31])],
    "sendto": [Fd(), Buf(), Len(1), Flags([0x40, 0x4000, 0x8000, 0x800]), Struct(_sockaddr, null=0.5), Len(4)],
    "recvfrom": [Fd(), Buf(data=False), Len(1), Flags([0x40, 0x2, 0x100, 0x800]), Out(128), Out(4)],
    "sendmsg": [Fd(), Struct(_msghdr), Flags([0x40, 0x4000, 0x800])],
    "recvmsg": [Fd(), Struct(_msghdr), Flags([0x40, 0x2, 0x100, 0x800])],
    "shutdown": [Fd(), Enum([0, 1, 2, 3])],
    "getsockname": [Fd(), Out(128), Out(4)],
    "getpeername": [Fd(), Out(128), Out(4)],
    "setsockopt": [Fd(), Enum([1, 6, 0, 17, 41]), Enum([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 21, 0x1000]), Buf(), Len(3)],
    "getsockopt": [Fd(), Enum([1, 6, 0, 17]), Enum([1, 2, 3, 4, 5, 6, 7, 8, 9, 10]), Out(64), Out(4)],
    "clone": [Flags([0x100, 0x200, 0x400, 0x800, 0x10000, 0x20000, 0x1000, 0x4000, 0x8000, 0x10000]), Addr(), Out(16),
              Out(16), Out(16)],
    "fork": [],
    "vfork": [],
    "execve": [Enum([0]), Struct(_argv), Struct(_argv)],
    "wait4": [Pid(), Out(8), Flags([1, 2, 0x40000000, 0x20000000, 0x1000000]), Out(144)],
    "kill": [Pid(), SIGNALS],
    "tkill": [Pid(), SIGNALS],
    "tgkill": [Pid(), Pid(), SIGNALS],
    "uname": [Out(390)],
    "fcntl": [Fd(), Enum([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 1024, 1025, 1030, 1031, 1032, 12345]),
              Int(0, 1 << 12, [0, 1, 0x800, 0x400, 1 << 20])],
    "fsync": [Fd()],
    "fdatasync": [Fd()],
    "getcwd": [Buf(data=False), Len(0)],
    "chdir": [Path()],
    "rename": [Path(), Path()],
    "mkdir": [Path(), MODE],
    "rmdir": [Path()],
    "link": [Path(), Path()],
    "unlink": [Path()],
    "symlink": [Path(), Path()],
    "readlink": [Path(), Buf(data=False), Len(1)],
    "gettimeofday": [Out(16), Out(8)],
    "getrlimit": [Enum(list(range(0, 17))), Out(16)],
    "setrlimit": [Enum([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]), Struct(_rlimit)],
    "prlimit64": [Pid(), Enum(list(range(0, 17))), Struct(_rlimit), Out(16)],
    "sysinfo": [Out(112)],
    "syslog": [Enum([3, 4, 8, 10, 0, 1, 2, 5, 6, 7, 9, 11]), Buf(data=False), Len(1)],
    "setpgid": [Pid(), Pid()],
    "getpgid": [Pid()],
    "getsid": [Pid()],
    "setsid": [],
    "sigaltstack": [Struct(_sigaltstack), Out(24)],
    "utime": [Path(), Struct(lambda r: struct.pack("<qq", r.choice([0, 1000, 1 << 40]), r.choice([0, 1000, 1 << 40])))],
    "statfs": [Path(), Out(120)],
    "fstatfs": [Fd(), Out(120)],
    "getpriority": [Enum([0, 1, 2, 3]), Pid()],
    "setpriority": [Enum([0, 1, 2, 3]), Pid(), Int(-25, 25, [0, 19, 20, (1 << 64) - 20])],
    "arch_prctl": [Enum([0x1001, 0x1002, 0x1003, 0x1004, 0x2001, 0x2002, 0x2003, 0x3001, 0x4001]),
                   Addr()],
    "getdents64": [Fd(), Buf(data=False), Len(1)],
    "getrandom": [Buf(data=False), Len(0), Flags([1, 2, 4])],
    "set_tid_address": [Out(8)],
    "time": [Out(8)],
    "clock_gettime": [CLOCKS, Out(16)],
    "clock_getres": [CLOCKS, Out(16)],
    "getuid": [], "getgid": [], "geteuid": [], "getegid": [], "getppid": [], "gettid": [], "getpgrp": [],
}

# Calls that give back a resource.
RETURNS = {"open": "fd", "openat": "fd", "socket": "fd", "accept": "fd", "accept4": "fd", "dup": "fd", "dup2": "fd",
           "fcntl": "fd", "mmap": "addr", "brk": "addr", "getpid": "pid", "fork": "pid", "vfork": "pid", "clone": "pid",
           "gettid": "pid", "getpgrp": "pid", "setsid": "pid"}
# Calls that store descriptors in memory: name -> (argument index of the buffer, number of u32 descriptors)
STORES_FDS = {"pipe": (0, 2), "pipe2": (0, 2), "socketpair": (3, 2)}


def _read(path):
    with open(os.path.join(SRC, path)) as f:
        return f.read()


def kernel_calls():
    """{name: (number, [C types])} for every call in the kernel's table, from the sources."""
    nums = {m.group(1): int(m.group(2), 0)
            for m in re.finditer(r"#define\s+SYS_([A-Z0-9_]+)\s+(0x[0-9a-fA-F]+|\d+)", _read("include/libc/syscall_numbers.h"))}
    table = re.findall(r"\[SYS_([A-Z0-9_]+)\]\s*=\s*SC\((sc_\w+)\)", _read("kernel/cpu/syscall.c"))
    protos = {}
    for m in re.finditer(r"DECL\((sc_\w+)\(([^)]*)\)\)", _read("kernel/cpu/syscall_internal.h")):
        args = [a.strip() for a in m.group(2).split(",") if a.strip() and a.strip() != "void"]
        protos[m.group(1)] = [re.sub(r"\s+\w+$", "", a) if re.search(r"\s\w+$", a) and " " in a and
                              not a.endswith(("long", "int", "_t")) else a for a in args]
    calls = {}
    for name, handler in table:
        calls[name.lower()] = (nums[name], protos.get(handler, []))
    return calls


def _generic(ctype):
    ctype = ctype.replace("unsigned long", "ulong").replace("unsigned", "uint")
    if "uintptr_t" in ctype or "*" in ctype:
        return Out(64)
    if ctype in ("int64_t", "uint64_t", "long", "ulong", "size_t"):
        return Int(0, 1 << 12)
    return Int(0, 64, [(1 << 32) - 1, 1 << 31], width=32)


class Syscall:
    def __init__(self, name, nr, args, returns=None, stores=None):
        self.name, self.nr, self.args, self.returns, self.stores = name, nr, args, returns, stores


def build():
    """Syscall descriptions for every call the kernel implements, minus the excluded ones."""
    out = []
    for name, (nr, ctypes) in sorted(kernel_calls().items(), key=lambda kv: kv[1][0]):
        if name in EXCLUDED:
            continue
        args = DESC.get(name)
        if args is None:
            args = [_generic(t) for t in ctypes]
        out.append(Syscall(name, nr, args, RETURNS.get(name), STORES_FDS.get(name)))
    return out
