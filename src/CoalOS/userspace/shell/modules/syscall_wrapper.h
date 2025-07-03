/**
 * @file syscall_wrapper.h
 * @brief System call wrapper interface for Coal OS Shell
 * @version 1.0
 * @author Modular refactoring
 */

#ifndef SYSCALL_WRAPPER_H
#define SYSCALL_WRAPPER_H

#include "../shell_types.h"

//============================================================================
// System Call Numbers
//============================================================================

#define SYS_EXIT    1
#define SYS_FORK    2
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_PUTS    7
#define SYS_EXECVE  11
#define SYS_CHDIR   12
#define SYS_WAITPID 17
#define SYS_LSEEK   19
#define SYS_GETPID  20
#define SYS_READ_TERMINAL_LINE 21
#define SYS_DUP2    33
#define SYS_KILL    37
#define SYS_PIPE    42
#define SYS_SIGNAL  48
#define SYS_GETPPID 64
#define SYS_GETCWD  183

//============================================================================
// System Call Interface
//============================================================================

/**
 * @brief Low-level system call interface
 * @param syscall_number System call number
 * @param arg1 First argument
 * @param arg2 Second argument
 * @param arg3 Third argument
 * @return System call return value
 */
int32_t syscall(int32_t syscall_number, int32_t arg1, int32_t arg2, int32_t arg3);

//============================================================================
// System Call Wrappers
//============================================================================

#define sys_exit(code)       syscall(SYS_EXIT, (code), 0, 0)
#define sys_fork()           syscall(SYS_FORK, 0, 0, 0)
#define sys_read(fd,buf,n)   syscall(SYS_READ, (fd), (int32_t)(uintptr_t)(buf), (n))
#define sys_write(fd,buf,n)  syscall(SYS_WRITE, (fd), (int32_t)(uintptr_t)(buf), (n))
#define sys_open(p,f,m)      syscall(SYS_OPEN, (int32_t)(uintptr_t)(p), (f), (m))
#define sys_close(fd)        syscall(SYS_CLOSE, (fd), 0, 0)
#define sys_puts(p)          syscall(SYS_PUTS, (int32_t)(uintptr_t)(p), 0, 0)
#define sys_execve(p,a,e)    syscall(SYS_EXECVE, (int32_t)(uintptr_t)(p), (int32_t)(uintptr_t)(a), (int32_t)(uintptr_t)(e))
#define sys_chdir(p)         syscall(SYS_CHDIR, (int32_t)(uintptr_t)(p), 0, 0)
#define sys_waitpid(p,s,o)   syscall(SYS_WAITPID, (p), (int32_t)(uintptr_t)(s), (o))
#define sys_getpid()         syscall(SYS_GETPID, 0, 0, 0)
#define sys_getppid()        syscall(SYS_GETPPID, 0, 0, 0)
#define sys_read_terminal_line(buf,n) syscall(SYS_READ_TERMINAL_LINE, (int32_t)(uintptr_t)(buf), (n), 0)
#define sys_dup2(o,n)        syscall(SYS_DUP2, (o), (n), 0)
#define sys_kill(p,s)        syscall(SYS_KILL, (p), (s), 0)
#define sys_pipe(p)          syscall(SYS_PIPE, (int32_t)(uintptr_t)(p), 0, 0)
#define sys_signal(s,h)      syscall(SYS_SIGNAL, (s), (int32_t)(uintptr_t)(h), 0)
#define sys_getcwd(buf,size) syscall(SYS_GETCWD, (int32_t)(uintptr_t)(buf), (size), 0)

#endif // SYSCALL_WRAPPER_H