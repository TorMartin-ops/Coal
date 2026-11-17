"""Test fixtures: agentd built as a 32-bit Linux binary on a pty (fast), or the real thing in QEMU."""
import os
import pty
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import tty
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
GATEWAY_DIR = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(GATEWAY_DIR))
AGENTD_BUILD = os.path.join(REPO, "src", "CoalOS", "userspace", "agentd", "test", "build_host.sh")
sys.path.insert(0, GATEWAY_DIR)

from coal_gateway.agent import AgentClient          # noqa: E402
from coal_gateway.transport import parse_address    # noqa: E402


def bridge(sock_path, master):
    """Forwards bytes between a unix socket client and a pty master (QEMU's COM2 chardev, in miniature)."""
    srv = socket.socket(socket.AF_UNIX)
    srv.bind(sock_path)
    srv.listen(1)

    def run():
        while True:
            conn, _ = srv.accept()
            while True:
                r, _, _ = select.select([conn, master], [], [])
                try:
                    if conn in r:
                        d = conn.recv(65536)
                        if not d:
                            break
                        os.write(master, d)
                    if master in r:
                        conn.sendall(os.read(master, 65536))
                except OSError:
                    break
            conn.close()
    threading.Thread(target=run, daemon=True).start()


class HostAgent:
    """agentd running on this machine's kernel, reachable through a unix socket."""

    reports_rlimits = False      # the Linux kernel has no RlimitHits line in /proc/<pid>/status

    def __init__(self):
        if not shutil.which("gcc"):
            raise unittest.SkipTest("gcc not available")
        self.tmp = tempfile.mkdtemp(prefix="agentd-test-")
        binary = os.path.join(self.tmp, "agentd")
        if subprocess.call(["sh", AGENTD_BUILD, binary], stderr=subprocess.DEVNULL) != 0:
            shutil.rmtree(self.tmp, ignore_errors=True)
            raise unittest.SkipTest("cannot build 32-bit agentd on this host")
        master, slave = pty.openpty()
        tty.setraw(master)
        self.spill = os.path.join(self.tmp, "spill")
        self.proc = subprocess.Popen([binary, os.ttyname(slave), self.spill])
        self.socket = os.path.join(self.tmp, "com2.sock")
        bridge(self.socket, master)
        self.address = "unix:" + self.socket

    def client(self):
        return AgentClient(parse_address(self.address))

    def close(self):
        self.proc.kill()
        self.proc.wait()
        shutil.rmtree(self.tmp, ignore_errors=True)


class QemuAgent:
    """The real kernel and agentd under QEMU (TCG, headless), COM2 on a unix socket. Disk writes are discarded."""

    reports_rlimits = True

    def __init__(self):
        if not shutil.which("qemu-system-i386"):
            raise unittest.SkipTest("qemu-system-i386 not available")
        self.build = os.environ.get("COAL_TEST_BUILD", os.path.join(tempfile.gettempdir(), "coal-gateway-test-build"))
        src = os.path.join(REPO, "src", "CoalOS")
        for cmd in (["cmake", "-S", src, "-B", self.build], ["cmake", "--build", self.build, "-j", str(os.cpu_count() or 2)]):
            p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if p.returncode:
                raise unittest.SkipTest("cannot build Coal OS: " + p.stdout.decode()[-400:])
        self.tmp = tempfile.mkdtemp(prefix="coal-qemu-test-")
        self.socket = os.path.join(self.tmp, "com2.sock")
        self.address = "unix:" + self.socket
        self.proc = subprocess.Popen(
            ["qemu-system-i386", "-display", "none", "-m", "128", "-snapshot", "-nic", "none", "-no-reboot",
             "-serial", "file:" + os.path.join(self.tmp, "com1.log")] + self.boot_args(),
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def boot_args(self):
        return ["-boot", "d", "-cdrom", os.path.join(self.build, "kernel.iso"), "-hda", os.path.join(self.build, "disk.img"),
                "-serial", "unix:%s,server=on,wait=off" % self.socket]

    def client(self):
        c = AgentClient(parse_address(self.address))
        deadline = 60
        import time
        end = time.monotonic() + deadline
        while not os.path.exists(self.socket) and time.monotonic() < end:
            time.sleep(0.05)
        if not c.wait_ready(deadline, lambda: self.proc.poll() is None):
            raise AssertionError("agentd did not answer; kernel log:\n" + self.log())
        return c

    def log(self):
        try:
            with open(os.path.join(self.tmp, "com1.log"), errors="replace") as f:
                return f.read()[-2000:]
        except OSError:
            return ""

    def close(self):
        self.proc.kill()
        self.proc.wait()
        shutil.rmtree(self.tmp, ignore_errors=True)


class VirtioQemuAgent(QemuAgent):
    """As QemuAgent, with a virtio-blk root and the agentd link on a virtio console; rng is attached too."""

    features = "blk,console,rng"

    def devices(self):
        """(QEMU device arguments, kernel command line words); sets the agent address."""
        from coal_gateway.machine import Machine
        args, cmdline, self.address = Machine(virtio=self.features)._virtio_setup(
            os.path.join(self.build, "disk.img"), self.socket)
        return args, cmdline

    def boot_args(self):
        args, cmdline = self.devices()
        return args + ["-kernel", os.path.join(self.build, "kernel.bin"), "-append", " ".join(cmdline)]


class VsockQemuAgent(VirtioQemuAgent):
    """agentd over virtio-vsock. /dev/vhost-vsock is not needed: the device is vhost-device-vsock (set
    COAL_VHOST_DEVICE_VSOCK to the binary), reached through its hybrid-vsock unix socket."""

    features = "blk"

    def __init__(self):
        self.backend_bin = os.environ.get("COAL_VHOST_DEVICE_VSOCK") or shutil.which("vhost-device-vsock")
        if not self.backend_bin:
            raise unittest.SkipTest("vhost-device-vsock not available (COAL_VHOST_DEVICE_VSOCK)")
        self.backend = None
        super().__init__()

    def devices(self):
        args, cmdline = super().devices()
        sock, uds = os.path.join(self.tmp, "vhost.sock"), os.path.join(self.tmp, "vsock")
        self.backend = subprocess.Popen([self.backend_bin, "--guest-cid", "3", "--socket", sock, "--uds-path", uds],
                                        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        while not os.path.exists(sock):
            time.sleep(0.05)
        self.address = "hvsock:%s:1024" % uds
        return (args + ["-object", "memory-backend-memfd,id=mem,size=128M,share=on", "-machine", "memory-backend=mem",
                        "-chardev", "socket,id=vs,path=" + sock, "-device", "vhost-user-vsock-pci,chardev=vs,disable-legacy=on"],
                cmdline + ["agentd.link=vsock:1024"])

    def client(self):
        c = AgentClient(parse_address(self.address))
        if not c.wait_ready(60, lambda: self.proc.poll() is None):
            raise AssertionError("agentd did not answer; kernel log:\n" + self.log())
        return c

    def close(self):
        if self.backend:
            self.backend.kill()
            self.backend.wait()
        super().close()
