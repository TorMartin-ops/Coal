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
    bool is_kernel_task;            // True if this is a kernel task (not user process)
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

    // === Process Hierarchy for Linux Compatibility ===
    uint32_t ppid;                  // Parent Process ID
    struct pcb *parent;             // Direct pointer to parent PCB
    struct pcb *children;           // Linked list of child processes
    struct pcb *sibling;            // Next sibling in parent's children list
    uint32_t exit_status;           // Exit status when process terminates
    bool has_exited;                // True if process has exited but not reaped
    spinlock_t children_lock;       // Protects children list manipulation
    
    // === Process Groups and Sessions ===
    uint32_t pid_namespace;         // PID namespace (for future container support)
    uint32_t sid;                   // Session ID - identifies the session
    uint32_t pgid;                  // Process Group ID - for job control
    struct pcb *session_leader;     // Pointer to session leader process
    struct pcb *pgrp_leader;        // Pointer to process group leader
    struct pcb *pgrp_next;          // Next process in same process group
    struct pcb *pgrp_prev;          // Previous process in same process group
    bool is_session_leader;         // True if this process is a session leader
    bool is_pgrp_leader;            // True if this process is a process group leader
    
    // === Terminal Control ===
    void *controlling_terminal;     // Pointer to controlling terminal (if any)
    uint32_t tty_pgrp;              // Foreground process group for controlling terminal
    bool has_controlling_tty;       // True if process has controlling terminal

    // === Signal Management ===
    uint32_t signal_mask;           // Blocked signals bitmap (1 bit per signal)
    uint32_t pending_signals;       // Pending signals bitmap (1 bit per signal)
    void *signal_handlers[32];      // Signal handler function pointers (user space addresses)
    void *signal_altstack;          // Alternative signal stack (if set via sigaltstack)
    uint32_t signal_flags;          // Signal-related flags (SA_RESTART, etc.)
    uint32_t in_signal_handler;     // Non-zero if currently executing a signal handler
    spinlock_t signal_lock;         // Protects signal-related fields

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
 * @brief Creates and initializes a basic PCB structure
 * @param name Process name (for debugging)
 * @return Pointer to the newly allocated PCB, or NULL on failure
 */
pcb_t* process_create(const char* name);

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

// === Process Memory Management Functions ===

/**
 * @brief Allocates and maps kernel stack pages for a process.
 * 
 * @param proc Pointer to the PCB to setup the kernel stack for.
 * @return true on success, false on failure.
 */
bool allocate_kernel_stack(pcb_t *proc);

/**
 * @brief Frees kernel stack memory for a process.
 * 
 * @param proc Pointer to the PCB whose kernel stack should be freed.
 */
void free_kernel_stack(pcb_t *proc);

// === Process Hierarchy Management Functions ===

/**
 * @brief Establishes parent-child relationship between processes.
 * 
 * @param parent Pointer to the parent PCB.
 * @param child Pointer to the child PCB.
 */
void process_add_child(pcb_t *parent, pcb_t *child);

/**
 * @brief Removes a child from parent's children list.
 * 
 * @param parent Pointer to the parent PCB.
 * @param child Pointer to the child PCB to remove.
 */
void process_remove_child(pcb_t *parent, pcb_t *child);

/**
 * @brief Finds a child process by PID.
 * 
 * @param parent Pointer to the parent PCB.
 * @param child_pid PID of the child to find.
 * @return Pointer to child PCB if found, NULL otherwise.
 */
pcb_t *process_find_child(pcb_t *parent, uint32_t child_pid);

/**
 * @brief Marks a process as exited and notifies parent.
 * 
 * @param proc Pointer to the exiting process PCB.
 * @param exit_status Exit status code.
 */
void process_exit_with_status(pcb_t *proc, uint32_t exit_status);

/**
 * @brief Reaps zombie children and cleans up their resources.
 * 
 * @param parent Pointer to the parent PCB.
 * @param child_pid PID of specific child to reap, or -1 for any child.
 * @param status Pointer to store exit status (can be NULL).
 * @return PID of reaped child, or negative errno.
 */
int process_reap_child(pcb_t *parent, int child_pid, int *status);

/**
 * @brief Load ELF executable and initialize process memory.
 * 
 * @param path Path to the executable file.
 * @param mm Pointer to the process's memory management structure.
 * @param entry_point Output parameter for the ELF entry point virtual address.
 * @param initial_brk Output parameter for the initial program break address.
 * @return 0 on success, negative error code on failure.
 */
int load_elf_and_init_memory(const char *path,
                             struct mm_struct *mm,
                             uint32_t *entry_point,
                             uintptr_t *initial_brk);

/**
 * @brief Initialize process memory management subsystem.
 * 
 * @return E_SUCCESS on success, error code on failure.
 */
error_t process_memory_init(void);

/**
 * @brief Cleanup process memory management subsystem.
 */
void process_memory_cleanup(void);

// === Process Loading Functions ===

/**
 * @brief Allocate a unique process ID.
 * 
 * @return New process ID.
 */
uint32_t allocate_process_id(void);

/**
 * @brief Initialize process loading subsystem.
 * 
 * @return E_SUCCESS on success, error code on failure.
 */
error_t process_loader_init(void);

/**
 * @brief Cleanup process loading subsystem.
 */
void process_loader_cleanup(void);

// === Process Groups and Sessions Management ===

/**
 * @brief Create a new session with the calling process as session leader.
 * @param proc Pointer to the process PCB.
 * @return New session ID on success, negative error code on failure.
 */
int process_setsid(pcb_t *proc);

/**
 * @brief Get the session ID of a process.
 * @param proc Pointer to the process PCB.
 * @return Session ID of the process.
 */
uint32_t process_getsid(pcb_t *proc);

/**
 * @brief Set the process group ID of a process.
 * @param proc Pointer to the process PCB.
 * @param pgid New process group ID (0 = use process PID).
 * @return 0 on success, negative error code on failure.
 */
int process_setpgid(pcb_t *proc, uint32_t pgid);

/**
 * @brief Get the process group ID of a process.
 * @param proc Pointer to the process PCB.
 * @return Process group ID of the process.
 */
uint32_t process_getpgid(pcb_t *proc);

/**
 * @brief Add a process to a process group.
 * @param proc Pointer to the process PCB.
 * @param pgrp_leader Pointer to the process group leader PCB.
 * @return 0 on success, negative error code on failure.
 */
int process_join_pgrp(pcb_t *proc, pcb_t *pgrp_leader);

/**
 * @brief Remove a process from its current process group.
 * @param proc Pointer to the process PCB.
 */
void process_leave_pgrp(pcb_t *proc);

/**
 * @brief Set the foreground process group for a terminal.
 * @param proc Pointer to the process PCB (must be session leader).
 * @param pgid Process group ID to set as foreground.
 * @return 0 on success, negative error code on failure.
 */
int process_tcsetpgrp(pcb_t *proc, uint32_t pgid);

/**
 * @brief Get the foreground process group for a terminal.
 * @param proc Pointer to the process PCB.
 * @return Foreground process group ID, or negative error code.
 */
int process_tcgetpgrp(pcb_t *proc);

/**
 * @brief Initialize process group and session fields for a new process.
 * @param proc Pointer to the process PCB.
 * @param parent Pointer to the parent process PCB (NULL for init).
 */
void process_init_pgrp_session(pcb_t *proc, pcb_t *parent);


#ifdef __cplusplus
}
#endif

#endif // PROCESS_H
