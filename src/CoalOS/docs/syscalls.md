# System calls

Numbers are defined in `include/kernel/cpu/syscall.h` and copied to
`include/libc/syscall_numbers.h` for user programs
(`scripts/check_syscall_numbers.sh` compares the two). Calls use `int 0x80`
with the number in EAX and arguments in EBX, ECX, EDX.

Status is taken from `kernel/cpu/` and describes the code in this
repository: "stub" means the handler only returns `-ENOSYS`.

| Number | Name | Status |
|---|---|---|
| 1 | exit | implemented |
| 2 | fork | stub |
| 3 | read | implemented |
| 4 | write | implemented |
| 5 | open | implemented |
| 6 | close | implemented |
| 7 | waitpid | stub (returns `-ECHILD`) |
| 10 | unlink | implemented |
| 11 | execve | stub |
| 12 | chdir | stub |
| 19 | lseek | implemented |
| 20 | getpid | implemented |
| 37 | kill | stub |
| 39 | mkdir | implemented |
| 40 | rmdir | implemented |
| 42 | pipe | stub |
| 45 | brk | stub |
| 48 | signal | stub |
| 57 | setpgid | stub |
| 63 | dup2 | implemented |
| 64 | getppid | implemented |
| 65 | getpgrp | stub |
| 66 | setsid | stub |
| 90 | mmap | stub |
| 106 | stat | stub |
| 132 | getpgid | stub |
| 141 | getdents | stub |
| 147 | getsid | stub |
| 183 | getcwd | returns "/" only |
| 410 | tcsetpgrp | stub |
| 411 | tcgetpgrp | stub |
| 7000 | puts | stub; user programs use `write` |
| 7001 | read_terminal_line | stub; the shell falls back to `read` |

Errors are returned as negated error numbers in EAX.
