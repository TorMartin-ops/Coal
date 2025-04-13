#include "process.h"
#include "mm.h"
#include "kmalloc.h"
#include "elf_loader.h"
#include "paging.h"     // Includes paging_free_user_space, flags, etc.
#include "terminal.h"
#include "types.h"
#include "string.h"
#include "scheduler.h"
#include "read_file.h"
#include "buddy.h"      // Needed for underlying frame allocator
#include "frame.h"
#include "kmalloc_internal.h" // For ALIGN_UP

// --- Definitions ---
extern uint32_t* g_kernel_page_directory_virt; // Use globals from paging.c
extern uint32_t g_kernel_page_directory_phys;
extern bool g_nx_supported; // From paging.c

// Define temporary mapping address directly if not already defined
#ifndef TEMP_PD_MAP_ADDR
#define TEMP_PD_MAP_ADDR (KERNEL_SPACE_VIRT_START - 1 * PAGE_SIZE)
#endif
#ifndef TEMP_MAP_ADDR_PF // Define temp address for page frame mapping
#define TEMP_MAP_ADDR_PF (KERNEL_SPACE_VIRT_START - 3 * PAGE_SIZE)
#endif

// Ensure PROCESS_KSTACK_SIZE is defined (it's undef'd and redf'd in your original)
#undef PROCESS_KSTACK_SIZE
#define PROCESS_KSTACK_SIZE PAGE_SIZE


static uint32_t next_pid = 1;

pcb_t* get_current_process(void) {
    tcb_t* current_tcb = get_current_task();
    if (current_tcb && current_tcb->process) {
        return current_tcb->process;
    }
    return NULL;
}

// Copies kernel entries (PDEs for indices >= KERNEL_PDE_INDEX)
static void copy_kernel_pde_entries(uint32_t *new_pd_virt) {
    if (!g_kernel_page_directory_virt) {
        terminal_write("[Process] Error: copy_kernel_pde_entries called when kernel PD virtual addr is NULL!\n");
        return;
    }
    for (size_t i = KERNEL_PDE_INDEX; i < TABLES_PER_DIR; i++) { // Use size_t for loop
        // Skip recursive entry, let caller handle it for the new PD.
        if (i == RECURSIVE_PDE_INDEX) {
             new_pd_virt[i] = 0;
             continue;
        }
        if (g_kernel_page_directory_virt[i] & PAGE_PRESENT) {
            // Copy kernel PDE, ensuring USER flag is cleared
            new_pd_virt[i] = g_kernel_page_directory_virt[i] & ~PAGE_USER;
        } else {
            new_pd_virt[i] = 0;
        }
    }
}

// Allocates kernel stack using frame_alloc
static bool allocate_kernel_stack(pcb_t *proc) {
     uintptr_t kstack_phys_base = frame_alloc();
     if (!kstack_phys_base) {
         terminal_write("[Process] Failed to allocate kernel stack frame.\n");
         return false;
     }
     proc->kernel_stack_phys_base = (uint32_t)kstack_phys_base;

     uintptr_t kstack_virt_base = KERNEL_SPACE_VIRT_START + kstack_phys_base;
     uintptr_t kstack_virt_top = kstack_virt_base + PROCESS_KSTACK_SIZE;
     proc->kernel_stack_vaddr_top = (uint32_t *)kstack_virt_top;

     terminal_printf("  Allocated kernel stack: Phys=0x%x, Expected VirtTop=0x%x\n",
                     kstack_phys_base, kstack_virt_top);
     return true;
}

