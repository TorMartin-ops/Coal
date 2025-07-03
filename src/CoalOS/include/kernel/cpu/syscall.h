#ifndef SYSCALL_H
#define SYSCALL_H

#include <libc/stdint.h> // For int32_t, uint32_t
#include <kernel/cpu/isr_frame.h>   // For isr_frame_t

// Define MAX_SYSCALLS if not already defined elsewhere (e.g., in a config file)
// This value should be greater than the highest syscall number used.
#ifndef MAX_SYSCALLS
#define MAX_SYSCALLS 256 // Or a more appropriate number for your system
#endif

// Linux x86 32-bit compatible syscall numbers
// Fully aligned with standard Linux ABI
#define SYS_EXIT    1   // __NR_exit
#define SYS_FORK    2   // __NR_fork  
#define SYS_READ    3   // __NR_read
#define SYS_WRITE   4   // __NR_write
#define SYS_OPEN    5   // __NR_open
#define SYS_CLOSE   6   // __NR_close
#define SYS_WAITPID 7   // __NR_waitpid
#define SYS_EXECVE  11  // __NR_execve
#define SYS_CHDIR   12  // __NR_chdir
#define SYS_LSEEK   19  // __NR_lseek
#define SYS_GETPID  20  // __NR_getpid
#define SYS_KILL    37  // __NR_kill
#define SYS_MKDIR   39  // __NR_mkdir
#define SYS_RMDIR   40  // __NR_rmdir
#define SYS_PIPE    42  // __NR_pipe
#define SYS_BRK     45  // __NR_brk
#define SYS_SIGNAL  48  // __NR_signal
#define SYS_DUP2    63  // __NR_dup2
#define SYS_GETPPID 64  // __NR_getppid
#define SYS_MMAP    90  // __NR_mmap
#define SYS_STAT    106 // __NR_stat
#define SYS_GETDENTS 141 // __NR_getdents (CORRECTED from 89)
#define SYS_GETCWD  183 // __NR_getcwd
#define SYS_UNLINK  10  // __NR_unlink

// Coal OS specific syscalls (non-standard)
#define SYS_PUTS               7000  // Custom puts syscall
#define SYS_READ_TERMINAL_LINE 7001  // Custom terminal line reader

// Process Groups and Sessions syscalls
#define SYS_SETSID      66 // POSIX setsid()
#define SYS_GETSID      147 // POSIX getsid()
#define SYS_SETPGID     57 // POSIX setpgid()
#define SYS_GETPGID     132 // POSIX getpgid()
#define SYS_GETPGRP     65 // POSIX getpgrp()
#define SYS_TCSETPGRP   410 // POSIX tcsetpgrp()
#define SYS_TCGETPGRP   411 // POSIX tcgetpgrp()

// Add other syscall numbers here as needed

/**
 * @brief Function pointer type for system call handlers.
 *
 * Each handler receives the three general-purpose arguments passed in EBX, ECX, EDX,
 * and a pointer to the full interrupt stack frame for access to all registers.
 * It should return an int32_t, which will be placed in EAX for the user process.
 */
typedef int32_t (*syscall_fn_t)(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);

/**
 * @brief Initializes the system call dispatch table.
 * Must be called once during kernel initialization.
 */
void syscall_init(void);

/**
 * @brief The C-level system call dispatcher.
 * This function is called from the assembly interrupt handler (int 0x80).
 * It identifies the syscall number and calls the appropriate handler.
 *
 * @param regs Pointer to the interrupt stack frame containing all saved registers.
 * @return The result of the system call, to be placed in the user's EAX.
 */
int32_t syscall_dispatcher(isr_frame_t *regs); // Corrected prototype

#endif // SYSCALL_H
