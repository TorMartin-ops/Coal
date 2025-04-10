#include "process.h"
#include "mm.h"         // Include mm header
#include "kmalloc.h"
#include "elf_loader.h"
#include "paging.h"
#include "terminal.h"
#include "types.h"
#include "string.h"
#include "scheduler.h" // For get_current_task

extern uint32_t *kernel_page_directory; // Virtual address
#define KERNEL_PDE_INDEX (KERNEL_SPACE_VIRT_START >> 22)

// User stack configuration
#define USER_STACK_PAGES        4
#define USER_STACK_SIZE         (USER_STACK_PAGES * PAGE_SIZE)
#define USER_STACK_TOP_VIRT_ADDR (KERNEL_SPACE_VIRT_START) // Stack grows down from here
#define USER_STACK_BOTTOM_VIRT  (USER_STACK_TOP_VIRT_ADDR - USER_STACK_SIZE)

static uint32_t next_pid = 1;

// --- Function to get current process ---
// Assumes scheduler provides get_current_task() returning tcb_t*
pcb_t* get_current_process(void) {
    tcb_t* current_tcb = get_current_task(); // From scheduler.h
    if (current_tcb && current_tcb->process) {
        return current_tcb->process;
    }
    terminal_write("[Process] Warning: get_current_process() called with no running task!\n");
    return NULL;
}

// Copies kernel PDE entries to a new page directory (virtual address)
static void copy_kernel_pde_entries(uint32_t *new_pd_virt) {
    // ... (implementation as before) ...
    if (!kernel_page_directory) { /* ... error ... */ return; }
    for (int i = KERNEL_PDE_INDEX; i < 1024; i++) {
        new_pd_virt[i] = kernel_page_directory[i] & ~PAGE_USER; // Ensure kernel only
    }
}

// Allocates kernel stack (implementation as before)
static bool allocate_kernel_stack(pcb_t *proc) {
    // ... (implementation as before - buddy_alloc, check range, calculate virt top) ...
     uint32_t kstack_phys_base = (uint32_t)buddy_alloc(PROCESS_KSTACK_SIZE);
     if (!kstack_phys_base) { return false; }
     proc->kernel_stack_phys_base = kstack_phys_base;
     #define PHYS_MAPPING_SIZE_FOR_KERNEL (64 * 1024 * 1024) // Example
     if (kstack_phys_base + PROCESS_KSTACK_SIZE > PHYS_MAPPING_SIZE_FOR_KERNEL) { buddy_free((void*)kstack_phys_base, PROCESS_KSTACK_SIZE); return false; }
     uintptr_t kstack_virt_top = KERNEL_SPACE_VIRT_START + kstack_phys_base + PROCESS_KSTACK_SIZE;
     proc->kernel_stack_vaddr_top = (uint32_t *)kstack_virt_top;
     return true;
}