// Helper to map, copy, and unmap temporarily for ELF loading
static int copy_elf_segment_data(uintptr_t frame_paddr,
                                 const uint8_t* file_data_buffer, size_t file_buffer_offset,
                                 size_t size_to_copy, size_t zero_padding)
{
    void* temp_map_addr = (void*)TEMP_MAP_ADDR_PF;

    // Map the physical frame into kernel space temporarily
    if (paging_map_single_4k((uint32_t*)g_kernel_page_directory_phys, (uintptr_t)temp_map_addr, frame_paddr, PTE_KERNEL_DATA_FLAGS) != 0) {
        terminal_printf("[Process] Failed to temp map frame 0x%x to V=0x%p for ELF copy.\n", frame_paddr, temp_map_addr);
        return -1;
    }

    // Copy data from ELF file buffer
    if (size_to_copy > 0) {
        memcpy(temp_map_addr, file_data_buffer + file_buffer_offset, size_to_copy);
    }
    // Zero out padding (BSS section)
    if (zero_padding > 0) {
        // Ensure we don't write past the end of the temp mapped page
        size_t offset_after_copy = size_to_copy;
        if (offset_after_copy < PAGE_SIZE) {
             size_t zero_clamped = (offset_after_copy + zero_padding > PAGE_SIZE) ? (PAGE_SIZE - offset_after_copy) : zero_padding;
             memset((uint8_t*)temp_map_addr + offset_after_copy, 0, zero_clamped);
        }
    }

    // Unmap the temporary kernel mapping
    paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, (uintptr_t)temp_map_addr, PAGE_SIZE);
    paging_invalidate_page(temp_map_addr); // Flush TLB for the temp address

    return 0; // Success
}


/**
 * @brief Loads ELF segments into the process address space.
 * Allocates physical frames, maps them to user virtual addresses,
 * and copies/zeroes data from the ELF file buffer.
 *
 * @param path Path to the ELF file (for logging).
 * @param mm Process memory manager struct.
 * @param entry_point Output pointer for ELF entry point.
 * @param initial_brk Output pointer for initial program break address.
 * @return 0 on success, negative error code on failure.
 */
