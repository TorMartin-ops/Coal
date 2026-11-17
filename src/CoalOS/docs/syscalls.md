# System calls

Coal OS implements the Linux x86_64 system call ABI: `syscall`, number in RAX,
arguments in RDI, RSI, RDX, R10, R8, R9, result or `-errno` in RAX (RCX and R11 are clobbered), Linux
numbers and Linux struct layouts (`struct stat`, `linux_dirent64`, `timespec`, `sigaction`,
`rt_sigframe`, `msghdr`, `sockaddr`, `rlimit`, `sysinfo`, `utsname` with machine `x86_64`).
The kernel entry in `kernel/cpu/syscall.asm` builds the frame the interrupt gates build.
The vDSO serves `clock_gettime`, `gettimeofday`, `time` and `getcpu` without a system call. There
is one table, `syscall_table` in `kernel/cpu/syscall.c`; any number not listed below returns `-ENOSYS`.
The table below is generated from it. `scripts/check_syscall_numbers.sh`
compares `include/libc/syscall_numbers.h` with the Linux headers.

| Number | Name | Handler | Note |
|---:|---|---|---|
| 0 | read | `sc_read` |  |
| 1 | write | `sc_write` |  |
| 2 | open | `sc_open` |  |
| 3 | close | `sc_close` |  |
| 4 | stat | `sc_stat` |  |
| 5 | fstat | `sc_fstat` |  |
| 6 | lstat | `sc_stat` | same as stat (no symbolic links) |
| 7 | poll | `sc_poll` |  |
| 8 | lseek | `sc_lseek` |  |
| 9 | mmap | `sc_mmap` | anonymous private mappings only |
| 10 | mprotect | `sc_mprotect` | splits VMAs; PROT_NONE pages fault |
| 11 | munmap | `sc_munmap` |  |
| 12 | brk | `sc_brk` |  |
| 13 | rt_sigaction | `sc_rt_sigaction` | SA_RESTORER is required with a handler (EINVAL otherwise) |
| 14 | rt_sigprocmask | `sc_rt_sigprocmask` |  |
| 15 | rt_sigreturn | `sc_rt_sigreturn` |  |
| 16 | ioctl | `sc_ioctl` |  |
| 19 | readv | `sc_readv` |  |
| 20 | writev | `sc_writev` |  |
| 21 | access | `sc_access` |  |
| 22 | pipe | `sc_pipe` |  |
| 23 | select | `sc_select` | descriptors below 32 |
| 24 | sched_yield | `sc_sched_yield` |  |
| 28 | madvise | `sc_madvise` | accepted, no effect |
| 32 | dup | `sc_dup` |  |
| 33 | dup2 | `sc_dup2` |  |
| 35 | nanosleep | `sc_nanosleep` |  |
| 39 | getpid | `sc_getpid` |  |
| 41 | socket | `sc_socket` |  |
| 42 | connect | `sc_connect` |  |
| 43 | accept | `sc_accept` |  |
| 44 | sendto | `sc_sendto` |  |
| 45 | recvfrom | `sc_recvfrom` |  |
| 46 | sendmsg | `sc_sendmsg` |  |
| 47 | recvmsg | `sc_recvmsg` |  |
| 48 | shutdown | `sc_shutdown` |  |
| 49 | bind | `sc_bind` |  |
| 50 | listen | `sc_listen` |  |
| 51 | getsockname | `sc_getsockname` |  |
| 52 | getpeername | `sc_getpeername` |  |
| 53 | socketpair | `sc_socketpair` | AF_UNIX, SOCK_STREAM only |
| 54 | setsockopt | `sc_setsockopt` |  |
| 55 | getsockopt | `sc_getsockopt` |  |
| 57 | fork | `sc_fork` |  |
| 58 | vfork | `sc_fork` | same as fork (copy-on-write); the parent does not wait |
| 59 | execve | `sc_execve` |  |
| 60 | exit | `sc_exit` |  |
| 61 | wait4 | `sc_wait4` |  |
| 62 | kill | `sc_kill` |  |
| 63 | uname | `sc_uname` |  |
| 72 | fcntl | `sc_fcntl` |  |
| 74 | fsync | `sc_fsync` | writes the file's data and metadata to disk |
| 75 | fdatasync | `sc_fsync` | same as fsync |
| 79 | getcwd | `sc_getcwd` |  |
| 80 | chdir | `sc_chdir` |  |
| 82 | rename | `sc_rename` |  |
| 83 | mkdir | `sc_mkdir` |  |
| 84 | rmdir | `sc_rmdir` |  |
| 86 | link | `sc_link` | EPERM on FAT |
| 87 | unlink | `sc_unlink` |  |
| 88 | symlink | `sc_symlink` | EPERM on FAT |
| 89 | readlink | `sc_readlink` | EINVAL: no symbolic links |
| 96 | gettimeofday | `sc_gettimeofday` | served by the vDSO without a system call |
| 97 | getrlimit | `sc_getrlimit` |  |
| 99 | sysinfo | `sc_sysinfo` | uptime, memory, process count |
| 102 | getuid | `sc_zero` |  |
| 103 | syslog | `sc_syslog` |  |
| 104 | getgid | `sc_zero` |  |
| 107 | geteuid | `sc_zero` |  |
| 108 | getegid | `sc_zero` |  |
| 109 | setpgid | `sc_setpgid` |  |
| 110 | getppid | `sc_getppid` |  |
| 111 | getpgrp | `sc_getpgrp` |  |
| 112 | setsid | `sc_setsid` |  |
| 121 | getpgid | `sc_getpgid` |  |
| 124 | getsid | `sc_getsid` |  |
| 131 | sigaltstack | `sc_sigaltstack` | SA_ONSTACK handlers run on it |
| 132 | utime | `sc_utime` |  |
| 137 | statfs | `sc_statfs` |  |
| 138 | fstatfs | `sc_fstatfs` |  |
| 140 | getpriority | `sc_getpriority` | `PRIO_PROCESS` only; returns 20 - nice like Linux |
| 141 | setpriority | `sc_setpriority` | `PRIO_PROCESS` only |
| 158 | arch_prctl | `sc_arch_prctl` | ARCH_SET_FS/GET_FS/SET_GS/GET_GS |
| 160 | setrlimit | `sc_setrlimit` | CPU, AS, NPROC only (see below) |
| 162 | sync | `sc_sync` |  |
| 165 | mount | `sc_mount` | types `fat` (device `hdb`..`hdd` or `/dev/hdX`) and `procfs` |
| 166 | umount2 | `sc_umount2` |  |
| 169 | reboot | `sc_reboot` |  |
| 186 | gettid | `sc_getpid` | same as getpid |
| 200 | tkill | `sc_tkill` | single-threaded: same as kill |
| 201 | time | `sc_time` | served by the vDSO without a system call |
| 217 | getdents64 | `sc_getdents64` |  |
| 218 | set_tid_address | `sc_set_tid_address` |  |
| 228 | clock_gettime | `sc_clock_gettime` | served by the vDSO without a system call for CLOCK_REALTIME and CLOCK_MONOTONIC |
| 229 | clock_getres | `sc_clock_getres` | 1 ns |
| 231 | exit_group | `sc_exit` |  |
| 234 | tgkill | `sc_tgkill` | single-threaded: same as kill |
| 257 | openat | `sc_openat` |  |
| 262 | newfstatat | `sc_newfstatat` | AT_FDCWD and absolute paths |
| 270 | pselect6 | `sc_pselect6` | temporary signal mask, timespec timeout |
| 288 | accept4 | `sc_accept4` |  |
| 293 | pipe2 | `sc_pipe2` |  |
| 302 | prlimit64 | `sc_prlimit64` | any process; 64-bit values |
| 318 | getrandom | `sc_getrandom` | RDRAND, else a TSC-seeded generator |

