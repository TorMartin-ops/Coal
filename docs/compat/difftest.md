# Differential and external-suite testing

Tools: `tools/difftest/` (corpus, `difftest.py`, `fuzz.py`, `external/`). Machine-readable results: `docs/compat/difftest.json`.
Kernel under test: branch `difftest`, Linux x86_64 reference: the build host (static musl binaries on both sides).

## How it works

- `difftest.py` builds each `corpus/*.c` (static musl, raw `syscall`), runs it on the host and in Coal, normalises pids, addresses and times, and diffs the trace lines. A divergence is classified as *missing syscall* (Coal returns ENOSYS), *wrong errno*, *wrong semantics* (same call, different result) or *crash* (signal or missing output).
- `fuzz.py` generates seeded random file/descriptor scripts for `corpus/fsops.c`, runs host and Coal, and shrinks each new divergence signature (`op:host:coal`) with ddmin to a minimal script.
- `external/build.sh [OUTDIR]` builds the external suites reproducibly (sources cached in `~/.cache/coal-difftest`). `external/ltp.py` runs every test on the host (private PID namespace) and in Coal, and counts pass/fail/brok/conf per syscall.
- Everything that boots Coal takes the host-wide slot (`flock /tmp/coal-testkit/slots/0`) through `machine.py`. Suites are plain Python modules with `run(machine, ...)` returning JSON, so a test kit can call them later.

## libc-test

Not run. `git clone https://git.musl-libc.org/git/libc-test` fails from this host (the proxy answers 403), and no other mirror was tried. `build.sh` contains the clone, static build and per-test run; it prints "not reachable; skipped" and exits 0 when the clone fails. Re-run `build.sh` where the host is reachable; no kernel work depends on it.

## LTP (curated syscalls subset)

137 syscall directories, 645 test programs, LTP pinned to c4be3107, built static against musl. The LTP harness region (`tst_test.c`, a file-backed `MAP_SHARED` mapping) is patched at build time to shared anonymous memory because Coal has no shared file mappings; tests that re-exec with the checkpoint file still fail.

| | Linux | Coal before | Coal after |
|---|---:|---:|---:|
| tests passing | 567 | 9 (1.6%) | 180 (31.7%) |

Verdicts in Coal after: 180 pass, 143 fail, 259 brok, 51 conf, 7 timeout, 5 no result.

### Per syscall (directories where Linux passes at least one test)

