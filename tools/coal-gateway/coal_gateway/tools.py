"""The Coal tools: one table of (name, schema, function), shared by every transport.

A tool function takes the call context and its arguments and returns text, or (text, data) when it
also has structured content. Failures are ToolError. Nothing here knows about JSON-RPC, HTTP or tasks.
"""
import base64
import json
import re
import threading
import time
import urllib.error
import urllib.request

from .errors import ToolError
from .guide import GUIDE
from .machine import default_memory_mb, summarize_build

READ_CHUNK = 96 * 1024         # fallback when agentd has not said its fs_chunk yet
MAX_READ = 4 * 1024 * 1024     # default cap for an in-chat read; save_to has no cap
LONG_RUN_MS = 30000            # an explicit coal_run timeout above this makes it a task
JOB_HANDLE = re.compile(r"j(\d+)-(\d+)")


class Call:
    """What a tool function may use: the machine, the run it belongs to and a cancellation flag."""

    def __init__(self, machine, run_id, cancel=None):
        self.machine = machine
        self.run_id = run_id
        self.cancel = cancel or threading.Event()

    def agent(self):
        return self.machine.need_agent()


class Tool:
    def __init__(self, name, description, props, required, annotations, fn, task=None):
        self.name = name
        self.description = description
        self.props = props
        self.required = required
        self.annotations = annotations
        self.fn = fn
        self.task = task               # None, "always", or a function of the arguments

    def runs_as_task(self, args):
        return self.task == "always" or (callable(self.task) and self.task(args))

    def listing(self):
        props = dict(self.props)
        props.setdefault("workspace_id", {"type": "string", "description": "Workspace handle from coal_status (optional check)."})
        props.setdefault("run_id", {"type": "string", "description": "Name of the run this call belongs to; every call of a run is logged together (default: this session's run)."})
        schema = {"type": "object", "properties": props, "required": list(self.required),
                  "additionalProperties": False}
        return {"name": self.name, "title": self.annotations["title"], "description": self.description,
                "inputSchema": schema, "annotations": self.annotations}


TOOLS = []
BY_NAME = {}


def tool(name, description, props=None, required=(), title=None, read_only=False, destructive=False,
         idem=False, task=None):
    def register(fn):
        ann = {"title": title or name, "readOnlyHint": read_only, "destructiveHint": destructive,
               "idempotentHint": idem, "openWorldHint": False}
        t = Tool(name, description, props or {}, required, ann, fn, task)
        TOOLS.append(t)
        BY_NAME[name] = t
        return fn
    return register


# --- argument helpers -------------------------------------------------------------

def _int(args, key, default, lo, hi):
    v = args.get(key, default)
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise ToolError("%s must be a number" % key)
    return max(lo, min(hi, int(v)))


def _opt_int(args, key, lo, hi):
    return None if args.get(key) is None else _int(args, key, 0, lo, hi)


def _str(args, key, required=True):
    v = args.get(key)
    if v is None and not required:
        return None
    if not isinstance(v, str) or (required and v == ""):
        raise ToolError("%s must be a non-empty string" % key)
    return v


def _env(args):
    env = args.get("env")
    if env is None:
        return None
    if not isinstance(env, dict) or not all(isinstance(k, str) and isinstance(v, str) for k, v in env.items()):
        raise ToolError("env must be an object of string values")
    return env


def _fields(**kw):
    """Request fields with the unset ones dropped."""
    return {k: v for k, v in kw.items() if v is not None}


def fmt_mode(mode, typ):
    t = {"dir": "d", "symlink": "l", "chr": "c", "blk": "b", "fifo": "p", "sock": "s"}.get(typ, "-")
    bits = "".join(("rwx"[i % 3] if mode & (1 << (8 - i)) else "-") for i in range(9))
    return t + bits


def job_handle(call, n):
    return "j%d-%d" % (call.agent().boot_seq, n)


def job_number(call, handle):
    """'j<boot>-<n>' -> n. A handle from an earlier boot names a job that no longer exists."""
    m = JOB_HANDLE.fullmatch(handle) if isinstance(handle, str) else None
    if not m:
        raise ToolError("job_id must look like j1-3 (as returned by coal_spawn)")
    if int(m.group(1)) != call.agent().boot_seq:
        raise ToolError("job %s belongs to an earlier boot of the VM and is gone" % handle)
    return int(m.group(2))


