/**
 * process.c - Process Management Implementation
 *
 * Handles creation, destruction, and management of process control blocks (PCBs)
 * and their associated memory structures (mm_struct). Includes ELF loading and
 * initial user context setup placeholder.
 */

 #include "process.h"
 #include "mm.h"               // For mm_struct, vma_struct, create_mm, destroy_mm, insert_vma
 #include "kmalloc.h"            // For kmalloc, kfree
 #include "paging.h"             // For paging functions, flags, constants (PAGE_SIZE etc.), registers_t
 #include "terminal.h"           // For kernel logging
 #include "types.h"              // Core type definitions
 #include "string.h"             // For memset, memcpy
 #include "scheduler.h"          // For get_current_task() (and potentially scheduler_add_ready)
 #include "read_file.h"          // For read_file() helper
 #include "frame.h"              // For frame_alloc, put_frame
 #include "kmalloc_internal.h"   // For ALIGN_UP (used by PAGE_ALIGN_UP in paging.h)
 #include "elf.h"                // ELF header definitions
 #include <libc/stddef.h>        // For NULL
 #include "assert.h"             // For KERNEL_ASSERT
 #include "gdt.h"                // For GDT_USER_CODE_SELECTOR, GDT_USER_DATA_SELECTOR
 
 // ------------------------------------------------------------------------
 // Definitions
 // ------------------------------------------------------------------------
 #ifndef KERNEL_VIRT_BASE
 #define KERNEL_VIRT_BASE 0xC0000000 // Start of kernel virtual address space
 #endif
 
 #ifndef KERNEL_STACK_VIRT_START
 #define KERNEL_STACK_VIRT_START 0xE0000000 // Example: Start of a region for kernel stacks
 #endif
 #ifndef KERNEL_STACK_VIRT_END
 #define KERNEL_STACK_VIRT_END   0xF0000000 // Example: End of the region
 #endif
 
 #ifndef MAX
 #define MAX(a, b) ((a) > (b) ? (a) : (b))
 #endif
 #ifndef MIN
 #define MIN(a, b) ((a) < (b) ? (a) : (b))
 #endif
 
 // ------------------------------------------------------------------------
 // Externals & Globals
 // ------------------------------------------------------------------------
 extern uint32_t g_kernel_page_directory_phys; // Physical address of kernel's page directory
 extern bool g_nx_supported;                   // NX support flag
 
 // Process ID counter
 static uint32_t next_pid = 1; // TODO: Add locking for SMP
 
 // Simple linear allocator for kernel stack virtual addresses
 // WARNING: Placeholder only! Not suitable for production/SMP. Needs proper allocator.
 static uintptr_t g_next_kernel_stack_virt_base = KERNEL_STACK_VIRT_START;
 
 // ------------------------------------------------------------------------
 // Local Prototypes
 // ------------------------------------------------------------------------
 static bool allocate_kernel_stack(pcb_t *proc);
 static int load_elf_and_init_memory(const char *path, mm_struct_t *mm, uint32_t *entry_point, uintptr_t *initial_brk);
 static int copy_elf_segment_data(uintptr_t frame_paddr, const uint8_t* file_data_buffer, size_t file_buffer_offset, size_t size_to_copy, size_t zero_padding);
 extern void copy_kernel_pde_entries(uint32_t *new_pd_virt); // From paging.c
 
 // ------------------------------------------------------------------------
 // allocate_kernel_stack - Allocates and maps kernel stack pages
 // ------------------------------------------------------------------------
 static bool allocate_kernel_stack(pcb_t *proc)
 {
     KERNEL_ASSERT(proc != NULL, "allocate_kernel_stack: NULL proc");
 
     size_t stack_alloc_size = PROCESS_KSTACK_SIZE; // From process.h
 
     // Validate stack size configuration
     if (stack_alloc_size == 0 || (stack_alloc_size % PAGE_SIZE) != 0) {
         terminal_printf("[Process] Error: Invalid PROCESS_KSTACK_SIZE (%u bytes) - must be multiple of %u and > 0.\n",
                         (unsigned)stack_alloc_size, (unsigned)PAGE_SIZE);
         return false;
     }
 
     size_t num_pages = stack_alloc_size / PAGE_SIZE;
     terminal_printf("  Allocating %u pages (%u bytes) for kernel stack...\n", (unsigned)num_pages, (unsigned)stack_alloc_size);
 
     // Use heap allocation for the temporary frame list to avoid kernel stack overflow
     uintptr_t *phys_frames = kmalloc(num_pages * sizeof(uintptr_t));
     if (!phys_frames) {
         terminal_write("  [Process] ERROR: kmalloc failed for phys_frames array.\n");
         return false;
     }
     memset(phys_frames, 0, num_pages * sizeof(uintptr_t));
 
     // 1. Allocate Physical Frames
     size_t allocated_count = 0;
     for (allocated_count = 0; allocated_count < num_pages; allocated_count++) {
         phys_frames[allocated_count] = frame_alloc();
         if (!phys_frames[allocated_count]) {
             terminal_printf("  [Process] ERROR: Out of physical frames allocating frame %u/%u for kernel stack.\n", (unsigned)(allocated_count + 1), (unsigned)num_pages);
             // Cleanup already allocated frames
             for (size_t j = 0; j < allocated_count; j++) {
                 put_frame(phys_frames[j]);
             }
             kfree(phys_frames);
             return false;
         }
     }
     proc->kernel_stack_phys_base = (uint32_t)phys_frames[0]; // Store base for info/debug
     terminal_printf("  Successfully allocated %u physical frames for kernel stack.\n", (unsigned)allocated_count);
 
 
     // 2. Allocate Virtual Range (Placeholder Linear Allocator)
     // TODO: Replace with a proper kernel virtual memory allocator & add locking for SMP
     uintptr_t kstack_virt_base = g_next_kernel_stack_virt_base;
     uintptr_t kstack_virt_end = kstack_virt_base + stack_alloc_size;
 
     if (kstack_virt_base < KERNEL_STACK_VIRT_START || kstack_virt_end > KERNEL_STACK_VIRT_END) {
          terminal_printf("  [Process] Error: Kernel stack virtual address space exhausted or invalid.\n");
          // Cleanup frames
          for (size_t i = 0; i < allocated_count; i++) put_frame(phys_frames[i]);
          kfree(phys_frames);
          return false;
     }
     g_next_kernel_stack_virt_base = kstack_virt_end; // Advance linear allocator pointer
     KERNEL_ASSERT((kstack_virt_base % PAGE_SIZE) == 0, "Kernel stack virt base not page aligned");
     terminal_printf("  Allocated kernel stack VIRTUAL range: [0x%x - 0x%x)\n",
                     (unsigned)kstack_virt_base, (unsigned)kstack_virt_end);
 
     // 3. Map Physical Frames to Virtual Range in Kernel Page Directory
     for (size_t i = 0; i < num_pages; i++) {
         uintptr_t target_vaddr = kstack_virt_base + (i * PAGE_SIZE);
         uintptr_t phys_addr    = phys_frames[i];
 
         int map_res = paging_map_single_4k(
             (uint32_t*)g_kernel_page_directory_phys, // Target KERNEL PD
             target_vaddr,
             phys_addr,
             PTE_KERNEL_DATA_FLAGS // Kernel RW-, NX
         );
         if (map_res != 0) {
             terminal_printf("  [Process] ERROR: Failed to map kernel stack page %u (V=0x%x -> P=0x%x), code=%d.\n",
                             (unsigned)i, (unsigned)target_vaddr, (unsigned)phys_addr, map_res);
             // Unmap already mapped pages (0 to i-1)
             if (i > 0) {
                 paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, kstack_virt_base, i * PAGE_SIZE);
             }
             // Free ALL allocated physical frames
             for (size_t j = 0; j < allocated_count; j++) put_frame(phys_frames[j]);
             kfree(phys_frames);
             // Rollback VA allocation (simple linear case)
             g_next_kernel_stack_virt_base = kstack_virt_base;
             return false;
         }
     }
 
     // Store the top virtual address (used for TSS setup or context switch init)
     proc->kernel_stack_vaddr_top = (uint32_t*)kstack_virt_end;
 
     kfree(phys_frames); // Free the temporary array
 
     terminal_printf("  Kernel stack mapped: PhysBase=0x%x, VirtBase=0x%x, VirtTop=0x%x\n",
                     (unsigned)proc->kernel_stack_phys_base,
                     (unsigned)kstack_virt_base,
                     (unsigned)proc->kernel_stack_vaddr_top);
     return true; // Success
 }
 
 // ------------------------------------------------------------------------
 // get_current_process - Retrieve PCB of the currently running process
 // ------------------------------------------------------------------------
 pcb_t* get_current_process(void)
 {
     // Assumes scheduler maintains the current task control block (TCB)
     tcb_t* current_tcb = get_current_task(); // Assuming get_current_task() exists
     if (current_tcb && current_tcb->process) {
         return current_tcb->process;
     }
     return NULL; // Early boot or kernel context
 }
 
 // ------------------------------------------------------------------------
 // copy_elf_segment_data - Populate a physical frame with ELF data
 // ------------------------------------------------------------------------
 static int copy_elf_segment_data(uintptr_t frame_paddr,
                                  const uint8_t* file_data_buffer,
                                  size_t file_buffer_offset,
                                  size_t size_to_copy,
                                  size_t zero_padding)
 {
     KERNEL_ASSERT(frame_paddr != 0 && (frame_paddr % PAGE_SIZE) == 0, "copy_elf_segment_data: Invalid physical address");
     KERNEL_ASSERT(size_to_copy + zero_padding <= PAGE_SIZE, "ELF copy + zero exceeds frame");
 
     // Temporarily map the physical frame into kernel space
     void* temp_vaddr = paging_temp_map(frame_paddr);
     if (temp_vaddr == NULL) {
         terminal_printf("[Process] copy_elf_segment_data: ERROR: paging_temp_map failed (paddr=0x%x).\n", (unsigned)frame_paddr);
         return -1;
     }
 
     // Copy data from ELF file buffer
     if (size_to_copy > 0) {
         KERNEL_ASSERT(file_data_buffer != NULL, "copy_elf_segment_data: NULL file_data_buffer");
         memcpy(temp_vaddr, file_data_buffer + file_buffer_offset, size_to_copy);
     }
     // Zero out the BSS portion within this page
     if (zero_padding > 0) {
         memset((uint8_t*)temp_vaddr + size_to_copy, 0, zero_padding);
     }
 
     // Unmap the temporary kernel mapping
     paging_temp_unmap(temp_vaddr);
     return 0; // success
 }
 
 
 // ------------------------------------------------------------------------
 // load_elf_and_init_memory - Load ELF, setup VMAs, populate pages
 // ------------------------------------------------------------------------
 static int load_elf_and_init_memory(const char *path,
                                     mm_struct_t *mm,
                                     uint32_t *entry_point,
                                     uintptr_t *initial_brk)
 {
     KERNEL_ASSERT(path != NULL && mm != NULL && entry_point != NULL && initial_brk != NULL, "load_elf: Invalid arguments");
 
     size_t file_size = 0;
     uint8_t *file_data = NULL;
     uintptr_t phys_page = 0; // Track frame allocation for cleanup
     int result = -1;         // Assume failure
 
     // 1. Read ELF file into kernel memory
     file_data = (uint8_t*)read_file(path, &file_size);
     if (!file_data) {
         terminal_printf("[Process] load_elf: ERROR: read_file failed for '%s'.\n", path);
         goto cleanup_load_elf;
     }
     if (file_size < sizeof(Elf32_Ehdr)) {
         terminal_printf("[Process] load_elf: ERROR: File '%s' too small for ELF header.\n", path);
         goto cleanup_load_elf;
     }
 
     // 2. Parse and Validate ELF Header
     Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_data;
     // --- CORRECTED ELF MAGIC CHECK ---
     if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
         ehdr->e_ident[EI_MAG2] != ELFMAG2 || ehdr->e_ident[EI_MAG3] != ELFMAG3 ||
         ehdr->e_ident[EI_CLASS] != ELFCLASS32 || ehdr->e_ident[EI_DATA] != ELFDATA2LSB || // Added Little Endian check
         ehdr->e_type != ET_EXEC || ehdr->e_machine != EM_386 ||
         ehdr->e_version != EV_CURRENT || ehdr->e_phentsize != sizeof(Elf32_Phdr) || ehdr->e_phoff == 0 ||
         ehdr->e_phnum == 0 || (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize) > file_size ||
         ehdr->e_entry == 0)
     {
          terminal_printf("[Process] load_elf: ERROR: Invalid ELF header or properties for '%s'.\n", path);
          goto cleanup_load_elf;
     }
      if (ehdr->e_entry >= KERNEL_VIRT_BASE) {
          terminal_printf("[Process] load_elf: Warning: Entry point 0x%x is in kernel space for '%s'.\n", (unsigned)ehdr->e_entry, path);
          // Allow for now, but suspicious
      }
 
     *entry_point = ehdr->e_entry;
     terminal_printf("  ELF Entry Point: 0x%x\n", (unsigned)*entry_point);
 
     // 3. Process Program Headers (Segments)
     Elf32_Phdr *phdr_table = (Elf32_Phdr *)(file_data + ehdr->e_phoff);
     uintptr_t highest_addr_loaded = 0;
 
     for (Elf32_Half i = 0; i < ehdr->e_phnum; i++) {
         Elf32_Phdr *phdr = &phdr_table[i];
 
         if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) { continue; } // Only load PT_LOAD segments with size > 0
 
         // Validate segment addresses and sizes thoroughly
         if (phdr->p_vaddr >= KERNEL_VIRT_BASE || // Starts in kernel space
            (phdr->p_vaddr + phdr->p_memsz > KERNEL_VIRT_BASE && phdr->p_vaddr < KERNEL_VIRT_BASE) || // Crosses into kernel space (and didn't start there)
            (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr) || // Wraps around address space
             phdr->p_filesz > phdr->p_memsz ||                 // File size > memory size
             phdr->p_offset > file_size || phdr->p_filesz > (file_size - phdr->p_offset)) // File data out of bounds
         {
             terminal_printf("  -> Error: Invalid segment %d geometry or placement for '%s'.\n", (int)i, path);
             goto cleanup_load_elf;
         }
 
         terminal_printf("  Segment %d: VAddr=0x%x, MemSz=%u, FileSz=%u, Offset=0x%x, Flags=%c%c%c\n",
                         (int)i, (unsigned)phdr->p_vaddr, (unsigned)phdr->p_memsz, (unsigned)phdr->p_filesz, (unsigned)phdr->p_offset,
                         (phdr->p_flags & PF_R) ? 'R' : '-',
                         (phdr->p_flags & PF_W) ? 'W' : '-',
                         (phdr->p_flags & PF_X) ? 'X' : '-');
 
         // Calculate page-aligned virtual range
         uintptr_t vm_start = PAGE_ALIGN_DOWN(phdr->p_vaddr);
         uintptr_t vm_end = PAGE_ALIGN_UP(phdr->p_vaddr + phdr->p_memsz);
         if (vm_end == 0 && (phdr->p_vaddr + phdr->p_memsz) > 0) vm_end = KERNEL_VIRT_BASE; // Handle align-up overflow
 
         if (vm_start >= vm_end) { continue; } // Skip segments that result in zero pages
 
         // Determine VMA and Page protection flags
         uint32_t vma_flags = VM_USER | VM_ANONYMOUS;
         uint32_t page_prot = PAGE_PRESENT | PAGE_USER;
         if (phdr->p_flags & PF_R) { vma_flags |= VM_READ; }
         if (phdr->p_flags & PF_W) { vma_flags |= VM_WRITE; page_prot |= PAGE_RW; }
         if (phdr->p_flags & PF_X) { vma_flags |= VM_EXEC; }
         else if (g_nx_supported)  { page_prot |= PAGE_NX_BIT; } // Set NX if not executable and supported
 
         terminal_printf("  -> VMA [0x%x - 0x%x), VMA Flags=0x%x, PageProt=0x%x\n",
                         (unsigned)vm_start, (unsigned)vm_end, (unsigned)vma_flags, (unsigned)page_prot);
 
         // Insert VMA into the memory structure
         if (!insert_vma(mm, vm_start, vm_end, vma_flags, page_prot, NULL, 0)) {
             terminal_printf("  -> Error: Failed to insert VMA for segment %d.\n", (int)i);
             goto cleanup_load_elf; // relies on destroy_mm called later
         }
 
         // Allocate frames, populate them, and map them
         terminal_printf("  -> Mapping and populating pages...\n");
         for (uintptr_t page_v = vm_start; page_v < vm_end; page_v += PAGE_SIZE) {
             // Allocate frame
             phys_page = frame_alloc();
             if (!phys_page) {
                 terminal_printf("  -> Error: Out of physical frames at V=0x%x.\n", (unsigned)page_v);
                 goto cleanup_load_elf;
             }
 
             // Calculate copy/zero overlap for this specific page
             uintptr_t file_copy_start_vaddr = phdr->p_vaddr;
             uintptr_t file_copy_end_vaddr = phdr->p_vaddr + phdr->p_filesz;
             uintptr_t page_start_vaddr = page_v;
             uintptr_t page_end_vaddr = page_v + PAGE_SIZE;
             if (page_end_vaddr < page_v) page_end_vaddr = KERNEL_VIRT_BASE; // Handle overflow
 
             uintptr_t copy_v_start = MAX(page_start_vaddr, file_copy_start_vaddr);
             uintptr_t copy_v_end = MIN(page_end_vaddr, file_copy_end_vaddr);
             size_t copy_size_this_page = (copy_v_end > copy_v_start) ? (copy_v_end - copy_v_start) : 0;
             size_t file_buffer_offset = (copy_size_this_page > 0) ? (copy_v_start - phdr->p_vaddr) + phdr->p_offset : 0;
 
             uintptr_t mem_end_vaddr = phdr->p_vaddr + phdr->p_memsz;
             if (mem_end_vaddr < phdr->p_vaddr) mem_end_vaddr = KERNEL_VIRT_BASE; // Handle overflow
             uintptr_t zero_v_start = page_start_vaddr + copy_size_this_page;
             uintptr_t zero_v_end = MIN(page_end_vaddr, mem_end_vaddr);
             size_t zero_padding_this_page = (zero_v_end > zero_v_start) ? (zero_v_end - zero_v_start) : 0;
 
             if (copy_size_this_page + zero_padding_this_page > PAGE_SIZE) {
                  terminal_printf("  -> Error: Internal calc error: copy(%u)+zero(%u) > PAGE_SIZE for V=0x%x\n",
                                  (unsigned)copy_size_this_page, (unsigned)zero_padding_this_page, (unsigned)page_v);
                  put_frame(phys_page); phys_page = 0;
                  goto cleanup_load_elf;
             }
 
             // Populate the physical frame
             if (copy_elf_segment_data(phys_page, file_data, file_buffer_offset, copy_size_this_page, zero_padding_this_page) != 0) {
                 terminal_printf("  -> Error: copy_elf_segment_data failed at V=0x%x.\n", (unsigned)page_v);
                 put_frame(phys_page); phys_page = 0;
                 goto cleanup_load_elf;
             }
 
             // Map the frame into the process's page directory
             int map_res = paging_map_single_4k(mm->pgd_phys, page_v, phys_page, page_prot);
             if (map_res != 0) {
                 terminal_printf("  -> Error: paging_map_single_4k for V=0x%x -> P=0x%x failed (code=%d).\n",
                                 (unsigned)page_v, (unsigned)phys_page, map_res);
                 put_frame(phys_page); phys_page = 0;
                 goto cleanup_load_elf;
             }
             // Successfully mapped, frame ownership transferred to page tables (managed by mm_struct)
             phys_page = 0; // Reset tracker
         } // End loop for pages
 
         // Update highest loaded address
         uintptr_t current_segment_end = phdr->p_vaddr + phdr->p_memsz;
         if (current_segment_end < phdr->p_vaddr) current_segment_end = KERNEL_VIRT_BASE;
         if (current_segment_end > highest_addr_loaded) {
             highest_addr_loaded = current_segment_end;
         }
     } // End loop through segments
 
     // 4. Set Initial Program Break
     *initial_brk = PAGE_ALIGN_UP(highest_addr_loaded);
     if (*initial_brk == 0 && highest_addr_loaded > 0) *initial_brk = KERNEL_VIRT_BASE; // Handle align up overflow
 
     terminal_printf("  ELF load complete. initial_brk=0x%x\n", (unsigned)*initial_brk);
     result = 0; // Success
 
 cleanup_load_elf:
     if (file_data) { kfree(file_data); }
     // Free frame if allocated but mapping failed
     if (phys_page != 0) { put_frame(phys_page); }
     // Cleanup of successfully mapped pages/VMAs is handled by destroy_mm if needed
     return result;
 }
 
 
 // ------------------------------------------------------------------------
 // create_user_process - Main function to create a new process
 // ------------------------------------------------------------------------
 pcb_t *create_user_process(const char *path)
 {
     KERNEL_ASSERT(path != NULL, "create_user_process: NULL path");
     terminal_printf("[Process] Creating user process from '%s'.\n", path);
 
     pcb_t *proc = NULL;
     uintptr_t pd_phys = 0;
     void* proc_pd_virt_temp = NULL;
     bool pd_mapped_temp = false;
     bool kstack_allocated = false;
     // bool mm_created = false; // Removed
     bool initial_stack_mapped = false;
     uintptr_t initial_stack_phys_frame = 0;
     int ret_status = -1; // Track status for cleanup message
 
     // 1. Allocate PCB
     proc = (pcb_t *)kmalloc(sizeof(pcb_t));
     if (!proc) {
         terminal_write("[Process] ERROR: kmalloc PCB failed.\n");
         return NULL;
     }
     memset(proc, 0, sizeof(pcb_t));
     proc->pid = next_pid++;
     // TODO: Initialize process state, e.g., proc->state = PROC_INITIALIZING;
 
     // 2. Allocate Page Directory frame
     pd_phys = frame_alloc();
     if (!pd_phys) {
         terminal_printf("[Process] ERROR: frame_alloc PD failed for PID %u.\n", (unsigned)proc->pid);
         ret_status = -1;
         goto fail_create;
     }
     proc->page_directory_phys = (uint32_t*)pd_phys;
     terminal_printf("  Allocated PD Phys: 0x%x for PID %u\n", (unsigned)pd_phys, (unsigned)proc->pid);
 
     // 3. Initialize Page Directory
     proc_pd_virt_temp = paging_temp_map(pd_phys);
     if (proc_pd_virt_temp == NULL) {
         terminal_printf("[Process] ERROR: Failed to temp map new PD for PID %u.\n", (unsigned)proc->pid);
         ret_status = -2;
         goto fail_create;
     }
     pd_mapped_temp = true;
     memset(proc_pd_virt_temp, 0, PAGE_SIZE);
     copy_kernel_pde_entries((uint32_t*)proc_pd_virt_temp);
     ((uint32_t*)proc_pd_virt_temp)[RECURSIVE_PDE_INDEX] = (pd_phys & PAGING_ADDR_MASK) | PAGE_PRESENT | PAGE_RW;
     paging_temp_unmap(proc_pd_virt_temp);
     pd_mapped_temp = false;
     proc_pd_virt_temp = NULL;
 
     // 4. Allocate Kernel Stack
     kstack_allocated = allocate_kernel_stack(proc);
     if (!kstack_allocated) {
         terminal_printf("[Process] ERROR: Failed to allocate kernel stack for PID %u.\n", (unsigned)proc->pid);
         ret_status = -3;
         goto fail_create;
     }
 
     // 5. Create Memory Management structure
     proc->mm = create_mm(proc->page_directory_phys);
     if (!proc->mm) {
         terminal_printf("[Process] ERROR: create_mm failed for PID %u.\n", (unsigned)proc->pid);
         ret_status = -4;
         goto fail_create;
     }
 
     // 6. Load ELF executable
     uintptr_t initial_brk_addr = 0;
     if (load_elf_and_init_memory(path, proc->mm, &proc->entry_point, &initial_brk_addr) != 0) {
         terminal_printf("[Process] ERROR: load_elf failed for '%s', PID %u.\n", path, (unsigned)proc->pid);
         ret_status = -5;
         goto fail_create;
     }
     proc->mm->start_brk = initial_brk_addr;
     proc->mm->end_brk   = initial_brk_addr;
 
     // 7. Setup standard VMAs (Heap placeholder, User Stack)
     // --- Heap VMA ---
     {
         uint32_t heap_flags = VM_READ | VM_WRITE | VM_USER | VM_ANONYMOUS;
         uint32_t heap_page_prot = PTE_USER_DATA_FLAGS;
         if (g_nx_supported) heap_page_prot |= PAGE_NX_BIT;
         if (!insert_vma(proc->mm, initial_brk_addr, initial_brk_addr, heap_flags, heap_page_prot, NULL, 0)) {
             terminal_printf("[Process] Warning: failed to insert zero-size heap VMA for PID %u.\n", (unsigned)proc->pid);
         }
     }
     // --- User Stack VMA ---
     {
         uintptr_t stack_bottom = USER_STACK_BOTTOM_VIRT; // From process.h
         uintptr_t stack_top    = USER_STACK_TOP_VIRT_ADDR; // From process.h
         KERNEL_ASSERT(stack_bottom < stack_top && stack_top <= KERNEL_VIRT_BASE && (stack_bottom % PAGE_SIZE) == 0 && (stack_top % PAGE_SIZE) == 0, "Invalid user stack definitions");
 
         uint32_t stk_flags = VM_READ | VM_WRITE | VM_USER | VM_GROWS_DOWN | VM_ANONYMOUS;
         uint32_t stk_prot  = PTE_USER_DATA_FLAGS;
         if (g_nx_supported) stk_prot |= PAGE_NX_BIT;
 
         terminal_printf("  Inserting User Stack VMA [0x%x - 0x%x) (Grows Down) for PID %u\n",
                         (unsigned)stack_bottom, (unsigned)stack_top, (unsigned)proc->pid);
         if (!insert_vma(proc->mm, stack_bottom, stack_top, stk_flags, stk_prot, NULL, 0)) {
             terminal_printf("[Process] ERROR: Failed to insert user stack VMA for PID %u.\n", (unsigned)proc->pid);
             ret_status = -6;
             goto fail_create;
         }
         proc->user_stack_top = (void *)USER_STACK_TOP_VIRT_ADDR;
     }
 
     // 8. Allocate and Map Initial User Stack Page (Below the Top)
     uintptr_t initial_stack_page_vaddr = USER_STACK_TOP_VIRT_ADDR - PAGE_SIZE;
     KERNEL_ASSERT(initial_stack_page_vaddr >= USER_STACK_BOTTOM_VIRT, "Initial stack page calculation error");
 
     initial_stack_phys_frame = frame_alloc();
     if (!initial_stack_phys_frame) {
         terminal_printf("[Process] ERROR: Failed to allocate initial user stack frame for PID %u.\n", (unsigned)proc->pid);
         ret_status = -7;
         goto fail_create;
     }
     terminal_printf("  Allocated initial user stack frame P=0x%x for V=0x%x\n", (unsigned)initial_stack_phys_frame, (unsigned)initial_stack_page_vaddr);
 
     uint32_t stack_page_prot = PTE_USER_DATA_FLAGS; // Present, User, RW, NX (if supported)
     if (g_nx_supported) stack_page_prot |= PAGE_NX_BIT;
 
     int map_res = paging_map_single_4k(proc->page_directory_phys,
                                        initial_stack_page_vaddr,
                                        initial_stack_phys_frame,
                                        stack_page_prot);
     if (map_res != 0) {
         terminal_printf("[Process] ERROR: Failed to map initial user stack page for PID %u (err %d)\n", (unsigned)proc->pid, map_res);
         initial_stack_mapped = false; // Mark as not mapped for cleanup
         ret_status = -8;
         goto fail_create;
     }
     initial_stack_mapped = true;
     // Frame ownership transferred to page tables
 
     // 9. Initialize User Context Placeholder
     //    The previous attempt to initialize proc->context directly was removed
     //    because context_switch.asm doesn't use it and registers_t has the wrong layout.
     //
     //    *** IMPORTANT ***
     //    You MUST implement the actual preparation for the first kernel-to-user
     //    mode switch here or in the scheduler/dispatch code. This usually involves:
     //    a) Preparing the KERNEL stack: Push values onto proc->kernel_stack_vaddr_top
     //       in the exact order expected by IRET (User SS, User ESP, EFLAGS, User CS, User EIP).
     //    b) Storing Kernel ESP: Save the resulting kernel stack pointer (after pushes)
     //       into the TCB/PCB field your context switch mechanism uses to restore ESP.
     //    c) Initializing other registers: Decide if general-purpose registers (EAX etc.)
     //       are zeroed by default or pushed onto the kernel stack as well before the
     //       IRET sequence, to be popped by the context switch restore path.
     //
     //    The current context_switch.asm only handles kernel->kernel switches via pushad/popad.
     //    A separate routine using IRET is needed for the initial user launch.
     //
     terminal_printf("  User context initialization block REMOVED. PID %u EIP=0x%x, ESP=0x%x\n",
                     (unsigned)proc->pid, (unsigned)proc->entry_point, (unsigned)proc->user_stack_top);
     terminal_printf("  WARNING: Kernel stack NOT prepared for IRET. Process will not start correctly!\n");
 
     // (Keep the register initialization lines commented out or deleted)
     // // --- Initialize general purpose registers ---
     // proc->context.eax = 0; proc->context.ebx = 0; // ... etc ...
     // // --- Initialize Segment Registers (Using GDT Selectors) ---
     // proc->context.cs = GDT_USER_CODE_SELECTOR; // ... etc ...
     // // --- Initialize Stack Pointer and Instruction Pointer ---
     // proc->context.esp = (uint32_t)proc->user_stack_top; // WRONG structure member
     // proc->context.eip = proc->entry_point;
     // // --- Initialize EFLAGS ---
     // proc->context.eflags = 0x202; // IF=1, Bit 1=1
 
 
     // 10. Add to Scheduler Ready Queue (Adapt to your scheduler implementation)
     // TODO: Create TCB if separate from PCB, set TCB state, add to ready queue.
     // KERNEL_ASSERT(proc->tcb != NULL, "Process PCB missing associated TCB");
     // proc->state = PROC_READY;
     // scheduler_add_ready(proc->tcb);
     terminal_printf("  Process PID %u configuration complete. Ready to be scheduled (manual step).\n", (unsigned)proc->pid);
 
     // --- SUCCESS ---
     terminal_printf("[Process] Successfully created PCB PID %u structure for '%s'.\n",
                     (unsigned)proc->pid, path);
     return proc; // Return the prepared PCB
 
 fail_create:
     // --- Cleanup on Failure ---
     terminal_printf("[Process] Cleanup after create_user_process failed (PID %u, Status %d).\n",
                     (unsigned)(proc ? proc->pid : 0), ret_status);
 
     // Ensure temporary PD mapping is undone if it was active
     if (pd_mapped_temp && proc_pd_virt_temp != NULL) {
         paging_temp_unmap(proc_pd_virt_temp);
     }
 
     // Free the initial user stack physical frame ONLY if it was allocated but mapping FAILED
     if (initial_stack_phys_frame != 0 && !initial_stack_mapped) {
          terminal_printf("  Freeing unmapped initial user stack frame P=0x%x\n", (unsigned)initial_stack_phys_frame);
         put_frame(initial_stack_phys_frame);
     }
     // NOTE: If stack mapping succeeded, its frame is managed by the mm_struct / VMA cleanup within destroy_process
 
     // Call destroy_process for comprehensive cleanup
     if (proc) {
         destroy_process(proc); // Handles mm, kstack, pd_phys, pcb itself
     } else {
         // Only PD frame might exist if PCB allocation failed
         if (pd_phys) {
             put_frame(pd_phys);
         }
     }
 
     return NULL; // Indicate failure
 }
 
 // ------------------------------------------------------------------------
 // destroy_process - Frees all process resources
 // ------------------------------------------------------------------------
 void destroy_process(pcb_t *pcb)
 {
     if (!pcb) return;
 
     uint32_t pid = pcb->pid;
     terminal_printf("[Process] Destroying process PID %u.\n", (unsigned)pid);
 
     // 1. Destroy Memory Management structure and associated resources
     //    (Includes VMAs, user page tables, user page frames via VMA cleanup)
     if (pcb->mm) {
         destroy_mm(pcb->mm); // Assumes destroy_mm frees user pages/tables & mm struct
         pcb->mm = NULL;
     }
 
     // 2. Free Kernel Stack (Physical frames and Kernel Virtual Mapping)
     if (pcb->kernel_stack_vaddr_top != NULL) {
         uintptr_t stack_top = (uintptr_t)pcb->kernel_stack_vaddr_top;
         size_t stack_size = PROCESS_KSTACK_SIZE;
         uintptr_t stack_base = stack_top - stack_size;
         terminal_printf("  Freeing kernel stack: V=[0x%x-0x%x)\n", (unsigned)stack_base, (unsigned)stack_top);
 
         // Iterate through virtual addresses, get physical, free frame
         for (uintptr_t v_addr = stack_base; v_addr < stack_top; v_addr += PAGE_SIZE) {
             uintptr_t phys_addr = 0;
             // Look up in KERNEL page directory
             if (paging_get_physical_address((uint32_t*)g_kernel_page_directory_phys, v_addr, &phys_addr) == 0) {
                 if (phys_addr != 0) {
                     put_frame(phys_addr); // Free the physical frame
                 } else {
                      terminal_printf("  Warning: No physical frame found for kernel stack V=0x%x during destroy.\n", (unsigned)v_addr);
                  }
             } else {
                  terminal_printf("  Warning: Could not get PTE for kernel stack V=0x%x during destroy.\n", (unsigned)v_addr);
             }
         }
         // Unmap the virtual range from the KERNEL page directory
         paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, stack_base, stack_size);
 
         pcb->kernel_stack_vaddr_top = NULL;
         pcb->kernel_stack_phys_base = 0;
     }
 
     // 3. Free the process's Page Directory frame
     //    (Must happen AFTER destroy_mm frees user page tables)
     if (pcb->page_directory_phys) {
         terminal_printf("  Freeing process PD frame: P=0x%x\n", (unsigned)pcb->page_directory_phys);
         put_frame((uintptr_t)pcb->page_directory_phys);
         pcb->page_directory_phys = NULL;
     }
 
     // 4. Free the PCB structure itself
     kfree(pcb);
     terminal_printf("[Process] PCB PID %u resources freed.\n", (unsigned)pid);
 }