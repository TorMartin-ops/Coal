# Changelog

## Unreleased

- mcpd: every command runs in its own session and is killed as a group on timeout; `coal_run`
  returns when the shell exits (background jobs no longer hold it); unlimited output is drained and
  flagged as truncated; errors carry errno text; numbers are range-checked instead of clamped.
- File transfer: large `coal_write_file` requests were corrupted because the guest's COM2 receive
  ring (2 KiB) drops bytes; writes now use small requests verified against mcpd's byte count, and
  `coal_read_file` with `save_to` fetches the whole file.
- sh: variables (`$?`, `$VAR`) are expanded when each command runs, so `false; echo $?` prints 1.
- coal-mcp: a bug inside a tool is a tool error, not "Method not found"; Python 3.9+ everywhere;
  the server version is read from `VERSION`; guide and docs describe 512 MiB defaults and the network.
- CI: read-only token permissions, `actions/checkout` pinned by commit SHA, concurrency group.

## 0.2.0 - 2026-11-17

Coal OS is now a Linux i386 ABI compatible system that boots to a shell.

- System calls: one table with Linux i386 numbers and struct layouts; about 70
  calls including fork/execve/wait4, pipes, signals with handlers, poll, mmap2,
  getdents64, stat64, nanosleep, syslog.
- Kernel: O(1) scheduler with wait queues, copy-on-write fork, VMA tree, demand
  paging, per-process state in the PCB (cwd, signals, TLS), signal delivery on
  return from interrupts, system calls run with interrupts enabled.
- File systems: VFS with FAT16/32 (long names, utime), devfs, procfs, pipes.
- User space: libc, `coalbox` multi-call binary, `sh`, `init`, `mcpd`.
- Logging: one `klog` path with a 16 KiB ring behind `dmesg`; debug levels compile out.
- Build: kernel `-O2 -Werror`, zero warnings; CI runs the build, the system call
  number check, the coal-mcp tests and a QEMU smoke test.
- coal-mcp: MCP server so Claude can build, boot and drive Coal OS, with a system
  guide, snapshot/restore/reset and file transfer.
- Removed dead code: the old shell and hello images, MSR/CPUID helpers, the
  init result framework, debug scaffolding, the pid-keyed side tables.

## 0.1.0 - 2025-01-30

First public version as a personal project: paging and allocators, ELF
loading, FAT16 with a VFS layer, drivers for keyboard, timer, serial and ATA
disk, a small shell. Boots with GRUB (Multiboot2).
