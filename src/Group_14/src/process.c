#include "process.h"
#include "mm.h"
#include "kmalloc.h"
#include "paging.h"     // Includes paging_free_user_space, flags, etc.
#include "terminal.h"
#include "types.h"
#include "string.h"
#include "scheduler.h"
#include "read_file.h"
#include "buddy.h"      // Needed for underlying frame allocator and BUDDY_PANIC/ASSERT
#include "frame.h"
#include "kmalloc_internal.h" // For ALIGN_UP
#include "elf.h"

// --- Definitions ---
extern uint32_t* g_kernel_page_directory_virt; // From paging.c
extern uint32_t  g_kernel_page_directory_phys;
extern bool      g_nx_supported;               // From paging.c

// Define temporary mapping address if not already defined
#ifndef TEMP_PD_MAP_ADDR
#define TEMP_PD_MAP_ADDR (KERNEL_SPACE_VIRT_START - 1 * PAGE_SIZE)
#endif
#ifndef TEMP_MAP_ADDR_PF // Define temp address for page frame mapping
#define TEMP_MAP_ADDR_PF (KERNEL_SPACE_VIRT_START - 3 * PAGE_SIZE)
#endif

// --- Kernel Assertion Macro ---
#ifndef KERNEL_ASSERT
#define KERNEL_ASSERT(condition, msg) do { \
    if (!(condition)) { \
        terminal_printf("\n[ASSERT FAILED] %s at %s:%d\n", msg, __FILE__, __LINE__); \
        terminal_printf("System Halted.\n"); \
        while (1) { asm volatile("cli; hlt"); } \
    } \
} while (0)
#endif
// --- End Assertion Macro ---

// By default, ensure your process.h has something like:
// #define PROCESS_KSTACK_SIZE (PAGE_SIZE * 4)

static uint32_t next_pid = 1;

pcb_t* get_current_process(void)
{
    tcb_t* current_tcb = get_current_task();
    if (current_tcb && current_tcb->process) {
        return current_tcb->process;
    }
    return NULL;
}

