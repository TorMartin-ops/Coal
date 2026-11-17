# Coal OS

A 32-bit x86 hobby operating system written in C and assembly. It started as a
group project in IKT218 Advanced Operating Systems at the University of Agder
(spring 2025) and is now maintained by one person.

## What exists

- Boot through GRUB with Multiboot2; the kernel is linked at 1 MiB
- GDT, IDT, PIT timer at 1000 Hz, PS/2 keyboard, VGA text mode, serial port
- Memory: frame allocator, buddy and slab allocators, paging
- Processes: ELF loader, user mode, preemptive priority scheduler
- System calls through `int 0x80`, numbered like Linux i386
- VFS with a FAT16 driver on an ATA disk
- User space: a small libc, a shell, a hello world program and two test programs

Not finished: `fork`, `execve`, `waitpid`, pipes and several other system
calls are stubs, so the shell cannot start other programs yet. See
[src/CoalOS/docs/syscalls.md](src/CoalOS/docs/syscalls.md).

## Build and run

Needed: CMake 3.22 or newer, NASM, a C compiler for 32-bit x86
(`i686-elf-gcc`, or `gcc` with 32-bit support, for example `gcc-multilib`),
`grub-mkrescue` with `xorriso` and `grub-pc-bin`, `mtools`, `dosfstools` and
`qemu-system-i386`. `.devcontainer/Dockerfile` installs all of them, with an
`i686-elf` cross compiler.

```bash
cmake -S src/CoalOS -B build/CoalOS
cmake --build build/CoalOS            # builds kernel.bin, user programs, disk.img, kernel.iso
cmake --build build/CoalOS --target run   # starts QEMU, serial output in the terminal
```

Targets: `kernel`, `hello`, `shell`, `libc_test`, `syscall_test`, `image`
(ISO and disk image), `run`.

To debug with gdb, start QEMU with
`src/CoalOS/scripts/start_qemu.sh --debug build/CoalOS/kernel.iso` and
attach with the "Qemu Debug" launch configuration in VS Code.

`libc_test` is built for the same 32-bit ABI as Linux i386, so on a Linux
machine that can run 32-bit programs it can also be run directly:
`build/CoalOS/libc_test.elf`.

## Layout

See [src/CoalOS/docs/ARCHITECTURE.md](src/CoalOS/docs/ARCHITECTURE.md).

## Credits

- Tor Martin Kohle: kernel, memory management, processes, file system, libc, shell
- Bjarte Wik: group member in IKT218
- Per-Arne Andersen: course template and build setup for IKT218 at UiA
- The [OSDev wiki](https://wiki.osdev.org) community

## License

MIT, see [LICENSE](LICENSE).
