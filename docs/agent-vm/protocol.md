# Coal agent protocol

The wire protocol between a host gateway and `agentd`, the agent inside the Coal OS guest. It is
independent of what carries it: today the second serial port (COM2), later a virtio-vsock stream.
Nothing above the transport layer changes when the carrier does.

```
client (MCP)  <--stdio / HTTP-->  coal-gateway  <--agent protocol, framed-->  agentd (guest)
                                      |                                          |
                              Transport (host side)                      link_open (guest side)
                              unix socket now,                           serial now,
                              AF_VSOCK later                             AF_VSOCK later
```

Version 1. Source of truth: `src/CoalOS/userspace/agentd/agentd.c` (guest) and
`tools/coal-gateway/coal_gateway/{frames,agent,transport}.py` (host).

## Framing

A byte stream carries a sequence of frames:

```
+-------------------+----------------------------+
| u32 length, LE    | length bytes of UTF-8 JSON |
+-------------------+----------------------------+
```

* The body is one JSON object. Length 0 is an empty frame and is ignored.
* Maximum body: 262144 bytes (`max_frame`, announced in `hello`). A sender never exceeds it; the
  gateway refuses to send a larger request (a `ToolError`), agentd answers an oversize announcement
  with a `link_error` event.
* Byte order of the length is little-endian on both ends.
* Strings are JSON strings. Binary data (file contents) is base64 inside a string.
* Numbers are integers. agentd rejects values outside 32 bits with `number out of range`.

### Resynchronisation

A stream has no record boundaries, so both sides need a way back after an error:

| Situation | agentd does |
|---|---|
| A length above `max_frame` | sends a `link_error` event (`frame too large`), then discards input until the line has been quiet for 50 ms |
| A partial frame stalls for 1 s | sends a `link_error` event (`incomplete frame dropped`) and drops it |
| A complete frame that is not valid JSON, or is not an object | answers `bad_request` with `"id":null` and carries on (the length already says where it ends) |

A client that sees a `link_error`, or loses track, stops sending, waits 100 ms of silence, and sends
a `ping`. The framing is otherwise self-synchronising only through those quiet periods, so a sender
must never pause in the middle of a frame.

## Messages

### Request

```json
{"id": 7, "op": "exec", "cmd": "ls /", "timeout_ms": 5000}
```

* `id`: any JSON number or string chosen by the client, unique among its outstanding requests. It
  is echoed unchanged in the response. `cancel` can only name numeric ids.
* `op`: the operation. Other fields depend on the op; unknown fields are ignored.

### Response

```json
{"id": 7, "ok": true, ...}
{"id": 7, "ok": false, "error": {"code": "ENOENT", "errno": 2, "message": "open failed: No such file or directory"}}
```

`error.code` is an errno name (`ENOENT`, `EAGAIN`, ...) with `errno`, or a protocol-level code with
no errno: `bad_request` (malformed or missing fields) and `unknown_op`.

### Events

Unsolicited messages have no `id`; they have `"event"` instead.

| Event | When | Fields |
|---|---|---|
| `hello` | agentd starts (once per boot) | as the `hello` op |
| `job.exit` | a job or exec has ended | the job state, as `job.poll` |
| `rlimit` | a resource limit was hit | `job_id`, `resource` (`cpu`, `nproc`, `as`) |
| `link_error` | a framing problem was detected | `message` |

The `hello` event doubles as a boot marker: a gateway counts them, and a job id is only valid within
the boot that issued it.

### Multiplexing

Requests are answered when they finish, in any order. A slow request (`exec`, `job.wait`) never
holds up a fast one (`ping`, `fs.stat`): agentd is a single-threaded event loop that polls the link,
a SIGCHLD pipe and the output pipes of running commands. A client may have many requests outstanding
and matches responses by `id`. Limits: 6 running `exec` calls, 32 jobs (exec and spawn together).
Beyond that the request fails with `EAGAIN`.

There is no credit window: the transports in use are lossless (the serial driver and a vsock stream
both apply back-pressure), so a sender that writes whole frames is never overrun.

## Operations

`ping` returns `{"pong":true}`. `hello` returns the agent description:

```json
{"proto":1,"agent":"agentd","max_frame":262144,"max_jobs":32,"max_exec":6,"fs_chunk":98304,
 "spill_dir":"/tmp/agentd","rlimits":["cpu_s","mem_mb","nproc"]}
```

### Running commands

Every command runs as `/bin/sh -c CMD`, non-interactively:

* stdin is `/dev/null`, there is no controlling terminal (new session, own process group),
* environment: `PAGER=cat EDITOR=true CI=1 TERM=dumb GIT_TERMINAL_PROMPT=0`, plus `env`,
* working directory `cwd` (default `/`),
* wall-clock limit `timeout_ms`; when it expires the whole process group gets SIGKILL.

Common request fields of `exec` and `spawn`:

| Field | Meaning |
|---|---|
| `cmd` | the command line (required) |
| `cwd` | working directory |
| `env` | object of string values added to or overriding the defaults (at most 16 entries) |
| `timeout_ms` | `exec`: default 120000, at most 3600000. `spawn`: default none |
| `mem_mb` | `RLIMIT_AS` in MiB |
| `cpu_s` | `RLIMIT_CPU` in seconds (SIGXCPU at the limit, SIGKILL two seconds later) |
| `nproc` | `RLIMIT_NPROC`: at most this many processes more than exist at spawn time |

#### `exec`: run and wait

Extra fields: `head` (default 2000) and `tail` (default 6000) bytes kept per stream, `spill_max`
(default 4 MiB, at most 16 MiB). The response comes when the command has ended:

```json
{"id":7,"ok":true,"job_id":3,"pid":41,"state":"exited","exit_code":7,"signal":0,"duration_ms":8,
 "timed_out":false,"killed_by":null,"cwd":"/","rlimit_hits":[],
 "stdout":{"text":"out\n","bytes_total":4,"bytes_returned":4,"truncated":false},
 "stderr":{"text":"","bytes_total":0,"bytes_returned":0,"truncated":false},
 "placeholder":"(no output)"}
```

* `exit_code`: the shell's exit status; `signal`: the terminating signal or 0. A command killed by
  SIGKILL reports `exit_code` 137 and `signal` 9.
* `killed_by`: `timeout`, `cancel`, `signal`, `rlimit_cpu` or `null`. `timed_out` is true for
  `timeout`.
* `rlimit_hits`: names of limits the kernel enforced against the job's process tree (`cpu`,
  `nproc`, `as`).
* `placeholder`: `"(no output)"` appears only when both streams are empty.
* Truncation keeps the first `head` and the last `tail` bytes of a stream, joined by a line
  `[... N bytes omitted ...]`. `bytes_total` counts everything the command wrote, `bytes_returned`
  what `text` holds. When `truncated` is true and the output fitted the spill cap, `path` names a
  file in the guest with the **complete** stream; read it with `fs.read` (offset/limit). Above
  `spill_max` the file holds the first `spill_max` bytes. The 32 newest spill sets are kept; the spill
  directory is emptied when agentd starts.

#### `spawn`: run in the background

Returns at once with `job_id`, `pid`, `stdout_path`, `stderr_path`. The job's output goes straight to
those files (no pumping through agentd, so there is no size limit beyond the disk).

#### Jobs

| Op | Fields | Result |
|---|---|---|
| `job.poll` | `job_id` | job state: `job_id pid state exit_code signal duration_ms timed_out killed_by cwd rlimit_hits cmd`, plus `stdout_path`/`stderr_path` for spawned jobs. `state` is `running` or `exited`; `exit_code` and `signal` are present once exited |
| `job.wait` | `job_id`, `timeout_ms` (default 30000, 0 = poll) | as `job.poll`, delayed until the job ends or the time is up |
| `job.tail` | `job_id`, `stream` (`stdout`/`stderr`), `offset`, `limit` | `text offset len next_offset size eof`; pass `next_offset` back to follow a running job |
| `job.signal` | `job_id`, `sig` (1-31, default 15) | signals the job's process group; `killed_by` becomes `signal` |
| `job.list` | | `jobs`: `job_id pid state exec age_ms cmd` |
| `job.release` | `job_id` | frees the job slot and its output files (only once exited) |
| `cancel` | `target` (numeric request id of an `exec`) | kills that exec's group; its response has `killed_by` `cancel`. `cancelled:false` if it already finished |

`exec` jobs are not reachable through `job.*`: their output comes with the response.

### Files

| Op | Fields | Result |
|---|---|---|
| `fs.stat` | `path` | `type size mode uid gid mtime` (`type`: `file dir symlink chr blk fifo sock other`) |
| `fs.read` | `path`, `offset`, `limit` (at most `fs_chunk`) | `size offset len eof data` (base64) |
| `fs.write` | `path`, `data` (base64), `mode`, `more`, `append` | `written committed` |
| `fs.list` | `path` | `entries` (`name type size mode uid gid mtime`), `truncated` |
| `fs.mkdir` | `path`, `mode`, `parents` | `created`; with `parents` an existing directory is accepted (`created` false), without it `EEXIST` |
| `fs.remove` | `path`, `recursive` | refuses `/`; a non-empty directory needs `recursive` |
| `sys.ps` | | `processes`: `pid ppid name state` |
| `sys.info` | | `sysname nodename release version machine meminfo uptime` |

**Atomic write.** Without `append`, `fs.write` writes into `<path>.agentd-tmp` and renames it over the
target when a chunk arrives with `more` false (or absent). A file larger than one frame is sent as
several `fs.write` requests for the same path, `more:true` on all but the last; the target is
untouched until the last arrives. One chunked upload can be open at a time: a write to another path
abandons it (the temporary file is removed). With `append:true` the data is appended in place, one
request at a time, which is not atomic. The reply's `written` must equal the chunk length; the
gateway treats a mismatch as a link error.

## Transport seam

**Host** (`tools/coal-gateway/coal_gateway/transport.py`): the interface is

```python
class Transport:
    def connect(self, timeout): ...
    def send(self, data: bytes): ...       # whole bytes, blocking
    def recv(self) -> bytes: ...           # b"" at end of stream
    def close(self): ...
```

`SocketTransport` implements it for `unix:PATH`, `tcp:HOST:PORT` and `vsock:CID:PORT`
(`AF_VSOCK`). `AgentClient` (`agent.py`) sits on it: it frames requests, runs the reader thread,
routes responses by id and events to listeners. Nothing in `AgentClient` or above names a carrier.

**Guest** (`agentd.c`): `link_open()` is the only function that knows what the link is. It opens the
serial device in raw mode and makes it file descriptors 0 and 1; the loop reads and writes only those.
A vsock carrier replaces `link_open()`.

## Limits at a glance

| Item | Value |
|---|---|
| Frame body | 262144 bytes |
| `fs.read` / `fs.write` chunk | 98304 raw bytes |
| Kept per stream by default | 2000 head + 6000 tail |
| Spill cap per stream | 4 MiB (16 MiB at most) |
| `exec` timeout | 120 s default, 3600 s at most |
| Concurrent `exec` / jobs | 6 / 32 |
| Partial frame timeout, resync quiet time | 1 s, 50 ms |
