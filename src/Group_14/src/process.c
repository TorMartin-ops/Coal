#include "process.h"
#include "mm.h"
#include "kmalloc.h"
#include "elf_loader.h"
#include "paging.h"     // Includes paging_free_user_space
#include "terminal.h"
#include "types.h"
#include "string.h"
#include "scheduler.h"
#include "read_file.h"
#include "buddy.h"
#include "frame.h"
#include "kmalloc_internal.h"     // Now used directly for kernel stack

// --- Definitions --- (KERNEL_SPACE_VIRT_START, USER_STACK_*, TEMP_PD_MAP_ADDR, etc.)
extern uint32_t* g_kernel_page_directory_virt; // Use globals from paging.c
extern uint32_t g_kernel_page_directory_phys;
#ifndef TEMP_PD_MAP_ADDR
#define TEMP_PD_MAP_ADDR (KERNEL_SPACE_VIRT_START - 1 * PAGE_SIZE)
#endif
#ifndef TEMP_PT_MAP_ADDR
#define TEMP_PT_MAP_ADDR (KERNEL_SPACE_VIRT_START - 2 * PAGE_SIZE)
#endif
// PROCESS_KSTACK_SIZE should likely be PAGE_SIZE if using frame_alloc
#undef PROCESS_KSTACK_SIZE
#define PROCESS_KSTACK_SIZE PAGE_SIZE


static uint32_t next_pid = 1;

// get_current_process() remains the same
pcb_t* get_current_process(void) {
    tcb_t* current_tcb = get_current_task();
    if (current_tcb && current_tcb->process) {
        return current_tcb->process;
    }
    return NULL;
}


// copy_kernel_pde_entries() remains the same
static void copy_kernel_pde_entries(uint32_t *new_pd_virt) {
    if (!g_kernel_page_directory_virt) { return; }
    for (int i = KERNEL_PDE_INDEX; i < 1024; i++) {
        if (g_kernel_page_directory_virt[i] & PAGE_PRESENT) {
            new_pd_virt[i] = g_kernel_page_directory_virt[i] & ~PAGE_USER;
        } else {
            new_pd_virt[i] = 0;
        }
    }
}

// Allocates kernel stack using frame_alloc
static bool allocate_kernel_stack(pcb_t *proc) {
     // Kernel stacks are typically page-aligned and page-sized. Use frame_alloc.
     uintptr_t kstack_phys_base = frame_alloc(); // Allocate one page frame
     if (!kstack_phys_base) {
         terminal_write("[Process] Failed to allocate kernel stack frame.\n");
         return false;
     }
     proc->kernel_stack_phys_base = (uint32_t)kstack_phys_base;

     // Calculate virtual address for the top of the stack in higher-half
     uintptr_t kstack_virt_base = KERNEL_SPACE_VIRT_START + kstack_phys_base;
     uintptr_t kstack_virt_top = kstack_virt_base + PROCESS_KSTACK_SIZE; // Use defined size (PAGE_SIZE)
     proc->kernel_stack_vaddr_top = (uint32_t *)kstack_virt_top;

     terminal_printf("  Allocated kernel stack: Phys=0x%x, VirtTop=0x%x\n",
                     kstack_phys_base, kstack_virt_top);
     return true;
}

// load_elf_and_init_memory() remains the same
int load_elf_and_init_memory(const char *path, mm_struct_t *mm, uint32_t *entry_point, uintptr_t *initial_brk) {
    // ... (implementation from previous step is fine) ...
    // Reads ELF, creates VMAs, pre-populates pages, sets initial_brk
    size_t file_size = 0;
    void *file_data = read_file(path, &file_size);
    if (!file_data) return -1;

    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_data;
    if (memcmp(ehdr->e_ident, "\x7F" "ELF", 4) != 0) { kfree(file_data); return -1; }
    if (ehdr->e_type != 2 || ehdr->e_machine != 3) { kfree(file_data); return -1; }
    *entry_point = ehdr->e_entry;

    Elf32_Phdr *phdr = (Elf32_Phdr *)((uint8_t *)file_data + ehdr->e_phoff);
    uintptr_t max_addr = 0;

    for (Elf32_Half i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD || phdr[i].p_memsz == 0) continue;
        uint32_t seg_vaddr = phdr[i].p_vaddr, seg_filesz = phdr[i].p_filesz, seg_memsz = phdr[i].p_memsz;
        uint32_t seg_flags = phdr[i].p_flags, seg_offset = phdr[i].p_offset;
        uintptr_t vm_start = PAGE_ALIGN_DOWN(seg_vaddr);
        uintptr_t vm_end = ALIGN_UP(seg_vaddr + seg_memsz, PAGE_SIZE);
        uint32_t vm_flags = VM_READ | VM_ANONYMOUS;
        if (seg_flags & 2) { vm_flags |= VM_WRITE; }
        if (seg_flags & 1) { vm_flags |= VM_EXEC; }
        uint32_t page_prot = PAGE_PRESENT | PAGE_USER;
        if (vm_flags & VM_WRITE) page_prot |= PAGE_RW;

        vma_struct_t* vma = insert_vma(mm, vm_start, vm_end, vm_flags, page_prot, NULL, 0);
        if (!vma) { kfree(file_data); return -1; }
        if ((seg_vaddr + seg_memsz) > max_addr) max_addr = seg_vaddr + seg_memsz;

        // Pre-population loop (simplified - full code in previous responses)
         for (uintptr_t page_v = vm_start; page_v < vm_end; page_v += PAGE_SIZE) {
             uintptr_t phys_page_addr = frame_alloc(); if (!phys_page_addr) { kfree(file_data); return -1; }
             // Map temp kernel, copy/zero, unmap temp kernel
             // Map process page
             int map_res = paging_map_single_4k(mm->pgd_phys, page_v, phys_page_addr, page_prot);
             if (map_res != 0) { put_frame(phys_page_addr); kfree(file_data); return -1;}
         }
    }
    *initial_brk = ALIGN_UP(max_addr, PAGE_SIZE);
    kfree(file_data);
    return 0;
}