Behaviour worth knowing:

- Resource limits: `RLIMIT_CPU` (SIGXCPU at the soft limit, repeated every second, SIGKILL at the
  hard limit; counted per process in timer ticks, children start at zero), `RLIMIT_AS` (`mmap` and
  `brk` fail with `ENOMEM`; the stack is not counted) and `RLIMIT_NPROC` (`fork` fails with `EAGAIN`;
  there is one user, so the limit applies to every process in the system). Other resources read as
  unlimited and `setrlimit` on them returns `EINVAL`. A hard limit cannot be raised (`EPERM`). Limits
  that stopped a process are listed in `RlimitHits` of `/proc/<pid>/status` and are merged into the
  parent when the process exits, so the parent can read them from the zombie before it reaps it.
- A process whose parent exits sends it `SIGCHLD` (ignored by default). A parent with a handler
  therefore wakes from `poll` with `EINTR` when a child dies.
- Single-threaded processes, no users: `getuid` and friends return 0.
- Signals: handlers, `sa_mask`, `SA_RESETHAND`, `SA_NODEFER`, `SA_RESTORER` and
  `SA_RESTART` work; delivery happens on return from system calls and interrupts.
  `SIGSTOP`/`SIGTSTP` have no effect (no job control stop).
- `SA_RESTART`: `read`, `write`, `readv`, `writev`, `wait4`/`waitpid`, `accept4`, `sendto`,
  `sendmsg`, `recvfrom` and `recvmsg` interrupted by such a handler are re-executed after
  `sigreturn`; `poll`, `select`, `pselect6`, `nanosleep` and `connect` always
  return `EINTR`, as on Linux. Sockets with `SO_RCVTIMEO` restart with the full timeout.