| syscall | tests | pass on Linux | Coal before | Coal after |
|---|---:|---:|---:|---:|
| fcntl | 76 | 60 | 0 | 9 |
| pwrite | 8 | 8 | 0 | 8 |
| pipe | 15 | 15 | 3 | 9 |
| fork | 11 | 9 | 0 | 6 |
| dup2 | 7 | 7 | 0 | 5 |
| write | 6 | 6 | 0 | 5 |
| dup | 7 | 7 | 0 | 4 |
| pselect | 6 | 6 | 0 | 4 |
| signal | 6 | 6 | 0 | 4 |
| getrandom | 5 | 5 | 0 | 4 |
| writev | 6 | 5 | 0 | 4 |
| ftruncate | 6 | 4 | 0 | 4 |
| getegid | 4 | 4 | 0 | 4 |
| kill | 9 | 8 | 1 | 4 |
| nice | 5 | 5 | 0 | 3 |
| poll | 4 | 4 | 0 | 3 |
| read | 4 | 4 | 0 | 3 |
| getrlimit | 3 | 3 | 0 | 3 |
| setgid | 6 | 3 | 0 | 3 |
| mmap | 23 | 23 | 0 | 2 |
| open | 16 | 13 | 0 | 2 |
| stat | 8 | 8 | 0 | 2 |
| creat | 9 | 7 | 0 | 2 |
| setrlimit | 6 | 5 | 0 | 2 |
| fstat | 4 | 4 | 0 | 2 |
| fstatfs | 4 | 4 | 0 | 2 |
| getcwd | 4 | 4 | 0 | 2 |
| lseek | 4 | 4 | 0 | 2 |
| pread | 4 | 4 | 0 | 2 |
| select | 4 | 4 | 0 | 2 |
| truncate | 4 | 4 | 0 | 2 |
| chdir | 3 | 3 | 0 | 2 |
| setuid | 6 | 3 | 0 | 2 |
| wait4 | 3 | 3 | 0 | 2 |
| brk | 2 | 2 | 0 | 2 |
| close | 2 | 2 | 0 | 2 |
| dup3 | 2 | 2 | 0 | 2 |
| geteuid | 4 | 2 | 0 | 2 |
| getgid | 4 | 2 | 0 | 2 |
| getpgid | 2 | 2 | 0 | 2 |
| getppid | 2 | 2 | 0 | 2 |
| getsid | 2 | 2 | 0 | 2 |
| getuid | 4 | 2 | 0 | 2 |
| rt_sigaction | 3 | 2 | 0 | 2 |
| tkill | 2 | 2 | 0 | 2 |
| wait | 2 | 2 | 0 | 2 |
| rename | 14 | 14 | 0 | 1 |
| waitpid | 11 | 11 | 0 | 1 |
| setsockopt | 11 | 8 | 0 | 1 |
| madvise | 11 | 7 | 0 | 1 |
| chown | 10 | 5 | 0 | 1 |
| mprotect | 5 | 5 | 0 | 1 |
| access | 4 | 4 | 0 | 1 |
| clock_nanosleep | 4 | 4 | 0 | 1 |
| fsync | 4 | 4 | 0 | 1 |
| fdatasync | 3 | 3 | 0 | 1 |
| munmap | 3 | 3 | 0 | 1 |
| nanosleep | 3 | 3 | 0 | 1 |
| rmdir | 3 | 3 | 0 | 1 |
| sysinfo | 3 | 3 | 0 | 1 |
| uname | 3 | 3 | 0 | 1 |
| connect | 3 | 2 | 0 | 1 |
| gettimeofday | 2 | 2 | 0 | 1 |
| readv | 2 | 2 | 0 | 1 |
| setpgid | 4 | 2 | 0 | 1 |
| shutdown | 2 | 2 | 0 | 1 |
| sigaltstack | 2 | 2 | 1 | 2 |
| socket | 2 | 2 | 0 | 1 |
| socketpair | 2 | 2 | 0 | 1 |
| times | 2 | 2 | 0 | 1 |
| vfork | 2 | 2 | 0 | 1 |
| accept4 | 1 | 1 | 0 | 1 |
| clock_getres | 1 | 1 | 0 | 1 |
| getpgrp | 1 | 1 | 0 | 1 |
| getpid | 1 | 1 | 0 | 1 |
| rt_sigprocmask | 2 | 1 | 0 | 1 |
| sbrk | 3 | 1 | 0 | 1 |
| time | 1 | 1 | 0 | 1 |
| mknod | 9 | 9 | 0 | 0 |
| mremap | 9 | 9 | 0 | 0 |
| chmod | 7 | 7 | 0 | 0 |
| prctl | 10 | 7 | 0 | 0 |
| utime | 7 | 7 | 0 | 0 |
| fchmod | 6 | 6 | 0 | 0 |
| lstat | 6 | 6 | 0 | 0 |
| preadv | 6 | 6 | 0 | 0 |
| pwritev | 6 | 6 | 0 | 0 |
| statfs | 6 | 6 | 0 | 0 |
| alarm | 5 | 5 | 0 | 0 |
| bind | 6 | 5 | 0 | 0 |
| clock_gettime | 5 | 5 | 0 | 0 |
| mkdir | 5 | 5 | 0 | 0 |
| unlink | 5 | 5 | 0 | 0 |
| link | 4 | 4 | 0 | 0 |
| mlock | 5 | 4 | 0 | 0 |
| msync | 4 | 4 | 0 | 0 |
| accept | 3 | 3 | 0 | 0 |
| fchdir | 3 | 3 | 0 | 0 |
| tgkill | 3 | 3 | 0 | 0 |
| exit | 2 | 2 | 1 | 1 |
| getcpu | 2 | 2 | 0 | 0 |
| getdents | 2 | 2 | 0 | 0 |
| getitimer | 2 | 2 | 0 | 0 |
| getpriority | 2 | 2 | 0 | 0 |
| getrusage | 5 | 2 | 0 | 0 |
| getsockopt | 2 | 2 | 0 | 0 |
| gettid | 2 | 2 | 0 | 0 |
| mkdirat | 2 | 2 | 0 | 0 |
| munlock | 2 | 2 | 0 | 0 |
| openat | 5 | 2 | 0 | 0 |
| pause | 2 | 2 | 0 | 0 |
| pipe2 | 4 | 2 | 0 | 0 |
| send | 2 | 2 | 0 | 0 |
| sendmsg | 3 | 2 | 0 | 0 |
| sendto | 3 | 2 | 0 | 0 |
| setitimer | 2 | 2 | 0 | 0 |
| setpriority | 2 | 2 | 0 | 0 |
| sigaction | 2 | 2 | 1 | 1 |
| sigsuspend | 2 | 2 | 0 | 0 |
| symlink | 3 | 2 | 0 | 0 |
| arch_prctl | 1 | 1 | 0 | 0 |
| exit_group | 1 | 1 | 0 | 0 |
| getpeername | 1 | 1 | 0 | 0 |
| getsockname | 1 | 1 | 0 | 0 |
| listen | 1 | 1 | 1 | 1 |
| ppoll | 1 | 1 | 0 | 0 |
| readlink | 2 | 1 | 0 | 0 |
| recv | 1 | 1 | 0 | 0 |
| recvfrom | 1 | 1 | 0 | 0 |
| recvmsg | 3 | 1 | 0 | 0 |
| sched_yield | 1 | 1 | 1 | 1 |
| setsid | 1 | 1 | 0 | 0 |
| sigpending | 1 | 1 | 0 | 0 |
| sigprocmask | 1 | 1 | 0 | 0 |
| umask | 1 | 1 | 0 | 0 |
| unlinkat | 1 | 1 | 0 | 0 |
| utimes | 1 | 1 | 0 | 0 |

