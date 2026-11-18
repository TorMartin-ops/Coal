"""The fuzzing guest: QEMU with the coverage plugin, a qcow2 disk with a clean snapshot, and the executor link.

A fuzz image is the build's disk image with /sbin/agentd replaced by fuzz_executor, converted to qcow2. The machine is
booted once, and when the executor announces itself the state is saved with `savevm clean`. Every later start (or
restore after a crash) is `loadvm clean`, which is much quicker than a boot and puts memory, devices and disk back.
"""
import os
import re
import select
import shutil
import socket
import struct
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.join(HERE, "plugin", "libcoverage.so")
MAP_SIZE = 65536
MAGIC_RESP = 0x3152465A
TAG = "clean"


class VMError(Exception):
    pass


def _die_with_parent():
    try:
        import ctypes
        ctypes.CDLL("libc.so.6").prctl(1, 9)
    except OSError:
        pass


def kernel_text_range(kernel):
    out = subprocess.run(["nm", kernel], stdout=subprocess.PIPE, universal_newlines=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] in ("_kernel_text_start", "_kernel_text_end"):
            syms[p[2]] = int(p[0], 16)
    return syms["_kernel_text_start"], syms["_kernel_text_end"]


def symbol_table(kernel):
    """Sorted (address, name) of the kernel's functions, for naming a guest RIP."""
    out = subprocess.run(["nm", "-n", "--defined-only", kernel], stdout=subprocess.PIPE, universal_newlines=True,
                         check=True).stdout
    return [(int(p[0], 16), p[2]) for p in (l.split() for l in out.splitlines()) if len(p) == 3 and p[1] in "Tt"]


def prepare_image(build_dir, workdir):
    """Copy the build's disk image, put the executor on it as /sbin/agentd, convert to qcow2. Returns the qcow2 path."""
    raw = os.path.join(workdir, "fuzz.raw")
    qcow = os.path.join(workdir, "fuzz.qcow2")
    shutil.copyfile(os.path.join(build_dir, "disk.img"), raw)
    subprocess.run(["mcopy", "-D", "o", "-i", raw, os.path.join(build_dir, "fuzz_executor.elf"), "::/sbin/agentd"], check=True)
    if os.path.exists(qcow):
        os.remove(qcow)
    subprocess.run(["qemu-img", "convert", "-O", "qcow2", raw, qcow], check=True, stdout=subprocess.DEVNULL)
    os.remove(raw)
    return qcow