def _limits(args):
    return _fields(mem_mb=_opt_int(args, "mem_mb", 1, 1 << 20), nproc=_opt_int(args, "nproc", 1, 1 << 16),
                   cpu_s=_opt_int(args, "cpu_s", 1, 1 << 20))


LIMIT_PROPS = {
    "mem_mb": {"type": "integer", "description": "Address-space limit in MiB for the command (RLIMIT_AS)."},
    "nproc": {"type": "integer", "description": "At most this many more processes than exist now (RLIMIT_NPROC)."},
    "cpu_s": {"type": "integer", "description": "CPU-time limit in seconds (RLIMIT_CPU)."}}
EXEC_PROPS = {
    "command": {"type": "string", "description": "Shell command line."},
    "cwd": {"type": "string", "description": "Working directory (default /)."},
    "env": {"type": "object", "additionalProperties": {"type": "string"},
            "description": "Extra environment variables (PAGER=cat EDITOR=true CI=1 TERM=dumb GIT_TERMINAL_PROMPT=0 are preset)."},
    **LIMIT_PROPS}


# --- lifecycle ----------------------------------------------------------------------

@tool("coal_guide", "Return the Coal OS system guide: file system layout, available commands, supported system "
      "calls, limits, what is safe or destructive and how to recover. Read it before changing anything.",
      title="Coal OS guide", read_only=True)
def coal_guide(call, a):
    return GUIDE


@tool("coal_boot", "Boot Coal OS in QEMU (headless) and wait until its in-OS agent agentd answers. "
      "Builds the OS first if no images exist. Returns once the VM is usable or fails with the kernel log tail. "
      "Disk changes persist between boots unless snapshot=true.",
      {"rebuild": {"type": "boolean", "description": "Rebuild before booting (default false)."},
       "memory_mb": {"type": "integer", "description": "Guest RAM in MiB (default: --memory / COAL_MEMORY_MB, else 512)."},
       "snapshot": {"type": "boolean", "description": "Discard disk writes on shutdown (default false)."},
       "timeout_s": {"type": "integer", "description": "Seconds to wait for agentd (default 60)."}},
      title="Boot Coal OS", idem=True, task="always")
def coal_boot(call, a):
    return call.machine.boot(rebuild=bool(a.get("rebuild", False)),
                             memory_mb=_int(a, "memory_mb", default_memory_mb(), 16, 4096),
                             snapshot=bool(a.get("snapshot", False)), timeout=_int(a, "timeout_s", 60, 5, 600))


@tool("coal_shutdown", "Power off the VM immediately (QEMU quit; unsynced writes may be lost, run `sync` first). "
      "The disk image keeps its contents.", title="Shut down Coal OS", idem=True)
def coal_shutdown(call, a):
    return call.machine.shutdown()


@tool("coal_reboot", "Hard-reset the running VM (like the reset button: unsynced writes may be lost) and wait for "
      "agentd again. Disk contents are kept. Use this instead of running reboot inside Coal. Jobs do not survive.",
      {"timeout_s": {"type": "integer", "description": "Seconds to wait (default 60)."}},
      title="Reboot Coal OS", task="always")
def coal_reboot(call, a):
    return call.machine.reboot(_int(a, "timeout_s", 60, 5, 600))


@tool("coal_build", "Configure and build Coal OS with CMake. Returns warning/error counts and the diagnostics. "
      "An existing disk.img is not rebuilt (use coal_reset for a fresh image); clean=true deletes the whole "
      "build directory including snapshots.",
      {"clean": {"type": "boolean", "description": "Delete the build directory first."},
       "target": {"type": "string", "description": "CMake target (default: all)."}},
      title="Build Coal OS", idem=True, task="always")
def coal_build(call, a):
    ok, text = call.machine.build(clean=bool(a.get("clean", False)), target=a.get("target"))
    s = summarize_build(text)
    if not ok:
        raise ToolError("build FAILED\n" + s)
    return "build OK\n" + s


