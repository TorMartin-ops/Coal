# Coal OS

An ultra-light 32-bit x86 kernel that speaks the **Linux i386 system call ABI**. Static ELF32
binaries built for Linux run on it unchanged. It boots to a shell in about half a second, fits in
a 140 KB kernel image, and ships with an MCP server so an AI assistant can build it, boot it and
drive it like a remote machine.

Coal started as a group project in IKT218 Advanced Operating Systems at the University of Agder
(spring 2025) and is now a one-person project. It is a hobby kernel: single CPU, no network, no
users. See [SECURITY.md](SECURITY.md).

## Features

- **Kernel**: Multiboot2 boot through GRUB, O(1) preemptive priority scheduler, wait queues,
  paging with copy-on-write `fork`, demand-paged VMAs, frame/buddy/slab allocators, signals with
  user handlers, `poll`, TLS, a 16 KiB kernel log ring (`dmesg`).
- **File systems**: VFS with FAT16/32 (long file names) on ATA, devfs, procfs, pipes. Linux
  layout: `/bin /sbin /etc /home /root /tmp /dev /proc /usr /var`.
- **System calls**: about 70 Linux i386 calls with Linux numbers and struct layouts, one table
  ([docs/syscalls.md](src/CoalOS/docs/syscalls.md)). Unimplemented calls return `-ENOSYS`.
- **User space**: a small libc, `coalbox` (BusyBox-style multi-call binary with 37 applets),
  `sh` (pipes, redirection, `&&`, `||`, background jobs, history), `init`, and `mcpd`.
- **MCP integration**: `tools/coal-mcp` lets Claude boot Coal in QEMU and run commands, transfer
  files, read the kernel log, take disk snapshots and roll back mistakes.
- **Quality gates**: the kernel builds with `-O2 -Werror` and zero warnings; CI runs the build,
  the syscall-number check against Linux headers, unit tests and a QEMU smoke test.

## Numbers

Measured on one development machine (QEMU/TCG, no KVM, 128 MB guest):

| | |
|---|---|
| Boot, QEMU start to `mcpd` answering | 0.4 - 0.5 s |
| Kernel start to first user process | 13 ms |
| `kernel.bin` text / data / bss | 118.5 KB / 8.7 KB / 93 KB (stripped file 140 KB, with debug info 592 KB) |
| `coalbox` / `sh` / `init` / `mcpd` | 29.7 KB / 20.0 KB / 9.7 KB / 13.7 KB |
| Boot ISO (GRUB + kernel) / disk image | 5.7 MB / 32 MiB FAT16 (115 KB used) |
| RAM in use after boot | 1.6 MB of 128 MB |
| Source size | about 9,000 lines of kernel, 4,400 of user space |

## Quick start

Needs CMake 3.22+, NASM, a 32-bit capable GCC (`gcc-multilib` or `i686-elf-gcc`), `grub-mkrescue`
with `xorriso` and `grub-pc-bin`, `mtools`, `dosfstools` and `qemu-system-i386`
(`.devcontainer/Dockerfile` installs them all).

```sh
cmake -S src/CoalOS -B build
cmake --build build                 # kernel, user programs, disk.img, kernel.iso
cmake --build build --target run    # QEMU window, serial log on the terminal
```

### Let Claude drive it

```sh
claude mcp add coal -- python3 tools/coal-mcp/server.py
```

A session:

```
> coal_boot
built OS; booted in 0.5s; mcpd is ready
> coal_run command="uname -a; ls /bin; cat /proc/meminfo | head -2"
exit code: 0
--- stdout ---
Coal coal 0.2.0 #1 i386
coalbox
sh
MemTotal:      129268 kB
MemFree:       127632 kB
> coal_snapshot name="clean"
> coal_run command="/usr/bin/syscall_test"
syscall_test: 43 checks, 0 failed
> coal_restore name="clean"          # undo everything since the snapshot
```

Details, tools and the trust model: [tools/coal-mcp/README.md](tools/coal-mcp/README.md).

## Layout

```
src/CoalOS/boot, kernel, include     the kernel (core cpu drivers fs memory process sync)
src/CoalOS/userspace                 libc, coalbox, sh, init, mcpd, tests
src/CoalOS/docs                      ARCHITECTURE.md, syscalls.md
src/CoalOS/rootfs                    files copied into the disk image
tools/coal-mcp                       MCP server and its tests
.github/workflows/ci.yml             CI
```

Start with [ARCHITECTURE.md](src/CoalOS/docs/ARCHITECTURE.md).

## Known limitations

No network, threads, symbolic links, file-backed `mmap`, SMP or job control (`SIGSTOP` is ignored).
The shell has no subshells, `$!` or command substitution. Output sent to the screen is not
logged to the serial port. The disk is FAT16 with no permissions.

## Credits

- Tor Martin Kohle: kernel, memory management, processes, file system, libc, shell, MCP integration
- Bjarte Wik: group member in IKT218
- Per-Arne Andersen: course template and build setup for IKT218 at UiA
- The [OSDev wiki](https://wiki.osdev.org) community

## License

MIT, see [LICENSE](LICENSE).
