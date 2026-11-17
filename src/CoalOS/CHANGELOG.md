# Changelog

## Unreleased

- User space: one list of system call numbers and one system call helper,
  shared by libc and the shell. Output from the shell and hello works again
  because they no longer use wrong numbers.
- libc: fixed `malloc` overflow, `strtol` end pointer, `lldiv`, `strncat`,
  `getcwd`, `getppid`, `brk`, `atexit`, `fprintf` and `stdin`.
- Shell: rewritten on top of libc; the parser no longer keeps pointers into
  freed stack memory and several buffer overflows are gone.
- `hello` is a hello world again; the old test code is now `libc_test` and
  `syscall_test`.
- Documentation reduced to the README, `docs/ARCHITECTURE.md` and
  `docs/syscalls.md`.

## 0.1.0 - 2025-01-30

First public version as a personal project: paging and allocators, ELF
loading, FAT16 with a VFS layer, drivers for keyboard, timer, serial and ATA
disk, a small shell. Boots with GRUB (Multiboot2).