@tool("coal_snapshot", "Save the current disk image under a name (runs sync first). Snapshots live in "
      "<build-dir>/snapshots and survive reboots and coal_shutdown. Overwrites a snapshot of the same name.",
      {"name": {"type": "string", "description": "1-64 characters: letters, digits, _ . -"}}, ["name"],
      title="Snapshot Coal OS disk", idem=True)
def coal_snapshot(call, a):
    return call.machine.snapshot(_str(a, "name"))


@tool("coal_restore", "DESTRUCTIVE: replace the disk image with a snapshot, discarding every change made since, "
      "and reboot (the VM is stopped and, if it was running, booted again).",
      {"name": {"type": "string", "description": "Snapshot name from coal_snapshot."}}, ["name"],
      title="Restore Coal OS snapshot", destructive=True, idem=True, task="always")
def coal_restore(call, a):
    return call.machine.restore(_str(a, "name"))


@tool("coal_reset", "DESTRUCTIVE: throw away the disk image, rebuild the pristine one from the sources and reboot. "
      "Recovers from any mistake, including a broken /sbin/init; all files you created are lost.",
      title="Reset Coal OS to pristine", destructive=True, idem=True, task="always")
def coal_reset(call, a):
    return call.machine.restore(None)


@tool("coal_status", "Report whether the VM is running, the workspace_id and which build directory is used.",
      title="Coal OS status", read_only=True)
def coal_status(call, a):
    m = call.machine
    up = "attached" if m.attached else ("running (pid %d)" % m.proc.pid if m.running() else "stopped")
    data = {"state": up, "workspace_id": m.workspace_id, "run_id": call.run_id, "build_dir": m.build_dir}
    return "Coal: %s\nworkspace_id: %s\nrun_id: %s\nbuild dir: %s" % (up, m.workspace_id, call.run_id, m.build_dir), data


# --- commands -------------------------------------------------------------------------

def _stream_note(name, s):
    if not s.get("truncated"):
        return None
    where = " in the VM at %s" % s["path"] if s.get("path") else " (not kept: over the spill limit)"
    return "%s: %d of %d bytes shown; the full output is%s. Read the rest with coal_read_file offset/max_bytes." % (
        name, s["bytes_returned"], s["bytes_total"], where)


def format_exec(r, timeout_ms):
    """The text a client reads for a finished command: status line, then each stream."""
    out = "exit code: %d" % r["exit_code"]
    if r.get("timed_out"):
        out += " (timed out after %d ms, killed)" % timeout_ms
    elif r.get("killed_by") == "cancel":
        out += " (cancelled)"
    elif r.get("killed_by"):
        out += " (killed: %s)" % r["killed_by"]
    elif r.get("signal"):
        out += " (signal %d)" % r["signal"]
    if r["stdout"].get("truncated") or r["stderr"].get("truncated"):
        out += " (output truncated)"
    for name in ("stdout", "stderr"):
        if r[name]["text"]:
            out += "\n--- %s ---\n%s" % (name, r[name]["text"])
    if r.get("placeholder"):
        out += "\n" + r["placeholder"]
    notes = [n for n in (_stream_note("stdout", r["stdout"]), _stream_note("stderr", r["stderr"])) if n]
    notes += ["rlimit hit: %s" % ", ".join(r["rlimit_hits"])] if r.get("rlimit_hits") else []
    return out + ("\n" + "\n".join(notes) if notes else "")


@tool("coal_run", "Run a shell command inside Coal OS (/bin/sh -c) and return exit code, stdout and stderr, "
      "plus structured content: exit_code, signal, duration_ms, timed_out, killed_by, cwd and per stream "
      "bytes_total, bytes_returned, truncated and the path of the full output. Runs as root with full access; "
      "changes persist on the disk image. rm, mv, redirections (>), mount and writes to /sbin/init, "
      "/sbin/agentd or /bin/sh are destructive; poweroff/halt/reboot end the session. The command is "
      "non-interactive (stdin is /dev/null, no terminal). Output keeps the first head_bytes (2000) and the last "
      "tail_bytes (6000) of each stream; the full text is saved in the VM. After timeout_ms (default 120000) the "
      "command and its whole process group are killed. Use coal_spawn for long commands.",
      {**EXEC_PROPS,
       "timeout_ms": {"type": "integer", "description": "Kill after this many ms (default 120000, max 600000)."},
       "head_bytes": {"type": "integer", "description": "Bytes kept from the start of each stream (default 2000)."},
       "tail_bytes": {"type": "integer", "description": "Bytes kept from the end of each stream (default 6000)."}},
      ["command"], title="Run command in Coal OS",
      task=lambda a: isinstance(a.get("timeout_ms"), (int, float)) and a["timeout_ms"] > LONG_RUN_MS)
