# coal-gateway

The host side of the Coal Agent VM. It starts Coal OS in QEMU (or attaches to a running one), talks to
`agentd` with the [agent protocol](protocol.md), and exposes the VM to an MCP client as tools, over
stdio or over Streamable HTTP. Python 3.9+, standard library only.

```
tools/coal-gateway/
  gateway.py                 command line: gateway.py stdio | http
  coal_gateway/
    frames.py transport.py agent.py     agent protocol: framing, carrier, multiplexed client
    machine.py                          build, QEMU, snapshots, handles (workspace_id)
    tools.py guide.py                   the 27 tools and the system guide
    mcp.py tasks.py                     JSON-RPC dispatcher, tasks extension
    runlog.py                           per-run JSONL log
    stdio.py streamable_http.py auth.py transports and authentication
    cli.py                              options
tools/coal-mcp/server.py     the stdio entry point kept for existing setups (same tools, same options)
```

## Run it

### stdio

```sh
python3 tools/coal-gateway/gateway.py stdio                  # builds and boots on coal_boot
python3 tools/coal-mcp/server.py                             # the same thing, the original entry point
```

Options (both entry points): `--attach unix:PATH|tcp:HOST:PORT|vsock:CID:PORT|hvsock:PATH:PORT` uses an `agentd` that is
already running instead of managing QEMU; `--build-dir DIR` (default `build/coal-mcp`); `--memory MIB`
(default guest RAM, else `$COAL_MEMORY_MB`, else 512); `--log-dir DIR` (default `<build-dir>/runs`).
`--virtio blk,console,rng,vsock[=CID]` (or `$COAL_VIRTIO`) gives the guest virtio devices and moves the agentd link off COM2 (see `virtio.md`). `COAL_QEMU_ARGS` adds QEMU arguments, `COAL_NIC` replaces the network device.

### Streamable HTTP

```sh
python3 tools/coal-gateway/gateway.py http                   # 127.0.0.1:7077, endpoint /mcp
python3 tools/coal-gateway/gateway.py http --port 8123 --token-file ~/coal.token
```

The server listens on `127.0.0.1` only. On first run it writes a random bearer token to
`~/.config/coal-gateway/token` (`$XDG_CONFIG_HOME` respected), mode 0600, in a 0700 directory. An
existing token file that is readable by group or others, or holds fewer than 16 characters, is
refused. Print it with `cat ~/.config/coal-gateway/token`; delete the file to rotate it.

| Option | Meaning |
|---|---|
| `--host`, `--port` | bind address (default `127.0.0.1`, `7077`) |
| `--token-file FILE` | where the bearer token lives |
| `--allow-origin ORIGIN` | accept browser requests from this origin as well (localhost origins always pass) |
| `--allow-non-loopback` | required to bind any non-loopback address; put TLS and OAuth 2.1 in front first |

Try it:

```sh
TOKEN=$(cat ~/.config/coal-gateway/token)
curl -s http://127.0.0.1:7077/mcp -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: server/discover' \
  -d '{"jsonrpc":"2.0","id":1,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}'
```

### Connect Claude Code

stdio, in the project's `.mcp.json` (or `claude mcp add coal -- python3 /path/to/repo/tools/coal-gateway/gateway.py stdio`):

```json
{
  "mcpServers": {
    "coal": {
      "command": "python3",
      "args": ["/path/to/repo/tools/coal-gateway/gateway.py", "stdio"]
    }
  }
}
```

HTTP (start the gateway first; the token reaches the client through an environment variable so it never
lands in a file you commit):

```json
{
  "mcpServers": {
    "coal": {
      "type": "http",
      "url": "http://127.0.0.1:7077/mcp",
      "headers": { "Authorization": "Bearer ${COAL_GATEWAY_TOKEN}" }
    }
  }
}
```

```sh
export COAL_GATEWAY_TOKEN=$(cat ~/.config/coal-gateway/token)
claude
```

## Tools

27 tools. Every tool also accepts `workspace_id` and `run_id` (see Handles).

| Group | Tools |
|---|---|
| Lifecycle | `coal_guide`, `coal_boot`, `coal_shutdown`, `coal_reboot`, `coal_build`, `coal_snapshot`, `coal_restore`, `coal_reset`, `coal_status` |
| Commands | `coal_run` (wait for the result), `coal_spawn` (background job) |
| Jobs | `coal_jobs`, `coal_job_status` (`wait_ms`, `release`), `coal_job_output` (offset based), `coal_job_signal` |
| Files | `coal_read_file`, `coal_write_file` (atomic), `coal_download`, `coal_ls`, `coal_stat`, `coal_mkdir`, `coal_remove` |
| System | `coal_ps`, `coal_sysinfo`, `coal_kernel_log`, `coal_screen`, `coal_keys` |

`coal_run` returns text (`exit code: N`, `--- stdout ---`, `--- stderr ---`, notes) and
`structuredContent` with the envelope: `exit_code signal duration_ms timed_out killed_by cwd
rlimit_hits stdout stderr` where each stream has `text bytes_total bytes_returned truncated path`.
Arguments: `command`, `cwd`, `env`, `timeout_ms` (default 120000), `head_bytes` (2000), `tail_bytes`
(6000), `mem_mb`, `nproc`, `cpu_s`. Long output is cut to head and tail and the whole stream stays in a
file in the VM; the note names it, and `coal_read_file` with `offset`/`max_bytes` reads any part of it.
Commands are non-interactive (stdin `/dev/null`, no terminal, `PAGER=cat EDITOR=true CI=1 TERM=dumb
GIT_TERMINAL_PROMPT=0`).

## Handles

The server keeps no session; state lives behind explicit handles passed as ordinary arguments.