// Modified load_elf_binary to create VMAs instead of direct mapping
// It now needs the mm_struct to add VMAs to.
int load_elf_and_create_vmas(const char *path, mm_struct_t *mm, uint32_t *entry_point) {
    size_t file_size = 0;
    void *file_data = read_file(path, &file_size);
    if (!file_data) { /* ... */ return -1; }

    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_data;
    // --- ELF Header Validation ---
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' || ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
       terminal_write("[ELF Loader] Invalid ELF magic.\n"); kfree(file_data); return -1;
    }
    if (ehdr->e_type != 2 || ehdr->e_machine != 3) {
        terminal_write("[ELF Loader] Unsupported ELF type/machine.\n"); kfree(file_data); return -1;
    }
    *entry_point = ehdr->e_entry;

    Elf32_Phdr *phdr = (Elf32_Phdr *)((uint8_t *)file_data + ehdr->e_phoff);

    // --- Iterate Program Headers and Create VMAs ---
    for (Elf32_Half i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue; // Only care about loadable segments

        uint32_t seg_vaddr  = phdr[i].p_vaddr;
        uint32_t seg_filesz = phdr[i].p_filesz;
        uint32_t seg_memsz  = phdr[i].p_memsz;
        uint32_t seg_flags  = phdr[i].p_flags;
        uint32_t seg_offset = phdr[i].p_offset;

        // Align addresses for VMA
        uintptr_t vm_start = PAGE_ALIGN_DOWN(seg_vaddr);
        uintptr_t vm_end = ALIGN_UP(seg_vaddr + seg_memsz, PAGE_SIZE);
        if (vm_end <= vm_start) continue; // Skip empty segments

        // Determine VMA flags
        uint32_t vm_flags = VM_READ | VM_ANONYMOUS; // Default: Read, Anon
        if (seg_flags & 2) vm_flags |= VM_WRITE;
        if (seg_flags & 1) vm_flags |= VM_EXEC;
        // TODO: Could set VM_FILEBACKED if needed later, requires VFS integration

        // Determine Page Protection flags for PTEs
        uint32_t page_prot = PTE_USER_READONLY;
        if (vm_flags & VM_WRITE) page_prot |= PAGE_RW;

        terminal_printf("  ELF Segment %d: [0x%x-0x%x) Flags: V=0x%x P=0x%x\n",
                       i, vm_start, vm_end, vm_flags, page_prot);

        // Create the VMA
        vma_struct_t* vma = insert_vma(mm, vm_start, vm_end, vm_flags, page_prot, NULL, 0);
        if (!vma) {
            terminal_printf("[ELF Loader] Failed to create VMA for segment %d.\n", i);
            // TODO: Need to destroy previously created VMAs for this process
            kfree(file_data);
            return -1;
        }

        // Pre-populate pages for the file size portion (simplistic approach)
        // A true demand loader would skip this and let the PF handler load pages.
        uintptr_t file_data_end_vaddr = seg_vaddr + seg_filesz;
        for (uintptr_t page_v = vm_start; page_v < vm_end; page_v += PAGE_SIZE) {
            // Only pre-populate if page overlaps with file data region
             if (page_v < file_data_end_vaddr) {
                 void* phys_page = buddy_alloc(PAGE_SIZE);
                 if (!phys_page) { /* ... Out of memory ... */ kfree(file_data); return -1; }

                 // Map page temporarily in kernel to copy data
                  if (paging_map_single(kernel_page_directory, TEMP_PT_MAP_ADDR, (uint32_t)phys_page, PTE_KERNEL_DATA) != 0) {
                      terminal_write("[ELF Loader] Failed temporary map for page copy.\n");
                      buddy_free(phys_page, PAGE_SIZE); kfree(file_data); return -1;
                  }
                 uint8_t* page_virt = (uint8_t*)TEMP_PT_MAP_ADDR;

                 // Calculate copy source and destination within the page
                 uintptr_t copy_start_vaddr = (page_v > seg_vaddr) ? page_v : seg_vaddr;
                 uintptr_t copy_end_vaddr = (page_v + PAGE_SIZE < file_data_end_vaddr) ? page_v + PAGE_SIZE : file_data_end_vaddr;
                 uintptr_t copy_len = copy_end_vaddr - copy_start_vaddr;

                 if (copy_len > 0 && copy_len <= PAGE_SIZE) {
                     uintptr_t file_offset = seg_offset + (copy_start_vaddr - seg_vaddr);
                     uintptr_t page_offset = copy_start_vaddr - page_v;
                     memcpy(page_virt + page_offset, (uint8_t*)file_data + file_offset, copy_len);
                 }

                 // Zero BSS part within this page
                 uintptr_t zero_start_vaddr = (copy_end_vaddr > page_v) ? copy_end_vaddr : page_v;
                 uintptr_t zero_end_vaddr = (page_v + PAGE_SIZE);
                 uintptr_t zero_len = (zero_end_vaddr > zero_start_vaddr) ? zero_end_vaddr - zero_start_vaddr : 0;

                 if (zero_len > 0 && zero_len <= PAGE_SIZE) {
                      uintptr_t page_offset = zero_start_vaddr - page_v;
                      memset(page_virt + page_offset, 0, zero_len);
                 }

                 // Unmap temporary page
                 paging_unmap_range(kernel_page_directory, TEMP_PT_MAP_ADDR, PAGE_SIZE);
                 paging_invalidate_page((void*)TEMP_PT_MAP_ADDR);


                 // Map page into process address space (need process PGD)
                 if (paging_map_single(kernel_page_directory, TEMP_PD_MAP_ADDR, (uint32_t)mm->pgd_phys, PTE_KERNEL_DATA)!=0) { /* error */ buddy_free(phys_page, PAGE_SIZE); kfree(file_data); return -1;}
                 uint32_t* proc_pd_virt = (uint32_t*)TEMP_PD_MAP_ADDR;
                 if (paging_map_single(proc_pd_virt, page_v, (uint32_t)phys_page, vma->page_prot) != 0) {
                     /* error */
                      buddy_free(phys_page, PAGE_SIZE);
                      paging_unmap_range(kernel_page_directory, TEMP_PD_MAP_ADDR, PAGE_SIZE);
                      paging_invalidate_page((void*)TEMP_PD_MAP_ADDR);
                      kfree(file_data); return -1;
                 }
                  paging_unmap_range(kernel_page_directory, TEMP_PD_MAP_ADDR, PAGE_SIZE);
                  paging_invalidate_page((void*)TEMP_PD_MAP_ADDR);


             } // end if page overlaps file data
             // Else: Page is pure BSS or beyond file size, will be demand-zeroed by page fault handler
        } // end for loop over pages
    } // end for loop over segments

    kfree(file_data);
    terminal_write("[ELF Loader] VMAs created for ELF segments.\n");
    return 0;
}