### Largest remaining LTP blockers (tests affected, Coal after)

| tests | blocker | needs |
|---:|---|---|
| 53 | "Failed to acquire device" (loop device and mkfs) | block-device and mount support; out of scope |
| 15 | cannot parse kernel .config | /proc/config.gz or /boot/config; tests are skipped on Linux when absent |
| 15 | `tst_checkpoint_wait` ENOSYS | futex |
| 6 | utime/utimensat ENOSYS | set file times (FAT stores them): cheap |
| 6 | `F_SETLEASE` EINVAL | leases; out of scope |
| 4 | `malloc` ENOMEM in fd-exhaustion tests | MAX_FD is 32 and RLIMIT_NOFILE is not implemented |
| 4 | `F_SETLK` EINVAL | POSIX record locks |
| 4 | `chown` EPERM | file owners (FAT has none) |
| 7 | shared or read-only file `mmap`, `mlock` ENOSYS | shared file mappings; mlock can be a checked no-op only if the corpus line is kept honest |
| 3 | alarm ENOSYS | timers and SIGALRM |
| 3 | mknod ENOSYS | fifo and device nodes on FAT |
| 2 | setgroups ENOSYS | supplementary groups |

## Syscall corpus

24 programs (16 area programs, 8 regression programs), 991 trace lines, 251 diverging (25%). The whole-corpus count on the base commit was not captured (the base disk image could not be booted alongside the LTP run), but each `reg_*` program is a divergence that existed on the base commit and is now clean.

### Per program