int load_elf_and_init_memory(const char *path, mm_struct_t *mm, uint32_t *entry_point, uintptr_t *initial_brk) {
    size_t file_size = 0;
    uint8_t *file_data = (uint8_t*)read_file(path, &file_size); // Read as bytes
    if (!file_data) {
         terminal_printf("[Process] load_elf: read_file failed for '%s'.\n", path);
        return -1;
    }

    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_data;

    // Validate ELF header
    if (memcmp(ehdr->e_ident, "\x7F" "ELF", 4) != 0) { terminal_printf("[Process] load_elf: Invalid ELF magic.\n"); kfree(file_data); return -1; }
    if (ehdr->e_type != 2 /* ET_EXEC */ || ehdr->e_machine != 3 /* EM_386 */ || ehdr->e_version != 1 /* EV_CURRENT */) {
         terminal_printf("[Process] load_elf: Invalid ELF type/machine/version.\n"); kfree(file_data); return -1;
    }
    if (ehdr->e_phentsize != sizeof(Elf32_Phdr)) { terminal_printf("[Process] load_elf: Invalid program header size.\n"); kfree(file_data); return -1; }
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) { terminal_printf("[Process] load_elf: No program headers found.\n"); kfree(file_data); return -1; }

    *entry_point = ehdr->e_entry;
    terminal_printf("  ELF Entry Point: 0x%x\n", *entry_point);

    Elf32_Phdr *phdr_table = (Elf32_Phdr *)(file_data + ehdr->e_phoff);
    uintptr_t highest_addr_loaded = 0;

    // --- Iterate through Program Headers ---
    for (Elf32_Half i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr *phdr = &phdr_table[i];

        if (phdr->p_type != PT_LOAD) { continue; } // Skip non-loadable

        terminal_printf("  Processing Segment %d: VAddr=0x%x, MemSz=%u, FileSz=%u, Offset=0x%x, Flags=%c%c%c\n",
                       i, phdr->p_vaddr, phdr->p_memsz, phdr->p_filesz, phdr->p_offset,
                       (phdr->p_flags & 1) ? 'X' : '-', // PF_X
                       (phdr->p_flags & 2) ? 'W' : '-', // PF_W
                       (phdr->p_flags & 4) ? 'R' : '-'); // PF_R

        if (phdr->p_memsz == 0) { continue; } // Skip empty
        if (phdr->p_filesz > phdr->p_memsz) { terminal_printf("   -> Error: FileSz > MemSz.\n"); kfree(file_data); return -1; }
        if (phdr->p_offset > file_size || (phdr->p_offset + phdr->p_filesz) > file_size) { terminal_printf("   -> Error: Segment file range OOB.\n"); kfree(file_data); return -1; }

        // Determine VMA range
        uintptr_t vm_start = PAGE_ALIGN_DOWN(phdr->p_vaddr);
        uintptr_t seg_end_addr = phdr->p_vaddr + phdr->p_memsz;
        if (seg_end_addr < phdr->p_vaddr) seg_end_addr = UINTPTR_MAX;
        uintptr_t vm_end = PAGE_ALIGN_UP(seg_end_addr);
        if (vm_end == 0 && seg_end_addr > 0) vm_end = UINTPTR_MAX;
        if (vm_start >= vm_end) { continue; } // Skip zero-size after alignment

        // Determine VMA flags and Page Protection flags
        uint32_t vm_flags = VM_READ | VM_USER | VM_ANONYMOUS; // Base flags
        uint32_t page_prot = PAGE_PRESENT | PAGE_USER;        // Base page flags

        bool is_writable = (phdr->p_flags & 2); // PF_W
        bool is_executable = (phdr->p_flags & 1); // PF_X

        if (is_writable) {
            vm_flags |= VM_WRITE;
            page_prot |= PAGE_RW;
        }
        if (is_executable) {
            vm_flags |= VM_EXEC;
            // Leave page executable (don't add PAGE_NX_BIT)
        } else {
            // Not executable, add NX bit if supported
            if (g_nx_supported) {
                page_prot |= PAGE_NX_BIT;
            }
        }
        // We assume readable (PF_R) implicitly for PT_LOAD

        terminal_printf("   -> VMA Range [0x%x - 0x%x), VMA Flags=0x%x, PageProt=0x%x\n",
                       vm_start, vm_end, vm_flags, page_prot);

        // Insert VMA for the segment
        vma_struct_t* vma = insert_vma(mm, vm_start, vm_end, vm_flags, page_prot, NULL, 0);
        if (!vma) {
             terminal_printf("   -> Error: Failed to insert VMA.\n"); kfree(file_data); return -1;
        }

        // Allocate Frames, Map Pages, and Copy/Zero Data
        terminal_printf("   -> Mapping and populating pages...\n");
        for (uintptr_t page_v = vm_start; page_v < vm_end; page_v += PAGE_SIZE) {
            // 1. Allocate Physical Frame
            uintptr_t phys_page_addr = frame_alloc();
            if (!phys_page_addr) {
                 terminal_printf("   -> Error: Out of physical frames at V=0x%x.\n", page_v);
                 // TODO: Robust cleanup needed
                 destroy_mm(mm); kfree(file_data); return -1;
            }

            // 2. Calculate data source for this page
            uintptr_t file_offset_in_segment = 0;
            size_t copy_size = 0;
            size_t zero_size = 0;

            // Find overlap between current page [page_v, page_v+PAGE_SIZE)
            // and segment's file data [p_vaddr, p_vaddr+p_filesz)
            uintptr_t copy_start_v = (page_v > phdr->p_vaddr) ? page_v : phdr->p_vaddr;
            uintptr_t copy_end_v = ((page_v + PAGE_SIZE) < (phdr->p_vaddr + phdr->p_filesz)) ? (page_v + PAGE_SIZE) : (phdr->p_vaddr + phdr->p_filesz);

            if (copy_start_v < copy_end_v) { // Does this page contain data from the file?
                 file_offset_in_segment = phdr->p_offset + (copy_start_v - phdr->p_vaddr);
                 copy_size = copy_end_v - copy_start_v;
            }

            // Calculate zero fill size for the rest of the page up to segment end or page end
            uintptr_t zero_start_v = (copy_start_v + copy_size); // Address after copied data
            uintptr_t segment_end_v = phdr->p_vaddr + phdr->p_memsz; // Virtual end of segment in memory
            uintptr_t zero_end_v = ((page_v + PAGE_SIZE) < segment_end_v) ? (page_v + PAGE_SIZE) : segment_end_v;

            if (zero_start_v < zero_end_v) {
                 zero_size = zero_end_v - zero_start_v;
            }

            // Ensure total doesn't exceed page size
            if (copy_size + zero_size > PAGE_SIZE) {
                 terminal_printf("   -> Internal Error: copy+zero size > PAGE_SIZE for V=0x%x\n", page_v);
                 put_frame(phys_page_addr); destroy_mm(mm); kfree(file_data); return -1;
            }

            // 3. Copy/Zero data into the allocated frame using helper
             if (copy_elf_segment_data(phys_page_addr, file_data, file_offset_in_segment, copy_size, zero_size) != 0)
             {
                  terminal_printf("   -> Error: Failed to copy/zero segment data for V=0x%x.\n", page_v);
                  put_frame(phys_page_addr); destroy_mm(mm); kfree(file_data); return -1;
             }

            // 4. Map the page into the process address space
            terminal_printf("USER_MAP DEBUG: Mapping ELF V=0x%x -> P=0x%x Flags=0x%x (Segment %d)\n",
                            page_v, phys_page_addr, page_prot, i); // Added logging
            int map_res = paging_map_single_4k(mm->pgd_phys, page_v, phys_page_addr, page_prot);
            if (map_res != 0) {
                 terminal_printf("   -> Error: paging_map_single_4k failed for V=0x%x (code %d)\n", page_v, map_res);
                 put_frame(phys_page_addr); destroy_mm(mm); kfree(file_data); return -1;
            }
        } // End loop through pages

        // Update highest address loaded by ANY segment
        if (seg_end_addr > highest_addr_loaded) {
            highest_addr_loaded = seg_end_addr;
        }
    } // End loop through program headers

    // Set initial program break (heap start)
    *initial_brk = PAGE_ALIGN_UP(highest_addr_loaded);
    terminal_printf("  ELF Loading complete. Initial Brk set to: 0x%x\n", *initial_brk);

    kfree(file_data); // Free the buffer holding the ELF file
    return 0; // Success
}