def coal_run(call, a):
    t = _int(a, "timeout_ms", 120000, 100, 600000)
    fields = _fields(cmd=_str(a, "command"), cwd=_str(a, "cwd", False), env=_env(a), timeout_ms=t,
                     head=_opt_int(a, "head_bytes", 0, 1 << 20), tail=_opt_int(a, "tail_bytes", 0, 1 << 20),
                     **_limits(a))
    r = call.agent().request("exec", timeout=t / 1000 + 10, cancel=call.cancel, **fields)
    data = {k: r[k] for k in ("exit_code", "signal", "duration_ms", "timed_out", "killed_by", "cwd",
                              "rlimit_hits", "stdout", "stderr") if k in r}
    if r.get("placeholder"):
        data["placeholder"] = r["placeholder"]
    return format_exec(r, t), data


@tool("coal_spawn", "Start a shell command in the background and return a job handle at once. The job has its own "
      "process group and its output goes to files in the VM; follow it with coal_job_status and coal_job_output. "
      "Jobs end when the VM reboots.",
      {**EXEC_PROPS,
       "timeout_ms": {"type": "integer", "description": "Kill the job after this many ms (default: never)."}},
      ["command"], title="Start a background job")
def coal_spawn(call, a):
    r = call.agent().request("spawn", **_fields(cmd=_str(a, "command"), cwd=_str(a, "cwd", False), env=_env(a),
                                                timeout_ms=_opt_int(a, "timeout_ms", 1, 24 * 3600000),
                                                **_limits(a)))
    h = job_handle(call, r["job_id"])
    data = {"job_id": h, "pid": r["pid"], "stdout_path": r["stdout_path"], "stderr_path": r["stderr_path"]}
    return "started job %s (pid %d); output in %s and %s" % (h, r["pid"], r["stdout_path"], r["stderr_path"]), data


@tool("coal_jobs", "List the background jobs of this boot: handle, state, age and command.",
      title="List background jobs", read_only=True)
def coal_jobs(call, a):
    jobs = [j for j in call.agent().request("job.list")["jobs"] if not j.get("exec")]
    for j in jobs:
        j["job_id"] = job_handle(call, j["job_id"])
    rows = ["%-8s %-8s %8d ms  %s" % (j["job_id"], j["state"], j["age_ms"], j["cmd"]) for j in jobs]
    return "%d job(s)\n%s" % (len(jobs), "\n".join(rows)), {"jobs": jobs}


@tool("coal_job_status", "State of a background job: running or exited, exit code, signal, duration, killed_by "
      "(timeout, cancel, signal, rlimit_cpu) and any resource limit it hit. With wait_ms it blocks until the "
      "job ends or the time is up. release=true frees the job and deletes its output files once it has exited.",
      {"job_id": {"type": "string", "description": "Handle from coal_spawn, like j1-3."},
       "wait_ms": {"type": "integer", "description": "Wait up to this long for the job to end (default 0, max 600000)."},
       "release": {"type": "boolean", "description": "Forget the job after reporting it (only if it has exited)."}},
      ["job_id"], title="Background job status")
