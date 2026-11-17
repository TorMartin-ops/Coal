# Architecture

Coal OS is a small monolithic kernel for 32-bit x86 (i386). It boots through
GRUB using the Multiboot2 protocol, runs on one CPU, and starts two user
programs from a FAT16 disk. This document describes what the code does
today. Where something is a stub it says so.

## Boot

1. GRUB loads `kernel.bin` (an ELF file, see `boot/grub.cfg`) and jumps to
   `_start` in `boot/multiboot2.asm` in 32-bit protected mode.
2. `_start` sets up a 64 KiB stack, pushes the Multiboot2 magic and the
   physical address of the info structure and calls `main()` in
   `kernel/core/kernel.c`.
3. `main()` calls `kernel_main_init()` (`kernel/core/init.c`), which runs
   these steps in order and stops if one fails:
   serial port, VGA terminal and console device; Multiboot2 check;
   GDT, memory (paging, frame allocator, buddy, slab, `kmalloc`) and IDT;
   PIT timer (1000 Hz) and scheduler; keyboard; disk and file system;
   launch of the initial user programs; keyboard controller setup;
   system call table.

## Memory layout

The kernel is linked at physical address 1 MiB (`src/arch/i386/linker.ld`)
and starts executing there. Early paging (`kernel/memory/paging_early.c`)
builds the first page directory with:

- an identity mapping of the first 4 MiB, so the kernel keeps running at its
  link address;
- the kernel image, the early heap and the VGA buffer also mapped at
  `0xC0000000` and up (`KERNEL_VIRT_BASE` in `include/kernel/core/constants.h`).

The kernel code itself is not linked to run at `0xC0000000`; the higher half
mapping is an alias of the same physical pages. A new process page directory
also copies the identity mapping of the first 4 MiB, because the kernel still
runs from there (`kernel/memory/paging_process.c`).

User space is everything below `0xC0000000`. User programs are linked at
`0x08048000` (`src/arch/i386/user.ld`); the user stack top is
`0xBFFF0000`.

Allocators, from the bottom up:

| Layer | File | Purpose |
|---|---|---|
| Frame allocator | `kernel/memory/frame.c` | Physical 4 KiB frames |
| Buddy allocator | `kernel/memory/buddy*.c` | Power-of-two blocks of frames |
| Slab allocator | `kernel/memory/slab.c` | Small fixed-size kernel objects |
| `kmalloc` | `kernel/memory/kmalloc.c` | General kernel heap |

## Processes and scheduling

- A process is a `pcb_t` (`include/kernel/process/process.h`) with its own
  page directory, a file descriptor table of 16 entries and a kernel stack.
- The ELF loader (`kernel/process/elf_loader.c`) maps the segments of a
  32-bit executable and `jump_user_mode.asm` enters ring 3.
- The scheduler (`kernel/process/scheduler_*.c`) is preemptive and driven by
  the PIT. It has four priority levels with time slices of 200, 100, 50
  and 25 ms (level 0 is the kernel, level 3 the idle task).
- `fork`, `execve` and `waitpid` are stubs at the moment, so a user program
  cannot yet start another one.

At the end of boot the kernel starts `/hello.elf` and `/bin/shell.elf` from
the disk image (`kernel/core/kernel.c`).

## System calls

User programs enter the kernel with `int 0x80`: number in EAX, arguments in
EBX, ECX, EDX, result in EAX (negative values are negated error numbers).
The numbers follow Linux i386 where a call exists there; Coal OS adds
`SYS_PUTS` (7000) and `SYS_READ_TERMINAL_LINE` (7001). The dispatcher is in
`kernel/cpu/syscall_dispatcher.c`. See [syscalls.md](syscalls.md) for the
list and which calls are implemented.

## File system

- `kernel/fs/vfs/` is a small VFS: mount table, vnodes, file operations and
  a page cache.
- `kernel/fs/fat/` is the FAT16 driver, including long file names.
- `kernel/drivers/block/ata_driver.c` and `kernel/drivers/storage/` provide
  the ATA disk, the block device layer and a buffer cache.

The disk image built by CMake is a 32 MiB FAT16 volume containing
`/hello.elf`, `/bin/shell.elf`, `/bin/libc_test.elf` and
`/bin/syscall_test.elf`.

## Drivers

| Driver | File |
|---|---|
| VGA text mode terminal | `kernel/drivers/display/` |
| Serial port (COM1, used for logs) | `kernel/drivers/display/serial.c` |
| PS/2 keyboard | `kernel/drivers/input/` |
| PIT timer | `kernel/drivers/timer/pit.c` |
| ATA disk | `kernel/drivers/block/ata_driver.c` |

## User space

- `userspace/libc/` is a small C library: string and memory functions,
  `printf` family, `malloc`, environment, and thin system call wrappers.
  Functions return the kernel result unchanged; there is no `errno`.
- `include/libc/syscall.h` holds the single `int 0x80` helper and
  `include/libc/syscall_numbers.h` the numbers.
  `scripts/check_syscall_numbers.sh` checks them against the kernel header.
- `userspace/entry.asm` is `_start`: it calls `main(0, NULL)` and then
  `exit()`.
- `userspace/shell/` is the shell (parser, built-ins, environment).
- `userspace/hello/` is hello world; `userspace/tests/` holds `libc_test`
  and `syscall_test`.

## Source layout

```
boot/            Multiboot2 entry
kernel/          core, cpu, memory, process, fs, drivers, hal, lib, sync
include/kernel/  kernel headers
include/libc/    headers shared by the kernel and user programs
userspace/       libc, shell, hello, tests, entry.asm
scripts/         QEMU launcher, syscall number check
src/arch/i386/   linker scripts
```
