/**
 * @file syscall.h
 * @brief System Call Interface Definitions (Uses isr_frame.h)
 */

 #ifndef SYSCALL_H
 #define SYSCALL_H
 
 #include <types.h>     // Include basic types like uint32_t, int etc.
 #include <isr_frame.h> // <<< Include the canonical frame definition
 
 // --- System Call Numbers ---
 // ... (Keep existing definitions: SYS_EXIT, SYS_READ, etc.) ...
 #define SYS_EXIT     1
 #define SYS_READ     3
 #define SYS_WRITE    4
 #define SYS_OPEN     5
 #define SYS_CLOSE    6
 #define SYS_PUTS     7
 #define SYS_LSEEK    19
 #define SYS_GETPID   20
 #define MAX_SYSCALLS 128
 
 // --- Syscall Argument Convention ---
 // EAX: Syscall number
 // EBX: Argument 1
 // ECX: Argument 2
 // EDX: Argument 3
 
 // --- REMOVED syscall_regs_t structure definition ---
 
 // --- REMOVED typedef syscall_regs_t isr_frame_t; ---
 
 /**
  * @brief Function pointer type for system call implementation functions.
  * Uses the canonical isr_frame_t structure.
  */
typedef int (*syscall_fn_t)(uint32_t a1, uint32_t a2, uint32_t a3, isr_frame_t *frame);
 
 // --- Function Prototypes ---
void syscall_init(void);
 
 /**
  * @brief The C entry point for system call dispatching.
  * Called by the assembly handler (`syscall_handler_asm`).
  * Receives a pointer to the standard interrupt stack frame.
  *
  * @param regs Pointer to the interrupt stack frame (isr_frame_t).
  */
void syscall_dispatcher(isr_frame_t *frame);
 
 #endif // SYSCALL_H