def coal_job_status(call, a):
    handle, n = _str(a, "job_id"), job_number(call, _str(a, "job_id"))
    wait = _int(a, "wait_ms", 0, 0, 600000)
    agent = call.agent()
    r = agent.request("job.wait" if wait else "job.poll", timeout=wait / 1000 + 10, job_id=n,
                      **({"timeout_ms": wait} if wait else {}), cancel=call.cancel if wait else None)
    data = {k: r[k] for k in ("pid", "state", "exit_code", "signal", "duration_ms", "timed_out", "killed_by",
                              "cwd", "rlimit_hits", "stdout_path", "stderr_path", "cmd") if k in r}
    data["job_id"] = handle
    text = "job %s: %s" % (handle, r["state"])
    if r["state"] == "exited":
        text += ", exit code %d" % r["exit_code"]
        if r.get("signal"):
            text += ", signal %d" % r["signal"]
        if r.get("killed_by"):
            text += ", killed by %s" % r["killed_by"]
    text += " after %d ms" % r["duration_ms"]
    if r.get("rlimit_hits"):
        text += "\nrlimit hit: %s" % ", ".join(r["rlimit_hits"])
    if a.get("release"):
        agent.request("job.release", job_id=n)
        text += "\nreleased"
    return text, data


@tool("coal_job_output", "Read the output of a background job from a byte offset; pass next_offset back to "
      "read on (tail -f style). stream is stdout or stderr.",
      {"job_id": {"type": "string", "description": "Handle from coal_spawn."},
       "stream": {"type": "string", "enum": ["stdout", "stderr"], "description": "Default stdout."},
       "offset": {"type": "integer", "description": "Byte offset to start at (default 0)."},
       "max_bytes": {"type": "integer", "description": "At most this many bytes (default 32768)."}},
      ["job_id"], title="Background job output", read_only=True)
def coal_job_output(call, a):
    r = call.agent().request("job.tail", job_id=job_number(call, _str(a, "job_id")),
                             stream=a.get("stream", "stdout"), offset=_int(a, "offset", 0, 0, (1 << 31) - 1),
                             limit=_int(a, "max_bytes", 32768, 1, 90000))
    data = {k: r[k] for k in ("offset", "len", "next_offset", "size", "eof")}
    return "%s: %d bytes from offset %d, next_offset %d%s\n%s" % (
        a.get("stream", "stdout"), r["len"], r["offset"], r["next_offset"],
        " (end of output)" if r["eof"] else "", r["text"]), data


@tool("coal_job_signal", "Send a signal to a running background job's whole process group (default SIGTERM).",
      {"job_id": {"type": "string", "description": "Handle from coal_spawn."},
       "signal": {"type": "integer", "description": "Signal number 1-31 (default 15; 9 kills)."}},
      ["job_id"], title="Signal a background job", destructive=True)
def coal_job_signal(call, a):
    sig = _int(a, "signal", 15, 1, 31)
    call.agent().request("job.signal", job_id=job_number(call, _str(a, "job_id")), sig=sig)
    return "sent signal %d to job %s" % (sig, a["job_id"])


# --- files ----------------------------------------------------------------------------

def _read_chunks(agent, path, offset, limit):
    """Yields (raw bytes, file size) pieces of path from offset, at most limit bytes (None: to the end)."""
    chunk = agent.hello.get("fs_chunk", READ_CHUNK)
    done = 0
    while limit is None or done < limit:
        want = chunk if limit is None else min(chunk, limit - done)
        r = agent.request("fs.read", path=path, offset=offset + done, limit=want)
        piece = base64.b64decode(r["data"])
        if len(piece) != r["len"]:
            raise ToolError("fs.read: agentd sent %d bytes but announced %d (link error)" % (len(piece), r["len"]))
        if not piece:
            return
        done += len(piece)
        yield piece, r["size"]
        if r.get("eof"):
            return


@tool("coal_read_file", "Read a file from the Coal OS file system. Text is returned as is, binary as base64. With save_to the "
      "bytes are written to that path on the HOST (any path the server user can write, overwriting an "
      "existing file).",
      {"path": {"type": "string", "description": "Absolute path inside Coal."},
       "offset": {"type": "integer", "description": "Start offset in bytes."},
       "max_bytes": {"type": "integer", "description": "Bytes to read (default 65536, max 4 MiB; with save_to the default is the whole file)."},
       "encoding": {"type": "string", "enum": ["utf-8", "base64"], "description": "Force base64 output."},
       "save_to": {"type": "string", "description": "Host path to write the bytes to instead of returning them (overwrites)."}},
      ["path"], title="Read file in Coal OS", read_only=True)
