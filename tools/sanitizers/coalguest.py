"""Boot a Coal OS build in QEMU and talk to agentd; shared by the sanitizer suite and the fuzzer.

Plain module, JSON-friendly results. It does not take the host-wide guest slot itself: run the
calling script under `flock /tmp/coal-testkit/slots/0 ...` (one guest at a time on the shared host).
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "coal-gateway"))
from coal_gateway.agent import AgentClient          # noqa: E402
from coal_gateway.errors import AgentError, ToolError  # noqa: E402
from coal_gateway.transport import parse_address    # noqa: E402

# Lines that mean the kernel found something: a panic, a sanitizer report, a lock-checker report.
BUG_RE = re.compile(r"KERNEL PANIC|\*\*\* KERNEL|BUG: |UBSAN: |KSAN: |LOCKDEP: |KFENCE: |stack smashing|"
                    r"Unhandled exception|General Protection")
FATAL_RE = re.compile(r"KERNEL PANIC|\*\*\* KERNEL PANIC|stack smashing")


class GuestDied(Exception):
    pass


def _die_with_parent():
    try:
        import ctypes
        ctypes.CDLL("libc.so.6").prctl(1, 9)        # PR_SET_PDEATHSIG, SIGKILL
    except OSError:
        pass


class Guest:
    def __init__(self, build_dir, cmdline="", memory_mb=256, workdir=None, extra_args=(), snapshot=True,
                 disk=None, qcow=False):
        self.build_dir = build_dir
        self.cmdline = cmdline
        self.memory_mb = memory_mb
        self.extra_args = list(extra_args)
        self.snapshot = snapshot
        self.disk = disk or os.path.join(build_dir, "disk.img")
        self.qcow = qcow
        self.workdir = workdir or tempfile.mkdtemp(prefix="coalguest-")
        self.proc = None
        self.agent = None
        self.pidfile = os.path.join(self.workdir, "qemu.pid")
        self.log_path = os.path.join(self.workdir, "com1.log")
        self.com2 = os.path.join(self.workdir, "com2.sock")
        self.monitor_path = os.path.join(self.workdir, "monitor.sock")
        self.boot_seconds = None

    # -- life cycle -----------------------------------------------------------------------------
    def command(self):
        cmd = ["qemu-system-x86_64", "-display", "none", "-m", str(self.memory_mb), "-cpu", "max",
               "-pidfile", self.pidfile, "-monitor", "unix:%s,server=on,wait=off" % self.monitor_path,
               "-serial", "file:" + self.log_path,
               "-serial", "unix:%s,server=on,wait=off" % self.com2,
               "-kernel", os.path.join(self.build_dir, "kernel.bin"),
               "-append", ("root=hda " + self.cmdline).strip(),
               "-drive", "file=%s,format=%s,index=0,media=disk" % (self.disk, "qcow2" if self.qcow else "raw"),
               "-nic", "user,model=e1000", "-no-reboot"]
        if self.snapshot and not self.qcow:
            cmd.append("-snapshot")
        return cmd + self.extra_args

    def start(self):
        open(self.log_path, "wb").close()
        self.proc = subprocess.Popen(self.command(), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE, preexec_fn=_die_with_parent)
        self.t0 = time.monotonic()

    def connect(self, timeout=90):
        end = time.monotonic() + timeout
        while not os.path.exists(self.com2):
            if not self.alive() or time.monotonic() > end:
                raise GuestDied("QEMU did not create COM2: " + self.tail(15))
            time.sleep(0.05)
        self.agent = AgentClient(parse_address("unix:" + self.com2))
        if not self.agent.wait_ready(max(1.0, end - time.monotonic()), self.alive):
            raise GuestDied("agentd did not answer: " + self.tail(25))
        self.boot_seconds = time.monotonic() - self.t0

    def boot(self, timeout=120):
        self.start()
        self.connect(timeout)
        return self

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def stop(self):
        if self.agent:
            self.agent.close()
            self.agent = None
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        if self.proc and self.proc.stderr:
            self.proc.stderr.close()

    def cleanup(self):
        self.stop()
        shutil.rmtree(self.workdir, ignore_errors=True)

    # -- running things -------------------------------------------------------------------------
    def run(self, cmd, timeout=60):
        """exec through agentd; returns the agentd response dict (stdout, exit_code, ...). Raises GuestDied."""
        try:
            return self.agent.request("exec", timeout=timeout + 15, cmd=cmd, timeout_ms=int(timeout * 1000))
        except (ToolError, AgentError) as e:
            raise GuestDied("%s | %s" % (e, self.tail(15)))

    def log(self):
        try:
            with open(self.log_path, "rb") as f:
                return f.read().decode("utf-8", "replace")
        except OSError:
            return ""

    def tail(self, n=30):
        return "\n".join([l for l in self.log().splitlines() if l.strip()][-n:])

    def monitor(self, command, timeout=10.0):
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(timeout)
        try:
            s.connect(self.monitor_path)
            buf = b""
            while not buf.rstrip().endswith(b"(qemu)"):
                buf += s.recv(4096)
            s.sendall(command.encode() + b"\n")
            out = b""
            while not out.rstrip().endswith(b"(qemu)"):
                chunk = s.recv(65536)
                if not chunk:
                    break
                out += chunk
            return out.decode("utf-8", "replace")
        finally:
            s.close()


def bug_lines(log):
    """Sanitizer or panic reports in a serial log, as a list of lines."""
    return [l for l in log.splitlines() if BUG_RE.search(l)]
