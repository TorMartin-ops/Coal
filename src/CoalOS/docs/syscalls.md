# System calls

Coal OS implements the Linux i386 system call ABI: `int 0x80`, number in EAX,
arguments in EBX, ECX, EDX, ESI, EDI, EBP, result or `-errno` in EAX, Linux
numbers and Linux struct layouts (`stat64`, `linux_dirent64`, `timespec`,
`sigaction`, ...). There is one table, `syscall_table` in
`kernel/cpu/syscall.c`; any number not listed below returns `-ENOSYS`.
The table below is generated from it. `scripts/check_syscall_numbers.sh`
compares `include/libc/syscall_numbers.h` with the Linux headers.

| Number | Name | Handler | Note |
|---:|---|---|---|
| 1 | exit | `sc_exit` |  |
| 2 | fork | `sc_fork` |  |
| 3 | read | `sc_read` |  |
| 4 | write | `sc_write` |  |
| 5 | open | `sc_open` |  |
| 6 | close | `sc_close` |  |
| 7 | waitpid | `sc_waitpid` |  |
| 9 | link | `sc_link` | EPERM on FAT |
| 10 | unlink | `sc_unlink` |  |
| 11 | execve | `sc_execve` |  |
| 12 | chdir | `sc_chdir` |  |
| 13 | time | `sc_time` |  |
| 19 | lseek | `sc_lseek` |  |
| 20 | getpid | `sc_getpid` |  |
| 21 | mount | `sc_mount` | types `fat` (device `hdb`..`hdd` or `/dev/hdX`) and `procfs` |
| 24 | getuid | `sc_zero` |  |
| 30 | utime | `sc_utime` |  |
| 33 | access | `sc_access` |  |
| 36 | sync | `sc_sync` |  |
| 37 | kill | `sc_kill` |  |
| 38 | rename | `sc_rename` |  |
| 39 | mkdir | `sc_mkdir` |  |
| 40 | rmdir | `sc_rmdir` |  |
| 41 | dup | `sc_dup` |  |
| 42 | pipe | `sc_pipe` |  |
| 45 | brk | `sc_brk` |  |
| 47 | getgid | `sc_zero` |  |
| 49 | geteuid | `sc_zero` |  |
| 50 | getegid | `sc_zero` |  |
| 52 | umount2 | `sc_umount2` |  |
| 54 | ioctl | `sc_ioctl` |  |
| 57 | setpgid | `sc_setpgid` |  |
| 63 | dup2 | `sc_dup2` |  |
| 64 | getppid | `sc_getppid` |  |
| 65 | getpgrp | `sc_getpgrp` |  |
| 66 | setsid | `sc_setsid` |  |
| 78 | gettimeofday | `sc_gettimeofday` |  |
| 82 | select | `sc_oldselect` | old argument-block form |
| 83 | symlink | `sc_symlink` | EPERM on FAT |
| 85 | readlink | `sc_readlink` | EINVAL: no symbolic links |
| 88 | reboot | `sc_reboot` |  |
| 91 | munmap | `sc_munmap` |  |
| 99 | statfs | `sc_statfs` |  |
| 100 | fstatfs | `sc_fstatfs` |  |
| 102 | socketcall | `sc_socketcall` | multiplexer for the calls below |
| 103 | syslog | `sc_syslog` |  |
| 114 | wait4 | `sc_wait4` |  |
| 118 | fsync | `sc_fsync` | writes the file's data and metadata to disk |
| 122 | uname | `sc_uname` |  |
| 132 | getpgid | `sc_getpgid` |  |
| 140 | llseek | `sc_llseek` |  |
| 142 | _newselect | `sc_newselect` | built on the poll machinery |
| 145 | readv | `sc_readv` |  |
| 146 | writev | `sc_writev` |  |
| 147 | getsid | `sc_getsid` |  |
| 148 | fdatasync | `sc_fsync` | same as fsync |
| 158 | sched_yield | `sc_sched_yield` |  |
| 162 | nanosleep | `sc_nanosleep` |  |
| 168 | poll | `sc_poll` |  |
| 173 | rt_sigreturn | `sc_rt_sigreturn` |  |
| 174 | rt_sigaction | `sc_rt_sigaction` |  |
| 175 | rt_sigprocmask | `sc_rt_sigprocmask` |  |
| 183 | getcwd | `sc_getcwd` |  |
| 192 | mmap2 | `sc_mmap2` |  |
| 195 | stat64 | `sc_stat64` |  |
| 196 | lstat64 | `sc_stat64` | no symbolic links |
| 197 | fstat64 | `sc_fstat64` |  |
| 199 | getuid32 | `sc_zero` |  |
| 200 | getgid32 | `sc_zero` |  |
| 201 | geteuid32 | `sc_zero` |  |
| 202 | getegid32 | `sc_zero` |  |
| 220 | getdents64 | `sc_getdents64` |  |
| 221 | fcntl64 | `sc_fcntl64` |  |
| 224 | gettid | `sc_getpid` |  |
| 243 | set_thread_area | `sc_set_thread_area` |  |
| 252 | exit_group | `sc_exit` |  |
| 258 | set_tid_address | `sc_set_tid_address` |  |
| 265 | clock_gettime | `sc_clock_gettime` |  |
| 268 | statfs64 | `sc_statfs64` |  |
| 269 | fstatfs64 | `sc_fstatfs64` |  |
| 295 | openat | `sc_openat` |  |
| 300 | fstatat64 | `sc_fstatat64` |  |
| 308 | pselect6 | `sc_pselect6` | temporary signal mask, timespec timeout |
| 331 | pipe2 | `sc_pipe2` |  |
| 359 | socket | `sc_socket` |  |
| 360 | socketpair | `sc_socketpair` | AF_UNIX, SOCK_STREAM only |
| 361 | bind | `sc_bind` |  |
| 362 | connect | `sc_connect` |  |
| 363 | listen | `sc_listen` |  |
| 364 | accept4 | `sc_accept4` |  |
| 365 | getsockopt | `sc_getsockopt` |  |
| 366 | setsockopt | `sc_setsockopt` |  |
| 367 | getsockname | `sc_getsockname` |  |
| 368 | getpeername | `sc_getpeername` |  |
| 369 | sendto | `sc_sendto` |  |
| 370 | sendmsg | `sc_sendmsg` |  |
| 371 | recvfrom | `sc_recvfrom` |  |
| 372 | recvmsg | `sc_recvmsg` |  |
| 373 | shutdown | `sc_shutdown` |  |