def coal_read_file(call, a):
    path, offset, save = _str(a, "path"), _int(a, "offset", 0, 0, (1 << 31) - 1), _str(a, "save_to", False)
    agent = call.agent()
    if save:
        limit = _int(a, "max_bytes", 0, 1, 1 << 31) if a.get("max_bytes") is not None else None
        total, size = 0, None
        try:
            with open(save, "wb") as f:
                for piece, size in _read_chunks(agent, path, offset, limit):
                    f.write(piece)
                    total += len(piece)
        except OSError as e:
            raise ToolError("cannot write host file: %s" % e)
        return "saved %d bytes to %s (file size %s)" % (total, save, size)
    data, size = b"", None
    for piece, size in _read_chunks(agent, path, offset, _int(a, "max_bytes", 65536, 1, MAX_READ)):
        data += piece
    head = "%s: %d bytes from offset %d (file size %s)" % (path, len(data), offset, size)
    if a.get("encoding") == "base64":
        return head + "\n" + base64.b64encode(data).decode()
    try:
        return head + "\n" + data.decode("utf-8")
    except UnicodeDecodeError:
        return head + " [binary; showing base64]\n" + base64.b64encode(data).decode()


def write_bytes(agent, path, data, mode=0o644, append=False):
    """Stores data in the guest in chunks; agentd renames the finished file into place (atomic).

    With append the chunks are appended in place instead, which is not atomic.
    """
    step = agent.hello.get("fs_chunk", READ_CHUNK)
    for i in range(0, max(len(data), 1), step):
        piece = data[i:i + step]
        r = agent.request("fs.write", timeout=30, path=path, mode=mode, append=append,
                          more=i + step < len(data), data=base64.b64encode(piece).decode())
        if r.get("written") != len(piece):
            raise ToolError("fs.write: agentd stored %s of %d bytes at offset %d (link error); "
                            "the file is incomplete" % (r.get("written"), len(piece), i))


def _mode(a):
    mode = a.get("mode", 0o644)
    try:
        return int(mode, 8) if isinstance(mode, str) else int(mode)
    except ValueError:
        raise ToolError("mode must be an octal string or an integer")


@tool("coal_write_file", "Write a file in the Coal OS file system. Provide exactly one of content, "
      "content_base64 or host_path (any file readable on the host). The write is atomic (temp file, then "
      "rename). Overwrites an existing file unless append is set. Overwriting /sbin/init, /sbin/agentd, "
      "/bin/sh or /bin/coalbox can make the VM unreachable (recover with coal_restore/coal_reset); take a "
      "coal_snapshot first.",
      {"path": {"type": "string", "description": "Absolute destination path inside Coal."},
       "content": {"type": "string", "description": "Text content."},
       "content_base64": {"type": "string", "description": "Binary content, base64 encoded."},
       "host_path": {"type": "string", "description": "Host file to upload."},
       "append": {"type": "boolean", "description": "Append instead of truncating."},
       "mode": {"type": ["integer", "string"], "description": "Permissions for a new file, e.g. \"755\" (octal string) or 493."}},
      ["path"], title="Write file in Coal OS", destructive=True, idem=True)
def coal_write_file(call, a):
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
    write_bytes(call.agent(), path, data, _mode(a), bool(a.get("append", False)))
    return "wrote %d bytes to %s" % (len(data), path)


@tool("coal_download", "Download a URL on the HOST (http and https) and write it into the Coal file system. "
      "Gives Coal HTTPS downloads, which Coal itself cannot do yet. Overwrites an existing file; the same "
      "cautions as coal_write_file apply.",
      {"url": {"type": "string", "description": "http:// or https:// URL."},
       "path": {"type": "string", "description": "Absolute destination path inside Coal."},
       "max_bytes": {"type": "integer", "description": "Abort above this size (default 16 MiB, max 256 MiB)."},
       "timeout_s": {"type": "integer", "description": "Network timeout in seconds (default 30)."},
       "mode": {"type": ["integer", "string"], "description": "Permissions, e.g. \"755\"."}},
      ["url", "path"], title="Download a URL into Coal OS", destructive=True, idem=True)
