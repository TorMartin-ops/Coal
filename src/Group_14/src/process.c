#include "process.h"
#include "kmalloc.h"       // For PCB, PDE memory
#include "elf_loader.h"    // ELF loader
#include "paging.h"        // For paging_map_range, PDE copying
#include "terminal.h"      // For debug logs
#include "types.h"
#include "string.h"        // for memset, memcpy if needed

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

// Suppose we keep a pointer to the kernel's PDE
extern uint32_t *kernel_page_directory;  

// Suppose PDE indices 768..1023 (3GB..4GB) are kernel
#define KERNEL_SPACE_PDE_START 768   // 768*4MB=3GB
#define USER_STACK_TOP 0xBFFF0000    // Some address below 3GB

// Global process list and next PID
static process_t *process_list = NULL;
static uint32_t next_pid = 1;

/**
 * copy_kernel_pde_entries
 *
 * Copies the kernel portion of PDE from 'kernel_page_directory' 
 * into 'new_pd', so the new process can access kernel memory 
 * from 0xC0000000..0xFFFFFFFF while having its own user PDE below.
 * 
 * For a typical higher-half kernel, PDE entries 768..1023 
 * might contain kernel space. We replicate them.
 */
static void copy_kernel_pde_entries(uint32_t *new_pd) {
    // new_pd is zeroed, so we just copy PDE [768..1023].
    for (int i = KERNEL_SPACE_PDE_START; i < 1024; i++) {
        new_pd[i] = kernel_page_directory[i];
    }
}

/**
 * map_user_stack
 *
 * Maps a single page at 'USER_STACK_TOP - PAGE_SIZE' so the user 
 * stack can start near 0xBFFF0000. 
 * We'll store the pointer to that memory in 'proc->user_stack' 
 * or we might just store the top address for ring 3 context.
 */
static bool map_user_stack(process_t *proc) {
    // Let's choose stack base at (USER_STACK_TOP - PAGE_SIZE).
    uint32_t stack_base = (USER_STACK_TOP - PAGE_SIZE);
    // We'll pass PAGE_PRESENT|PAGE_RW|PAGE_USER as flags
    // so user-mode can read/write.
    int result = paging_map_range(proc->page_directory, stack_base, PAGE_SIZE,
                                  PAGE_PRESENT | PAGE_RW | PAGE_USER);
    if (result != 0) {
        terminal_write("[process] map_user_stack: paging_map_range failed.\n");
        return false;
    }
    // We'll store user_stack as the top (empty) address
    // so the user-mode initial ESP is 0xBFFF0000.
    proc->user_stack = (void *)USER_STACK_TOP;
    return true;
}

/**
 * create_user_process
 *
 * The “real–deal” version: 
 *   1) Allocates a PCB
 *   2) Creates a PDE, copies kernel PDE entries
 *   3) Loads ELF segments
 *   4) Maps a user stack
 *   5) On success, returns the new PCB
 *   6) On failure, cleans up partial resources
 */
process_t *create_user_process(const char *path) {
    // 1) Allocate a new PCB
    process_t *proc = (process_t *)kmalloc(sizeof(process_t));
    if (!proc) {
        terminal_write("[process] create_user_process: no memory for PCB.\n");
        return NULL;
    }
    // Initialize some fields
    proc->pid = next_pid++;
    proc->next = NULL;
    proc->entry_point = 0;
    proc->user_stack = NULL;  
    // 2) PDE
    proc->page_directory = (uint32_t *)kmalloc(PAGE_SIZE);
    if (!proc->page_directory) {
        terminal_write("[process] PDE alloc failed.\n");
        kfree(proc, sizeof(process_t));
        return NULL;
    }
    // Zero the PDE
    memset(proc->page_directory, 0, PAGE_SIZE);

    // Copy kernel PDE entries
    copy_kernel_pde_entries(proc->page_directory);

    // 3) Load the ELF
    if (load_elf_binary(path, proc->page_directory, &proc->entry_point) != 0) {
        terminal_write("[process] ELF load failed.\n");
        // cleanup
        kfree(proc->page_directory, PAGE_SIZE);
        kfree(proc, sizeof(process_t));
        return NULL;
    }

    // 4) Map user stack near 0xBFFF0000
    if (!map_user_stack(proc)) {
        // cleanup
        kfree(proc->page_directory, PAGE_SIZE);
        kfree(proc, sizeof(process_t));
        return NULL;
    }

    terminal_write("[process] Created user process: PID=");
    // Optionally convert pid to string
    // ...
    terminal_write("\n");
    return proc;
}

/**
 * add_process_to_scheduler
 *
 * Inserts 'proc' into a circular list. 
 * Real OS might do more advanced logic (priority, queues, etc.).
 *
 * concurrency disclaimers:
 *   If SMP or interrupts, disable or spinlock around 'process_list'.
 */
void add_process_to_scheduler(process_t *proc) {
    if (!proc) {
        terminal_write("[process] add_process_to_scheduler: proc is NULL.\n");
        return;
    }
    if (!process_list) {
        process_list = proc;
        proc->next = proc;
    } else {
        process_t *temp = process_list;
        while (temp->next != process_list) {
            temp = temp->next;
        }
        temp->next = proc;
        proc->next = process_list;
    }
    terminal_write("[process] Added process to scheduler (PID=");
    // ...
    terminal_write(")\n");
}
