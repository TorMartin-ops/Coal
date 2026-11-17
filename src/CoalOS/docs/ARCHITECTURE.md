# Architecture

Coal OS is a small monolithic kernel for x86_64 that runs static Linux x86_64
binaries (ELF64, `ET_EXEC` and static PIE). One CPU, no SMP. About 11,000 lines of kernel C (15,000 with headers and assembly),
including an IPv4 stack.

## Boot

1. Two loaders reach one entry in `boot/boot.asm`, which runs in 32-bit protected mode: GRUB with
   Multiboot2 (`boot/grub.cfg`, magic in EAX, information in EBX) and a hypervisor that boots
   an ELF directly with the Xen PVH note (`qemu-system-x86_64 -kernel kernel.bin`, `hvm_start_info`
   in EBX). The trampoline checks for long mode and NX, builds boot page tables (identity and
   direct map of the low 4 GiB with 2 MiB pages, kernel image at -2 GiB), enables PAE, EFER.LME and
   NXE, and jumps to 64-bit code at `kernel_entry`, which calls `main(magic, info)`.
   The image is linked by `src/arch/x86_64/linker.ld`: the trampoline at its physical address,
   the rest at `0xFFFFFFFF80000000` (`-mcmodel=kernel`).
2. `kernel_main_init()` (`kernel/core/init.c`) runs straight through: serial ports,
   VGA terminal, CPU setup (GDT, TSS with IST stacks, per-CPU data in GS, SMEP, SMAP, FSGSBASE, SYSCALL
   MSRs), the memory map (`boot_info.c` parses the Multiboot2 map or the PVH map; the kernel image and
   the information structure are carved out), memory, IDT, timers (TSC clock, local APIC or PIT clock
   event), the vDSO, FPU, scheduler, keyboard, file systems, then creates
   `/sbin/init`, enables interrupts and starts the scheduler. A failure before that point panics.
3. `/sbin/init` (PID 1, `userspace/init`) starts `/bin/sh` on the console and
   `/sbin/agentd` (the host agent) and restarts them when they exit.

## Memory

- Four-level paging. The layout is documented in `include/kernel/memory/paging.h`: user space is
  below 2^47 (`0x10000..0x7FFFFFFFF000`), all physical memory is mapped at `0xFFFF800000000000` with
  2 MiB pages and no execute, kernel stacks (guard page plus stack per task) start at
  `0xFFFFC00000000000`, the kernel image is at `0xFFFFFFFF80000000` (text read-only and executable, data
  not executable), device registers are mapped uncached by `ioremap()`. All kernel PML4 entries exist from
  boot, so a new address space copies them and stays in sync. There is no highmem and no temporary
  mapping: every frame is reachable through the direct map.
- Static executables link at `0x400000` (`user.ld`), static PIE loads at `0x555555554000`, the stack
  (8 MiB limit) ends at `0x7FFFFFFFD000`, below it the vvar page and the vDSO; `mmap` allocates
  downwards from `0x7F0000000000`. Data pages are never executable.
- The buddy allocator has two zones: below 4 GiB (DMA-capable, used for descriptor rings and
  PRD tables) and above. Frame allocator with reference counts (`frame.c`), buddy allocator
  (`buddy.c`), slab caches for 16 B - 1 KiB objects (`slab.c`), `kmalloc` on top.
- Per-process `mm_struct` with a red-black tree of VMAs (`mm.c`), demand-paged
  anonymous memory, copy-on-write fork, `brk`, anonymous `mmap`, `mprotect` (splits VMAs;
  `PROT_NONE` is a present page without the user bit) and `madvise` (accepted, no effect).
- User pointers are accessed through `uaccess.asm` with an exception table
  (`kernel/cpu/exception_table.c`): a fault inside a copy returns `-EFAULT`. SMAP is enforced:
  the copy routines bracket their accesses with `stac`/`clac`, any other kernel touch of user memory
  faults. A kernel-mode fault without a fixup panics (`kernel_fault_panic`).

## CPU state

`struct percpu` (`include/kernel/cpu/percpu.h`) is reached through GS in kernel mode: self pointer,
the kernel stack top that SYSCALL loads, the scratch slot for the user stack pointer, the current
task, and the GDT, TSS and IST stacks (`#DF`, NMI, `#MC`). Entry stubs `swapgs` when they come from
ring 3; NMI and `#DF` use the paranoid variant that tests the GS base instead. The user FS and GS
bases are saved per process and loaded on switch (`arch_prctl`, WRFSBASE/WRGSBASE when available).
Interrupts, exceptions and SYSCALL all build the same 176-byte `isr_frame_t` (`include/kernel/cpu/isr_frame.h`),
which fork, execve and signal delivery read and rewrite; the syscall frame is at the top of the kernel
stack, so a handler finds it with `user_regs()`.

## Processes and scheduling

- `pcb_t` (`include/kernel/process/process.h`) holds ids, the address space, 32
  descriptors, cwd, close-on-exec bits, signal state and the user FS/GS bases.
  Everything per process lives in the PCB; fork copies it, execve resets it.
- EEVDF scheduler (`scheduler.c`): Linux nice weights, eligibility by lag, earliest virtual
  deadline first, two red-black trees, 3 ms slices (0.75 ms for tasks that sleep a lot). A running
  task keeps the CPU until its slice ends. System calls run with interrupts on, so a task in the
  kernel can be preempted.
