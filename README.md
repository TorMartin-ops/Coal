# Coal OS

A 32-bit x86 hobby operating system written from scratch in C and x86 assembly.

Coal OS started as a group project in IKT218 Advanced Operating Systems at the University of Agder (spring 2025). After the course I kept working on it alone, added my own libc and Linux-compatible system calls, and cleaned up the architecture.

## What it does

- **Boot:** Multiboot2, higher-half kernel at `0xC0100000`
- **Interrupts:** GDT, IDT, ISR/IRQ handling, PIT timer at 1000 Hz
- **Memory:** physical frame allocator, buddy allocator, slab allocator, paging and virtual memory
- **Processes:** ELF loader, user mode, preemptive priority scheduler, signals, process groups
- **System calls:** native calls plus a Linux i386-compatible dispatcher (`fork`, `exec`, `read`, `write`, `open`, `close` and more)
- **File system:** VFS layer and FAT16 with long file names, read and write, buffer cache
- **Drivers:** PS/2 keyboard, VGA text mode, serial port, IDE/ATA disk
- **User space:** own small libc and a shell with a command parser

## Project layout

```
src/CoalOS/
├── boot/        Multiboot2 entry and early setup
├── kernel/      cpu, memory, process, fs, drivers, lib
├── include/     kernel headers
├── userspace/   libc and shell
├── tests/       kernel test suite
├── scripts/     QEMU and disk image helpers
└── docs/        architecture and development notes
```

More detail is in [`src/CoalOS/docs/ARCHITECTURE.md`](src/CoalOS/docs/ARCHITECTURE.md).

## Building and running

The easiest way is the dev container in `.devcontainer/` (VS Code: "Reopen in Container"). It has the i686-elf cross compiler, NASM, CMake and QEMU.

```bash
mkdir -p build/CoalOS && cd build/CoalOS
cmake ../../src/CoalOS/
make
../../src/CoalOS/scripts/start_qemu.sh kernel.iso
```

Requirements outside the container: `i686-elf-gcc`, NASM, CMake 3.22+, QEMU, `xorriso` and `mtools`.

## Credits

- **Tor Martin Kohle** – most of the kernel, memory management, processes, file system, libc and shell
- **Bjarte Wik** – group member in IKT218
- **Per-Arne Andersen** – course template and build setup for IKT218 at UiA
- The [OSDev wiki](https://wiki.osdev.org) community

## License

MIT, see [LICENSE](LICENSE).
