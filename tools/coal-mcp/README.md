# coal-mcp

An [MCP](https://modelcontextprotocol.io) server that lets Claude build, boot and
drive Coal OS. One Python file, standard library only (Python 3.9+).

```
Claude  <-- MCP / stdio -->  tools/coal-mcp/server.py  <-- COM2, JSON lines -->  /sbin/mcpd  (inside Coal)
                                       |
                                       +-- QEMU (headless): COM1 = kernel log file, monitor = screen dump
```

## Requirements

`cmake`, a C toolchain that builds Coal OS, `nasm`, `mtools`, `grub-mkrescue`
(see the top-level README) and `qemu-system-i386`.

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
`--attach unix:PATH|tcp:HOST:PORT` talks to an already running mcpd instead of
managing QEMU.

## Tools

| Tool | Purpose |
| --- | --- |
| `coal_build` | CMake configure and build; returns warning and error counts and diagnostics |
| `coal_boot` | Build if needed, start QEMU headless, wait until mcpd answers |
| `coal_shutdown`, `coal_reboot` | Stop or hard-reset the VM (disk is kept) |
| `coal_run` | `/bin/sh -c` a command; returns exit code, stdout, stderr (timeout kills the whole process group, 64 KiB per stream, "output truncated" beyond that) |
| `coal_read_file`, `coal_write_file` | Binary-safe file transfer; `save_to` (the whole file unless `max_bytes` is given) / `host_path` read and write any host path the server user can access |
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
a fresh image. Overwriting `/sbin/init`, `/sbin/mcpd` or `/bin/sh` is the way to lose the link to the VM;
reset recovers it. Snapshots are stored in `<build-dir>/snapshots`; `coal_build clean=true` deletes them.
The server `instructions` and the guide tell the model all of this.

## What the OS image needs

`/sbin/mcpd` (built from `src/CoalOS/userspace/mcpd/mcpd.c`) must be on the disk
image and started by init. It opens `/dev/ttyS1` and needs the Linux i386
calls `fork execve setsid pipe dup2 wait4 poll kill open read write close lseek getdents64
lstat64 fstat64 uname clock_gettime nanosleep ioctl(TCGETS/TCSETS)`, a
`/bin/sh`, and `/proc/meminfo`, `/proc/uptime`, `/proc/<pid>/status`.

mcpd protocol: one JSON object per line in each direction, e.g.
`{"id":1,"op":"run","cmd":"ls -l /","timeout_ms":5000}` answered by
`{"id":1,"ok":true,"exit":0,"stdout":"...","stderr":"...","timed_out":false,"truncated":false}`.
Other ops: `ping read_file write_file list_dir stat ps sysinfo`. On start it
sends `{"event":"ready","version":1}`. Failures are `{"id":1,"ok":false,"error":"open failed: No space left
on device","errno":28}`; numbers outside 32 bits are rejected, not clamped.

`run` starts the command in its own session (`setsid`), so on timeout `kill(-pgid, SIGKILL)`
takes down pipelines and their children. It returns when the shell exits, not when every writer
of its pipes has closed: a background job (`cmd &`) keeps running, but its output pipe is closed
after a short grace period. Output beyond 64 KiB per stream is read and discarded and the answer
carries `"truncated":true`.

File transfer: the guest's COM2 receive ring is 2 KiB and drops bytes when `mcpd` falls behind, so
`server.py` writes in requests of at most about 1.9 KB (`WIRE_LINE`) and checks each answer's
`written` count; `write_file` with `"more":true` keeps the file open for the next chunk. Reads
return up to 32 KiB per request (`READ_CHUNK`) and `save_to` loops until the end of the file. A
3 MiB file takes about 20 s up and 7 s down.

## Example session

```
> coal_boot
booted in 0.5s; mcpd is ready
> coal_run command="uname -a; ls /bin"
exit code: 0
--- stdout ---
Coal coal 0.2.0 #1 i386
> coal_write_file path="/tmp/hello.txt" content="hi\n"
wrote 3 bytes to /tmp/hello.txt
> coal_run command="cat /tmp/hello.txt"
> coal_kernel_log lines=20
> coal_shutdown
```

## Tests

```sh
python3 tools/coal-mcp/tests/test_server.py      # protocol and mcpd end-to-end on a pty
python3 tools/coal-mcp/tests/smoke_test.py DIR   # builds in DIR, boots in QEMU, runs commands, a 3 MiB transfer,
                                                 # network checks, snapshot/restore
```

Covers the MCP handshakes and tool list (a bug inside a tool is a tool error, only unknown
methods give -32601), and runs the real mcpd (built as a
32-bit static Linux binary with `userspace/mcpd/test/build_host.sh`) on a pty
bridged to a unix socket in place of COM2.

The disk image persists between boots and is not rebuilt by `coal_boot`: after changing user-space
sources (`mcpd`, `sh`, `coalbox`) run `coal_reset` to put the new binaries on a fresh image. The kernel
is rebuilt on every boot.