- The x87 and SSE state is per process (lazy switching); a signal frame carries it in
  `sigcontext.fpstate`.
- `execve` runs `#!interpreter [arg]` scripts (the rest of the line is one argument, at most four
  levels, `ELOOP` beyond); other formats give `ENOEXEC`.
- Pipes hold 64 KiB; writes of up to `PIPE_BUF` (4096) bytes are atomic. `socketpair`
  (`AF_UNIX`, `SOCK_STREAM`) is a pair of such pipes; the descriptors are sockets for `fstat`,
  `send`/`recv` and `shutdown`.
- `rmdir` and `rename` over an open directory follow Linux: the open descriptor then sees an
  empty directory with `st_nlink` 0.
- `select` and `pselect6` accept descriptors below 32 (`EBADF` above) and report only read,
  write and error readiness like `poll`.
- `statfs`/`fstatfs` report cluster counts for FAT and a type for devfs/procfs. `link` and
  `symlink` return `EPERM` (FAT), `readlink` returns `EINVAL` for existing paths.
- Networking: `127.0.0.0/8` and the own address are looped back without a NIC (interface `lo`,
  16-packet queue, UDP overflow is dropped); a connected UDP socket sees `ECONNREFUSED` once
  after an ICMP port unreachable.
- `poll` asks each driver (tty, pipe, serial) for readiness and sleeps until an
  event or the timeout. Regular files are always ready.
- `mmap` supports anonymous private mappings only (`MAP_FIXED` included); `brk` grows the heap. `mprotect`
  changes protections page-exactly, `PROT_NONE` pages fault, copy-on-write pages stay read-only until written.
- Only `SA_RESTORER` handlers are accepted: the kernel has no restorer of its own (the vDSO has none either), so
  `rt_sigaction` with a handler and no restorer returns `EINVAL`. libc supplies one. A signal frame skips the 128-byte
  red zone and honours `sigaltstack`.
- `lstat` is `stat` (no symbolic links). `utime` stores the modification
  time on FAT.
- `syslog` reads the 16 KiB kernel log ring (`dmesg`).
- `arch_prctl` keeps the FS and GS bases per process; they are saved on switch and reset by `execve`.