def coal_download(call, a):
    url, path = _str(a, "url"), _str(a, "path")
    limit = _int(a, "max_bytes", 16 * 1024 * 1024, 1, 256 * 1024 * 1024)
    timeout = _int(a, "timeout_s", 30, 1, 600)
    if not re.match(r"https?://", url, re.I):
        raise ToolError("url must start with http:// or https://")
    chunks, size = [], 0
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "coal-gateway"}),
                                    timeout=timeout) as resp:
            while True:
                part = resp.read(65536)
                if not part:
                    break
                size += len(part)
                if size > limit:
                    raise ToolError("download exceeds max_bytes (%d)" % limit)
                chunks.append(part)
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise ToolError("download failed: %s" % e)
    data = b"".join(chunks)
    write_bytes(call.agent(), path, data, _mode(a))
    return "downloaded %d bytes from %s to %s" % (len(data), url, path)


@tool("coal_ls", "List a directory in Coal OS in ls -l style (type/mode, uid, gid, size, name).",
      {"path": {"type": "string", "description": "Directory (default /)."}},
      title="List directory in Coal OS", read_only=True)
def coal_ls(call, a):
    path = a.get("path", "/")
    r = call.agent().request("fs.list", path=path)
    rows = ["%s %5d %5d %9d %s%s" % (fmt_mode(e["mode"], e["type"]), e["uid"], e["gid"], e["size"], e["name"],
                                    "/" if e["type"] == "dir" else "")
            if "type" in e else "?????????? %s" % e["name"]
            for e in sorted(r["entries"], key=lambda e: e["name"])]
    if r.get("truncated"):
        rows.append("... (listing truncated)")
    return "%s: %d entries\n%s" % (path, len(r["entries"]), "\n".join(rows)), {"entries": r["entries"]}


@tool("coal_stat", "Type, size, mode, owner and modification time of one path in Coal OS.",
      {"path": {"type": "string", "description": "Absolute path."}}, ["path"],
      title="Stat a path in Coal OS", read_only=True)
def coal_stat(call, a):
    path = _str(a, "path")
    r = call.agent().request("fs.stat", path=path)
    data = {k: r[k] for k in ("type", "size", "mode", "uid", "gid", "mtime") if k in r}
    return "%s: %s %s %d bytes uid %d gid %d mtime %d" % (
        path, r["type"], fmt_mode(r["mode"], r["type"]), r["size"], r["uid"], r["gid"], r["mtime"]), data


@tool("coal_mkdir", "Create a directory in Coal OS (parents=true also creates missing parents and accepts an existing one).",
      {"path": {"type": "string", "description": "Absolute path."},
       "parents": {"type": "boolean", "description": "Create parents as needed."},
       "mode": {"type": ["integer", "string"], "description": "Permissions, e.g. \"755\"."}},
      ["path"], title="Make a directory in Coal OS", idem=True)
def coal_mkdir(call, a):
    path = _str(a, "path")
    call.agent().request("fs.mkdir", path=path, parents=bool(a.get("parents", False)), mode=_mode({"mode": a.get("mode", 0o755)}))
    return "created " + path


@tool("coal_remove", "DESTRUCTIVE: delete a file or an empty directory in Coal OS; recursive=true deletes a whole tree.",
      {"path": {"type": "string", "description": "Absolute path."},
       "recursive": {"type": "boolean", "description": "Delete directories with their contents."}},
      ["path"], title="Remove a path in Coal OS", destructive=True, idem=True)
def coal_remove(call, a):
    path = _str(a, "path")
    call.agent().request("fs.remove", path=path, recursive=bool(a.get("recursive", False)))
    return "removed " + path


@tool("coal_ps", "List processes in Coal OS (from /proc): pid, parent, state, name.",
      title="List processes in Coal OS", read_only=True)
def coal_ps(call, a):
    procs = sorted(call.agent().request("sys.ps")["processes"], key=lambda p: p["pid"])
    rows = ["  PID  PPID STATE        NAME"] + ["%5d %5d %-12s %s" % (p["pid"], p["ppid"], p["state"], p["name"]) for p in procs]
    return "\n".join(rows), {"processes": procs}


