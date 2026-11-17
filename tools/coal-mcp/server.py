#!/usr/bin/env python3
"""coal-mcp: a Model Context Protocol server that lets Claude drive Coal OS.

Speaks MCP over stdio (JSON-RPC, newline-delimited). It builds Coal OS, runs it
in QEMU and talks to the in-OS agent /sbin/mcpd over the second serial port
(COM2) using newline-delimited JSON. Python 3.8+, standard library only.

Both protocol eras are served: the stateless 2026-07-28 revision (per-request
_meta, server/discover) and the initialize handshake of 2025-11-25 and earlier.
"""
import argparse
import base64
import ctypes
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor

SERVER_INFO = {"name": "coal-mcp", "version": "1.0.0"}
MODERN_VERSIONS = ["2026-07-28"]
LEGACY_VERSIONS = ["2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"]
META_VERSION = "io.modelcontextprotocol/protocolVersion"
META_SERVER = "io.modelcontextprotocol/serverInfo"
INSTRUCTIONS = (
    "Drives a Coal OS virtual machine (a small Linux-compatible i386 OS) in QEMU. "
    "Call coal_boot first (it builds the OS if needed), then use coal_run, "
    "coal_read_file, coal_write_file, coal_ls, coal_ps and coal_sysinfo. Use "
    "coal_kernel_log and coal_screen to see what the kernel itself printed. "
    "Call coal_shutdown when finished.")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC_DIR = os.path.join(ROOT, "src", "CoalOS")
CHUNK = 64 * 1024            # raw bytes per file transfer request
MAX_READ = 4 * 1024 * 1024


class ToolError(Exception):
    """A failure to report to the model as a tool result with isError set."""


def log(msg):
    print("coal-mcp: " + msg, file=sys.stderr, flush=True)


# --- Link to mcpd on COM2 ------------------------------------------------------

class Link:
    """Request/response channel to mcpd over a stream socket (QEMU's COM2)."""

    def __init__(self, address):
        self.address = address           # ("unix", path) or ("tcp", (host, port))
        self.sock = None
        self.next_id = 1
        self.lock = threading.Lock()     # one request in flight; mcpd is serial
        self.cond = threading.Condition()
        self.responses = {}
        self.ready_events = 0

    def connect(self, timeout=5.0):
        if self.sock:
            return
        kind, addr = self.address
        s = socket.socket(socket.AF_UNIX if kind == "unix" else socket.AF_INET,
                          socket.SOCK_STREAM)
        s.settimeout(timeout)
        try:
            s.connect(addr)
        except OSError:
            s.close()
            raise
        s.settimeout(None)
        self.sock = s
        threading.Thread(target=self._reader, args=(s,), daemon=True).start()

    def close(self):
        s, self.sock = self.sock, None
        if s:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            s.close()

    def _reader(self, s):
        buf = b""
        try:
            while True:
                data = s.recv(65536)
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    self._dispatch(line)
        except OSError:
            pass
        with self.cond:
            if self.sock is s:
                self.sock = None
            self.cond.notify_all()

    def _dispatch(self, line):
        try:
            msg = json.loads(line.decode("utf-8", "replace"))
        except ValueError:
            return                       # boot noise or a partial line
        if not isinstance(msg, dict):
            return
        with self.cond:
            if msg.get("event") == "ready":
                self.ready_events += 1
            elif "id" in msg:
                self.responses[msg["id"]] = msg
            self.cond.notify_all()

    def request(self, op, timeout=15.0, **fields):
        """Sends one request and returns mcpd's response; raises ToolError."""
        with self.lock:
            try:
                self.connect()
            except OSError as e:
                raise ToolError("cannot reach Coal's COM2 (%s). Is it booted?" % e)
            rid = self.next_id
            self.next_id += 1
            fields.update(id=rid, op=op)
            line = json.dumps(fields, separators=(",", ":")).encode() + b"\n"
            try:
                self.sock.sendall(line)
            except (OSError, AttributeError):
                self.close()
                raise ToolError("connection to Coal was lost; retry or call coal_boot")
            deadline = time.monotonic() + timeout
            with self.cond:
                while rid not in self.responses:
                    left = deadline - time.monotonic()
                    if left <= 0:
                        raise ToolError("no answer from mcpd within %.0fs "
                                        "(is it running? see coal_kernel_log)" % timeout)
                    if self.sock is None:
                        raise ToolError("connection to Coal was lost")
                    self.cond.wait(left)
                resp = self.responses.pop(rid)
        if not resp.get("ok"):
            err = resp.get("error", "unknown error")
            if "errno" in resp:
                err += " (errno %d)" % resp["errno"]
            raise ToolError(err)
        return resp

    def wait_ready(self, timeout, alive=lambda: True):
        """Pings until mcpd answers; returns True on success."""
        end = time.monotonic() + timeout
        while time.monotonic() < end and alive():
            try:
                self.request("ping", timeout=1.0)
                return True
            except ToolError:
                time.sleep(0.25)
        return False


