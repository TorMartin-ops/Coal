// process.h

#ifndef PROCESS_H
#define PROCESS_H

#include <kernel/core/types.h>      // Include necessary base types
#include <kernel/memory/paging.h>      // Include for PAGE_SIZE, KERNEL_SPACE_VIRT_START, registers_t
// #include <kernel/fs/vfs/fs_limits.h>   // Define MAX_FD directly below instead

// Forward declare mm_struct to avoid circular dependency with mm.h if needed
struct mm_struct;
// Forward declare sys_file if needed, OR include sys_file.h if it only contains declarations/typedefs
struct sys_file;

// === Configuration Constants ===

// Define the maximum number of open file descriptors per process
#define MAX_FD 16 // <<<--- DEFINE MAX_FD HERE (adjust value as needed)

// Define the size for the kernel stack allocated per process
// (Must be page-aligned and > 0)
#define PROCESS_KSTACK_SIZE (PAGE_SIZE *4) // Example: 4 pages (16KB)

// Define User Stack Layout Constants
#define USER_STACK_PAGES        4                      // Example: 4 pages
#define USER_STACK_SIZE         (USER_STACK_PAGES * PAGE_SIZE)    // Example: 16KB
#define USER_STACK_TOP_VIRT_ADDR (KERNEL_SPACE_VIRT_START)        // Stack grows down from just below kernel space
#define USER_STACK_BOTTOM_VIRT  (USER_STACK_TOP_VIRT_ADDR - USER_STACK_SIZE) // Lowest valid stack address

#ifdef __cplusplus
extern "C" {
#endif

// Define process states if you use them
typedef enum {
    PROC_INITIALIZING,
    PROC_READY,
    PROC_RUNNING,
    PROC_SLEEPING,
    PROC_ZOMBIE
} process_state_t;


// === Process Control Block (PCB) Structure ===
typedef struct pcb {
    uint32_t pid;                   // Process ID
    uint32_t *page_directory_phys;  // Physical address of the process's page directory
    uint32_t entry_point;           // Virtual address of the program's entry point
    void *user_stack_top;           // Virtual address for the initial user ESP setting

    // Per-process file descriptor table
    struct sys_file *fd_table[MAX_FD]; // MAX_FD is now defined above
    spinlock_t       fd_table_lock;

    // Kernel Stack Info (Used when process is in kernel mode)
    uint32_t kernel_stack_phys_base; // Physical address of the base frame (for potential debugging/info)
    uint32_t *kernel_stack_vaddr_top; // Virtual address of the top of the kernel stack (highest address + 1)

    // Stores the kernel stack pointer (ESP) after the initial IRET frame has been pushed.
    // Used by the context switch mechanism for the first switch to this process.
    uint32_t kernel_esp_for_switch;

    // Memory Management Info
    struct mm_struct *mm;           // Pointer to the memory structure (VMAs, page dir etc.)

    // Process State & Scheduling Info (Examples - Adapt to your design)
    process_state_t state;          // e.g., PROC_RUNNING, PROC_READY, PROC_SLEEPING - Uncomment if used
    // int priority;
    struct pcb *next;               // For linking in scheduler queues
    // struct tcb *tcb;              // Link to associated Task Control Block if separate

    // === CPU Context ===
    // Stores the register state when the process is context-switched OUT.
    // This is typically filled by the context switch assembly code.
    // The initial state for the *first* run is prepared on the kernel stack, not here.
    registers_t context;

} pcb_t;


// === Public Process Management Functions ===

/**
 * @brief Creates a new user process by loading an ELF executable.
 * Sets up PCB, memory space (page directory, VMAs), kernel stack, user stack,
 * loads ELF segments, and initializes the user context.
 *
 * @param path Path to the executable file.
 * @return Pointer to the newly created PCB on success, NULL on failure.
 */
pcb_t *create_user_process(const char *path);

/**
 * @brief Destroys a process and frees all associated resources.
 * Frees memory space (VMAs, page tables, frames), kernel stack, page directory, and PCB.
 * IMPORTANT: Ensure the process is no longer running or scheduled before calling this.
 *
 * @param pcb Pointer to the PCB of the process to destroy.
 */
void destroy_process(pcb_t *pcb);

/**
 * @brief Gets the PCB of the currently running process.
 * Relies on the scheduler providing the current task/thread control block.
 *
 * @return Pointer to the current PCB, or NULL if no process context is active.
 */
pcb_t* get_current_process(void);

/**
 * @brief Initializes the file descriptor table for a new process.
 * Should be called during process creation after the PCB is allocated.
 *
 * @param proc Pointer to the new process's PCB.
 */
void process_init_fds(pcb_t *proc);

/**
 * @brief Closes all open file descriptors for a terminating process.
 * Should be called during process termination *before* freeing the PCB memory.
 *
 * @param proc Pointer to the terminating process's PCB.
 */
void process_close_fds(pcb_t *proc);


#ifdef __cplusplus
}
#endif

#endif // PROCESS_H
