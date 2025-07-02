#ifndef SYSCALL_H
#define SYSCALL_H

#include <libc/stdint.h> // For int32_t, uint32_t
#include <kernel/cpu/isr_frame.h>   // For isr_frame_t

// Define MAX_SYSCALLS if not already defined elsewhere (e.g., in a config file)
// This value should be greater than the highest syscall number used.
#ifndef MAX_SYSCALLS
#define MAX_SYSCALLS 256 // Or a more appropriate number for your system
#endif

// Syscall numbers (ensure these match your definitions in hello.c and elsewhere)
#define SYS_EXIT    1
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_PUTS    7
#define SYS_LSEEK   19
#define SYS_GETPID  20
#define SYS_READ_TERMINAL_LINE 21
#define SYS_FORK        2  // POSIX fork()
#define SYS_EXECVE      11 // POSIX execve()
#define SYS_WAITPID     17 // POSIX waitpid()
#define SYS_PIPE        42 // POSIX pipe()
#define SYS_GETPPID     64 // POSIX getppid()
#define SYS_DUP2        33 // POSIX dup2()
#define SYS_SIGNAL      48 // POSIX signal()
#define SYS_KILL        37 // POSIX kill()
#define SYS_CHDIR       12 // POSIX chdir()
#define SYS_GETCWD      183 // POSIX getcwd()
#define SYS_STAT        4  // POSIX stat()
#define SYS_READDIR     89 // Read directory entries

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
