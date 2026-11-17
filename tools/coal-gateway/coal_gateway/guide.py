"""Texts the gateway shows to the client: server instructions and the Coal OS system guide."""

GUIDE_URI = "coal://guide"
INSTRUCTIONS = (
    "Drives a Coal OS virtual machine (a small Linux-compatible i386 OS) in QEMU. You have full control: "
    "commands run as root inside the VM, which is the sandbox. Call coal_guide once for the system guide "
    "(layout, commands, limits). Start with coal_boot, then use coal_run, coal_read_file, coal_write_file, "
    "coal_ls, coal_ps, coal_sysinfo; coal_spawn starts a long command as a job you poll with coal_job_status and coal_job_output; coal_kernel_log and coal_screen show what the kernel printed; coal_keys types on the console "
    "for interactive programs such as /usr/bin/snake. "
    "Changes persist on the disk image across reboots. Before risky changes run coal_snapshot; "
    "coal_restore or coal_reset undo any mistake. Never overwrite /sbin/init, /sbin/agentd or /bin/sh "
    "without a snapshot: agentd and the shell are how you reach the VM. Call coal_shutdown when finished.")

GUIDE = """Coal OS system guide

WHAT IT IS
A small Linux-ABI-compatible i386 kernel (static ELF32 Linux binaries run), single CPU, an e1000 NIC on
QEMU user networking (DHCP), no SMP, no users (everything is root). Boot to agentd takes about half a second;
512 MiB RAM by default, about 1.7 MB in use after boot.

FILE SYSTEM (FAT16 disk image, persistent; long file names, no symlinks, no permissions enforcement)
  /bin/coalbox   one multi-call binary; applets: [ basename cat chmod clear cmp cp cut date df diff dirname dmesg
                 du echo edit env expr false find fold free grep halt head hexdump ifconfig kill ln ls mkdir mount mv
                 nc nl nslookup ping poweroff printf ps pwd rev reboot rm rmdir seq sleep sort stat sync tail tee
                 test top touch tr true umount uname uptime wc wget which xxd yes
                 edit FILE is a full-screen editor (arrows, ^S save, ^Q quit, ^F find) you can drive with coal_keys;
                 top refreshes ps every second (q quits); nc -w SECS gives up when idle; chmod and ln -s report
                 when the kernel or FAT cannot do them; df needs statfs or /proc/mounts
  /bin/sh        a POSIX-style shell: if/elif/else, while/until, for, case, functions, $(...) and backticks, ( )
                 subshells, { }, pipes, redirections (including 2>&1 and here-documents), && || ; &, $! $? $# $@ $1..$9,
                 ${var:-x} ${#v} ${v%x} ${v#x}, $((...)), shift, set -e/-x, trap, read, ., test/[; scripts starting
                 with #! run through their interpreter. No job control, no aliases, no arrays
  /sbin/init     PID 1: starts agentd and a login shell
  /sbin/agentd     the agent that serves this MCP server over COM2
  /usr/bin       test programs: hello, libc_test, syscall_test, abi_test, and the self-checking scripts
                 sh_test.sh and applet_test.sh (run with /bin/sh /usr/bin/sh_test.sh)
  /etc /home /root /tmp /var   ordinary directories;   /dev  console, null, zero (devfs)
  /proc          meminfo, uptime, version, <pid>/status, self (procfs, read-only)

COMPILING C FOR COAL
Coal has no compiler; build on the host with tools/coal-cc and copy the result in:
  tools/coal-cc -o /tmp/prog prog.c        then   coal_write_file path=/usr/bin/prog host_path=/tmp/prog   and run it
It needs gcc with -m32 support and nasm. The exact command it runs (SRC = src/CoalOS in this repo):
  nasm -f elf32 $SRC/userspace/entry.asm -o entry.o
  gcc -m32 -march=i386 -ffreestanding -nostdinc -fno-stack-protector -fno-pie -fno-pic -mno-mmx -mno-sse -mno-sse2
      -mno-red-zone -Os -Wall -Wextra -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
      -I$SRC/include prog.c $SRC/userspace/libc/*.c entry.o -nostdlib -static -no-pie -s -Wl,--gc-sections
      -T$SRC/src/arch/i386/user.ld -o prog
Include Coal's headers as <libc/stdio.h>, <libc/stdlib.h>, <libc/unistd.h> ... (there is no glibc, no FPU:
no float/double math, printf %f is done in integers). main(argc, argv) is the entry point.

SYSTEM CALLS
The Linux i386 ABI (int 0x80, Linux numbers and struct layouts): files, directories, pipes, dup, fork/execve
(copy-on-write), wait4, signals with handlers (rt_sigaction, rt_sigreturn), poll, brk/mmap2/munmap (anonymous),
clock_gettime, nanosleep, uname, syslog, reboot, AF_INET sockets (socketcall and the direct socket calls:
TCP streams, UDP, raw ICMP). No threads/clone, ptrace, symlinks, chmod/chown.
Unimplemented calls return -ENOSYS.

NETWORK
TCP, UDP, ICMP over IPv4; DHCP at boot (falls back to 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3).
Commands: ifconfig, ping -c N host, nslookup name, wget [-O file] http://host/path, nc host port, nc -l port.
Coal speaks plain HTTP only: wget refuses https://. Use coal_download (the host fetches http and https, then
writes the file into Coal) for HTTPS. The guest is NAT'ed (QEMU user networking): it reaches the host at
10.0.2.2 and the internet if the host has it, but nothing connects in unless QEMU forwards a port.
No IPv6, no loopback interface, no fragment reassembly, TCP drops out-of-order segments.

LIMITS
FAT16 volume (32 MiB image). 32 descriptors per process. coal_run keeps the first 2000 and the last 6000
bytes of each output stream (head_bytes / tail_bytes change that); the full output, up to 4 MiB, is
written to a file in the VM whose path is returned, so read the middle with coal_read_file offset/max_bytes.
Every command runs non-interactively: stdin is /dev/null, there is no terminal, PAGER=cat EDITOR=true
CI=1 TERM=dumb GIT_TERMINAL_PROMPT=0. The default timeout is 120 s (max 600 s); on timeout the whole
process group is killed. For anything long use coal_spawn, which returns a job_id at once: coal_job_status
(wait_ms blocks until it ends), coal_job_output (offset based tail), coal_job_signal, coal_jobs. A job
gets its own process group and optional limits (mem_mb, nproc, cpu_s); a limit that is hit is reported.
Files move in checked chunks; coal_read_file with save_to fetches the whole file, without it the
default is 64 KiB (max 4 MiB). coal_write_file is atomic: the file appears complete or not at all.

SAFE: ls cat ps free uptime uname dmesg, writing under /tmp /home /root, coal_kernel_log, coal_screen.
DESTRUCTIVE: rm/mv/cp over existing files, coal_write_file over any existing file, writing /sbin/init,
/sbin/agentd, /bin/sh or /bin/coalbox (breaks boot or the link to the VM), mount/umount, writing /dev/*.
poweroff, halt and reboot inside Coal end or restart the session (use coal_reboot / coal_shutdown instead).
Everything done to the disk persists in the image until coal_restore / coal_reset.

RECOVERY
coal_snapshot NAME  saves the disk (runs sync first).  coal_restore NAME  puts it back and reboots.
coal_reset  rebuilds the pristine image from the sources and reboots.  coal_reboot keeps the disk.
If agentd does not answer: coal_kernel_log and coal_screen show the state; coal_restore/coal_reset recovers
even a broken /sbin/init. coal_boot with snapshot=true discards all disk writes when the VM stops.
"""
