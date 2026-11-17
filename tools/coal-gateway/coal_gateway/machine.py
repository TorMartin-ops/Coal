"""The Coal OS virtual machine: build, boot in QEMU, snapshots, and the link to agentd on COM2."""
import ctypes
import os
import re
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time
import uuid

from .agent import AgentClient
from .errors import ToolError
from .transport import parse_address

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
SRC_DIR = os.path.join(ROOT, "src", "CoalOS")
PROTO_VERSION = 1


def default_memory_mb():
    try:
        return max(16, min(4096, int(os.environ.get("COAL_MEMORY_MB", "512"))))
    except ValueError:
        return 512


class Machine:
    """One guest: QEMU started by us, or an agentd that is already running (--attach)."""

    def __init__(self, attach=None):
        self.proc = None
        self.workdir = None
        self.agent = None
        self.attached = attach is not None
        if attach:
            self.agent = AgentClient(parse_address(attach))
        self.build_dir = os.environ.get("COAL_BUILD_DIR", os.path.join(ROOT, "build", "coal-mcp"))
        self.lock = threading.Lock()

    # paths inside the work directory
    def _p(self, name):
        return os.path.join(self.workdir, name)

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def build(self, clean=False, target=None, timeout=900):
        if shutil.which("cmake") is None:
            raise ToolError("cmake is not installed")
        if clean:
            shutil.rmtree(self.build_dir, ignore_errors=True)
        os.makedirs(self.build_dir, mode=0o700, exist_ok=True)
        steps = [["cmake", "-S", SRC_DIR, "-B", self.build_dir],
                 ["cmake", "--build", self.build_dir, "-j", str(os.cpu_count() or 2)]
                 + (["--target", target] if target else [])]
        text, ok = "", True
        for cmd in steps:
            try:
                p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   timeout=timeout)
            except subprocess.TimeoutExpired:
                raise ToolError("build timed out after %ds" % timeout)
            text += p.stdout.decode("utf-8", "replace")
            if p.returncode != 0:
                ok = False
                break
        return ok, text

    def images(self):
        return (os.path.join(self.build_dir, "kernel.iso"),
                os.path.join(self.build_dir, "disk.img"))

    def boot(self, rebuild=False, memory_mb=None, snapshot=False, timeout=60):
        with self.lock:
            if self.attached:
                return "attached to an existing Coal instance; nothing to boot"
            if self.running():
                return "already running (pid %d)" % self.proc.pid
            qemu = shutil.which("qemu-system-x86_64")
            if qemu is None:
                raise ToolError("qemu-system-x86_64 is not installed")
            iso, disk = self.images()
            note = ""
            if rebuild or not (os.path.exists(iso) and os.path.exists(disk)):
                ok, text = self.build()
                if not ok:
                    raise ToolError("build failed:\n" + summarize_build(text))
                note = "built OS; "
            else:
                # Pick up kernel source changes; the disk image keeps its files.
                ok, text = self.build(target="iso")
                if not ok:
                    raise ToolError("kernel build failed:\n" + summarize_build(text))
            self._cleanup_workdir()
            self.workdir = tempfile.mkdtemp(prefix="coal-mcp-")      # mode 0700: QEMU monitor and COM2 sockets
            os.chmod(self.workdir, 0o700)
            com2, mon = self._p("com2.sock"), self._p("monitor.sock")
            cmd = [qemu, "-display", "none", "-m", str(memory_mb or default_memory_mb()),
                   "-cdrom", iso, "-hda", disk, "-boot", "d", "-no-shutdown",
                   "-serial", "file:" + self._p("com1.log"),
                   "-serial", "unix:%s,server=on,wait=off" % com2,
                   "-monitor", "unix:%s,server=on,wait=off" % mon,
                   "-nic", os.environ.get("COAL_NIC", "user,model=e1000")]
            if snapshot:
                cmd.append("-snapshot")
            cmd += os.environ.get("COAL_QEMU_ARGS", "").split()
            self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                         preexec_fn=_die_with_parent)
            self.agent = AgentClient(parse_address("unix:" + com2))
            start = time.monotonic()
            deadline = start + timeout
            while not os.path.exists(com2) and time.monotonic() < deadline and self.running():
                time.sleep(0.05)
        if self.agent.wait_ready(max(1.0, deadline - time.monotonic()), self.running):
            return "%sbooted in %.1fs; agentd is ready" % (note, time.monotonic() - start)
        if not self.running():
            err = self.proc.stderr.read().decode("utf-8", "replace").strip()
            raise ToolError("QEMU exited during boot: %s\n%s" % (err, self.kernel_log(20)))
        raise ToolError("the VM is running but agentd did not answer within %ds. "
                        "Is /sbin/agentd on the disk image and started by init?\n"
                        "Kernel log tail:\n%s" % (timeout, self.kernel_log(30)))

    def _cleanup_workdir(self):
        if self.workdir:
            shutil.rmtree(self.workdir, ignore_errors=True)
            self.workdir = None

    def shutdown(self):
        with self.lock:
            if self.attached:
                return "attached instance left running"
            if self.agent:
                self.agent.close()
            was = self.running()
            if was:
                try:
                    self._monitor("quit", timeout=2)
                except (ToolError, OSError):
                    pass
                try:
                    self.proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait()
            self.proc = None
            self._cleanup_workdir()
            return "stopped" if was else "was not running"

    def reboot(self, timeout=60):
        if self.attached:
            raise ToolError("cannot reboot an attached instance")
        if not self.running():
            raise ToolError("Coal is not running; call coal_boot")
        self._monitor("system_reset")
        time.sleep(0.3)
        if self.agent.wait_ready(timeout, self.running):
            return "rebooted; agentd is ready"
        raise ToolError("rebooted, but agentd did not answer within %ds.\n%s"
                        % (timeout, self.kernel_log(30)))

    def _monitor(self, command, timeout=5.0):
        if not self.running():
            raise ToolError("Coal is not running; call coal_boot")
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(timeout)
        try:
            s.connect(self._p("monitor.sock"))
            _read_until(s, b"(qemu)")
            s.sendall(command.encode() + b"\n")
            if command in ("quit", "q"):
                return ""
            out = _read_until(s, b"(qemu)")
        except (OSError, socket.timeout) as e:
            raise ToolError("QEMU monitor: %s" % e)
        finally:
            s.close()
        return out.decode("utf-8", "replace").rsplit("(qemu)", 1)[0]

    def screen(self):
        raw = self._monitor("xp /4000bx 0xb8000")
        dump = [l.split(":", 1)[1] for l in raw.splitlines() if re.match(r"^[0-9a-fA-F]+:", l)]
        bytes_ = [int(x, 16) for l in dump for x in re.findall(r"0x([0-9a-fA-F]{2})", l)]
        if len(bytes_) < 4000:
            raise ToolError("could not read VGA memory (graphical mode?)")
        rows = []
        for r in range(25):
            line = "".join(chr(b) if 32 <= b < 127 else " "
                           for b in bytes_[r * 160:(r + 1) * 160:2])
            rows.append(line.rstrip())
        while rows and not rows[-1]:
            rows.pop()
        return "\n".join(rows)

    def kernel_log(self, lines=100):
        if not self.workdir or not os.path.exists(self._p("com1.log")):
            raise ToolError("no kernel log (Coal was not booted by this server)")
        with open(self._p("com1.log"), "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - 256 * 1024))
            data = f.read().decode("utf-8", "replace")
        return "\n".join([l for l in data.splitlines() if l.strip()][-lines:])

    @property
    def snap_dir(self):
        return os.path.join(self.build_dir, "snapshots")

    def _snap_path(self, name):
        if not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", name or "") or name.startswith("."):
            raise ToolError("snapshot name must be 1-64 characters of A-Z a-z 0-9 _ . -")
        return os.path.join(self.snap_dir, name + ".img")

    def snapshot(self, name):
        path = self._snap_path(name)
        if self.attached:
            raise ToolError("cannot snapshot an attached instance")
        disk = self.images()[1]
        if not os.path.exists(disk):
            raise ToolError("no disk image yet; call coal_boot")
        if self.running():
            self.need_agent().request("exec", timeout=40, cmd="sync", timeout_ms=30000)
        os.makedirs(self.snap_dir, mode=0o700, exist_ok=True)
        shutil.copyfile(disk, path)
        return "snapshot '%s' saved (%d bytes)" % (name, os.path.getsize(path))

    def restore(self, name=None):
        """Put a snapshot (or, without a name, a freshly built pristine image) in place and reboot."""
        if self.attached:
            raise ToolError("cannot restore an attached instance")
        src = self._snap_path(name) if name else None
        if src and not os.path.exists(src):
            have = sorted(f[:-4] for f in os.listdir(self.snap_dir) if f.endswith(".img")) \
                if os.path.isdir(self.snap_dir) else []
            raise ToolError("no snapshot '%s'. Existing: %s" % (name, ", ".join(have) or "none"))
        was_running = self.running()
        self.shutdown()
        disk = self.images()[1]
        if src:
            shutil.copyfile(src, disk)
        else:
            if os.path.exists(disk):
                os.remove(disk)
            ok, text = self.build()
            if not ok:
                raise ToolError("build failed:\n" + summarize_build(text))
        what = "snapshot '%s'" % name if name else "the pristine image"
        if was_running:
            return "restored %s; %s" % (what, self.boot())
        return "restored %s; call coal_boot" % what

    def need_agent(self):
        if self.agent is None or (not self.attached and not self.running()):
            raise ToolError("Coal is not running; call coal_boot first")
        return self.agent

    @property
    def workspace_id(self):
        """Stable name of this workspace (disk image plus build directory); minted on first use."""
        path = os.path.join(self.build_dir, "workspace_id")
        try:
            with open(path) as f:
                return f.read().strip()
        except OSError:
            wid = "ws-" + uuid.uuid4().hex[:8]
            os.makedirs(self.build_dir, mode=0o700, exist_ok=True)
            with open(path, "w") as f:
                f.write(wid + "\n")
            return wid


def _die_with_parent():
    """Child setup: QEMU gets SIGKILL if this server dies without cleanup."""
    try:
        ctypes.CDLL("libc.so.6", use_errno=True).prctl(1, signal.SIGKILL)
    except (OSError, AttributeError):
        pass


def _read_until(s, marker):
    buf = b""
    while marker not in buf:
        d = s.recv(4096)
        if not d:
            break
        buf += d
    return buf


def summarize_build(text):
    lines = text.splitlines()
    diag = [l for l in lines if re.search(r"warning:|error:|undefined reference|Error \d", l)]
    head = "%d warning(s), %d error(s)" % (
        sum("warning:" in l for l in lines), sum("error:" in l for l in lines))
    body = diag[:60] if diag else lines[-30:]
    return head + "\n" + "\n".join(body)