class VM:
    def __init__(self, build_dir, workdir, memory_mb=96, cmdline="coal.san=warn coal.ubsan=warn coal.lockdep=warn coal.kfence=50",
                 coverage=True):
        self.build_dir = build_dir
        self.workdir = workdir
        self.memory_mb = memory_mb
        self.cmdline = cmdline
        self.kernel = os.path.join(build_dir, "kernel.bin")
        self.qcow = os.path.join(workdir, "fuzz.qcow2")
        self.log_path = os.path.join(workdir, "com1.log")
        self.hvc_path = os.path.join(workdir, "hvc0.sock")
        self.mon_path = os.path.join(workdir, "monitor.sock")
        self.map_path = os.path.join(workdir, "coverage.map")
        self.proc = None
        self.chan = None
        self.cov = None
        self.coverage = coverage
        self.log_pos = 0
        self.symbols = None
        self.restore_seconds = []

    # -- life cycle --------------------------------------------------------------------------------------------------
    def command(self, load):
        cmd = ["qemu-system-x86_64", "-display", "none", "-m", str(self.memory_mb), "-cpu", "max", "-no-reboot",
               "-monitor", "unix:%s,server=on,wait=off" % self.mon_path,
               "-serial", "file:" + self.log_path,
               "-kernel", self.kernel, "-append", "root=hda " + self.cmdline,
               "-drive", "file=%s,format=qcow2,if=ide,index=0" % self.qcow,
               "-nic", "user,model=e1000,restrict=on",
               "-device", "virtio-serial-pci,disable-legacy=on",
               "-chardev", "socket,id=hvc0,path=%s,server=on,wait=off" % self.hvc_path,
               "-device", "virtconsole,chardev=hvc0"]
        if self.coverage:
            lo, hi = kernel_text_range(self.kernel)
            cmd += ["-plugin", "%s,shm=%s,lo=%#x,hi=%#x" % (PLUGIN, self.map_path, lo, hi)]
        if load:
            cmd += ["-loadvm", TAG]
        return cmd

    def start(self, load=False):
        self.stop()
        for p in (self.hvc_path, self.mon_path):
            if os.path.exists(p):
                os.remove(p)
        if not os.path.exists(self.map_path):
            with open(self.map_path, "wb") as f:
                f.write(bytes(MAP_SIZE))
        import mmap
        fd = os.open(self.map_path, os.O_RDWR)
        if self.cov is None:
            self.cov = mmap.mmap(fd, MAP_SIZE)
        os.close(fd)
        self.cov[:] = bytes(MAP_SIZE)
        open(self.log_path, "wb").close()
        self.log_pos = 0
        self.proc = subprocess.Popen(self.command(load), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE, preexec_fn=_die_with_parent)
        end = time.monotonic() + 20
        while not (os.path.exists(self.hvc_path) and os.path.exists(self.mon_path)):
            if not self.alive() or time.monotonic() > end:
                raise VMError("QEMU did not start: " + self.stderr())
            time.sleep(0.02)
        self.chan = socket.socket(socket.AF_UNIX)
        self.chan.connect(self.hvc_path)
        self.chan.setblocking(False)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def stderr(self):
        try:
            self.proc.wait(timeout=1)
            return self.proc.stderr.read().decode("utf-8", "replace")[-500:]
        except Exception:
            return ""

    def stop(self):
        if self.chan:
            self.chan.close()
            self.chan = None
        if self.proc:
            if self.proc.poll() is None:
                self.proc.kill()
            self.proc.wait()
            if self.proc.stderr:
                self.proc.stderr.close()
            self.proc = None

    # -- executor link -------------------------------------------------------------------------------------------------
    def _recv(self, n, deadline):
        buf = b""
        while len(buf) < n:
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError
            r, _, _ = select.select([self.chan], [], [], min(left, 0.2))
            if r:
                chunk = self.chan.recv(n - len(buf))
                if not chunk:
                    raise VMError("executor link closed")
                buf += chunk
            elif not self.alive():
                raise VMError("QEMU exited")
        return buf

    def drain(self, quiet=0.05):
        end = time.monotonic() + quiet
        while time.monotonic() < end:
            r, _, _ = select.select([self.chan], [], [], 0.02)
            if r:
                if not self.chan.recv(65536):
                    break
                end = time.monotonic() + quiet

    def wait_ready(self, timeout=60):
        deadline = time.monotonic() + timeout
        while True:
            hdr = self._recv(16, deadline)
            magic, status, _, _ = struct.unpack("<IIII", hdr)
            if magic == MAGIC_RESP and status == 100:
                return

    def execute(self, wire, timeout=6.0):
        """Run one program. Returns {"status", "detail", "results"} or raises TimeoutError (no answer) / VMError."""
        self.chan.setblocking(True)
        try:
            self.chan.sendall(wire)
        finally:
            self.chan.setblocking(False)
        deadline = time.monotonic() + timeout
        magic, status, detail, done = struct.unpack("<IIII", self._recv(16, deadline))
        if magic != MAGIC_RESP:
            raise VMError("bad response magic %#x" % magic)
        results = struct.unpack("<%dq" % done, self._recv(8 * done, deadline)) if done else ()
        return {"status": status, "detail": detail, "results": list(results)}

    # -- snapshots -----------------------------------------------------------------------------------------------------
    def monitor(self, command, timeout=60.0):
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(timeout)
        try:
            s.connect(self.mon_path)
            buf = b""
            while b"(qemu)" not in buf:
                buf += s.recv(4096)
            s.sendall(command.encode() + b"\n")
            out = b""
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                out += chunk
                if out.rstrip().endswith(b"(qemu)"):
                    break
            return out.decode("utf-8", "replace")
        finally:
            s.close()

    def savevm(self):
        out = self.monitor("savevm " + TAG, 120)
        if "rror" in out:
            raise VMError("savevm: " + out)

    def loadvm(self):
        """Back to the clean state, in this QEMU process. Costs about a second."""
        t0 = time.monotonic()
        out = self.monitor("loadvm " + TAG, 120)
        if "rror" in out:
            raise VMError("loadvm: " + out)
        self.drain()
        self.log_pos = len(self._log_bytes())
        self.restore_seconds.append(time.monotonic() - t0)

    def boot_snapshot(self, timeout=90):
        """First start of an image: boot, wait for the executor, save the clean state."""
        self.start(load=False)
        self.wait_ready(timeout)
        time.sleep(0.3)
        self.savevm()
        self.drain()
        self.log_pos = len(self._log_bytes())

    def has_snapshot(self):
        out = subprocess.run(["qemu-img", "snapshot", "-l", self.qcow], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                             universal_newlines=True).stdout
        return TAG in out

    # -- observations --------------------------------------------------------------------------------------------------
    def _log_bytes(self):
        try:
            with open(self.log_path, "rb") as f:
                return f.read()
        except OSError:
            return b""

    def new_log(self):
        data = self._log_bytes()
        text = data[self.log_pos:].decode("utf-8", "replace")
        self.log_pos = len(data)
        return text

    def clear_coverage(self):
        self.cov[:] = bytes(MAP_SIZE)

    def read_coverage(self):
        return bytes(self.cov)

    def rip(self):
        """Name of the kernel function the guest is in (used when it stopped answering)."""
        try:
            out = self.monitor("info registers", 5)
        except Exception:
            return None
        m = re.search(r"RIP=([0-9a-f]+)", out)
        if not m:
            return None
        if self.symbols is None:
            self.symbols = symbol_table(self.kernel)
        addr = int(m.group(1), 16)
        best = None
        for a, n in self.symbols:
            if a > addr:
                break
            best = n
        return best
