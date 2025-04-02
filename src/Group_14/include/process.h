#pragma once
#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"

// Define the size for the kernel stack allocated per process
#define PROCESS_KSTACK_SIZE 4096

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pcb {
    uint32_t pid;
    uint32_t *page_directory;   // Physical address
    uint32_t entry_point;       // Virtual address
    void *user_stack_top;       // Virtual address

    // Kernel Stack Info
    uint32_t kernel_stack_phys_base; // Physical base address (for freeing)
    uint32_t *kernel_stack_vaddr_top; // Virtual top address (for ESP setup)

    // Add other process-specific info...
} pcb_t;

pcb_t *create_user_process(const char *path);
void destroy_process(pcb_t *pcb);

#ifdef __cplusplus
}
#endif

#endif // PROCESS_H