# --- The virtual machine -------------------------------------------------------

class Machine:
    def __init__(self, attach=None):
        self.proc = None
        self.workdir = None
        self.link = None
        self.attached = attach is not None
        if attach:
            self.link = Link(attach)
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
        os.makedirs(self.build_dir, exist_ok=True)
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
            qemu = shutil.which("qemu-system-i386")
            if qemu is None:
                raise ToolError("qemu-system-i386 is not installed")
            iso, disk = self.images()
            note = ""
            if rebuild or not (os.path.exists(iso) and os.path.exists(disk)):
                ok, text = self.build()
                if not ok:
                    raise ToolError("build failed:\n" + summarize_build(text))
                note = "built OS; "
            self._cleanup_workdir()
            self.workdir = tempfile.mkdtemp(prefix="coal-mcp-")
            com2, mon = self._p("com2.sock"), self._p("monitor.sock")
            cmd = [qemu, "-display", "none", "-m", str(memory_mb or default_memory_mb()),
                   "-cdrom", iso, "-hda", disk, "-boot", "d", "-no-shutdown",
                   "-serial", "file:" + self._p("com1.log"),
                   "-serial", "unix:%s,server=on,wait=off" % com2,
                   "-monitor", "unix:%s,server=on,wait=off" % mon]
            if snapshot:
                cmd.append("-snapshot")
            cmd += os.environ.get("COAL_QEMU_ARGS", "").split()
            self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                         preexec_fn=_die_with_parent)
            self.link = Link(("unix", com2))
            start = time.monotonic()
            deadline = start + timeout
            while not os.path.exists(com2) and time.monotonic() < deadline and self.running():
                time.sleep(0.05)
        if self.link.wait_ready(max(1.0, deadline - time.monotonic()), self.running):
            return "%sbooted in %.1fs; mcpd is ready" % (note, time.monotonic() - start)
        if not self.running():
            err = self.proc.stderr.read().decode("utf-8", "replace").strip()
            raise ToolError("QEMU exited during boot: %s\n%s" % (err, self.kernel_log(20)))
        raise ToolError("the VM is running but mcpd did not answer within %ds. "
                        "Is /sbin/mcpd on the disk image and started by init?\n"
                        "Kernel log tail:\n%s" % (timeout, self.kernel_log(30)))

    def _cleanup_workdir(self):
        if self.workdir:
            shutil.rmtree(self.workdir, ignore_errors=True)
            self.workdir = None

    def shutdown(self):
        with self.lock:
            if self.attached:
                return "attached instance left running"
            if self.link:
                self.link.close()
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
        if self.link.wait_ready(timeout, self.running):
            return "rebooted; mcpd is ready"
        raise ToolError("rebooted, but mcpd did not answer within %ds.\n%s"
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

    def need_link(self):
        if self.link is None or (not self.attached and not self.running()):
            raise ToolError("Coal is not running; call coal_boot first")
        return self.link


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


# --- Tools ---------------------------------------------------------------------

def _int(args, key, default, lo, hi):
    v = args.get(key, default)
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise ToolError("%s must be a number" % key)
    return max(lo, min(hi, int(v)))


def _str(args, key, required=True):
    v = args.get(key)
    if v is None and not required:
        return None
    if not isinstance(v, str) or (required and v == ""):
        raise ToolError("%s must be a non-empty string" % key)
    return v


def fmt_mode(mode, typ):
    t = {"dir": "d", "symlink": "l", "chr": "c", "blk": "b", "fifo": "p", "sock": "s"}.get(typ, "-")
    bits = "".join(("rwx"[i % 3] if mode & (1 << (8 - i)) else "-") for i in range(9))
    return t + bits


def default_memory_mb():
    try:
        return max(16, min(4096, int(os.environ.get("COAL_MEMORY_MB", "512"))))
    except ValueError:
        return 512


def tool_boot(m, a):
    return m.boot(rebuild=bool(a.get("rebuild", False)), memory_mb=_int(a, "memory_mb", default_memory_mb(), 16, 4096),
                  snapshot=bool(a.get("snapshot", False)), timeout=_int(a, "timeout_s", 60, 5, 600))


def tool_shutdown(m, a):
    return m.shutdown()


def tool_reboot(m, a):
    return m.reboot(_int(a, "timeout_s", 60, 5, 600))


def tool_build(m, a):
    ok, text = m.build(clean=bool(a.get("clean", False)), target=a.get("target"))
    s = summarize_build(text)
    if not ok:
        raise ToolError("build FAILED\n" + s)
    return "build OK\n" + s


def tool_run(m, a):
    cmd = _str(a, "command")
    t = _int(a, "timeout_ms", 10000, 100, 600000)
    r = m.need_link().request("run", timeout=t / 1000 + 10, cmd=cmd, timeout_ms=t)
    out = "exit code: %d" % r["exit"]
    if r.get("timed_out"):
        out += " (timed out after %d ms, killed)" % t
    if r.get("truncated"):
        out += " (output truncated)"
    if r["stdout"]:
        out += "\n--- stdout ---\n" + r["stdout"]
    if r["stderr"]:
        out += "\n--- stderr ---\n" + r["stderr"]
    return out


def tool_read_file(m, a):
    path = _str(a, "path")
    limit = _int(a, "max_bytes", 65536, 1, MAX_READ)
    offset = _int(a, "offset", 0, 0, 1 << 40)
    link = m.need_link()
    data, size = b"", None
    while len(data) < limit:
        r = link.request("read_file", path=path, offset=offset + len(data),
                         max=min(CHUNK, limit - len(data)))
        size = r["size"]
        chunk = base64.b64decode(r["data"])
        if not chunk:
            break
        data += chunk
    save = _str(a, "save_to", required=False)
    if save:
        with open(save, "wb") as f:
            f.write(data)
        return "saved %d bytes to %s (file size %s)" % (len(data), save, size)
    head = "%s: %d bytes from offset %d (file size %s)" % (path, len(data), offset, size)
    if a.get("encoding") == "base64":
        return head + "\n" + base64.b64encode(data).decode()
    try:
        return head + "\n" + data.decode("utf-8")
    except UnicodeDecodeError:
        return head + " [binary; showing base64]\n" + base64.b64encode(data).decode()


def tool_write_file(m, a):
    path = _str(a, "path")
    sources = [k for k in ("content", "content_base64", "host_path") if a.get(k) is not None]
    if len(sources) != 1:
        raise ToolError("give exactly one of content, content_base64, host_path")
    if "content" in sources:
        data = _str(a, "content", required=False).encode()
    elif "content_base64" in sources:
        try:
            data = base64.b64decode(_str(a, "content_base64", required=False), validate=True)
        except ValueError:
            raise ToolError("content_base64 is not valid base64")
    else:
        try:
            with open(a["host_path"], "rb") as f:
                data = f.read()
        except OSError as e:
            raise ToolError("cannot read host file: %s" % e)
    link, append = m.need_link(), bool(a.get("append", False))
    mode = a.get("mode", 0o644)
    try:
        mode = int(mode, 8) if isinstance(mode, str) else int(mode)
    except ValueError:
        raise ToolError("mode must be an octal string or an integer")
    for i in range(0, max(len(data), 1), CHUNK):
        link.request("write_file", path=path, mode=mode, append=append or i > 0,
                     data=base64.b64encode(data[i:i + CHUNK]).decode(), timeout=30)
    return "wrote %d bytes to %s" % (len(data), path)


def tool_ls(m, a):
    path = a.get("path", "/")
    link = m.need_link()
    r = link.request("list_dir", path=path)
    rows = []
    for e in sorted(r["entries"], key=lambda e: e["name"]):
        if "type" in e:
            rows.append("%s %5d %5d %9d %s%s" % (fmt_mode(e["mode"], e["type"]), e["uid"], e["gid"],
                                                 e["size"], e["name"], "/" if e["type"] == "dir" else ""))
        else:
            rows.append("?????????? %s" % e["name"])
    if r.get("truncated"):
        rows.append("... (listing truncated)")
    return "%s: %d entries\n%s" % (path, len(r["entries"]), "\n".join(rows))


def tool_ps(m, a):
    r = m.need_link().request("ps")
    rows = ["  PID  PPID STATE        NAME"]
    for p in sorted(r["processes"], key=lambda p: p["pid"]):
        rows.append("%5d %5d %-12s %s" % (p["pid"], p["ppid"], p["state"], p["name"]))
    return "\n".join(rows)


def tool_sysinfo(m, a):
    r = m.need_link().request("sysinfo")
    head = " ".join(r.get(k, "?") for k in ("sysname", "nodename", "release", "version", "machine"))
    return "%s\n--- /proc/uptime ---\n%s\n--- /proc/meminfo ---\n%s" % (
        head, r.get("uptime", "n/a").strip(), r.get("meminfo", "n/a").strip())


def tool_kernel_log(m, a):
    return m.kernel_log(_int(a, "lines", 100, 1, 5000))


def tool_screen(m, a):
    return m.screen()


def tool_status(m, a):
    if m.attached:
        up = "attached"
    else:
        up = "running (pid %d)" % m.proc.pid if m.running() else "stopped"
    return "Coal: %s\nbuild dir: %s" % (up, m.build_dir)


def schema(props=None, required=()):
    return {"type": "object", "properties": props or {}, "required": list(required),
            "additionalProperties": False}


def ann(title, read_only=False, destructive=False, idem=False):
    return {"title": title, "readOnlyHint": read_only, "destructiveHint": destructive,
            "idempotentHint": idem, "openWorldHint": False}


S = {"type": "string"}
TOOLS = [
    ("coal_boot", "Boot Coal OS in QEMU (headless) and wait until its in-OS agent mcpd answers. "
     "Builds the OS first if no images exist. Returns once the VM is usable or fails with the kernel log tail.",
     schema({"rebuild": {"type": "boolean", "description": "Rebuild before booting (default false)."},
             "memory_mb": {"type": "integer", "description": "Guest RAM in MiB (default: --memory / COAL_MEMORY_MB, else 512)."},
             "snapshot": {"type": "boolean", "description": "Discard disk writes on shutdown (default false)."},
             "timeout_s": {"type": "integer", "description": "Seconds to wait for mcpd (default 60)."}}),
     ann("Boot Coal OS", idem=True), tool_boot),
    ("coal_shutdown", "Stop the Coal OS virtual machine (QEMU) and clean up.",
     schema(), ann("Shut down Coal OS", idem=True), tool_shutdown),
    ("coal_reboot", "Hard-reset the running VM and wait for mcpd again. Disk contents are kept.",
     schema({"timeout_s": {"type": "integer", "description": "Seconds to wait (default 60)."}}),
     ann("Reboot Coal OS"), tool_reboot),
    ("coal_build", "Configure and build Coal OS with CMake. Returns warning/error counts and the diagnostics.",
     schema({"clean": {"type": "boolean", "description": "Delete the build directory first."},
             "target": {"type": "string", "description": "CMake target (default: all)."}}),
     ann("Build Coal OS", idem=True), tool_build),
    ("coal_run", "Run a shell command inside Coal OS (/bin/sh -c) and return exit code, stdout and stderr. "
     "Output is capped at 64 KiB per stream; the command is killed after timeout_ms.",
     schema({"command": {**S, "description": "Shell command line."},
             "timeout_ms": {"type": "integer", "description": "Kill after this many ms (default 10000, max 600000)."}},
            ["command"]), ann("Run command in Coal OS"), tool_run),
    ("coal_read_file", "Read a file from the Coal OS file system. Text is returned as is, binary as base64.",
     schema({"path": {**S, "description": "Absolute path inside Coal."},
             "offset": {"type": "integer", "description": "Start offset in bytes."},
             "max_bytes": {"type": "integer", "description": "Bytes to read (default 65536, max 4 MiB)."},
             "encoding": {"type": "string", "enum": ["utf-8", "base64"], "description": "Force base64 output."},
             "save_to": {**S, "description": "Host path to save the bytes to instead of returning them."}},
            ["path"]), ann("Read file in Coal OS", read_only=True), tool_read_file),
    ("coal_write_file", "Write a file in the Coal OS file system. Provide exactly one of content, "
     "content_base64 or host_path (a file on the host to upload).",
     schema({"path": {**S, "description": "Absolute destination path inside Coal."},
             "content": {**S, "description": "Text content."},
             "content_base64": {**S, "description": "Binary content, base64 encoded."},
             "host_path": {**S, "description": "Host file to upload."},
             "append": {"type": "boolean", "description": "Append instead of truncating."},
             "mode": {"type": ["integer", "string"], "description": "Permissions for a new file, e.g. \"755\" (octal string) or 493."}},
            ["path"]), ann("Write file in Coal OS", destructive=True, idem=True), tool_write_file),
    ("coal_ls", "List a directory in Coal OS in ls -l style (type/mode, uid, gid, size, name).",
     schema({"path": {**S, "description": "Directory (default /)."}}),
     ann("List directory in Coal OS", read_only=True), tool_ls),
    ("coal_ps", "List processes in Coal OS (from /proc): pid, parent, state, name.",
     schema(), ann("List processes in Coal OS", read_only=True), tool_ps),
    ("coal_sysinfo", "Kernel identification (uname), uptime and memory info from Coal OS.",
     schema(), ann("Coal OS system info", read_only=True), tool_sysinfo),
    ("coal_kernel_log", "Tail of the kernel's serial console log (COM1) since boot. Use it to diagnose boot "
     "failures and panics.",
     schema({"lines": {"type": "integer", "description": "Lines to return (default 100)."}}),
     ann("Kernel log", read_only=True), tool_kernel_log),
    ("coal_screen", "Read the 80x25 VGA text screen of the VM as plain text (what a monitor would show).",
     schema(), ann("Screen text", read_only=True), tool_screen),
    ("coal_status", "Report whether the VM is running and which build directory is used.",
     schema(), ann("Coal OS status", read_only=True), tool_status),
]
TOOL_INDEX = {t[0]: t for t in TOOLS}


def tool_list():
    return [{"name": n, "title": an["title"], "description": d, "inputSchema": sc, "annotations": an}
            for n, d, sc, an, _ in TOOLS]


# --- MCP / JSON-RPC ---------------------------------------------------------------

class Server:
    def __init__(self, machine):
        self.machine = machine
        self.out_lock = threading.Lock()
        self.pool = ThreadPoolExecutor(max_workers=8)

    def send(self, msg):
        data = json.dumps(msg, separators=(",", ":"), ensure_ascii=False) + "\n"
        with self.out_lock:
            sys.stdout.write(data)
            sys.stdout.flush()

    def error(self, rid, code, message, data=None):
        e = {"code": code, "message": message}
        if data is not None:
            e["data"] = data
        self.send({"jsonrpc": "2.0", "id": rid, "error": e})

    def serve(self):
        for raw in sys.stdin.buffer:
            raw = raw.strip()
            if not raw:
                continue
            try:
                msg = json.loads(raw)
            except ValueError:
                self.error(None, -32700, "Parse error")
                continue
            if isinstance(msg, list):
                self.error(None, -32600, "Batches are not supported")
            elif not isinstance(msg, dict):
                self.error(None, -32600, "Invalid Request")
            else:
                self.pool.submit(self.handle, msg)
        self.pool.shutdown(wait=False, cancel_futures=True)
        self.machine.shutdown()

    def handle(self, msg):
        rid, method = msg.get("id"), msg.get("method")
        if not isinstance(method, str):
            if "id" in msg and "method" not in msg:
                return                   # a response to a request we never send
            return self.error(rid, -32600, "Invalid Request")
        if "id" not in msg:
            return                       # notifications need no action
        params = msg.get("params") or {}
        if not isinstance(params, dict):
            return self.error(rid, -32602, "params must be an object")
        meta = params.get("_meta") if isinstance(params.get("_meta"), dict) else {}
        modern = META_VERSION in meta
        if modern and meta[META_VERSION] not in MODERN_VERSIONS:
            return self.error(rid, -32022, "Unsupported protocol version",
                              {"supported": MODERN_VERSIONS + LEGACY_VERSIONS,
                               "requested": meta[META_VERSION]})
        try:
            result = self.dispatch(method, params, modern)
        except ToolError as e:
            result = {"content": [{"type": "text", "text": str(e)}], "isError": True}
        except KeyError:
            return self.error(rid, -32601, "Method not found: " + method)
        except ValueError as e:
            return self.error(rid, -32602, str(e))
        except Exception as e:           # a bug here must not kill the server
            log("internal error in %s: %r" % (method, e))
            return self.error(rid, -32603, "Internal error: %s" % e)
        if modern:
            result.setdefault("resultType", "complete")
            result.setdefault("_meta", {})[META_SERVER] = SERVER_INFO
        self.send({"jsonrpc": "2.0", "id": rid, "result": result})

    def dispatch(self, method, params, modern):
        caps = {"tools": {"listChanged": False}}
        if method == "initialize":
            wanted = params.get("protocolVersion")
            ver = wanted if wanted in LEGACY_VERSIONS else LEGACY_VERSIONS[0]
            return {"protocolVersion": ver, "capabilities": caps, "serverInfo": SERVER_INFO,
                    "instructions": INSTRUCTIONS}
        if method == "server/discover":
            return {"supportedVersions": MODERN_VERSIONS + LEGACY_VERSIONS, "capabilities": caps,
                    "instructions": INSTRUCTIONS, "ttlMs": 3600000, "cacheScope": "public"}
        if method == "ping":
            return {}
        if method == "tools/list":
            return {"tools": tool_list()}
        if method == "tools/call":
            name = params.get("name")
            if name not in TOOL_INDEX:
                raise ValueError("Unknown tool: %s" % name)
            args = params.get("arguments") or {}
            if not isinstance(args, dict):
                raise ValueError("arguments must be an object")
            try:
                text = TOOL_INDEX[name][4](self.machine, args)
            except (OSError, ValueError) as e:
                raise ToolError("%s failed: %s" % (name, e))
            return {"content": [{"type": "text", "text": text}], "isError": False}
        raise KeyError(method)


def parse_attach(spec):
    kind, _, rest = spec.partition(":")
    if kind == "unix" and rest:
        return ("unix", rest)
    if kind == "tcp" and ":" in rest:
        host, _, port = rest.rpartition(":")
        return ("tcp", (host, int(port)))
    raise SystemExit("--attach expects unix:PATH or tcp:HOST:PORT")


def main():
    ap = argparse.ArgumentParser(description="MCP server that drives Coal OS in QEMU")
    ap.add_argument("--attach", metavar="unix:PATH|tcp:HOST:PORT",
                    help="use an already running mcpd on this stream socket instead of managing QEMU")
    ap.add_argument("--build-dir", help="CMake build directory (default: <repo>/build/coal-mcp)")
    ap.add_argument("--memory", type=int, metavar="MIB",
                    help="default guest RAM for coal_boot (default: $COAL_MEMORY_MB or 512)")
    args = ap.parse_args()
    if args.memory:
        os.environ["COAL_MEMORY_MB"] = str(args.memory)
    if args.build_dir:
        os.environ["COAL_BUILD_DIR"] = args.build_dir
    machine = Machine(parse_attach(args.attach) if args.attach else None)
    signal.signal(signal.SIGTERM, lambda *_: (machine.shutdown(), os._exit(0)))
    try:
        Server(machine).serve()
    except KeyboardInterrupt:
        machine.shutdown()


if __name__ == "__main__":
    main()
