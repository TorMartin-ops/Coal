# coal-mcp

An [MCP](https://modelcontextprotocol.io) server that lets Claude build, boot and
drive Coal OS. One Python file, standard library only (Python 3.8+).

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
(default `build/coal-mcp`), `COAL_QEMU_ARGS` adds extra QEMU arguments, and
`--attach unix:PATH|tcp:HOST:PORT` talks to an already running mcpd instead of
managing QEMU.

## Tools

| Tool | Purpose |
| --- | --- |
| `coal_build` | CMake configure and build; returns warning and error counts and diagnostics |
| `coal_boot` | Build if needed, start QEMU headless, wait until mcpd answers |
| `coal_shutdown`, `coal_reboot` | Stop or hard-reset the VM (disk is kept) |
| `coal_run` | `/bin/sh -c` a command; returns exit code, stdout, stderr (timeout, 64 KiB per stream) |
| `coal_read_file`, `coal_write_file` | Binary-safe file transfer; `save_to` / `host_path` read and write any host path the server user can access |
| `coal_guide` (also the resource `coal://guide`) | System guide: layout, commands, limits, what is destructive, how to recover |
| `coal_snapshot`, `coal_restore`, `coal_reset` | Save the disk under a name, put it back (reboots), or rebuild the pristine image (reboots) |
| `coal_ls`, `coal_ps`, `coal_sysinfo` | Directory listing, process list (`/proc`), uname, uptime, meminfo |
| `coal_kernel_log` | Tail of the COM1 serial log; first stop for boot failures and panics |
| `coal_screen` | The 80x25 VGA text screen as text |
| `coal_status` | Whether the VM is running |

The server speaks MCP 2026-07-28 (stateless, `server/discover`) and the
`initialize` handshake of 2025-11-25 and earlier.

## Trust model and safe use

The MCP client gets full control: commands run as root inside the VM (the VM is the sandbox, it
has no network), and `save_to` / `host_path` can read and write host files, so connect only clients
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
calls `fork execve pipe dup2 wait4 poll kill open read write lseek getdents64
lstat64 fstat64 uname clock_gettime nanosleep ioctl(TCGETS/TCSETS)`, a
`/bin/sh`, and `/proc/meminfo`, `/proc/uptime`, `/proc/<pid>/status`.

mcpd protocol: one JSON object per line in each direction, e.g.
`{"id":1,"op":"run","cmd":"ls -l /","timeout_ms":5000}` answered by
`{"id":1,"ok":true,"exit":0,"stdout":"...","stderr":"...","timed_out":false,"truncated":false}`.
Other ops: `ping read_file write_file list_dir stat ps sysinfo`. On start it
sends `{"event":"ready","version":1}`.

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
python3 tools/coal-mcp/tests/smoke_test.py       # builds, boots in QEMU, runs commands, snapshot/restore
```

Covers the MCP handshakes and tool list, and runs the real mcpd (built as a
32-bit static Linux binary with `userspace/mcpd/test/build_host.sh`) on a pty
bridged to a unix socket in place of COM2.
