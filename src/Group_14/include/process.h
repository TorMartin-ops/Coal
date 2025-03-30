#pragma once
#ifndef PROCESS_H
#define PROCESS_H

#include "libc/stddef.h"   // For size_t, NULL
#include "libc/stdint.h"   // For uint32_t, etc.
#include "libc/stdbool.h"  // For bool, true, false

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Process Control Block (PCB) structure.
 *
 * Represents a user process with its own page directory, entry point, user stack,
 * and additional context for scheduling. Processes are maintained in a circular linked list
 * for round-robin scheduling.
 */
typedef struct process {
    uint32_t pid;              // Unique process identifier.
    uint32_t *page_directory;  // Process-specific page directory pointer.
    uint32_t entry_point;      // ELF entry point for user-mode execution.
    void *user_stack;          // Pointer to the allocated user-mode stack.
    // Additional fields such as register context (EIP, ESP, etc.) can be added later.
    struct process *next;      // Pointer to the next process in the scheduler's list.
} process_t;

/**
 * @brief Creates a new user process by loading an ELF binary.
 *
 * This function allocates a new PCB, creates a new page directory for the process,
 * loads the ELF binary into the process's address space, and allocates a user stack.
 *
 * @param path Path to the ELF binary to load.
 * @return Pointer to the newly created process structure, or NULL on failure.
 */
process_t *create_user_process(const char *path);

/**
 * @brief Adds a process to the scheduler's process list.
 *
 * For simplicity, the process is added into a circular linked list.
 *
 * @param proc Pointer to the process structure to add.
 */
void add_process_to_scheduler(process_t *proc);

#ifdef __cplusplus
}
#endif

#endif // PROCESS_H