Behaviour worth knowing:

- Single-threaded processes, no users: `getuid` and friends return 0.
- Signals: handlers, `sa_mask`, `SA_RESETHAND`, `SA_NODEFER`, `SA_RESTORER` and
  `SA_RESTART` work; delivery happens on return from system calls and interrupts.
  `SIGSTOP`/`SIGTSTP` have no effect (no job control stop).
- `SA_RESTART`: `read`, `write`, `readv`, `writev`, `wait4`/`waitpid`, `accept4`, `sendto`,
  `sendmsg`, `recvfrom` and `recvmsg` interrupted by such a handler are re-executed after
  `sigreturn`; `poll`, `select`, `pselect6`, `nanosleep`, `connect` and `socketcall` always
  return `EINTR`, as on Linux. Sockets with `SO_RCVTIMEO` restart with the full timeout.
- The x87 state is per process (lazy switching); a signal frame carries it in
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
- `statfs`/`statfs64` report cluster counts for FAT and a type for devfs/procfs. `link` and
  `symlink` return `EPERM` (FAT), `readlink` returns `EINVAL` for existing paths.
- Networking: `127.0.0.0/8` and the own address are looped back without a NIC (interface `lo`,
  16-packet queue, UDP overflow is dropped); a connected UDP socket sees `ECONNREFUSED` once
  after an ICMP port unreachable.
- `poll` asks each driver (tty, pipe, serial) for readiness and sleeps until an
  event or the timeout. Regular files are always ready.
- `mmap2` supports anonymous private mappings only; `brk` grows the heap.
- `lstat64` is `stat64` (no symbolic links). `utime` stores the modification
  time on FAT.
- `syslog` reads the 16 KiB kernel log ring (`dmesg`).
- `set_thread_area` keeps one TLS descriptor per process.
