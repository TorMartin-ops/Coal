/**
 * process.c - Process Management Implementation
 *
 * Handles creation, destruction, and management of process control blocks (PCBs)
 * and their associated memory structures (mm_struct). Includes ELF loading.
 * Refined for robustness and clarity.
 */

 #include "process.h"
 #include "mm.h"               // For mm_struct, vma_struct, create_mm, destroy_mm, insert_vma
 #include "kmalloc.h"           // For kmalloc, kfree
 #include "paging.h"            // For paging functions, flags, constants (PAGE_SIZE etc.)
 #include "terminal.h"          // For kernel logging
 #include "types.h"             // Core type definitions
 #include "string.h"            // For memset, memcpy
 #include "scheduler.h"         // For get_current_task()
 #include "read_file.h"         // For read_file() helper
 #include "buddy.h"             // For buddy_alloc, buddy_free (used by allocate_kernel_stack)
 #include "frame.h"             // For frame_alloc, put_frame
 #include "kmalloc_internal.h"  // For ALIGN_UP (used by PAGE_ALIGN_UP in paging.h)
 #include "elf.h"               // ELF header definitions
 #include "fs_errno.h"          // <--- ADDED: Filesystem error codes
 #include <libc/stddef.h>      // For NULL
 #include "assert.h"            // For KERNEL_ASSERT
 
 
 // ------------------------------------------------------------------------
 // Externals & Globals
 // ------------------------------------------------------------------------
 extern uint32_t g_kernel_page_directory_phys; // Physical address of kernel's page directory (from paging.c)
 extern bool g_nx_supported;                   // NX support flag (from paging.c)
 
 // Process ID counter
 static uint32_t next_pid = 1; // TODO: Protect with lock if creating processes concurrently
 
 // --- Manual definitions for missing 64-bit limits (Keep for reference if 64-bit support added later) ---
 #ifndef LLONG_MAX
 #define LLONG_MAX 9223372036854775807LL
 #endif
 #ifndef LLONG_MIN
 #define LLONG_MIN (-LLONG_MAX - 1LL)
 #endif
 #ifndef ULLONG_MAX
 #define ULLONG_MAX 18446744073709551615ULL
 #endif
 
 // ------------------------------------------------------------------------
 // Local Prototypes
 // ------------------------------------------------------------------------
 static bool allocate_kernel_stack(pcb_t *proc);
 static int load_elf_and_init_memory(const char *path, mm_struct_t *mm, uint32_t *entry_point, uintptr_t *initial_brk);
 static int copy_elf_segment_data(uintptr_t frame_paddr, const uint8_t* file_data_buffer, size_t file_buffer_offset, size_t size_to_copy, size_t zero_padding);
 extern void copy_kernel_pde_entries(uint32_t *new_pd_virt); // From paging.c
 
 // ------------------------------------------------------------------------
 // allocate_kernel_stack -- Allocates and maps multiple pages for kernel stack
 // ------------------------------------------------------------------------
 /**
  * Allocates a multi-page kernel stack for the given process.
  * Maps the stack into the KERNEL's address space.
  * Returns true on success, false on failure (handles partial cleanup).
  */
 static bool allocate_kernel_stack(pcb_t *proc)
 {
     if (!proc) return false;
     size_t stack_alloc_size = PROCESS_KSTACK_SIZE;
     if (stack_alloc_size == 0 || (stack_alloc_size % PAGE_SIZE) != 0) {
         terminal_printf("[Process KStack] Error: Invalid PROCESS_KSTACK_SIZE (%u bytes).\n", (unsigned)stack_alloc_size); return false;
     }
     size_t num_pages = stack_alloc_size / PAGE_SIZE;
     terminal_printf("   Allocating %u pages (%u bytes) for kernel stack...\n", (unsigned)num_pages, (unsigned)stack_alloc_size);
 
     uintptr_t *phys_frames = NULL; void *kstack_virt_base_unaligned_ptr = NULL; uintptr_t kstack_virt_base = 0; bool success = false;
     phys_frames = kmalloc(num_pages * sizeof(uintptr_t));
     if (!phys_frames) { terminal_write("   [Process KStack] Failed to allocate temporary array for frames.\n"); goto cleanup_alloc_kstack; }
     memset(phys_frames, 0, num_pages * sizeof(uintptr_t));
 
     size_t allocated_count = 0;
     for (size_t i = 0; i < num_pages; i++) {
         phys_frames[i] = frame_alloc();
         if (!phys_frames[i]) {
             terminal_printf("   [Process KStack] Failed to allocate physical frame %u/%u.\n", (unsigned)(i + 1), (unsigned)num_pages);
             for (size_t j = 0; j < i; j++) put_frame(phys_frames[j]); allocated_count = 0; goto cleanup_alloc_kstack;
         } allocated_count++;
     }
     proc->kernel_stack_phys_base = (uint32_t)phys_frames[0];
     terminal_printf("   Successfully allocated %u physical frames.\n", (unsigned)allocated_count);
 
     kstack_virt_base_unaligned_ptr = buddy_alloc(stack_alloc_size);
     if (!kstack_virt_base_unaligned_ptr) { terminal_write("   [Process KStack] Failed to reserve virtual range via buddy_alloc.\n"); goto cleanup_alloc_kstack; }
     buddy_free(kstack_virt_base_unaligned_ptr);
 
     kstack_virt_base = PAGE_ALIGN_UP((uintptr_t)kstack_virt_base_unaligned_ptr);
     uintptr_t kstack_virt_top = kstack_virt_base + stack_alloc_size;
     terminal_printf("   Reserved/Aligned kernel stack VIRTUAL range: [0x%x - 0x%x)\n", (unsigned)kstack_virt_base, (unsigned)kstack_virt_top);
 
     for (size_t i = 0; i < num_pages; i++) {
         uintptr_t target_vaddr = kstack_virt_base + (i * PAGE_SIZE); uintptr_t phys_addr = phys_frames[i];
         // Implicit declaration warning for IS_PAGE_ALIGNED might appear, but logic should be sound.
         KERNEL_ASSERT(IS_PAGE_ALIGNED(target_vaddr), "Kernel stack target VA not aligned");
         int map_res = paging_map_single_4k((uint32_t*)g_kernel_page_directory_phys, target_vaddr, phys_addr, PTE_KERNEL_DATA_FLAGS);
         if (map_res != 0) {
             terminal_printf("   [Process KStack] Failed to map page %u (V=0x%x -> P=0x%x), code=%d.\n", (unsigned)i, (unsigned)target_vaddr, (unsigned)phys_addr, map_res);
             for (size_t j = 0; j < i; j++) paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, (kstack_virt_base + j * PAGE_SIZE), PAGE_SIZE);
             goto cleanup_alloc_kstack;
         }
     }
 
     proc->kernel_stack_vaddr_top = (uint32_t*)kstack_virt_top;
     terminal_printf("   Kernel stack mapped: PhysBase=0x%x, VirtBase=0x%x, VirtTop=0x%x\n", (unsigned)proc->kernel_stack_phys_base, (unsigned)kstack_virt_base, (unsigned)proc->kernel_stack_vaddr_top);
     success = true;
 
 cleanup_alloc_kstack:
     if (!success && allocated_count > 0) {
          terminal_printf("   Cleaning up %u physical frames due to KStack allocation/mapping failure.\n", (unsigned)allocated_count);
         for (size_t i = 0; i < allocated_count; i++) put_frame(phys_frames[i]);
     }
     if (phys_frames) kfree(phys_frames);
     return success;
 }
 
 // ------------------------------------------------------------------------
 // get_current_process
 // ------------------------------------------------------------------------
 pcb_t* get_current_process(void) {
     tcb_t* current_tcb = get_current_task();
     return (current_tcb && current_tcb->process) ? current_tcb->process : NULL;
 }
 
 // ------------------------------------------------------------------------
 // copy_elf_segment_data - Helper to copy ELF data via temporary mapping
 // ------------------------------------------------------------------------
 static int copy_elf_segment_data(uintptr_t frame_paddr, const uint8_t* file_data_buffer,
                                  size_t file_buffer_offset, size_t size_to_copy,
                                  size_t zero_padding) {
     void* temp_map_addr = (void*)TEMP_MAP_ADDR_PF;
     if (paging_map_single_4k((uint32_t*)g_kernel_page_directory_phys, (uintptr_t)temp_map_addr, frame_paddr, PTE_KERNEL_DATA_FLAGS) != 0) {
         terminal_printf("[Process] copy_elf: temp map failed (paddr=0x%x).\n", (unsigned)frame_paddr); return -1; }
     KERNEL_ASSERT(size_to_copy + zero_padding <= PAGE_SIZE, "ELF copy+zero exceeds frame");
     if (size_to_copy > 0) memcpy(temp_map_addr, file_data_buffer + file_buffer_offset, size_to_copy);
     if (zero_padding > 0) memset((uint8_t*)temp_map_addr + size_to_copy, 0, zero_padding);
     paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, (uintptr_t)temp_map_addr, PAGE_SIZE);
     paging_invalidate_page(temp_map_addr); return 0;
 }
 
 // ------------------------------------------------------------------------
 // load_elf_and_init_memory - Loads ELF segments and sets up initial memory
 // ------------------------------------------------------------------------
 static int load_elf_and_init_memory(const char *path, mm_struct_t *mm,
                                     uint32_t *entry_point, uintptr_t *initial_brk)
 {
     size_t file_size    = 0; uint8_t *file_data  = NULL; vma_struct_t* vma = NULL;
     uintptr_t phys_page = 0; int result          = FS_ERR_GENERAL; // Use FS error codes
 
     file_data = (uint8_t*)read_file(path, &file_size);
     if (!file_data) { terminal_printf("[Process] load_elf: read_file failed for '%s'.\n", path); return FS_ERR_NOT_FOUND; }
 
     Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_data;
 
     // --- Validate ELF Header ---
     // FIX: Use standard ELF magic number check
     if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
         ehdr->e_ident[EI_MAG2] != ELFMAG2 || ehdr->e_ident[EI_MAG3] != ELFMAG3) {
         terminal_printf("[Process] load_elf: Invalid ELF magic.\n"); result = FS_ERR_INVALID_FORMAT; goto cleanup_load_elf;
     }
     // ... (rest of header checks remain the same) ...
     if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) { terminal_printf("[Process] load_elf: Not a 32-bit ELF.\n"); result = FS_ERR_INVALID_FORMAT; goto cleanup_load_elf; }
     if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) { terminal_printf("[Process] load_elf: Not little-endian.\n"); result = FS_ERR_INVALID_FORMAT; goto cleanup_load_elf; }
     if (ehdr->e_type != ET_EXEC) { terminal_printf("[Process] load_elf: Not an executable file type (ET_EXEC).\n"); result = FS_ERR_INVALID_FORMAT; goto cleanup_load_elf; }
     if (ehdr->e_machine != EM_386) { terminal_printf("[Process] load_elf: Not an i386 ELF.\n"); result = FS_ERR_INVALID_FORMAT; goto cleanup_load_elf; }
     if (ehdr->e_version != EV_CURRENT) { terminal_printf("[Process] load_elf: Invalid ELF version.\n"); result = FS_ERR_INVALID_FORMAT; goto cleanup_load_elf; }
     if (ehdr->e_phentsize != sizeof(Elf32_Phdr)) { terminal_printf("[Process] load_elf: Invalid program header size.\n"); result = FS_ERR_INVALID_FORMAT; goto cleanup_load_elf; }
     if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) { terminal_printf("[Process] load_elf: No program headers found.\n"); result = FS_ERR_INVALID_FORMAT; goto cleanup_load_elf; }
     if (ehdr->e_phoff > file_size || (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize) > file_size) { terminal_printf("[Process] load_elf: Program header table out of bounds.\n"); result = FS_ERR_INVALID_FORMAT; goto cleanup_load_elf; }
     if (ehdr->e_entry >= KERNEL_SPACE_VIRT_START) { terminal_printf("[Process] load_elf: Entry point 0x%x is not in user space!\n", (unsigned)ehdr->e_entry); result = FS_ERR_INVALID_FORMAT; goto cleanup_load_elf; }
 
     *entry_point = ehdr->e_entry; terminal_printf("   ELF Entry Point: 0x%x\n", (unsigned)*entry_point);
     Elf32_Phdr *phdr_table = (Elf32_Phdr *)(file_data + ehdr->e_phoff); uintptr_t highest_addr_loaded = 0;
 
     // --- Iterate through Program Headers (Segments) ---
     for (Elf32_Half i = 0; i < ehdr->e_phnum; i++) {
         Elf32_Phdr *phdr = &phdr_table[i];
         if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) continue;
         terminal_printf("   Segment %d: VAddr=0x%x, MemSz=%u, FileSz=%u, Offset=0x%x, Flags=%c%c%c\n", (int)i, (unsigned)phdr->p_vaddr, (unsigned)phdr->p_memsz, (unsigned)phdr->p_filesz, (unsigned)phdr->p_offset, (phdr->p_flags & PF_R) ? 'R' : '-', (phdr->p_flags & PF_W) ? 'W' : '-', (phdr->p_flags & PF_X) ? 'X' : '-');
 
         if (phdr->p_filesz > phdr->p_memsz) { terminal_printf("    -> Error: FileSz > MemSz.\n"); result = FS_ERR_INVALID_FORMAT; goto cleanup_load_elf; }
         if (phdr->p_offset > file_size || phdr->p_filesz > (file_size - phdr->p_offset)) { terminal_printf("    -> Error: Segment file range out of bounds.\n"); result = FS_ERR_INVALID_FORMAT; goto cleanup_load_elf; }
         if (phdr->p_vaddr >= KERNEL_SPACE_VIRT_START) { terminal_printf("    -> Error: Segment virtual address 0x%x is in kernel space!\n", (unsigned)phdr->p_vaddr); result = FS_ERR_INVALID_FORMAT; goto cleanup_load_elf; }
         uintptr_t seg_mem_end_addr = phdr->p_vaddr + phdr->p_memsz; if (seg_mem_end_addr < phdr->p_vaddr) seg_mem_end_addr = UINTPTR_MAX;
         if (seg_mem_end_addr > KERNEL_SPACE_VIRT_START) { terminal_printf("    -> Error: Segment virtual address range crosses into kernel space!\n"); result = FS_ERR_INVALID_FORMAT; goto cleanup_load_elf; }
 
         uintptr_t vm_start = PAGE_ALIGN_DOWN(phdr->p_vaddr); uintptr_t vm_end = PAGE_ALIGN_UP(seg_mem_end_addr); if (vm_end == 0 && seg_mem_end_addr > 0) vm_end = UINTPTR_MAX; if (vm_start >= vm_end) continue;
         uint32_t vma_flags = VM_READ | VM_USER | VM_ANONYMOUS; uint32_t page_prot = PAGE_PRESENT | PAGE_USER;
         if (phdr->p_flags & PF_W) { vma_flags |= VM_WRITE; page_prot |= PAGE_RW; } if (phdr->p_flags & PF_X) vma_flags |= VM_EXEC; else if (g_nx_supported) page_prot |= PAGE_NX_BIT;
         terminal_printf("    -> VMA [0x%x - 0x%x), VMA Flags=0x%x, PageProt=0x%x\n", (unsigned)vm_start, (unsigned)vm_end, (unsigned)vma_flags, (unsigned)page_prot);
 
         vma = insert_vma(mm, vm_start, vm_end, vma_flags, page_prot, NULL, 0);
         if (!vma) { terminal_printf("    -> Error: Failed to insert VMA for segment %d.\n", (int)i); result = FS_ERR_OUT_OF_MEMORY; goto cleanup_load_elf; } vma = NULL;
 
         terminal_printf("    -> Mapping and populating pages...\n");
         for (uintptr_t page_v = vm_start; page_v < vm_end; page_v += PAGE_SIZE) {
             phys_page = frame_alloc(); if (!phys_page) { terminal_printf("    -> Error: Out of physical frames at V=0x%x.\n", (unsigned)page_v); result = FS_ERR_OUT_OF_MEMORY; goto cleanup_load_elf; }
             uintptr_t copy_start_vaddr = (page_v > phdr->p_vaddr) ? page_v : phdr->p_vaddr; uintptr_t copy_end_vaddr_file = phdr->p_vaddr + phdr->p_filesz; if (copy_end_vaddr_file < phdr->p_vaddr) copy_end_vaddr_file = UINTPTR_MAX; uintptr_t copy_end_vaddr_page = page_v + PAGE_SIZE; if (copy_end_vaddr_page < page_v) copy_end_vaddr_page = UINTPTR_MAX; uintptr_t actual_copy_end_vaddr = (copy_end_vaddr_page < copy_end_vaddr_file) ? copy_end_vaddr_page : copy_end_vaddr_file;
             size_t copy_size_this_page = (actual_copy_end_vaddr > copy_start_vaddr) ? (actual_copy_end_vaddr - copy_start_vaddr) : 0; size_t file_buffer_offset = (copy_start_vaddr - phdr->p_vaddr) + phdr->p_offset;
             size_t zero_padding_this_page = 0; uintptr_t zero_start_vaddr = copy_start_vaddr + copy_size_this_page; uintptr_t zero_end_vaddr_seg = phdr->p_vaddr + phdr->p_memsz; if (zero_end_vaddr_seg < phdr->p_vaddr) zero_end_vaddr_seg = UINTPTR_MAX; uintptr_t zero_end_vaddr_page = page_v + PAGE_SIZE; if (zero_end_vaddr_page < page_v) zero_end_vaddr_page = UINTPTR_MAX; uintptr_t actual_zero_end_vaddr = (zero_end_vaddr_page < zero_end_vaddr_seg) ? zero_end_vaddr_page : zero_end_vaddr_seg;
             if (actual_zero_end_vaddr > zero_start_vaddr) zero_padding_this_page = actual_zero_end_vaddr - zero_start_vaddr;
             if (copy_size_this_page + zero_padding_this_page > PAGE_SIZE) { terminal_printf("    -> Error: Internal calculation error for V=0x%x\n", (unsigned)page_v); put_frame(phys_page); phys_page = 0; result = FS_ERR_GENERAL; goto cleanup_load_elf; }
             if (copy_elf_segment_data(phys_page, file_data, file_buffer_offset, copy_size_this_page, zero_padding_this_page) != 0) { terminal_printf("    -> Error: copy_elf_segment_data failed at V=0x%x.\n", (unsigned)page_v); put_frame(phys_page); phys_page = 0; result = FS_ERR_GENERAL; goto cleanup_load_elf; }
             int map_res = paging_map_single_4k(mm->pgd_phys, page_v, phys_page, page_prot);
             if (map_res != 0) { terminal_printf("    -> Error: paging_map_single_4k for V=0x%x failed (code=%d).\n", (unsigned)page_v, map_res); put_frame(phys_page); phys_page = 0; result = FS_ERR_GENERAL; goto cleanup_load_elf; }
             phys_page = 0;
         }
         if (seg_mem_end_addr > highest_addr_loaded) highest_addr_loaded = seg_mem_end_addr;
     } // End loop through program headers
 
     *initial_brk = PAGE_ALIGN_UP(highest_addr_loaded); if (*initial_brk == 0 && highest_addr_loaded > 0) *initial_brk = UINTPTR_MAX;
     terminal_printf("   ELF load complete. initial_brk=0x%x\n", (unsigned)*initial_brk);
     result = FS_SUCCESS; // Success
 
 cleanup_load_elf:
     if (file_data) kfree(file_data); if (phys_page != 0) put_frame(phys_page);
     return result;
 }
 
 
 // ------------------------------------------------------------------------
 // create_user_process - Creates PCB, address space, loads ELF
 // ------------------------------------------------------------------------
 pcb_t *create_user_process(const char *path)
 {
     terminal_printf("[Process] Creating user process from '%s'.\n", path);
     pcb_t *proc           = NULL; uintptr_t pd_phys     = 0;
     bool kstack_allocated = false; bool mm_created       = false;
     int result            = FS_ERR_GENERAL; // Use filesystem error codes
 
     proc = (pcb_t *)kmalloc(sizeof(pcb_t)); if (!proc) { terminal_write("[Process] kmalloc PCB failed.\n"); return NULL; }
     memset(proc, 0, sizeof(pcb_t)); proc->pid = next_pid++;
 
     pd_phys = frame_alloc(); if (!pd_phys) { terminal_write("[Process] frame_alloc PD failed.\n"); goto fail_create; }
     proc->page_directory_phys = (uint32_t*)pd_phys; terminal_printf("   Allocated PD Phys: 0x%x\n", (unsigned)pd_phys);
 
     if (paging_map_single_4k((uint32_t*)g_kernel_page_directory_phys, (uintptr_t)TEMP_MAP_ADDR_PD_DST, pd_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
         terminal_write("[Process] Failed to temp map new PD.\n"); goto fail_create; }
     uint32_t *proc_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD_DST; memset(proc_pd_virt, 0, PAGE_SIZE); copy_kernel_pde_entries(proc_pd_virt);
     proc_pd_virt[RECURSIVE_PDE_INDEX] = (pd_phys & PAGING_ADDR_MASK) | PAGE_PRESENT | PAGE_RW;
     paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, (uintptr_t)TEMP_MAP_ADDR_PD_DST, PAGE_SIZE); paging_invalidate_page((void*)TEMP_MAP_ADDR_PD_DST); proc_pd_virt = NULL;
 
     if (!allocate_kernel_stack(proc)) { terminal_write("[Process] allocate_kernel_stack failed.\n"); goto fail_create; }
     kstack_allocated = true;
 
     proc->mm = create_mm(proc->page_directory_phys); if (!proc->mm) { terminal_write("[Process] create_mm failed.\n"); goto fail_create; }
     mm_created = true;
 
     uintptr_t initial_brk_addr = 0;
     result = load_elf_and_init_memory(path, proc->mm, &proc->entry_point, &initial_brk_addr);
     if (result != FS_SUCCESS) { terminal_printf("[Process] load_elf_and_init_memory failed for '%s' (code %d).\n", path, result); goto fail_create; }
     proc->mm->start_brk = initial_brk_addr; proc->mm->end_brk = initial_brk_addr;
 
     { uint32_t heap_flags = VM_READ | VM_WRITE | VM_USER | VM_ANONYMOUS; uint32_t heap_page_prot = PTE_USER_DATA_FLAGS; if (g_nx_supported) heap_page_prot |= PAGE_NX_BIT; if (!insert_vma(proc->mm, initial_brk_addr, initial_brk_addr, heap_flags, heap_page_prot, NULL, 0)) { terminal_write("[Process] Warning: failed to insert zero-size heap VMA.\n"); } }
 
     { proc->user_stack_top = (void *)USER_STACK_TOP_VIRT_ADDR; uintptr_t stack_bottom = USER_STACK_BOTTOM_VIRT; uintptr_t stack_top = USER_STACK_TOP_VIRT_ADDR;
       if (stack_bottom >= stack_top || stack_top > KERNEL_SPACE_VIRT_START || stack_bottom >= KERNEL_SPACE_VIRT_START) { terminal_printf("[Process] Error: Invalid user stack definitions.\n"); goto fail_create; }
       uint32_t stk_flags = VM_READ | VM_WRITE | VM_USER | VM_GROWS_DOWN | VM_ANONYMOUS; uint32_t stk_prot = PTE_USER_DATA_FLAGS; if (g_nx_supported) stk_prot |= PAGE_NX_BIT;
       terminal_printf("USER_MAP DEBUG: Inserting User Stack VMA [0x%x - 0x%x)\n", (unsigned)stack_bottom, (unsigned)stack_top);
       if (!insert_vma(proc->mm, stack_bottom, stack_top, stk_flags, stk_prot, NULL, 0)) { terminal_printf("[Process] Failed to insert user stack VMA.\n"); goto fail_create; } }
 
     terminal_printf("[Process] Successfully created PCB PID %u for '%s'.\n", (unsigned)proc->pid, path);
     return proc;
 
 fail_create:
     terminal_printf("[Process] Cleanup after create_user_process fail (PID %u).\n", (unsigned)(proc ? proc->pid : 0));
     if (proc) {
         if (mm_created && proc->mm) { destroy_mm(proc->mm); proc->mm = NULL; }
         if (kstack_allocated && proc->kernel_stack_vaddr_top != NULL) {
             uintptr_t stack_top = (uintptr_t)proc->kernel_stack_vaddr_top; size_t stack_size = PROCESS_KSTACK_SIZE; uintptr_t stack_base = stack_top - stack_size;
             terminal_printf("   Cleaning up kernel stack V=[0x%x-0x%x)\n", (unsigned)stack_base, (unsigned)stack_top);
             for (uintptr_t v_addr = stack_base; v_addr < stack_top; v_addr += PAGE_SIZE) {
                 uintptr_t phys_addr = 0;
                 if (paging_get_physical_address((uint32_t*)g_kernel_page_directory_phys, v_addr, &phys_addr) == 0 && phys_addr != 0) put_frame(phys_addr);
                 paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, v_addr, PAGE_SIZE);
             } }
         if (proc->page_directory_phys) { put_frame((uintptr_t)proc->page_directory_phys); }
         kfree(proc);
     } else { if (pd_phys) put_frame(pd_phys); }
     return NULL;
 }
 
 // ------------------------------------------------------------------------
 // destroy_process - Frees all resources associated with a process
 // ------------------------------------------------------------------------
 void destroy_process(pcb_t *pcb) {
      if (!pcb) return; uint32_t pid = pcb->pid; terminal_printf("[Process] Destroying process PID %u.\n", (unsigned)pid);
      if (pcb->mm) { destroy_mm(pcb->mm); pcb->mm = NULL; }
      if (pcb->page_directory_phys) { put_frame((uintptr_t)pcb->page_directory_phys); pcb->page_directory_phys = NULL; }
      if (pcb->kernel_stack_vaddr_top != NULL) {
          uintptr_t stack_top = (uintptr_t)pcb->kernel_stack_vaddr_top; size_t stack_size = PROCESS_KSTACK_SIZE; uintptr_t stack_base = stack_top - stack_size;
          terminal_printf("   Freeing kernel stack: V=[0x%x-0x%x)\n", (unsigned)stack_base, (unsigned)stack_top);
          for (uintptr_t v_addr = stack_base; v_addr < stack_top; v_addr += PAGE_SIZE) { uintptr_t phys_addr = 0;
              if (paging_get_physical_address((uint32_t*)g_kernel_page_directory_phys, v_addr, &phys_addr) == 0 && phys_addr != 0) put_frame(phys_addr);
              else terminal_printf("   Warning: Could not get physical address for kernel stack V=0x%x during destroy.\n", (unsigned)v_addr);
              paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, v_addr, PAGE_SIZE); }
          pcb->kernel_stack_vaddr_top = NULL; pcb->kernel_stack_phys_base = 0;
      }
      kfree(pcb); terminal_printf("[Process] PCB PID %u resources freed.\n", (unsigned)pid);
 }