/* Creates a user process */
pcb_t *create_user_process(const char *path) {
    terminal_printf("[Process] Creating user process from '%s'.\n", path);

    pcb_t *proc = (pcb_t *)kmalloc(sizeof(pcb_t));
    if (!proc) { terminal_write("[Process] kmalloc PCB failed.\n"); return NULL; }
    memset(proc, 0, sizeof(pcb_t));
    proc->pid = next_pid++;

    // Allocate Page Directory frame
    uintptr_t pd_phys_addr = frame_alloc();
    if (!pd_phys_addr) { terminal_write("[Process] frame_alloc PD failed.\n"); kfree(proc); return NULL; }
    proc->page_directory_phys = (uint32_t*)pd_phys_addr;

    // Temporarily map NEW PD to copy kernel entries and set recursive mapping
    if (paging_map_single_4k((uint32_t*)g_kernel_page_directory_phys, TEMP_PD_MAP_ADDR, pd_phys_addr, PTE_KERNEL_DATA_FLAGS) != 0) {
        terminal_write("[Process] Failed to temp map new PD.\n");
        put_frame(pd_phys_addr); kfree(proc); return NULL;
    }
    uint32_t *proc_pd_virt = (uint32_t*)TEMP_PD_MAP_ADDR;
    // Frame alloc should have zeroed it, but clear again for safety? Not strictly needed.
    // memset(proc_pd_virt, 0, PAGE_SIZE);
    copy_kernel_pde_entries(proc_pd_virt);
    proc_pd_virt[RECURSIVE_PDE_INDEX] = (pd_phys_addr & PAGING_ADDR_MASK) | PAGE_PRESENT | PAGE_RW;
    paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_PD_MAP_ADDR, PAGE_SIZE);
    paging_invalidate_page((void*)TEMP_PD_MAP_ADDR);

    // Allocate Kernel Stack
    if (!allocate_kernel_stack(proc)) {
        put_frame(pd_phys_addr); kfree(proc); return NULL;
    }

    // Create MM Structure
    proc->mm = create_mm(proc->page_directory_phys);
    if (!proc->mm) {
        put_frame(proc->kernel_stack_phys_base);
        put_frame(pd_phys_addr);
        kfree(proc);
        return NULL;
    }

    // Load ELF, Create VMAs, Map pages, Set Initial Break
    uintptr_t initial_brk_addr = 0;
    if (load_elf_and_init_memory(path, proc->mm, &proc->entry_point, &initial_brk_addr) != 0) {
        terminal_printf("[Process] Error: load_elf_and_init_memory failed for '%s'. Cleaning up.\n", path);
        // Cleanup sequence: destroy_mm handles VMAs and unmaps pages via paging_unmap_range->put_frame
        destroy_mm(proc->mm);
        put_frame(proc->kernel_stack_phys_base);
        // paging_free_user_space is not strictly needed if destroy_mm does its job via VMA traversal.
        // It might double-free page table frames if called here.
        // However, it ensures user PDE slots are cleared if destroy_mm fails partially.
        // Let's call it for extra safety, assuming put_frame handles double-free checks.
        paging_free_user_space(proc->page_directory_phys);
        put_frame(pd_phys_addr); // Free PD frame itself
        kfree(proc);
        return NULL;
    }
    proc->mm->start_brk = initial_brk_addr;
    proc->mm->end_brk = initial_brk_addr;

    // Create Initial Heap VMA (zero size initially)
    uint32_t heap_vm_flags = VM_READ | VM_WRITE | VM_USER | VM_ANONYMOUS;
    // ** VERIFY THIS FLAG in paging.h does not contain 0x8 (PAGE_PWT) **
    uint32_t heap_page_prot = PTE_USER_DATA_FLAGS;
    // Log flags being used for heap VMA insertion
    terminal_printf("USER_MAP DEBUG: Inserting Initial Heap VMA V=[0x%x-0x%x) PageProt=0x%x\n",
                   initial_brk_addr, initial_brk_addr, heap_page_prot);
    if (!insert_vma(proc->mm, initial_brk_addr, initial_brk_addr, heap_vm_flags, heap_page_prot, NULL, 0)) {
         terminal_write("[Process] Warning: Failed to insert initial (zero-size) heap VMA.\n");
    }

    // Create User Stack VMA
    proc->user_stack_top = (void *)USER_STACK_TOP_VIRT_ADDR;
    uint32_t stack_vm_flags = VM_READ | VM_WRITE | VM_USER | VM_ANONYMOUS | VM_GROWS_DOWN;
    // ** VERIFY THIS FLAG in paging.h does not contain 0x8 (PAGE_PWT) **
    uint32_t stack_page_prot = PTE_USER_DATA_FLAGS;
    // Add NX bit to stack pages if supported
    if (g_nx_supported) {
        stack_page_prot |= PAGE_NX_BIT;
    }
    // Log flags being used for stack VMA insertion
    terminal_printf("USER_MAP DEBUG: Inserting User Stack VMA V=[0x%x-0x%x) PageProt=0x%x\n",
                    USER_STACK_BOTTOM_VIRT, USER_STACK_TOP_VIRT_ADDR, stack_page_prot);
    if (!insert_vma(proc->mm, USER_STACK_BOTTOM_VIRT, USER_STACK_TOP_VIRT_ADDR,
                    stack_vm_flags, stack_page_prot, NULL, 0)) {
        terminal_printf("[Process] Error: Failed to insert stack VMA. Cleaning up.\n");
        destroy_mm(proc->mm);
        put_frame(proc->kernel_stack_phys_base);
        paging_free_user_space(proc->page_directory_phys);
        put_frame(pd_phys_addr);
        kfree(proc);
        return NULL;
    }

    terminal_printf("[Process] Successfully created PCB PID %d for '%s'\n", proc->pid, path);
    return proc;
}