/* Creates a user process */
pcb_t *create_user_process(const char *path) {
    terminal_printf("[Process] Creating user process from '%s'.\n", path);

    pcb_t *proc = (pcb_t *)kmalloc(sizeof(pcb_t));
    if (!proc) { return NULL; }
    memset(proc, 0, sizeof(pcb_t));
    proc->pid = next_pid++;

    // Allocate Page Directory (Physical)
    proc->page_directory_phys = (uint32_t *)buddy_alloc(PAGE_SIZE);
    if (!proc->page_directory_phys) { kfree(proc); return NULL; }

    // Temporarily map PD to clear and copy kernel entries
    if (paging_map_single(kernel_page_directory, TEMP_PD_MAP_ADDR, (uint32_t)proc->page_directory_phys, PTE_KERNEL_DATA) != 0) {
         buddy_free(proc->page_directory_phys, PAGE_SIZE); kfree(proc); return NULL;
    }
    uint32_t *proc_pd_virt = (uint32_t*)TEMP_PD_MAP_ADDR;
    memset(proc_pd_virt, 0, PAGE_SIZE);
    copy_kernel_pde_entries(proc_pd_virt);
    paging_unmap_range(kernel_page_directory, TEMP_PD_MAP_ADDR, PAGE_SIZE);
    paging_invalidate_page((void*)TEMP_PD_MAP_ADDR);


    // Allocate Kernel Stack
    if (!allocate_kernel_stack(proc)) {
        buddy_free(proc->page_directory_phys, PAGE_SIZE); kfree(proc); return NULL;
    }

    // *** Create Memory Management Structure ***
    proc->mm = create_mm(proc->page_directory_phys);
    if (!proc->mm) {
        kfree((void*)proc->kernel_stack_phys_base);
        buddy_free(proc->page_directory_phys, PAGE_SIZE);
        kfree(proc);
        return NULL;
    }

    // *** Load ELF and Create VMAs ***
    if (load_elf_and_create_vmas(path, proc->mm, &proc->entry_point) != 0) {
        destroy_mm(proc->mm); // Clean up mm struct and VMA list
        kfree((void*)proc->kernel_stack_phys_base);
        buddy_free(proc->page_directory_phys, PAGE_SIZE); // TODO: Free PTs/Pages allocated by ELF load
        kfree(proc);
        return NULL;
    }

    // *** Create and Map User Stack VMA ***
    proc->user_stack_top = (void *)USER_STACK_TOP_VIRT_ADDR;
    uint32_t stack_vm_flags = VM_READ | VM_WRITE | VM_ANONYMOUS | VM_GROWS_DOWN;
    uint32_t stack_page_prot = PTE_USER_DATA;
    if (!insert_vma(proc->mm, USER_STACK_BOTTOM_VIRT, USER_STACK_TOP_VIRT_ADDR,
                    stack_vm_flags, stack_page_prot, NULL, 0))
    {
        terminal_write("[Process] Failed to create user stack VMA.\n");
        destroy_mm(proc->mm);
        kfree((void*)proc->kernel_stack_phys_base);
        buddy_free(proc->page_directory_phys, PAGE_SIZE); // TODO: Free PTs/Pages
        kfree(proc);
        return NULL;
    }
    // Stack pages will be allocated on demand by the page fault handler.

    terminal_printf("[Process] Successfully created PCB PID %d (Phys PD: 0x%x).\n", proc->pid, (uintptr_t)proc->page_directory_phys);
    return proc;
}

/* Destroys a process */
void destroy_process(pcb_t *pcb) {
    if (!pcb) return;
    terminal_printf("[Process] Destroying process PID %d.\n", pcb->pid);

    // Free the kernel stack
    if (pcb->kernel_stack_phys_base) {
        buddy_free((void *)pcb->kernel_stack_phys_base, PROCESS_KSTACK_SIZE);
    }

    // --- Destroy Memory Management Structures ---
    if (pcb->mm) {
        // TODO: Implement proper unmapping of all pages associated with mm->vma_list
        // This requires iterating VMAs and calling paging_unmap_range for each.
        // Also need to free the physical frames backing the VMAs.
        terminal_write("[Process] TODO: Implement full VMA unmapping and frame freeing in destroy_mm/destroy_process.\n");

        destroy_mm(pcb->mm); // Frees VMA structs and the mm_struct itself
        pcb->mm = NULL;
    }

    // Free the process's page directory physical page
    if (pcb->page_directory_phys) {
        // Only free the directory itself after VMAs and page tables are handled
        buddy_free(pcb->page_directory_phys, PAGE_SIZE);
        pcb->page_directory_phys = NULL;
    }

    // Free the PCB structure
    kfree(pcb);
    terminal_printf("[Process] PCB PID %d destroyed.\n", pcb->pid); // Use saved PID if needed
}