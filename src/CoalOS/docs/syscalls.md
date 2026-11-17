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
| 10 | unlink | `sc_unlink` |  |
| 11 | execve | `sc_execve` |  |
| 12 | chdir | `sc_chdir` |  |
| 13 | time | `sc_time` |  |
| 19 | lseek | `sc_lseek` |  |
| 20 | getpid | `sc_getpid` |  |
| 21 | mount | `sc_mount` |  |
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
| 88 | reboot | `sc_reboot` |  |
| 91 | munmap | `sc_munmap` |  |
| 102 | socketcall | `sc_socketcall` | multiplexer for the calls below |
| 103 | syslog | `sc_syslog` |  |
| 114 | wait4 | `sc_wait4` |  |
| 122 | uname | `sc_uname` |  |
| 132 | getpgid | `sc_getpgid` |  |
| 140 | llseek | `sc_llseek` |  |
| 145 | readv | `sc_readv` |  |
| 146 | writev | `sc_writev` |  |
| 147 | getsid | `sc_getsid` |  |
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
| 295 | openat | `sc_openat` |  |
| 300 | fstatat64 | `sc_fstatat64` |  |
| 331 | pipe2 | `sc_pipe2` |  |
| 359 | socket | `sc_socket` |  |
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
- Signals: handlers, `sa_mask`, `SA_RESETHAND`, `SA_NODEFER` and `SA_RESTORER`
  work; delivery happens on return from system calls and interrupts.
  `SIGSTOP`/`SIGTSTP` have no effect (no job control stop).
- `poll` asks each driver (tty, pipe, serial) for readiness and sleeps until an
  event or the timeout. Regular files are always ready.
- `mmap2` supports anonymous private mappings only; `brk` grows the heap.
- `lstat64` is `stat64` (no symbolic links). `utime` stores the modification
  time on FAT.
- `syslog` reads the 16 KiB kernel log ring (`dmesg`).
- `set_thread_area` keeps one TLS descriptor per process.