| Handle | Form | Meaning |
|---|---|---|
| `workspace_id` | `ws-1a2b3c4d` | The VM image plus its build directory. Minted on first use, stored in `<build-dir>/workspace_id`, shown by `coal_status`. Optional on every call; a wrong value is an error, so a client talking to the wrong gateway finds out |
| `run_id` | `[A-Za-z0-9][A-Za-z0-9_.-]{0,63}` | Names a sequence of calls. The client picks it; calls without one belong to this process's default run. Every call of a run is logged to the same file |
| `job_id` | `j<boot>-<n>` | A background job from `coal_spawn`. `<boot>` counts VM boots, so a handle from before a reboot is rejected instead of naming a different job |
| `taskId` | opaque | A task (below) |

## Long calls: the tasks extension

The server advertises `io.modelcontextprotocol/tasks` in `server/discover`. A client that declares it in
`_meta["io.modelcontextprotocol/clientCapabilities"].extensions` gets a task handle instead of a
blocking result for `coal_build`, `coal_boot`, `coal_reboot`, `coal_restore`, `coal_reset`, and for
`coal_run` when it is given an explicit `timeout_ms` above 30000:

```json
{"resultType":"task","taskId":"...","status":"working","ttlMs":3600000,"pollIntervalMs":1000, ...}
```

The client polls `tasks/get` (`Mcp-Name` header = `taskId` over HTTP). A finished task carries the tool
result in `result` (status `completed`, or `cancelled`), or `error` (`failed`). `tasks/cancel`
is cooperative: for `coal_run` it kills the command in the VM. `tasks/update` is acknowledged (no tool
asks the client for input). Tasks live in memory for one hour after they finish; they do not survive a
gateway restart. Clients without the extension, and short calls, get an ordinary result.

## HTTP details

Stateless 2026-07-28: no `initialize`, no session id, `server/discover`, per-request `_meta`. `POST
/mcp` returns one JSON response (no SSE, no server-initiated messages); notifications get 202. Older
clients that send no `MCP-Protocol-Version` header and use `initialize` are served too.

| Check | Result |
|---|---|
| `Origin` header present and not localhost or `--allow-origin` | 403 |
| Missing or wrong bearer token (compared in constant time) | 401 with `WWW-Authenticate: Bearer` |
| `GET`, `PUT`, `DELETE` on `/mcp`; any other path | 405 / 404 |
| Body over 8 MiB, no `Content-Length` | 413 / 411 |
| Body not one JSON object | 400 (`-32700` or `-32600`) |
| `Mcp-Method` / `Mcp-Name` / `MCP-Protocol-Version` disagree with the body, or `Mcp-Method` missing on a 2026-07-28 request | 400, error `-32020` |
| Unsupported `_meta` protocol version | error `-32022` |

Error replies close the connection. The access log is off (paths and arguments stay out of stderr).

### Remote use and OAuth 2.1

A static bearer token on loopback is for one user on one machine. For remote use the target is the MCP
authorization model: OAuth 2.1 with the gateway as a resource server, RFC 9728 protected-resource
metadata, Client ID Metadata Documents for registration, and TLS from a reverse proxy in front.
`auth.py` is the seam: `Authenticator` has `authenticate(headers) -> Principal | None`, `challenge()`
(the `WWW-Authenticate` value) and `resource_metadata()` (served at
`/.well-known/oauth-protected-resource` when it returns a document). An OAuth implementation validates
the access token's signature, issuer, audience and expiry and returns a `Principal` with the token's
scopes; it replaces `BearerToken` in `cli.run_http` and nothing else changes. Until then, do not use
`--allow-non-loopback` without a TLS-terminating proxy that adds its own authentication.

## Run log

Every tool call is appended to `<log-dir>/<run_id>.jsonl` (default `<build-dir>/runs`; file 0600,
directory 0700), one JSON object per line, written when the call finishes (calls that ran as a task also carry `task_id`):

```json
{"run_id":"build-fix-2","seq":3,"ts":1795000123.4,"workspace_id":"ws-1a2b3c4d","tool":"coal_run",
 "args":{"command":"make test","cwd":"/src"},"is_error":false,
 "result":{"sha256":"9f2c...","bytes":412},"started_at":1795000121.9,"duration_ms":1480,"boot":2}
```

`seq` counts from 1 per run and continues across gateway restarts. The result is recorded as a
SHA-256 and size, not copied; with the arguments it is the input of a replay (re-run the calls in order
against a restored snapshot and compare digests). Secrets stay out: `content`, `content_base64`, `data`
and `text` arguments are logged as digest and length, `env` values and any key containing `token`,
`secret`, `passw`, `authorization`, `api_key`, `credential` or `cookie` are masked, and free text is
scrubbed of `Bearer ...`, `token=`/`password=`/`secret=`/`api_key=` pairs and `user:password@` in URLs.
Strings over 1024 characters are cut. The log cannot see what a command prints, only its digest.

## Tests

```sh
python3 tools/coal-gateway/tests/test_frames.py       # codec, addresses, redaction, run log (no VM)
python3 tools/coal-gateway/tests/test_protocol.py     # protocol cases on agentd (pty) and in QEMU (TCG)
python3 tools/coal-gateway/tests/test_http.py         # HTTP, auth, tasks, run log
python3 tools/coal-mcp/tests/test_server.py           # stdio server end to end
python3 tools/coal-mcp/tests/test_agentd_fuzz.py      # hostile frames against agentd
python3 tools/coal-mcp/tests/smoke_test.py DIR        # builds in DIR, boots in QEMU, runs everything
```

`COAL_TEST_NO_QEMU=1` skips the QEMU half of `test_protocol.py`; `COAL_TEST_BUILD` sets its build
directory.