| program | trace lines | diverging | missing syscall | wrong errno | wrong semantics | other |
|---|---:|---:|---:|---:|---:|---:|
| dirs | 53 | 15 | 1 | 2 | 12 | 0 |
| errno | 106 | 43 | 33 | 3 | 7 | 0 |
| fd | 50 | 0 | 0 | 0 | 0 | 0 |
| files | 92 | 10 | 0 | 1 | 9 | 0 |
| ids | 52 | 15 | 7 | 0 | 8 | 0 |
| isock | 59 | 4 | 0 | 2 | 2 | 0 |
| mmap | 55 | 14 | 7 | 0 | 5 | 2 |
| path | 91 | 58 | 20 | 7 | 31 | 0 |
| pipes | 42 | 11 | 1 | 1 | 9 | 0 |
| poll | 29 | 2 | 1 | 0 | 1 | 0 |
| proc | 27 | 3 | 0 | 3 | 0 | 0 |
| random | 26 | 0 | 0 | 0 | 0 | 0 |
| reg_chmod | 15 | 0 | 0 | 0 | 0 | 0 |
| reg_cloexec | 15 | 0 | 0 | 0 | 0 | 0 |
| reg_creds | 6 | 0 | 0 | 0 | 0 | 0 |
| reg_fdflags | 26 | 0 | 0 | 0 | 0 | 0 |
| reg_mmap_file | 10 | 0 | 0 | 0 | 0 | 0 |
| reg_mmap_shared | 4 | 0 | 0 | 0 | 0 | 0 |
| reg_procfs | 7 | 0 | 0 | 0 | 0 | 0 |
| reg_truncate | 25 | 0 | 0 | 0 | 0 | 0 |
| rlimit | 45 | 14 | 0 | 3 | 11 | 0 |
| signals | 54 | 4 | 1 | 0 | 3 | 0 |
| time | 39 | 13 | 6 | 0 | 7 | 0 |
| usock | 63 | 45 | 0 | 15 | 29 | 1 |

### Per syscall (top 30 of 109)

| syscall | divergences | kinds | programs |
|---|---:|---|---|
| stat | 32 | wrong semantics 29, wrong errno 3 | dirs, files, ids, path, pipes, usock |
| open | 12 | wrong errno 5, wrong semantics 7 | errno, ids, path, pipes |
| openat | 9 | missing syscall 9 | errno, path |
| connect | 7 | wrong semantics 1, wrong errno 6 | usock |
| write | 6 | wrong semantics 5, wrong errno 1 | errno, isock, pipes, rlimit, usock |
| lstat | 6 | wrong semantics 6 | dirs, path |
| clock_gettime | 6 | wrong semantics 6 | time |
| read | 5 | wrong semantics 4, missing line 1 | errno, pipes, usock |
| rename | 5 | wrong semantics 4, wrong errno 1 | files, path |
| fcntl | 4 | wrong errno 2, wrong semantics 2 | errno, pipes, rlimit |
| getsockopt | 4 | wrong semantics 3, wrong errno 1 | isock, usock |
| newfstatat | 4 | missing syscall 3, wrong semantics 1 | errno, path |
| prctl | 4 | missing syscall 4 | errno, ids |
| readlink | 4 | wrong semantics 4 | dirs, ids |
| symlink | 4 | wrong semantics 3, wrong errno 1 | dirs, path |
| utimensat | 4 | missing syscall 4 | errno, path |
| bind | 4 | wrong semantics 2, wrong errno 2 | usock |
| mmap | 4 | wrong semantics 2, missing syscall 1, missing line 1 | mmap |
| mremap | 4 | missing syscall 3, missing line 1 | mmap |
| recvfrom | 4 | wrong semantics 3, wrong errno 1 | usock |
| sendto | 4 | wrong semantics 3, wrong errno 1 | usock |
| setrlimit | 4 | wrong semantics 2, wrong errno 2 | rlimit |
| unlink | 3 | wrong semantics 3 | dirs, path, usock |
| close | 3 | wrong semantics 3 | pipes, usock |
| fchdir | 3 | missing syscall 3 | dirs, errno |
| getcwd | 3 | wrong semantics 3 | dirs, path |
| socket | 3 | wrong errno 2, wrong semantics 1 | isock, usock |
| getrusage | 3 | missing syscall 3 | time |
| socketpair | 3 | wrong semantics 2, wrong errno 1 | usock |
| unlinkat | 3 | missing syscall 3 | path |

