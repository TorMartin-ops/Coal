# Changelog

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