/* Destroys a process and frees its resources */
void destroy_process(pcb_t *pcb) {
    if (!pcb) return;
    uint32_t pid = pcb->pid;
    terminal_printf("[Process] Destroying process PID %d.\n", pid);

    // 1. Destroy Memory Management structures (VMAs)
    //    This calls destroy_vma_node_callback -> paging_unmap_range -> put_frame
    if (pcb->mm) {
        destroy_mm(pcb->mm); // This handles unmapping pages and freeing VMA structs
        pcb->mm = NULL;
    } else {
         terminal_printf("[Process] Warning: Process PID %d has no mm_struct during destroy.\n", pid);
    }

    // 2. Free User-Space Page Tables
    //    Frees the PT frames themselves after destroy_mm freed the data frames.
    if (pcb->page_directory_phys) {
        paging_free_user_space(pcb->page_directory_phys);
    }

    // 3. Free Kernel Stack Frame
    if (pcb->kernel_stack_phys_base) {
        put_frame(pcb->kernel_stack_phys_base);
        pcb->kernel_stack_phys_base = 0;
    }

    // 4. Free Page Directory Frame
    if (pcb->page_directory_phys) {
        put_frame((uintptr_t)pcb->page_directory_phys);
        pcb->page_directory_phys = NULL;
    }

    // 5. Free the PCB structure itself
    kfree(pcb);
    terminal_printf("[Process] PCB PID %d resources freed.\n", pid);
}