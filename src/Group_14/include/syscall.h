/**
 * @file syscall.h
 * @brief System Call Interface Definitions
 *
 * Defines system call numbers, the structure passed from the assembly handler,
 * and the function signature for syscall implementation functions.
 */

 #ifndef SYSCALL_H
 #define SYSCALL_H
 
 #include <types.h>     // Include basic types like uint32_t, int etc.
 #include <isr_frame.h> // Include the definition of isr_frame_t
 
 // --- System Call Numbers ---
 // Follows a Linux-like numbering scheme where possible for familiarity.
 
 #define SYS_EXIT     1  /**< Terminate current process */
 #define SYS_FORK     2  /**< Create child process (Not Implemented) */
 #define SYS_READ     3  /**< Read from file descriptor */
 #define SYS_WRITE    4  /**< Write to file descriptor */
 #define SYS_OPEN     5  /**< Open or create a file */
 #define SYS_CLOSE    6  /**< Close a file descriptor */
 // #define SYS_WAITPID  7  /**< Wait for process termination (Not Implemented) */
 // #define SYS_CREAT    8  /**< Create file (obsolete, use open) (Not Implemented) */
 // #define SYS_LINK     9  /**< Create hard link (Not Implemented) */
 // #define SYS_UNLINK  10  /**< Remove directory entry (Not Implemented) */
 // #define SYS_EXECVE  11  /**< Execute program (Not Implemented) */
 // #define SYS_CHDIR   12  /**< Change working directory (Not Implemented) */
 #define SYS_LSEEK    19 /**< Reposition file offset */
 #define SYS_GETPID   20 /**< Get process ID (Not Implemented) */
 // #define SYS_MOUNT   21  /**< Mount filesystem (Not Implemented) */
 // #define SYS_UMOUNT  22  /**< Unmount filesystem (Not Implemented) */
 #define SYS_BRK      45 /**< Change data segment size (Not Implemented) */
 // Add other syscall numbers as needed...
 
 // Maximum number of system calls supported by the table.
 #define MAX_SYSCALLS 128
 
 // --- Syscall Argument Convention (Assumed) ---
 // This kernel uses the following convention for passing arguments via registers:
 // EAX: System call number
 // EBX: Argument 0 (e.g., exit_code, fd, pathname ptr)
 // ECX: Argument 1 (e.g., user_buf ptr, flags)
 // EDX: Argument 2 (e.g., count, mode)
 // ESI: Argument 3 (Not currently used by base syscalls)
 // EDI: Argument 4 (Not currently used by base syscalls)
 //
 // Return Value:
 // EAX: Result of the system call (e.g., bytes read/written, fd, 0 on success)
 //      or a negative errno value on failure (e.g., -EFAULT, -ENOSYS).
 
 /**
  * @brief Structure containing the user process's registers saved by the assembly handler.
  *
  * Mirrors the isr_frame_t structure for consistency. The assembly handler pushes
  * a dummy error code (0) and the vector number (0x80) to match the layout.
  * Syscall arguments are extracted from the general-purpose registers within this struct.
  */
 typedef isr_frame_t syscall_regs_t;
 
 /**
  * @brief Function pointer type for system call implementation functions.
  *
  * Each syscall implementation receives a pointer to the saved registers
  * and returns an integer result (0 or positive value on success, negative errno on failure).
  */
 typedef int (*syscall_fn_t)(syscall_regs_t *regs);
 
 // --- Function Prototypes ---
 
 /**
  * @brief Initializes the system call dispatch table.
  * Called once during kernel initialization.
  */
 void syscall_init(void);
 
 /**
  * @brief The C entry point for system call dispatching.
  * Called by the assembly handler (`syscall_handler_asm`).
  * Validates the syscall number and calls the appropriate implementation function.
  *
  * @param regs Pointer to the saved register state from the user process.
  * The return value of the syscall is placed back into regs->eax.
  */
 void syscall_dispatcher(syscall_regs_t *regs);
 
 #endif // SYSCALL_H