Most `stat`, `lstat`, `open`, `rename` and `unlink` divergences follow from the FAT root: no symlinks, hard links, mode bits, owners, fifos or socket nodes (`path`, `usock`, `dirs`, `files`). They are one root cause, not many bugs.

## Fuzzer (seeds 1-40, length 12)

36 of 40 seeds clean; 65 guest runs including shrinking. Remaining signatures (all minimal, one operation):

| signature | host | Coal |
|---|---|---|
| `rename p1 .` | EBUSY | ENOENT |
| `rename . d0/p1` | ENOENT | EINVAL |
| `rename . d1` | EBUSY | EINVAL |

(`ftruncate` with a negative length on a bad descriptor was the fourth finding and is fixed.) The seeds before the fixes found truncate/ftruncate/chmod/fcntl-flag gaps, all fixed below.

## Fixes made (each with a corpus regression program)

| commit | effect | regression |
|---|---|---|
| Make bool a real _Bool | O_CLOEXEC lost in open, pipe2, dup3 (`bool` was `unsigned char`) | `reg_cloexec` |
| Add creat, truncate, ftruncate, umask, dup3, chown; honest fcntl flags, EINVAL for bad access modes | many ENOSYS | `reg_truncate`, `reg_fdflags` |
| Add chmod and fchmod backed by the FAT read-only attribute | only the owner write bit is stored | `reg_chmod` |
| Share MAP_SHARED anonymous pages across fork | LTP library could not start | `reg_mmap_shared` |
| Map private files by copy, ignore fd for anonymous mmap | 18+ LTP tests used fd 0 with MAP_ANONYMOUS | `reg_mmap_file` |
| Add nobody user and group file | `getpwnam(nobody)` | none (rootfs) |
| Track real and effective ids, add set*uid/set*gid family | ids were constant root; ids are kept but not enforced | `reg_creds` |
| Add /proc/cpuinfo, /proc/PID/stat, pid_max; real ids in status | LTP helpers; stat fields past the 7th read 0 | `reg_procfs` |
| Check the ftruncate length before the descriptor | errno order | `reg_truncate` |

## Ranked next steps (by programs or tests unblocked)

1. futex (+ `set_robust_list`, `rseq`, `clone3` threads): unblocks the LTP checkpoint tests (15 now) and every threaded program; the biggest missing feature.
2. A writable in-memory filesystem for /tmp (and symlinks, hard links, modes, owners, fifos, `mknod`): one change removes most of the 58 `path`, 45 `usock` and `dirs` divergences and the chown/mknod LTP blockers.
3. `*at` syscalls with a directory fd (`openat`, `newfstatat`, `unlinkat`, `mkdirat`, `fchdir`, `faccessat`): 30+ divergences; needs an open file to remember its path.
4. AF_UNIX `socket`/`bind`/`listen`/`accept`/`connect` (only `socketpair` works) and the remaining socket options.
5. utimensat/utime (cheap, FAT stores times), alarm/setitimer/getitimer with SIGALRM, `ppoll`, `getrusage`, more clock ids, `mlock`/`msync`/`mremap`.
6. Raise MAX_FD and implement RLIMIT_NOFILE/FSIZE (only CPU, NPROC and AS are implemented).
7. Shared file-backed `mmap` via the page cache; `epoll`, `eventfd`, `timerfd`, `signalfd`, `inotify`, `memfd_create`.
8. Path-walk errno details: ENOTDIR vs ENOENT, trailing slashes, `rmdir .`, rename onto `.`/`..`, getdents64 on a file.
9. Dynamic linking (`PT_INTERP`, shared objects), needed before most non-static programs run.
