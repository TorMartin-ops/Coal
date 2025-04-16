#pragma once
#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"
#include "paging.h" // Include for PAGE_SIZE, KERNEL_SPACE_VIRT_START

// Forward declare mm_struct to avoid circular dependency with mm.h
struct mm_struct;

// Define the size for the kernel stack allocated per process
#define PROCESS_KSTACK_SIZE (PAGE_SIZE*4) // Increased from 1 page to 4 pages (16KB)

// *** Moved User Stack Defines Here ***
#define USER_STACK_PAGES        4
#define USER_STACK_SIZE         (USER_STACK_PAGES * PAGE_SIZE)
#define USER_STACK_TOP_VIRT_ADDR (KERNEL_SPACE_VIRT_START) // Stack grows down from here
#define USER_STACK_BOTTOM_VIRT  (USER_STACK_TOP_VIRT_ADDR - USER_STACK_SIZE)


#ifdef __cplusplus
extern "C" {
#endif

typedef struct pcb {
    uint32_t pid;
    uint32_t *page_directory_phys; // Physical address
    uint32_t entry_point;          // Virtual address
    void *user_stack_top;          // Virtual address

    // Kernel Stack Info
    uint32_t kernel_stack_phys_base;
    uint32_t *kernel_stack_vaddr_top;

    // MMU structure
    struct mm_struct *mm;

} pcb_t;

pcb_t *create_user_process(const char *path);
void destroy_process(pcb_t *pcb);
pcb_t* get_current_process(void);


#ifdef __cplusplus
}
#endif

#endif // PROCESS_H