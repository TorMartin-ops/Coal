#include "process.h"
#include "kmalloc.h"
#include "elf_loader.h"
#include "paging.h"     // Includes PAGE_SIZE, KERNEL_SPACE_VIRT_START etc.
#include "terminal.h"
#include "types.h"
#include "string.h"

extern uint32_t *kernel_page_directory; // From paging.c
#define KERNEL_PDE_INDEX (KERNEL_SPACE_VIRT_START >> 22)

#define USER_STACK_PAGES        1
#define USER_STACK_SIZE         (USER_STACK_PAGES * PAGE_SIZE)
#define USER_STACK_BOTTOM_VIRT  (KERNEL_SPACE_VIRT_START - USER_STACK_SIZE)
#define USER_STACK_TOP_VIRT_ADDR KERNEL_SPACE_VIRT_START

static uint32_t next_pid = 1;

static void copy_kernel_pde_entries(uint32_t *new_pd) {
    // ... (same as before) ...
    if (!kernel_page_directory) {
        terminal_write("[Process] Error: Kernel page directory is NULL in copy_kernel_pde_entries.\n");
        return;
    }
    for (int i = KERNEL_PDE_INDEX; i < 1024; i++) {
        new_pd[i] = kernel_page_directory[i];
    }
}

static bool map_user_stack(pcb_t *proc) {
    // ... (same as before) ...
    terminal_printf("[Process] Mapping user stack at 0x%x - 0x%x\n", USER_STACK_BOTTOM_VIRT, USER_STACK_TOP_VIRT_ADDR);
    int result = paging_map_range(proc->page_directory,
                                  USER_STACK_BOTTOM_VIRT,
                                  USER_STACK_SIZE,
                                  PAGE_PRESENT | PAGE_RW | PAGE_USER);
    if (result != 0) {
        terminal_write("[Process] map_user_stack: paging_map_range failed.\n");
        return false;
    }
    proc->user_stack_top = (void *)USER_STACK_TOP_VIRT_ADDR;
    return true;
}

/**
 * @brief Allocates a kernel stack and calculates its virtual top address.
 */
static bool allocate_kernel_stack(pcb_t *proc) {
     // Allocate physical memory for the stack
     uint32_t kstack_phys_base = (uint32_t)kmalloc(PROCESS_KSTACK_SIZE);
     if (!kstack_phys_base) {
         terminal_write("[Process] Failed to allocate kernel stack physical memory.\n");
         return false;
     }
     proc->kernel_stack_phys_base = kstack_phys_base; // Store physical base for freeing

     // Calculate the corresponding virtual address top using the higher-half mapping
     // Assumes the physical memory [0..X] is mapped starting at KERNEL_SPACE_VIRT_START
     // Check if the allocated physical address is within the mapped range
     // Example mapping range from kernel.c: PHYS_MAPPING_SIZE = 64MB
     // This check might need adjustment based on your actual mapping size.
     #define PHYS_MAPPING_SIZE_FOR_KERNEL (64 * 1024 * 1024)
     if (kstack_phys_base + PROCESS_KSTACK_SIZE > PHYS_MAPPING_SIZE_FOR_KERNEL) {
         terminal_printf("[Process] Error: Allocated kernel stack (phys 0x%x) is outside initial kernel mapping range (up to 0x%x).\n",
                         kstack_phys_base, PHYS_MAPPING_SIZE_FOR_KERNEL);
         kfree((void*)kstack_phys_base, PROCESS_KSTACK_SIZE); // Free the unusable stack
         return false;
     }

     uintptr_t kstack_virt_top = KERNEL_SPACE_VIRT_START + kstack_phys_base + PROCESS_KSTACK_SIZE;
     proc->kernel_stack_vaddr_top = (uint32_t *)kstack_virt_top; // Store VIRTUAL top address

     terminal_printf("[Process] Allocated kernel stack for PID %d at phys [0x%x - 0x%x], virt top 0x%x\n",
                     proc->pid, kstack_phys_base, kstack_phys_base + PROCESS_KSTACK_SIZE, (uintptr_t)proc->kernel_stack_vaddr_top);
     return true;
}

pcb_t *create_user_process(const char *path) {
    // ... (Allocation of PCB, Page Directory same as before) ...
     terminal_printf("[Process] Creating user process from '%s'.\n", path);

    pcb_t *proc = (pcb_t *)kmalloc(sizeof(pcb_t));
    if (!proc) { /* ... error handling ... */ terminal_write("[Process] create_user_process: Failed to allocate PCB.\n"); return NULL; }
    memset(proc, 0, sizeof(pcb_t));
    proc->pid = next_pid++;

    proc->page_directory = (uint32_t *)kmalloc(PAGE_SIZE);
    if (!proc->page_directory) { /* ... error handling ... */ terminal_write("[Process] create_user_process: Failed to allocate page directory.\n"); kfree(proc, sizeof(pcb_t)); return NULL; }
    memset(proc->page_directory, 0, PAGE_SIZE);
    copy_kernel_pde_entries(proc->page_directory);
    terminal_printf("[Process] Allocated page directory for PID %d at 0x%x\n", proc->pid, proc->page_directory);

    // Allocate Kernel Stack (gets virtual top address)
    if (!allocate_kernel_stack(proc)) {
        kfree(proc->page_directory, PAGE_SIZE);
        kfree(proc, sizeof(pcb_t));
        return NULL;
    }

    // Load ELF
    if (load_elf_binary(path, proc->page_directory, &proc->entry_point) != 0) {
        // Clean up stack, page dir, pcb
        kfree((void*)proc->kernel_stack_phys_base, PROCESS_KSTACK_SIZE);
        kfree(proc->page_directory, PAGE_SIZE);
        kfree(proc, sizeof(pcb_t));
        return NULL;
    }
     terminal_printf("[Process] ELF loaded for PID %d, entry point: 0x%x\n", proc->pid, proc->entry_point);

    // Map User Stack
    if (!map_user_stack(proc)) {
        // Clean up ELF pages?, stack, page dir, pcb
        kfree((void*)proc->kernel_stack_phys_base, PROCESS_KSTACK_SIZE);
        kfree(proc->page_directory, PAGE_SIZE);
        kfree(proc, sizeof(pcb_t));
        return NULL;
    }
     terminal_printf("[Process] Mapped user stack for PID %d at 0x%x\n", proc->pid, proc->user_stack_top);

    terminal_printf("[Process] Successfully created PCB for PID %d.\n", proc->pid);
    return proc;
}

void destroy_process(pcb_t *pcb) {
    if (!pcb) return;
    terminal_printf("[Process] Destroying process PID %d.\n", pcb->pid);

    // Free the kernel stack using its physical base address
    if (pcb->kernel_stack_phys_base) {
        terminal_printf("[Process] Freeing kernel stack for PID %d (phys_base: 0x%x).\n", pcb->pid, pcb->kernel_stack_phys_base);
        kfree((void *)pcb->kernel_stack_phys_base, PROCESS_KSTACK_SIZE);
    }

    // Free the process's page directory and associated page tables
    if (pcb->page_directory) {
        terminal_printf("[Process] Freeing page directory for PID %d (addr: 0x%x).\n", pcb->pid, pcb->page_directory);
        // TODO: Implement page table freeing
        terminal_write("[Process] TODO: Implement page table freeing in destroy_process.\n");
        kfree(pcb->page_directory, PAGE_SIZE);
    }

    // Free the PCB structure
    terminal_printf("[Process] Freeing PCB for PID %d.\n", pcb->pid);
    kfree(pcb, sizeof(pcb_t));
}