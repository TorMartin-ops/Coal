#ifndef SYSCALL_H
#define SYSCALL_H

#include <types.h>
#include <isr_frame.h> // Include the definition of isr_frame_t

// Define system call numbers
#define SYS_EXIT     1
#define SYS_FORK     2  // Example
#define SYS_READ     3
#define SYS_WRITE    4
#define SYS_OPEN     5
#define SYS_CLOSE    6
#define SYS_LSEEK    19 // Linux compatible lseek number
// Add other syscall numbers here...
#define SYS_GETPID   20 // Example
#define SYS_BRK      45 // Example

#define MAX_SYSCALLS 128 // Maximum number of system calls allowed

// *** MODIFIED: Use isr_frame_t directly for syscalls ***
// This struct represents the state saved by syscall_handler_asm
// It now mirrors the interrupt frame for consistency, although
// some fields like err_code will always be 0 for syscalls.
typedef isr_frame_t syscall_regs_t;

// Define the function pointer type for syscall handlers
// Note: The arguments are now extracted from the syscall_regs_t struct
typedef int (*syscall_fn_t)(syscall_regs_t *regs);

// Function prototypes
void syscall_init(void);
void syscall_dispatcher(syscall_regs_t *regs); // Takes the full frame struct

#endif // SYSCALL_H