@tool("coal_sysinfo", "Kernel identification (uname), uptime and memory info from Coal OS.",
      title="Coal OS system info", read_only=True)
def coal_sysinfo(call, a):
    r = call.agent().request("sys.info")
    head = " ".join(r.get(k, "?") for k in ("sysname", "nodename", "release", "version", "machine"))
    return "%s\n--- /proc/uptime ---\n%s\n--- /proc/meminfo ---\n%s" % (
        head, r.get("uptime", "n/a").strip(), r.get("meminfo", "n/a").strip())


# --- console ---------------------------------------------------------------------------

@tool("coal_kernel_log", "Tail of the kernel's serial console log (COM1) since boot. Use it to diagnose boot "
      "failures and panics.", {"lines": {"type": "integer", "description": "Lines to return (default 100)."}},
      title="Kernel log", read_only=True)
def coal_kernel_log(call, a):
    return call.machine.kernel_log(_int(a, "lines", 100, 1, 5000))


@tool("coal_screen", "Read the 80x25 VGA text screen of the VM as plain text (what a monitor would show).",
      title="Screen text", read_only=True)
def coal_screen(call, a):
    return call.machine.screen()


# QEMU key names for typing on the console. The console uses the Norwegian
# layout, so punctuation is mapped to the physical keys that produce it there.
_KEYS = {" ": "spc", "\n": "ret", "\t": "tab", "-": "slash", "_": "shift-slash", ".": "dot",
         ",": "comma", ":": "shift-dot", ";": "shift-comma", "/": "shift-7", "=": "shift-0",
         "!": "shift-1", '"': "shift-2", "#": "shift-3", "&": "shift-6", "(": "shift-8",
         ")": "shift-9", "?": "shift-minus", "+": "minus", "*": "shift-backslash",
         "'": "backslash", "<": "less", ">": "shift-less", "|": "grave_accent"}
NAMED_KEYS = {"up", "down", "left", "right", "ret", "esc", "spc", "tab", "backspace", "delete",
              "home", "end", "pgup", "pgdn", "ctrl-c", "ctrl-d", "ctrl-f", "ctrl-l", "ctrl-q", "ctrl-s", "ctrl-u", "ctrl-z"}


def _qemu_key(ch):
    if ch in _KEYS:
        return _KEYS[ch]
    if ch.isdigit() or ("a" <= ch <= "z"):
        return ch
    if "A" <= ch <= "Z":
        return "shift-" + ch.lower()
    raise ToolError("cannot type %r on the console; use coal_run for such input" % ch)


@tool("coal_keys", "Type on the VM's keyboard, as a person at the console would: `text` is typed key by key "
      "(letters, digits, space, newline and common punctuation), then each name in `keys` is pressed "
      "(up, down, left, right, ret, esc, tab, backspace, ctrl-c, ctrl-d, ctrl-s, ctrl-q, ...). Use it for interactive, "
      "full-screen programs on the console (the shell on the screen, games, editors) and read the result "
      "with coal_screen. Input goes to whatever runs on the console; coal_run is better for scripted commands.",
      {"text": {"type": "string", "description": "Characters to type."},
       "keys": {"type": "array", "items": {"type": "string"}, "description": "Named keys to press after the text."},
       "delay_ms": {"type": "integer", "description": "Pause between keys (default 30)."}},
      title="Type on the console")
def coal_keys(call, a):
    text = a.get("text") or ""
    keys = a.get("keys") or []
    if not isinstance(text, str) or not isinstance(keys, list):
        raise ToolError("text must be a string and keys a list of key names")
    seq = [_qemu_key(c) for c in text]
    for k in keys:
        if k not in NAMED_KEYS:
            raise ToolError("unknown key %r; known: %s" % (k, ", ".join(sorted(NAMED_KEYS))))
        seq.append(k)
    if len(seq) > 500:
        raise ToolError("at most 500 keys per call")
    delay = _int(a, "delay_ms", 30, 0, 2000) / 1000.0
    for k in seq:
        call.machine._monitor("sendkey " + k)
        if delay:
            time.sleep(delay)
    return "sent %d key(s) to the console" % len(seq)


def listing():
    return [t.listing() for t in TOOLS]
