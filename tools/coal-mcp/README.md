# coal-mcp

An [MCP](https://modelcontextprotocol.io) server over stdio that lets Claude build, boot and
drive Coal OS. `server.py` is a thin entry point: the tools, the MCP protocol and the VM management
live in the gateway package (`tools/coal-gateway`, see [docs/agent-vm/gateway.md](../../docs/agent-vm/gateway.md)),
which also serves the same tools over Streamable HTTP. Standard library only (Python 3.9+).

```
Claude  <-- MCP / stdio -->  server.py  <-- COM2, framed JSON -->  /sbin/agentd  (inside Coal)
                                  |
                                  +-- QEMU (headless): COM1 = kernel log file, monitor = screen dump
```

## Requirements

`cmake`, a C toolchain that builds Coal OS, `nasm`, `mtools`, `grub-mkrescue`
(see the top-level README) and `qemu-system-x86_64`.

## Add it to Claude Code

```sh
claude mcp add coal -- python3 /path/to/repo/tools/coal-mcp/server.py
```

## Add it to Claude Desktop

In `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "coal": {
      "command": "python3",
      "args": ["/path/to/repo/tools/coal-mcp/server.py"]
    }
  }
}
```

Options: `--build-dir DIR` (or `COAL_BUILD_DIR`) selects the CMake build directory
(default `build/coal-mcp`), `COAL_QEMU_ARGS` adds extra QEMU arguments, `COAL_NIC` replaces the
network device (default `user,model=e1000`; add `hostfwd=tcp::HOSTPORT-:GUESTPORT` to reach a
guest server), `--memory` / `COAL_MEMORY_MB` sets the default guest RAM (512 MiB), and
`--attach unix:PATH|tcp:HOST:PORT|vsock:CID:PORT` talks to an already running agentd instead of
managing QEMU, and `--log-dir DIR` moves the per-run JSONL logs (default `<build-dir>/runs`).

## Tools

| Tool | Purpose |
| --- | --- |
| `coal_build` | CMake configure and build; returns warning and error counts and diagnostics |
| `coal_boot` | Build if needed, start QEMU headless, wait until agentd answers |
| `coal_shutdown`, `coal_reboot` | Stop or hard-reset the VM (disk is kept) |
| `coal_run` | `/bin/sh -c` a command, non-interactive; returns exit code, stdout, stderr and a structured envelope (timeout, default 120 s, kills the whole process group; long output keeps 2000 head + 6000 tail bytes and the full text stays in a file in the VM) |
| `coal_spawn`, `coal_jobs`, `coal_job_status`, `coal_job_output`, `coal_job_signal` | Background jobs: start, list, poll or wait, read output from an offset, signal; optional memory, process and CPU limits |
| `coal_read_file`, `coal_write_file` | Binary-safe file transfer (writes are atomic); `save_to` (the whole file unless `max_bytes` is given) / `host_path` read and write any host path the server user can access |
| `coal_stat`, `coal_mkdir`, `coal_remove` | Inspect, create and delete paths |
| `coal_download` | The host fetches an `http://` or `https://` URL and writes it into Coal (Coal itself has no TLS) |
| `coal_guide` (also the resource `coal://guide`) | System guide: layout, commands, limits, what is destructive, how to recover |
| `coal_snapshot`, `coal_restore`, `coal_reset` | Save the disk under a name, put it back (reboots), or rebuild the pristine image (reboots) |
| `coal_ls`, `coal_ps`, `coal_sysinfo` | Directory listing, process list (`/proc`), uname, uptime, meminfo |
| `coal_kernel_log` | Tail of the COM1 serial log; first stop for boot failures and panics |
| `coal_screen` | The 80x25 VGA text screen as text |
| `coal_keys` | Type text and named keys (arrows, Enter, Ctrl-C) on the console keyboard |
| `coal_status` | Whether the VM is running |

The server speaks MCP 2026-07-28 (stateless, `server/discover`) and the
`initialize` handshake of 2025-11-25 and earlier.

## Trust model and safe use

The MCP client gets full control: commands run as root inside the VM (the VM is the sandbox; it has a NAT'ed network and can reach the host at 10.0.2.2 and whatever the host can reach), and `save_to` / `host_path` can read and write host files, so connect only clients
you trust. The runtime directory holding the QEMU monitor and COM2 sockets is created with mode 0700
so other local users cannot attach to the VM.

Mistakes inside Coal are never fatal. The disk image persists between boots, so
`coal_snapshot name=x` before risky changes, then `coal_restore name=x` to undo them, or `coal_reset` for
a fresh image. Overwriting `/sbin/init`, `/sbin/agentd` or `/bin/sh` is the way to lose the link to the VM;
reset recovers it. Snapshots are stored in `<build-dir>/snapshots`; `coal_build clean=true` deletes them.
The server `instructions` and the guide tell the model all of this.

## What the OS image needs

`/sbin/agentd` (built from `src/CoalOS/userspace/agentd/agentd.c`) must be on the disk image and started
by init. It opens `/dev/ttyS1` and needs the Linux x86_64 calls `fork execve setsid pipe2 dup2 wait4 poll
kill open read write close lseek getdents64 lstat64 fstat64 rename mkdir rmdir unlink uname clock_gettime
nanosleep ioctl(TCGETS/TCSETS) setrlimit rt_sigaction`, a `/bin/sh`, and `/proc/meminfo`, `/proc/uptime`,
`/proc/<pid>/status`.

The wire protocol (length-prefixed JSON frames, multiplexed requests, the exec envelope, jobs, file
calls) is specified in [docs/agent-vm/protocol.md](../../docs/agent-vm/protocol.md). The serial link is
lossless, so frames of up to 256 KiB go through as they are and file transfers move 96 KiB per request.

## Example session

```
> coal_boot
booted in 0.5s; agentd is ready
> coal_run command="uname -a; ls /bin"
exit code: 0
--- stdout ---
Coal coal 0.2.0 #1 x86_64
> coal_write_file path="/tmp/hello.txt" content="hi\n"
wrote 3 bytes to /tmp/hello.txt
> coal_run command="cat /tmp/hello.txt"
> coal_kernel_log lines=20
> coal_shutdown
```

## Tests

```sh
python3 tools/coal-mcp/tests/test_server.py       # MCP protocol and the tools, end to end on a pty
python3 tools/coal-mcp/tests/test_agentd_fuzz.py  # malformed, oversized, nested and non-UTF-8 frames against agentd
python3 tools/coal-mcp/tests/smoke_test.py DIR    # builds in DIR, boots in QEMU, runs commands, a 3 MiB transfer,
                                                  # network checks, sh_test.sh and applet_test.sh inside Coal,
                                                  # snapshot/restore
python3 tools/coal-gateway/tests/test_protocol.py # agent protocol on a pty and in QEMU; see docs/agent-vm/gateway.md
```

Covers the MCP handshakes and tool list (a bug inside a tool is a tool error, only unknown
methods give -32601), and runs the real agentd (built as a 32-bit static Linux binary with
`userspace/agentd/test/build_host.sh`) on a pty bridged to a unix socket in place of COM2.

The disk image persists between boots and is not rebuilt by `coal_boot`: after changing user-space
sources (`agentd`, `sh`, `coalbox`) run `coal_reset` to put the new binaries on a fresh image. The kernel
is rebuilt on every boot.

## Compiling C for Coal

`tools/coal-cc [-o out] file.c [more.c] [gcc options]` builds a static x86_64 program against Coal's libc
(`#include <libc/stdio.h>`), with the same flags and `user.ld` as the in-tree programs. It needs `gcc` with
32-bit support and `nasm`. Install the result with `coal_write_file` (`host_path`), then `coal_run`. No FPU:
programs cannot use `float`/`double` arithmetic (`printf("%f")` works on values passed in).
`coal_guide` repeats the exact gcc command.