/* Creates a user process */
pcb_t *create_user_process(const char *path) {
    terminal_printf("[Process] Creating user process from '%s'.\n", path);

    pcb_t *proc = (pcb_t *)kmalloc(sizeof(pcb_t));
    if (!proc) { return NULL; }
    memset(proc, 0, sizeof(pcb_t));
    proc->pid = next_pid++;

    // Allocate Page Directory frame using frame_alloc
    uintptr_t pd_phys_addr = frame_alloc();
    if (!pd_phys_addr) { kfree(proc); return NULL; }
    proc->page_directory_phys = (uint32_t*)pd_phys_addr;

    // Temporarily map PD to clear and copy kernel entries
    if (paging_map_single_4k((uint32_t*)g_kernel_page_directory_phys, TEMP_PD_MAP_ADDR, (uint32_t)proc->page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
         put_frame(pd_phys_addr); kfree(proc); return NULL;
    }
    uint32_t *proc_pd_virt = (uint32_t*)TEMP_PD_MAP_ADDR;
    memset(proc_pd_virt, 0, PAGE_SIZE);
    copy_kernel_pde_entries(proc_pd_virt);
    paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_PD_MAP_ADDR, PAGE_SIZE);
    paging_invalidate_page((void*)TEMP_PD_MAP_ADDR);

    // Allocate Kernel Stack using frame_alloc via helper
    if (!allocate_kernel_stack(proc)) {
        put_frame(pd_phys_addr); kfree(proc); return NULL;
    }

    // Create MM Structure
    proc->mm = create_mm(proc->page_directory_phys);
    if (!proc->mm) {
        put_frame(proc->kernel_stack_phys_base); // Use put_frame for stack
        put_frame(pd_phys_addr);
        kfree(proc);
        return NULL;
    }

    // Load ELF, Create VMAs, Set Initial Break
    uintptr_t initial_brk_addr;
    if (load_elf_and_init_memory(path, proc->mm, &proc->entry_point, &initial_brk_addr) != 0) {
        destroy_mm(proc->mm); // Handles VMA cleanup
        put_frame(proc->kernel_stack_phys_base); // Use put_frame
        paging_free_user_space(proc->page_directory_phys); // Cleanup PTs
        put_frame(pd_phys_addr); // Free PD frame
        kfree(proc);
        return NULL;
    }
    proc->mm->start_brk = initial_brk_addr;
    proc->mm->end_brk = initial_brk_addr;

    // Create Initial Heap VMA
    uint32_t heap_vm_flags = VM_READ | VM_WRITE | VM_ANONYMOUS;
    uint32_t heap_page_prot = PTE_USER_DATA_FLAGS; 
    if (!insert_vma(proc->mm, initial_brk_addr, initial_brk_addr, heap_vm_flags, heap_page_prot, NULL, 0)) { /* Warning */ }

    // Create User Stack VMA
    proc->user_stack_top = (void *)USER_STACK_TOP_VIRT_ADDR;
    uint32_t stack_vm_flags = VM_READ | VM_WRITE | VM_ANONYMOUS | VM_GROWS_DOWN;
    uint32_t stack_page_prot = PTE_USER_DATA_FLAGS;
    if (!insert_vma(proc->mm, USER_STACK_BOTTOM_VIRT, USER_STACK_TOP_VIRT_ADDR,
                    stack_vm_flags, stack_page_prot, NULL, 0)) {
        destroy_mm(proc->mm);
        put_frame(proc->kernel_stack_phys_base); // Use put_frame
        paging_free_user_space(proc->page_directory_phys);
        put_frame(pd_phys_addr);
        kfree(proc);
        return NULL;
    }

    terminal_printf("[Process] Successfully created PCB PID %d\n", proc->pid);
    return proc;
}

/* Destroys a process - Resource cleanup already improved in previous step */
void destroy_process(pcb_t *pcb) {
    if (!pcb) return;
    uint32_t pid = pcb->pid;
    terminal_printf("[Process] Destroying process PID %d.\n", pid);

    // Destroy Memory Management (includes VMA cleanup -> unmap data pages via put_frame)
    if (pcb->mm) {
        destroy_mm(pcb->mm);
        pcb->mm = NULL;
    }

    // Free User-Space Page Tables (calls put_frame on PT frames)
    if (pcb->page_directory_phys) {
        paging_free_user_space(pcb->page_directory_phys);
    }

    // Free Kernel Stack Frame (Use put_frame)
    if (pcb->kernel_stack_phys_base) {
        put_frame(pcb->kernel_stack_phys_base);
        pcb->kernel_stack_phys_base = 0;
    }

    // Free Page Directory Frame (Use put_frame)
    if (pcb->page_directory_phys) {
        put_frame((uintptr_t)pcb->page_directory_phys);
        pcb->page_directory_phys = NULL;
    }

    // Free the PCB structure itself
    kfree(pcb);
    terminal_printf("[Process] PCB PID %d destroyed.\n", pid);
}