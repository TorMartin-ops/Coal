# Architecture

Coal OS is a small monolithic kernel for 32-bit x86 that runs static Linux i386
binaries. One CPU, no SMP. About 11,000 lines of kernel C (15,000 with headers and assembly),
including an IPv4 stack.

## Boot

1. GRUB loads `kernel.bin` (Multiboot2, linked at 1 MiB, `src/arch/i386/linker.ld`)
   and jumps to `_start` in `boot/multiboot2.asm`, which calls `main()`.
2. `kernel_main_init()` (`kernel/core/init.c`) runs straight through: serial ports,
   VGA terminal, GDT, memory (the largest free Multiboot region becomes the
   heap), IDT, PIT (1000 Hz), scheduler, keyboard, file systems, then creates
   `/sbin/init`, sets up the keyboard controller, enables interrupts and starts
   the scheduler. A failure before that point panics.
3. `/sbin/init` (PID 1, `userspace/init`) starts `/bin/sh` on the console and
   `/sbin/mcpd` (the MCP agent) and restarts them when they exit.

## Memory

- Kernel at 1 MiB, identity-mapped low 4 MiB (shared by every page directory)
  and a direct map of physical memory at `0xC0000000`; kernel stacks live in
  `0xE0000000..0xF0000000` (`include/kernel/memory/paging.h`).
- User space is `0x00400000..0xC0000000`: programs link at `0x08048000`
  (`user.ld`), the stack top is `0xBFFF0000`.
- Frame allocator with reference counts (`frame.c`), buddy allocator (`buddy.c`),
  slab caches for 16 B - 1 KiB objects (`slab.c`), `kmalloc` on top.
- Per-process `mm_struct` with a red-black tree of VMAs (`mm.c`), demand-paged
  anonymous memory, copy-on-write fork, `brk`, anonymous `mmap2`.
- User pointers are accessed through `uaccess.asm` with an exception table
  (`kernel/cpu/exception_table.c`): a fault inside a copy returns `-EFAULT`.

## Processes and scheduling

- `pcb_t` (`include/kernel/process/process.h`) holds ids, the address space, 32
  descriptors, cwd, close-on-exec bits, signal state and the TLS descriptor.
  Everything per process lives in the PCB; fork copies it, execve resets it.
- O(1) scheduler (`scheduler.c`): four priority levels with a bitmap of non-empty
  FIFO queues, 20 ms slices, a sorted sleep list. System calls run with
  interrupts on, so a task in the kernel can be preempted.
- Blocking uses wait queues (`drivers/wait.c`): tty, serial, pipes and `poll`/`select`
  sleep until a driver wakes them. `poll` asks each driver for readiness. A restartable call
  interrupted by a `SA_RESTART` handler is restarted by rewinding `eip` over `int 0x80`.
- The x87/SSE registers are switched lazily (`fpu.c`): CR0.TS is set when the running task is not
  the owner and the `#NM` trap swaps FXSAVE images.
- Out of memory (`oom.c`): a fault that finds no frame kills the process with the most resident
  pages (never init), logs one line and lets the faulting task retry.
- `fork`, `execve` (`exec.c`, `elf_loader.c`), `wait4`, sessions and process groups
  follow Linux. Signals: default actions in `signal.c`, user handlers on an rt
  signal frame built on return from system calls and interrupts
  (`syscall_signal.c`).

## System calls

One table indexed by the Linux i386 number (`kernel/cpu/syscall.c`); handlers are
grouped in `syscall_fs.c`, `syscall_proc.c`, `syscall_mm.c`, `syscall_signal.c`,
`syscall_misc.c`, `syscall_user.c` (user-space path and argument copying).
[syscalls.md](syscalls.md) lists them.

## File systems

`kernel/fs/vfs` is the VFS: mount table, path normalisation, file API and a
driver interface with optional `poll`/`utime`/`ioctl` operations. Drivers:

- FAT16/32 with long file names on the ATA disk (`kernel/fs/fat`), through a
  buffer cache (`kernel/drivers/storage`). Mounted on `/`.
- devfs on `/dev`: `console`, `tty`, `null`, `zero`, `ttyS0` (kernel log), `ttyS1`
  (MCP link).
- procfs on `/proc`: `meminfo`, `uptime`, `version`, `mounts`, `<pid>/status`, `self`.
- pipefs: pipes and socket pairs (`pipe`, `pipe2`, `socketpair`), a 64 KiB ring of lazily
  allocated pages per direction.

## Drivers

VGA text console with a VT100 subset (`terminal.c`; o-slash glyphs are uploaded into the VGA font
and UTF-8 for U+00A0..U+00FF is mapped to Latin-1/CP437), line discipline with
canonical/raw modes and job-control signal keys (`tty.c`), PS/2 keyboard with a
Norwegian keymap, lock-key LEDs and an ESC prefix for Alt, 16550 UART with interrupt-driven rings (`serial.c`), PIT, ATA
PIO with LBA48 (`block_device.c`). PCI bus 0 scan (`pci.c`) and an Intel e1000
(82540EM) NIC with MMIO at `KERNEL_MMIO_VADDR_START`, 32 RX and 16 TX
descriptors, interrupt-driven RX with a 10 ms poll fallback (`net/e1000.c`).

## Network

`kernel/net`: Ethernet, ARP cache, IPv4 (fragments dropped), ICMP echo and port unreachable,
UDP, a DHCP client with renewal (T1 unicast, T2 broadcast) and static fallback (10.0.2.15/24),
and TCP (RTO with backoff, slow start, a bounded out-of-order queue of four ranges kept in the
receive ring, 16 KiB buffers, 64 connections, listen backlog 32 that drops the oldest half-open
connection when full, MSS clamped to [536, MTU-40]). Packets to 127.0.0.0/8 or the own address
go through a 16-packet loopback queue delivered by the millisecond tick, so local sockets work
without a NIC. `socket.c` exposes the protocols as fds through a sockfs vfs driver;
`syscall_socket.c` implements the Linux socket calls. No IPv6 or fragment reassembly.

## Kernel log

`klog`/`LOG_*` (`kernel/core/log.c`) formats once and sends the line to the
screen, COM1 and a 16 KiB ring read by `syslog(2)` (`dmesg`). Levels below
`KLOG_LEVEL` (default INFO) are compiled out.

## User space

- `userspace/libc`: a small libc (stdio, stdlib, string, getopt, time, system call
  wrappers), no libgcc dependency.
- `userspace/coalbox`: BusyBox-style multi-call binary, installed as `/bin/coalbox`;
  `sh` finds the applets (cat, ls, cp, grep, ps, ...) through it.
- `userspace/sh`: shell with pipes, redirection, `&&`/`||`, background jobs, history.
- `userspace/init`, `userspace/mcpd`, and tests in `userspace/tests`.

## MCP integration

`tools/coal-mcp/server.py` (host) boots the image in QEMU and talks JSON lines
over COM2 to `/sbin/mcpd`, which runs commands and transfers files. See
`tools/coal-mcp/README.md`. The UART receive ring is 2 KiB and drops bytes when full (no flow
control), so the host keeps each request line below that size.

## Source layout

```
boot/            Multiboot2 entry
kernel/          core cpu drivers fs lib memory process sync
include/kernel/  kernel headers        include/libc/  headers shared with user space
userspace/       libc coalbox sh init mcpd tests hello
rootfs/etc/      files copied into the disk image
scripts/         QEMU launcher, system call number check
src/arch/i386/   linker scripts
```
