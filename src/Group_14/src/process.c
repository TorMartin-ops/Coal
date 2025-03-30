#include "process.h"
#include "kmalloc.h"       // Unified allocator for PCB and page tables.
#include "elf_loader.h"    // ELF loader for user binaries.
#include "paging.h"        // Paging functions to set up process page tables.
#include "terminal.h"      // For debug output

#include "libc/stddef.h"
#include "libc/stdint.h"
#include "libc/stdbool.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

// Global process list maintained as a circular linked list.
static process_t *process_list = 0;
static uint32_t next_pid = 1;

/**
 * create_user_process
 *
 * Loads an ELF binary specified by `path` into a new process address space.
 * It allocates a new PCB, creates a new page directory, and sets up the process' code and stack.
 *
 * @param path Path to the ELF binary file.
 * @return Pointer to the new process control block, or NULL if creation fails.
 */
process_t *create_user_process(const char *path) {
    // Allocate a new PCB.
    process_t *proc = (process_t *)kmalloc(sizeof(process_t));
    if (!proc) {
        terminal_write("create_user_process: Failed to allocate PCB.\n");
        return NULL;
    }
    proc->pid = next_pid++;

    // Allocate a new page directory for the process.
    // In a production system, ensure that the page directory is page-aligned.
    proc->page_directory = (uint32_t *)kmalloc(PAGE_SIZE);
    if (!proc->page_directory) {
        terminal_write("create_user_process: Failed to allocate page directory.\n");
        return NULL;
    }
    // Initialize the page directory. In a robust system, copy kernel mappings as needed.
    for (int i = 0; i < 1024; i++)
        proc->page_directory[i] = 0;

    // Load the ELF binary into the process's address space.
    // This function sets the entry point and maps the process segments.
    if (load_elf_binary(path, proc->page_directory, &proc->entry_point) != 0) {
        terminal_write("create_user_process: Failed to load ELF binary.\n");
        return NULL;
    }

    // Allocate a user-mode stack (typically one page).
    proc->user_stack = kmalloc(PAGE_SIZE);
    if (!proc->user_stack) {
        terminal_write("create_user_process: Failed to allocate user stack.\n");
        return NULL;
    }

    // In a full implementation, additional initialization is needed:
    // - Set up initial CPU register context (e.g., EIP=entry_point, ESP=user_stack top).
    // - Configure user segment selectors in the PCB.
    // - Possibly copy kernel mappings into the user page directory as needed.
    terminal_write("User process created successfully.\n");
    return proc;
}

/**
 * add_process_to_scheduler
 *
 * Adds the given process to the scheduler's process list (implemented as a circular linked list).
 *
 * @param proc Pointer to the process to add.
 */
void add_process_to_scheduler(process_t *proc) {
    if (!proc) return;
    
    if (!process_list) {
        process_list = proc;
        proc->next = proc;  // First process points to itself.
    } else {
        process_t *temp = process_list;
        while (temp->next != process_list)
            temp = temp->next;
        temp->next = proc;
        proc->next = process_list;
    }
    terminal_write("Process added to scheduler.\n");
}