- Timers (`drivers/timer/`): `clock_ns()` counts the TSC (calibrated against the PIT). All kernel
  timers live in one hierarchical timer wheel (`timer.c`, millisecond resolution). One clock event
  device is armed for the earliest deadline only: TSC-deadline or one-shot local APIC timer
  (`lapic.c`), else the PIT in one-shot mode, else (no TSC) a 1 kHz periodic PIT. An idle system
  takes almost no timer interrupts; `/proc/interrupts` shows the counts.
- Blocking uses wait queues (`drivers/wait.c`): tty, serial, pipes and `poll`/`select`
  sleep until a driver wakes them. `poll` asks each driver for readiness. A restartable call
  interrupted by a `SA_RESTART` handler is restarted by rewinding `rip` over the 2-byte `syscall`.
- The x87 and SSE registers are switched lazily (`fpu.c`): CR0.TS is set when the running task is
  not the owner and the `#NM` trap swaps FXSAVE64 images. A new process starts from a clean image.
  The kernel itself is built with `-mgeneral-regs-only`.
- Out of memory (`oom.c`): a fault that finds no frame kills the process with the most resident
  pages (never init), logs one line and lets the faulting task retry.
- `fork`, `execve` (`exec.c`, `elf_loader.c`), `wait4`, sessions and process groups
  follow Linux. Signals use the Linux layout (mask bit n-1, 64 signals): default actions in
  `signal.c`, user handlers on an x86_64 `rt_sigframe` (ucontext, siginfo, FXSAVE image, 128-byte
  red zone skipped, `sigaltstack` honoured) built on return from system calls and interrupts
  (`syscall_signal.c`). `rt_sigaction` requires `SA_RESTORER`, as the kernel has no restorer of its own.

## System calls

One table indexed by the Linux x86_64 number (`kernel/cpu/syscall.c`, numbers in
`include/libc/syscall_numbers.h`, checked by `scripts/check_syscall_numbers.sh`); handlers are
grouped in `syscall_fs.c`, `syscall_proc.c`, `syscall_mm.c`, `syscall_signal.c`,
`syscall_misc.c`, `syscall_poll.c`, `syscall_socket.c`, `syscall_user.c` (user-space path and
argument copying). [syscalls.md](syscalls.md) lists them.

The entry is SYSCALL (`kernel/cpu/syscall.asm`): the stub swaps GS and stacks by hand, builds the frame,
calls the dispatcher with `rdi rsi rdx r10 r8 r9` and returns with SYSRET unless the frame asks for IRET
(non-canonical or kernel `rip`, single-stepping, a changed code segment). `FMASK` clears IF, DF, TF, IOPL,
NT, RF and AC. There is no `int 0x80`, no sysenter and no socketcall. Every process maps a read-only
seqlock time page and the vDSO at the top of user space (`kernel/process/vdso.c`,
`include/kernel/cpu/vdso_abi.h`); exec announces the vDSO through AT_SYSINFO_EHDR (with the rest of the
x86_64 auxiliary vector: PHDR, PHENT, PHNUM, PAGESZ, ENTRY, RANDOM, HWCAP, HWCAP2, EXECFN, PLATFORM,
MINSIGSTKSZ). The vDSO exports `__vdso_clock_gettime`, `__vdso_gettimeofday`, `__vdso_time` and
`__vdso_getcpu` (version `LINUX_2.6`). The clock is the TSC calibrated against the PIT
(`kernel/drivers/timer/clock.c`).

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
with LBA48 and PIIX bus-master DMA (`block_device.c`; the PRD table and buffers sit below 4 GiB). PCI bus 0 scan (`pci.c`) and an Intel e1000
(82540EM) NIC with MMIO mapped through `ioremap()`, 32 RX and 16 TX
64-bit descriptors, interrupt-driven RX with a 10 ms poll fallback (`net/e1000.c`).

virtio (`kernel/drivers/virtio/`, details in `docs/agent-vm/virtio.md`): a core with modern-only feature
negotiation, split virtqueues, PCI and MMIO (`virtio_mmio.device=` on the command line) transports, and
block (`vda`, selectable with `root=`), console (`/dev/hvc0`), rng and vsock drivers. `AF_VSOCK` stream
sockets live in `kernel/net/vsock.c`. DMA memory and MMIO mappings come from `memory/dma.c`.

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
- `userspace/init`, `userspace/agentd`, and tests in `userspace/tests`.

## MCP integration

`tools/coal-gateway` (host) boots the image in QEMU and talks length-prefixed JSON frames over COM2 to
`/sbin/agentd`, which runs commands, manages background jobs with resource limits and transfers
files; the MCP front ends are `tools/coal-mcp/server.py` (stdio) and `tools/coal-gateway/gateway.py`
(stdio or HTTP). The protocol is in `docs/agent-vm/protocol.md`, the gateway in
`docs/agent-vm/gateway.md`. The UART holds the sender back when the 8 KiB receive ring is full, so the
link is lossless and frames of up to 256 KiB need no host-side pacing.

## Source layout

```
boot/            Multiboot2 and PVH entry, GRUB configuration
kernel/          core cpu drivers fs lib memory process sync
include/kernel/  kernel headers        include/libc/  headers shared with user space
userspace/       libc coalbox sh init agentd tests hello
rootfs/etc/      files copied into the disk image
scripts/         QEMU launcher, system call number check
src/arch/x86_64/ linker scripts
```