// Copies kernel entries (PDEs for indices >= KERNEL_PDE_INDEX)
static void copy_kernel_pde_entries(uint32_t *new_pd_virt)
{
    if (!g_kernel_page_directory_virt) {
        terminal_write("[Process] Error: copy_kernel_pde_entries called when kernel PD is NULL!\n");
        return;
    }
    for (size_t i = KERNEL_PDE_INDEX; i < TABLES_PER_DIR; i++) {
        // Skip the recursive entry; let the caller handle it for the new PD.
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
static bool allocate_kernel_stack(pcb_t *proc)
{
    // For simplicity, we assume PROCESS_KSTACK_SIZE == PAGE_SIZE.
    // If you want multi-page kernel stacks, you’ll need to revise this.
    size_t stack_alloc_size = PROCESS_KSTACK_SIZE;
    if (stack_alloc_size != PAGE_SIZE) {
        terminal_printf("[Process] Error: allocate_kernel_stack only supports stack = 1 page.\n");
        return false;
    }

    uintptr_t kstack_phys_base = frame_alloc();
    if (!kstack_phys_base) {
        terminal_write("[Process] Failed to allocate kernel stack frame.\n");
        return false;
    }

    proc->kernel_stack_phys_base = (uint32_t)kstack_phys_base;

    // In this design, kernel virtual space is a direct offset from the physical:
    uintptr_t kstack_virt_base = KERNEL_SPACE_VIRT_START + kstack_phys_base;
    uintptr_t kstack_virt_top  = kstack_virt_base + stack_alloc_size;
    proc->kernel_stack_vaddr_top = (uint32_t *)kstack_virt_top;

    terminal_printf("  Allocated kernel stack: Phys=0x%x, VirtTop=0x%x, Size=%u KB\n",
                    kstack_phys_base, kstack_virt_top, stack_alloc_size / 1024);
    return true;
}

// Helper to map, copy, and unmap temporarily for ELF loading
static int copy_elf_segment_data(uintptr_t frame_paddr,
                                 const uint8_t* file_data_buffer,
                                 size_t file_buffer_offset,
                                 size_t size_to_copy,
                                 size_t zero_padding)
{
    void* temp_map_addr = (void*)TEMP_MAP_ADDR_PF;

    // Temporarily map the physical frame into kernel space
    if (paging_map_single_4k((uint32_t*)g_kernel_page_directory_phys,
                             (uintptr_t)temp_map_addr,
                             frame_paddr,
                             PTE_KERNEL_DATA_FLAGS) != 0)
    {
        terminal_printf("[Process] Failed to temp map frame 0x%x to V=0x%p.\n",
                        frame_paddr, temp_map_addr);
        return -1;
    }

    // Calculate offset for zero padding
    size_t offset_after_copy = size_to_copy;
    size_t zero_clamped      = 0;
    if (zero_padding > 0 && offset_after_copy < PAGE_SIZE) {
        zero_clamped = (offset_after_copy + zero_padding > PAGE_SIZE)
                        ? (PAGE_SIZE - offset_after_copy)
                        : zero_padding;
    }

    // Assertions to ensure we do not overwrite beyond 4KB
    KERNEL_ASSERT(size_to_copy <= PAGE_SIZE, "ELF segment copy exceeds page size");
    KERNEL_ASSERT(zero_clamped <= PAGE_SIZE, "ELF segment zero fill exceeds page size");
    KERNEL_ASSERT(offset_after_copy + zero_clamped <= PAGE_SIZE, "ELF copy+zero exceeds page boundary");

    // Copy data from ELF buffer
    if (size_to_copy > 0) {
        memcpy(temp_map_addr, file_data_buffer + file_buffer_offset, size_to_copy);
    }

    // Zero the BSS area
    if (zero_clamped > 0) {
        memset((uint8_t*)temp_map_addr + offset_after_copy, 0, zero_clamped);
    }

    // Unmap and flush TLB
    paging_unmap_range((uint32_t*)g_kernel_page_directory_phys,
                       (uintptr_t)temp_map_addr, PAGE_SIZE);
    paging_invalidate_page(temp_map_addr);

    return 0; // Success
}

/**
 * @brief Loads ELF segments into the process address space.
 * Allocates physical frames, maps them to user virtual addresses,
 * and copies/zeroes data from the ELF file buffer.
 *
 * @param path          Path to the ELF file (for logging).
 * @param mm            Process memory manager struct.
 * @param entry_point   [out] ELF entry point.
 * @param initial_brk   [out] initial program break address.
 * @return 0 on success, negative on failure.
 */
int load_elf_and_init_memory(const char *path,
                             mm_struct_t *mm,
                             uint32_t *entry_point,
                             uintptr_t *initial_brk)
{
    size_t file_size       = 0;
    uint8_t *file_data     = NULL;
    vma_struct_t* vma      = NULL;
    uintptr_t phys_page    = 0;
    int result             = -1;

    file_data = (uint8_t*)read_file(path, &file_size);
    if (!file_data) {
        terminal_printf("[Process] load_elf: read_file failed for '%s'.\n", path);
        goto cleanup_load_elf;
    }

    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_data;

    // Validate ELF header
    if (memcmp(ehdr->e_ident, "\x7F" "ELF", 4) != 0) {
        terminal_printf("[Process] load_elf: Invalid ELF magic.\n");
        goto cleanup_load_elf;
    }
    if (ehdr->e_type != ET_EXEC || ehdr->e_machine != EM_386
        || ehdr->e_version != EV_CURRENT)
    {
        terminal_printf("[Process] load_elf: Invalid ELF type/machine/version.\n");
        goto cleanup_load_elf;
    }
    if (ehdr->e_phentsize != sizeof(Elf32_Phdr)) {
        terminal_printf("[Process] load_elf: Invalid program header size.\n");
        goto cleanup_load_elf;
    }
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        terminal_printf("[Process] load_elf: No program headers found.\n");
        goto cleanup_load_elf;
    }
    // Check that the program header table fits in the file
    if (ehdr->e_phoff > file_size
        || (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize) > file_size)
    {
        terminal_printf("[Process] load_elf: Program header table out of bounds.\n");
        goto cleanup_load_elf;
    }

    *entry_point = ehdr->e_entry;
    terminal_printf("  ELF Entry Point: 0x%x\n", *entry_point);

    Elf32_Phdr *phdr_table = (Elf32_Phdr *)(file_data + ehdr->e_phoff);
    uintptr_t highest_addr_loaded = 0;

    // --- Iterate through Program Headers ---
    for (Elf32_Half i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr *phdr = &phdr_table[i];

        if (phdr->p_type != PT_LOAD) {
            continue; // Skip non-loadable
        }

        terminal_printf("  Segment %d: VAddr=0x%x, MemSz=%u, FileSz=%u, Offset=0x%x, Flags=%c%c%c\n",
            i, phdr->p_vaddr, phdr->p_memsz, phdr->p_filesz, phdr->p_offset,
            (phdr->p_flags & PF_X) ? 'X' : '-',
            (phdr->p_flags & PF_W) ? 'W' : '-',
            (phdr->p_flags & PF_R) ? 'R' : '-');

        if (phdr->p_memsz == 0) {
            continue;
        }
        if (phdr->p_filesz > phdr->p_memsz) {
            terminal_printf("   -> Error: FileSz > MemSz.\n");
            goto cleanup_load_elf;
        }
        // Check segment range in file
        if (phdr->p_offset > file_size
            || phdr->p_filesz > (file_size - phdr->p_offset))
        {
            terminal_printf("   -> Error: Segment range OOB.\n");
            goto cleanup_load_elf;
        }

        // Align segment start/end
        uintptr_t vm_start = PAGE_ALIGN_DOWN(phdr->p_vaddr);
        uintptr_t seg_end_addr = phdr->p_vaddr + phdr->p_memsz;
        if (seg_end_addr < phdr->p_vaddr) seg_end_addr = UINTPTR_MAX; // Overflow guard
        uintptr_t vm_end = PAGE_ALIGN_UP(seg_end_addr);
        if (vm_end == 0 && seg_end_addr > 0) vm_end = UINTPTR_MAX; // Overflow guard
        if (vm_start >= vm_end) {
            continue;
        }

        // Determine flags
        uint32_t vm_flags  = VM_READ | VM_USER | VM_ANONYMOUS;
        uint32_t page_prot = PAGE_PRESENT | PAGE_USER;

        bool is_writable   = (phdr->p_flags & PF_W);
        bool is_executable = (phdr->p_flags & PF_X);

        if (is_writable) {
            vm_flags  |= VM_WRITE;
            page_prot |= PAGE_RW;
        }
        if (is_executable) {
            vm_flags |= VM_EXEC;
            // Keep pages executable.
        } else {
            // If NX is supported, set NX on non-executable segments
            if (g_nx_supported) {
                page_prot |= PAGE_NX_BIT;
            }
        }

        terminal_printf("   -> VMA [0x%x - 0x%x), VMA Flags=0x%x, PageProt=0x%x\n",
                        vm_start, vm_end, vm_flags, page_prot);

        // Insert the VMA
        vma = insert_vma(mm, vm_start, vm_end, vm_flags, page_prot, NULL, 0);
        if (!vma) {
            terminal_printf("   -> Error: Failed to insert VMA.\n");
            goto cleanup_load_elf;
        }
        vma = NULL; // Not used after insertion

        // Allocate frames + map pages
        terminal_printf("   -> Mapping and populating pages...\n");
        for (uintptr_t page_v = vm_start; page_v < vm_end; page_v += PAGE_SIZE) {
            // 1. Allocate physical frame
            phys_page = frame_alloc();
            if (!phys_page) {
                terminal_printf("   -> Error: Out of frames at V=0x%x.\n", page_v);
                goto cleanup_load_elf;
            }

            // 2. Determine ELF copy overlap
            uintptr_t file_offset_in_seg = 0;
            size_t copy_size            = 0;
            size_t zero_size            = 0;

            uintptr_t copy_start_v = (page_v > phdr->p_vaddr)
                                    ? page_v
                                    : phdr->p_vaddr;
            uintptr_t copy_end_v   = ((page_v + PAGE_SIZE) < (phdr->p_vaddr + phdr->p_filesz))
                                    ? (page_v + PAGE_SIZE)
                                    : (phdr->p_vaddr + phdr->p_filesz);

            if (copy_start_v < copy_end_v) {
                file_offset_in_seg = phdr->p_offset + (copy_start_v - phdr->p_vaddr);
                copy_size          = copy_end_v - copy_start_v;
            }

            uintptr_t zero_start_v = copy_start_v + copy_size;
            uintptr_t seg_end_v    = phdr->p_vaddr + phdr->p_memsz;
            if (seg_end_v < phdr->p_vaddr) seg_end_v = UINTPTR_MAX; // Overflow check
            uintptr_t zero_end_v   = ((page_v + PAGE_SIZE) < seg_end_v)
                                     ? (page_v + PAGE_SIZE)
                                     : seg_end_v;
            if (zero_end_v < zero_start_v) zero_end_v = zero_start_v;

            if (zero_start_v < zero_end_v) {
                zero_size = zero_end_v - zero_start_v;
            }

            // 3. Copy ELF data + zero BSS
            if (copy_elf_segment_data(phys_page,
                                      file_data,
                                      file_offset_in_seg,
                                      copy_size,
                                      zero_size) != 0)
            {
                terminal_printf("   -> Error: copy_elf_segment_data failed at V=0x%x.\n", page_v);
                put_frame(phys_page);
                phys_page = 0;
                goto cleanup_load_elf;
            }

            // 4. Map the page
            int map_res = paging_map_single_4k(mm->pgd_phys,
                                               page_v,
                                               phys_page,
                                               page_prot);
            if (map_res != 0) {
                terminal_printf("   -> Error: paging_map_single_4k for V=0x%x, code=%d.\n",
                                page_v, map_res);
                put_frame(phys_page);
                phys_page = 0;
                goto cleanup_load_elf;
            }
            // Reset phys_page after ownership transferred
            phys_page = 0;
        }

        if (seg_end_addr > highest_addr_loaded) {
            highest_addr_loaded = seg_end_addr;
        }
    }

    // Initialize brk to the next page boundary
    *initial_brk = PAGE_ALIGN_UP(highest_addr_loaded);
    terminal_printf("  ELF load complete. initial_brk=0x%x\n", *initial_brk);
    result = 0; // success

cleanup_load_elf:
    if (file_data) {
        kfree(file_data);
        file_data = NULL;
    }
    // If we bailed mid-way, destroy_mm will free/unmap everything, so we leave that to the caller.
    if (phys_page != 0) {
        put_frame(phys_page);
    }
    return result;
}

// Creates a user process
pcb_t *create_user_process(const char *path)
{
    terminal_printf("[Process] Creating user process from '%s'.\n", path);
    pcb_t *proc           = NULL;
    uintptr_t pd_phys     = 0;
    uintptr_t kstack_phys = 0;
    int result            = -1;

    proc = (pcb_t *)kmalloc(sizeof(pcb_t));
    if (!proc) {
        terminal_write("[Process] kmalloc PCB failed.\n");
        goto cleanup_create;
    }
    memset(proc, 0, sizeof(pcb_t));
    proc->pid = next_pid++;

    // Allocate Page Directory
    pd_phys = frame_alloc();
    if (!pd_phys) {
        terminal_write("[Process] frame_alloc PD failed.\n");
        goto cleanup_create;
    }
    proc->page_directory_phys = (uint32_t*)pd_phys;
    terminal_printf("  Allocated PD Phys: 0x%x\n", pd_phys);

    // Temporarily map the new PD to copy kernel entries + set recursive mapping
    if (paging_map_single_4k((uint32_t*)g_kernel_page_directory_phys,
                             TEMP_PD_MAP_ADDR,
                             pd_phys,
                             PTE_KERNEL_DATA_FLAGS) != 0)
    {
        terminal_write("[Process] Failed to temp map new PD.\n");
        goto cleanup_create;
    }
    uint32_t *proc_pd_virt = (uint32_t*)TEMP_PD_MAP_ADDR;
    copy_kernel_pde_entries(proc_pd_virt);

    // Set the recursive PDE
    proc_pd_virt[RECURSIVE_PDE_INDEX] = (pd_phys & PAGING_ADDR_MASK)
                                        | PAGE_PRESENT
                                        | PAGE_RW;

    // Unmap it from kernel to avoid conflicts
    paging_unmap_range((uint32_t*)g_kernel_page_directory_phys,
                       TEMP_PD_MAP_ADDR,
                       PAGE_SIZE);
    paging_invalidate_page((void*)TEMP_PD_MAP_ADDR);

    // Allocate kernel stack
    if (!allocate_kernel_stack(proc)) {
        goto cleanup_create;
    }
    kstack_phys = proc->kernel_stack_phys_base;

    // Create mm structure
    proc->mm = create_mm(proc->page_directory_phys);
    if (!proc->mm) {
        terminal_write("[Process] create_mm failed.\n");
        goto cleanup_create;
    }

    // Load the ELF, create VMAs, map pages
    uintptr_t initial_brk_addr = 0;
    if (load_elf_and_init_memory(path,
                                 proc->mm,
                                 &proc->entry_point,
                                 &initial_brk_addr) != 0)
    {
        terminal_printf("[Process] Error: ELF load failed for '%s'.\n", path);
        goto cleanup_create;
    }
    proc->mm->start_brk = initial_brk_addr;
    proc->mm->end_brk   = initial_brk_addr;

    // Insert an initial (empty) heap VMA
    {
        uint32_t heap_flags     = VM_READ | VM_WRITE | VM_USER | VM_ANONYMOUS;
        uint32_t heap_page_prot = PTE_USER_DATA_FLAGS;
        // Optionally mark heap NX if supported
        if (g_nx_supported) {
            heap_page_prot |= PAGE_NX_BIT;
        }
        terminal_printf("USER_MAP DEBUG: Inserting Heap VMA [0x%x - 0x%x)\n",
                        initial_brk_addr, initial_brk_addr);
        if (!insert_vma(proc->mm, initial_brk_addr, initial_brk_addr,
                        heap_flags, heap_page_prot, NULL, 0))
        {
            terminal_write("[Process] Warning: failed to insert zero-size heap VMA.\n");
        }
    }

    // Create user stack VMA
    {
        proc->user_stack_top = (void *)USER_STACK_TOP_VIRT_ADDR;
        uint32_t stk_flags   = VM_READ | VM_WRITE | VM_USER | VM_ANONYMOUS | VM_GROWS_DOWN;
        uint32_t stk_prot    = PTE_USER_DATA_FLAGS;
        if (g_nx_supported) {
            stk_prot |= PAGE_NX_BIT; // Mark stack non-executable
        }
        terminal_printf("USER_MAP DEBUG: Inserting User Stack VMA [0x%x - 0x%x)\n",
                        USER_STACK_BOTTOM_VIRT, USER_STACK_TOP_VIRT_ADDR);
        if (!insert_vma(proc->mm,
                        USER_STACK_BOTTOM_VIRT,
                        USER_STACK_TOP_VIRT_ADDR,
                        stk_flags,
                        stk_prot,
                        NULL, 0))
        {
            terminal_printf("[Process] Error: Failed to insert stack VMA.\n");
            goto cleanup_create;
        }
    }

    terminal_printf("[Process] Successfully created PCB PID %d for '%s'.\n",
                    proc->pid, path);
    return proc; // Success

cleanup_create:
    terminal_printf("[Process] Cleaning up failed process creation (PID %d).\n",
                    (proc ? proc->pid : 0));

    // Clean up in reverse order
    if (proc) {
        if (proc->mm) {
            destroy_mm(proc->mm);
            proc->mm = NULL;
        }
        if (proc->kernel_stack_phys_base) {
            put_frame(proc->kernel_stack_phys_base);
            proc->kernel_stack_phys_base = 0;
        }
        if (proc->page_directory_phys) {
            paging_free_user_space(proc->page_directory_phys);
            put_frame((uintptr_t)proc->page_directory_phys);
            proc->page_directory_phys = NULL;
        }
        kfree(proc);
    } else {
        if (pd_phys) {
            put_frame(pd_phys);
        }
    }
    return NULL;
}

// Destroys a process and frees its resources
void destroy_process(pcb_t *pcb)
{
    if (!pcb) return;

    uint32_t pid = pcb->pid;
    terminal_printf("[Process] Destroying process PID %d.\n", pid);

    // 1. Destroy Memory Management (unmaps pages, frees VMAs)
    if (pcb->mm) {
        destroy_mm(pcb->mm);
        pcb->mm = NULL;
    } else {
        terminal_printf("[Process] Warning: PID %d had no mm_struct.\n", pid);
    }

    // 2. Free user-space page tables
    if (pcb->page_directory_phys) {
        paging_free_user_space(pcb->page_directory_phys);
    }

    // 3. Free kernel stack
    if (pcb->kernel_stack_phys_base) {
        put_frame(pcb->kernel_stack_phys_base);
        pcb->kernel_stack_phys_base = 0;
    }

    // 4. Free the page directory frame
    if (pcb->page_directory_phys) {
        put_frame((uintptr_t)pcb->page_directory_phys);
        pcb->page_directory_phys = NULL;
    }

    // 5. Free PCB
    kfree(pcb);
    terminal_printf("[Process] PCB PID %d resources freed.\n", pid);
}
