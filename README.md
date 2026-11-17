# Coal OS

A small 32-bit x86 kernel that speaks the **Linux i386 system call ABI**. Static ELF32 binaries
built for Linux run on it unchanged. It boots to a shell in about half a second, the kernel image
is 173 KB stripped, and it ships with an MCP server so an AI assistant can build it, boot it and
drive it like a remote machine.

Coal started as a group project in IKT218 Advanced Operating Systems at the University of Agder
(spring 2025) and is now a one-person project. It is a hobby kernel: single CPU, no users, no
security boundary. See [SECURITY.md](SECURITY.md) and [Limitations](#limitations).

## Features

- **Kernel**: Multiboot2 boot through GRUB, O(1) preemptive priority scheduler, wait queues,
  paging with copy-on-write `fork`, demand-paged VMAs, frame/buddy/slab allocators, signals with
  user handlers, `poll`, TLS, a 16 KiB kernel log ring (`dmesg`).
- **File systems**: VFS with FAT16/32 (long file names) on ATA, devfs, procfs, pipes. Linux
  layout: `/bin /sbin /etc /home /root /tmp /dev /proc /usr /var`.
- **Network**: Intel e1000 driver, ARP, IPv4, ICMP, UDP, TCP, DHCP, DNS, and the Linux socket
  calls. `ping`, `nslookup`, `wget` (http), `nc` and `ifconfig` are applets.
- **System calls**: 86 Linux i386 calls with Linux numbers and struct layouts, one table
  ([docs/syscalls.md](src/CoalOS/docs/syscalls.md)). Unimplemented calls return `-ENOSYS`.
- **User space**: a small libc, `coalbox` (BusyBox-style multi-call binary with 65 applets, including `edit`, `top`, `find`, `diff`, `tr`, `printf`, `expr`),
  `sh` (POSIX-style: `if`/`while`/`for`/`case`, functions, `$(...)`, subshells, here-documents, `set -e`/`-x`, `trap`, `#!` scripts, history and Tab completion), `init`, and `mcpd`.
  `tools/coal-cc` compiles C programs for Coal on the host with Coal's libc.
- **MCP integration**: `tools/coal-mcp` lets Claude boot Coal in QEMU and run commands, transfer
  files, read the kernel log, take disk snapshots and roll back mistakes.
- **Quality gates**: the kernel builds with `-O2 -Werror` and zero warnings; CI runs the build,
  the syscall-number check against Linux headers, the coal-mcp tests and a QEMU smoke test.

## Numbers

Measured on one development machine (QEMU/TCG, no KVM, 512 MiB guest, the default of
`coal_boot`), at the commit of this README:

| | |
|---|---|
| Boot, QEMU start to `mcpd` answering | 0.4 - 0.5 s |
| `kernel.bin` text (code and read-only data) / data / bss | 151.5 KB / 8.8 KB / 100.9 KB |
| `kernel.bin` file size | 173 KB stripped, 747 KB with debug info |
| `coalbox` / `sh` / `init` / `mcpd` | 64.1 KB / 42.8 KB (38.4 KB of code and data) / 10.3 KB / 14.9 KB |
| Boot ISO (GRUB + kernel) / disk image | 5.9 MB / 32 MiB FAT16 (137 KB of files) |
| RAM in use after boot | 1.7 MB of 509 MiB (`/proc/meminfo`: MemTotal 521296 kB, MemFree 519620 kB) |
| Source size | about 11,100 lines of kernel C (15,300 with headers and assembly), 6,200 of user space |

1 KB is 1000 bytes. The boot time is what `coal_boot` reports: QEMU launch, GRUB, kernel,
`init` and `mcpd` up and answering a ping.

## Quick start

Needs CMake 3.22+, NASM, a 32-bit capable GCC (`gcc-multilib` or `i686-elf-gcc`), `grub-mkrescue`
with `xorriso` and `grub-pc-bin`, `mtools`, `dosfstools` and `qemu-system-i386`
(`.devcontainer/Dockerfile` installs them all).

```sh
cmake -S src/CoalOS -B build
cmake --build build                 # kernel, user programs, disk.img, kernel.iso
cmake --build build --target run    # QEMU window, serial log on the terminal
```

## Talk to it with Claude

`tools/coal-mcp/server.py` is an [MCP](https://modelcontextprotocol.io) server (Python 3.9+,
standard library only). It builds Coal, boots it headless in QEMU and talks to `/sbin/mcpd` inside
the VM over the second serial port. Claude gets 19 tools: boot, run a command, read and write
files, list directories and processes, kernel log, read the screen, type on the keyboard, snapshot
and restore.

```sh
claude mcp add coal -- python3 tools/coal-mcp/server.py
```

For Claude Desktop, add the same command to `claude_desktop_config.json`
([details](tools/coal-mcp/README.md)). Then ask in plain language:

- "Boot Coal OS, show me `uname -a` and how much memory is in use."
- "Take a snapshot, write a C program that prints its pid, build it on the host with
  `gcc -m32 -static`, upload it with `coal_write_file` (host_path) and run it in Coal."
- "Run `/usr/bin/syscall_test` and tell me if anything fails."
- "Start a web server on the host, fetch a page from it inside Coal with `wget` and show me the
  kernel log."
- "Download https://example.com/data.txt into /tmp with `coal_download` and count its lines in Coal."
- "I broke something: restore the snapshot."
- "Start `snake` on the console and play it." Claude reads the board with `coal_screen` and steers
  with `coal_keys`; in testing it reached 150 points before boxing itself in.

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
MemTotal:      521296 kB
MemFree:       519576 kB
> coal_snapshot name="clean"
> coal_run command="/usr/bin/syscall_test"
syscall_test: 43 checks, 0 failed
> coal_restore name="clean"          # undo everything since the snapshot
```

Commands run in their own process group and are killed as a group on timeout; a `cmd &` does not
keep the call waiting. Large files move in checked requests over a flow-controlled serial line. Trust model and details: [tools/coal-mcp/README.md](tools/coal-mcp/README.md).

## Networking

Coal has its own IPv4 stack: Ethernet, ARP, ICMP echo, UDP, TCP (in-order receive, retransmission
with backoff, 32 connections), a DHCP client that falls back to 10.0.2.15/24, and a resolver. The
guest gets an Intel e1000 on QEMU user networking, so it is behind NAT:

- Coal reaches the host at `10.0.2.2` and the internet if the host has it. The smoke test pings
  the gateway and fetches a file with `wget` from a web server on the host.
- Nothing reaches Coal unless QEMU forwards a port. Example: with
  `COAL_NIC="user,model=e1000,hostfwd=tcp:127.0.0.1:18080-:8080"` a host connection to port 18080
  reaches `nc -l 8080` inside Coal.
- Applets: `ifconfig`, `ping -c N host`, `nslookup name`, `wget [-O file] http://host/path`,
  `nc host port`, `nc -l port`.
- There is no TLS in the guest and `wget` refuses `https://`. For HTTPS use `coal_download`: the
  host fetches the URL and writes the file into Coal.

## Layout

```
src/CoalOS/boot, kernel, include     the kernel (core cpu drivers fs memory net process sync)
src/CoalOS/userspace                 libc, coalbox, sh, init, mcpd, tests
src/CoalOS/docs                      ARCHITECTURE.md, syscalls.md
src/CoalOS/rootfs                    files copied into the disk image
tools/coal-mcp                       MCP server and its tests
.github/workflows/ci.yml             CI
```

Start with [ARCHITECTURE.md](src/CoalOS/docs/ARCHITECTURE.md).

## Limitations

- One CPU (no SMP), no threads (`clone` is not implemented), no users or permissions: every
  process is root.
- Signals are delivered when a task returns to user mode, from a system call or an interrupt. There
  is no job control (`SIGSTOP` has no effect).
- The disk is a 32 MiB FAT16 image: no permissions, no symbolic links.
- `mmap2` supports anonymous private mappings only, no file-backed mappings. Programs must be
  static ELF32 executables; there is no dynamic loader.
- Network: IPv4 only, no TLS, no loopback interface, no IP fragment reassembly, no DHCP lease
  renewal, TCP drops out-of-order segments.
- The shell has no subshells, `$!` or command substitution.
- File data and metadata are written to disk on `sync`, `fsync`, unmount and by a flush thread every
  5 s; a hard reset can lose the last few seconds of changes.
- `mount` attaches extra FAT disks (`mount -t fat /dev/hdb /mnt`); the root disk is `hda`.
- Interrupted blocking calls return `EINTR` (no `SA_RESTART`).
- Hobby quality: not audited and not a security boundary.

## Credits

- Tor Martin Kohle: kernel, memory management, processes, file system, libc, shell, MCP integration
- Bjarte Wik: group member in IKT218
- Per-Arne Andersen: course template and build setup for IKT218 at UiA
- The [OSDev wiki](https://wiki.osdev.org) community

## License

MIT, see [LICENSE](LICENSE).
