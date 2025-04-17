/**
 * process.c - Process Management Implementation
 *
 * Handles creation, destruction, and management of process control blocks (PCBs)
 * and their associated memory structures (mm_struct). Includes ELF loading.
 */

 #include "process.h"
 #include "mm.h"               // For mm_struct, vma_struct, create_mm, destroy_mm, insert_vma
 #include "kmalloc.h"          // For kmalloc, kfree
 #include "paging.h"           // For paging functions, flags, constants (PAGE_SIZE etc.)
 #include "terminal.h"         // For kernel logging
 #include "types.h"            // Core type definitions
 #include "string.h"           // For memset, memcpy
 #include "scheduler.h"        // For get_current_task()
 #include "read_file.h"        // For read_file() helper
 #include "buddy.h"            // For buddy_alloc, buddy_free (used by allocate_kernel_stack)
 #include "frame.h"            // For frame_alloc, put_frame
 #include "kmalloc_internal.h" // For ALIGN_UP (used by PAGE_ALIGN_UP in paging.h)
 #include "elf.h"              // ELF header definitions
 #include <libc/stddef.h>      // For NULL
 
 // ------------------------------------------------------------------------
 // Externals & Globals
 // ------------------------------------------------------------------------
 extern uint32_t g_kernel_page_directory_phys; // Physical address of kernel's page directory (from paging.c)
 extern bool g_nx_supported;                   // NX support flag (from paging.c)
 
 // Process ID counter
 static uint32_t next_pid = 1;
 
 // KERNEL_ASSERT definition (ensure it's available, e.g., from terminal.h or types.h)
 #ifndef KERNEL_ASSERT
 #define KERNEL_ASSERT(condition, msg) do { \
     if (!(condition)) { \
         terminal_printf("\n[ASSERT FAILED] %s at %s:%d\n", msg, __FILE__, __LINE__); \
         terminal_printf("System Halted.\n"); \
         while (1) { asm volatile("cli; hlt"); } \
     } \
 } while (0)
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
  * 1. Computes number of pages from PROCESS_KSTACK_SIZE.
  * 2. Allocates physical frames using frame_alloc().
  * 3. Reserves a contiguous VIRTUAL range using buddy_alloc(), then frees the
  * underlying physical block (keeping the virtual range reservation).
  * 4. Maps the allocated physical frames into the reserved contiguous virtual range
  * within the KERNEL'S address space.
  * 5. Sets pcb->kernel_stack_vaddr_top.
  */
 static bool allocate_kernel_stack(pcb_t *proc)
 {
     if (!proc) return false;
 
     size_t stack_alloc_size = PROCESS_KSTACK_SIZE;
 
     // Must be >= PAGE_SIZE and multiple of PAGE_SIZE
     if (stack_alloc_size == 0 || (stack_alloc_size % PAGE_SIZE) != 0) {
         terminal_printf("[Process] Error: Invalid PROCESS_KSTACK_SIZE (%u bytes).\n", (unsigned)stack_alloc_size);
         return false;
     }
 
     size_t num_pages = stack_alloc_size / PAGE_SIZE;
     terminal_printf("  Allocating %u pages (%u bytes) for kernel stack...\n", (unsigned)num_pages, (unsigned)stack_alloc_size);
 
     // --------------------------------------------------------------------
     // 1) Allocate Physical Frames
     // --------------------------------------------------------------------
     uintptr_t *phys_frames = kmalloc(num_pages * sizeof(uintptr_t));
     if (!phys_frames) {
         terminal_write("  [Process] Failed to allocate temporary array for kernel stack frames.\n");
         return false;
     }
     memset(phys_frames, 0, num_pages * sizeof(uintptr_t));
 
     size_t allocated_count = 0;
     for (size_t i = 0; i < num_pages; i++) {
         uintptr_t frame_addr = frame_alloc();
         if (!frame_addr) {
             terminal_printf("  [Process] Failed to allocate physical frame %u/%u for kernel stack.\n", (unsigned)(i + 1), (unsigned)num_pages);
             // Cleanup previously allocated frames
             for (size_t j = 0; j < allocated_count; j++) {
                 if (phys_frames[j] != 0) {
                     put_frame(phys_frames[j]);
                 }
             }
             kfree(phys_frames);
             return false;
         }
         phys_frames[i] = frame_addr;
         allocated_count++;
     }
     proc->kernel_stack_phys_base = (uint32_t)phys_frames[0]; // Store base physical frame for info/debug
     terminal_printf("  Successfully allocated %u physical frames for kernel stack.\n", (unsigned)allocated_count);
 
     // --------------------------------------------------------------------
     // 2) Reserve contiguous virtual range via buddy_alloc
     // --------------------------------------------------------------------
     void *kstack_virt_base_ptr = buddy_alloc(stack_alloc_size);
     if (!kstack_virt_base_ptr) {
         terminal_write("  [Process] Failed to reserve kernel stack virtual range via buddy_alloc.\n");
         // Cleanup frames
         for (size_t i = 0; i < allocated_count; i++) {
             put_frame(phys_frames[i]);
         }
         kfree(phys_frames);
         return false;
     }
     uintptr_t kstack_virt_base = (uintptr_t)kstack_virt_base_ptr;
 
     // We only wanted the virtual range, so free the physical block back to buddy
     buddy_free(kstack_virt_base_ptr);
     terminal_printf("  Reserved kernel stack VIRTUAL range: [0x%x - 0x%x)\n",
                     (unsigned)kstack_virt_base,
                     (unsigned)(kstack_virt_base + stack_alloc_size));
 
     // --------------------------------------------------------------------
     // 3) Map each physical frame into that virtual range in the Kernel PD
     // --------------------------------------------------------------------
     for (size_t i = 0; i < num_pages; i++) {
         uintptr_t target_vaddr = kstack_virt_base + (i * PAGE_SIZE);
         uintptr_t phys_addr    = phys_frames[i];
 
         // Map into the global kernel page directory
         int map_res = paging_map_single_4k(
             (uint32_t*)g_kernel_page_directory_phys, // Map into KERNEL's address space
             target_vaddr,
             phys_addr,
             PTE_KERNEL_DATA_FLAGS // Kernel RW, NX flags
         );
         if (map_res != 0) {
             terminal_printf("  [Process] Failed to map kernel stack page %u (V=0x%x -> P=0x%x), code=%d.\n",
                             (unsigned)i, (unsigned)target_vaddr, (unsigned)phys_addr, map_res);
 
             // Unmap previously mapped pages from KERNEL PD
             for (size_t j = 0; j < i; j++) {
                 paging_unmap_range(
                     (uint32_t*)g_kernel_page_directory_phys,
                     (kstack_virt_base + j * PAGE_SIZE),
                     PAGE_SIZE
                 );
             }
             // Free ALL allocated physical frames
             for (size_t j = 0; j < allocated_count; j++) {
                 put_frame(phys_frames[j]);
             }
             kfree(phys_frames);
             // Note: The virtual range reserved via buddy is conceptually still reserved,
             // but since the mapping failed, it won't be used. Buddy doesn't track virtual allocs.
             return false;
         }
     }
 
     // The top of the stack is at the high end of the virtual range
     proc->kernel_stack_vaddr_top = (uint32_t*)(kstack_virt_base + stack_alloc_size);
 
     // We don't need the list of physical frames anymore
     kfree(phys_frames);
 
     terminal_printf("  Kernel stack mapped: PhysBase=0x%x, VirtBase=0x%x, VirtTop=0x%x\n",
                     (unsigned)proc->kernel_stack_phys_base,
                     (unsigned)kstack_virt_base,
                     (unsigned)proc->kernel_stack_vaddr_top);
 
     return true; // Success
 }
 
 // ------------------------------------------------------------------------
 // get_current_process
 // ------------------------------------------------------------------------
 pcb_t* get_current_process(void)
 {
     tcb_t* current_tcb = get_current_task();
     if (current_tcb && current_tcb->process) {
         return current_tcb->process;
     }
     return NULL;
 }
 
 // ------------------------------------------------------------------------
 // copy_elf_segment_data - Helper to copy ELF data via temporary mapping
 // ------------------------------------------------------------------------
 static int copy_elf_segment_data(uintptr_t frame_paddr,
                                  const uint8_t* file_data_buffer,
                                  size_t file_buffer_offset,
                                  size_t size_to_copy,
                                  size_t zero_padding)
 {
     // Use the temporary mapping address defined in paging.h
     void* temp_map_addr = (void*)TEMP_MAP_ADDR_PF;
 
     // Temporarily map the physical frame into kernel space
     if (paging_map_single_4k((uint32_t*)g_kernel_page_directory_phys,
                              (uintptr_t)temp_map_addr,
                              frame_paddr,
                              PTE_KERNEL_DATA_FLAGS) != 0) // Kernel RW
     {
         terminal_printf("[Process] copy_elf_segment_data: temp map failed (paddr=0x%x).\n", (unsigned)frame_paddr);
         return -1;
     }
 
     // Ensure we stay within 4KB bounds
     KERNEL_ASSERT(size_to_copy <= PAGE_SIZE, "ELF copy: size_to_copy > PAGE_SIZE");
     KERNEL_ASSERT(zero_padding <= PAGE_SIZE, "ELF copy: zero_padding > PAGE_SIZE");
     KERNEL_ASSERT(size_to_copy + zero_padding <= PAGE_SIZE, "ELF copy + zero exceeds 4KB frame");
 
     // Copy data from ELF buffer into the newly mapped page
     if (size_to_copy > 0) {
         memcpy(temp_map_addr, file_data_buffer + file_buffer_offset, size_to_copy);
     }
     // Zero the remainder (BSS section within this page)
     if (zero_padding > 0) {
         memset((uint8_t*)temp_map_addr + size_to_copy, 0, zero_padding);
     }
 
     // Unmap the temporary region
     paging_unmap_range((uint32_t*)g_kernel_page_directory_phys,
                        (uintptr_t)temp_map_addr, PAGE_SIZE);
     // Invalidate TLB for the temporary mapping address
     paging_invalidate_page(temp_map_addr);
 
     return 0; // success
 }
 
 // ------------------------------------------------------------------------
 // load_elf_and_init_memory - Loads ELF segments and sets up initial memory
 // ------------------------------------------------------------------------
 static int load_elf_and_init_memory(const char *path,
                                     mm_struct_t *mm,
                                     uint32_t *entry_point,
                                     uintptr_t *initial_brk)
 {
     size_t file_size       = 0;
     uint8_t *file_data     = NULL;
     vma_struct_t* vma      = NULL; // Keep track of VMA for potential cleanup on error
     uintptr_t phys_page    = 0;    // Track allocated page for cleanup on error
     int result             = -1;   // Assume failure
 
     file_data = (uint8_t*)read_file(path, &file_size);
     if (!file_data) {
         terminal_printf("[Process] load_elf: read_file failed for '%s'.\n", path);
         goto cleanup_load_elf;
     }
 
     Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_data;
 
     // --- Validate ELF Header ---
     if (memcmp(ehdr->e_ident, "\x7F" "ELF", 4) != 0) {
         terminal_printf("[Process] load_elf: Invalid ELF magic.\n");
         goto cleanup_load_elf;
     }
     if (ehdr->e_type != ET_EXEC || ehdr->e_machine != EM_386 || ehdr->e_version != EV_CURRENT) {
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
     // Check that the program header table fits within the loaded file data
     if (ehdr->e_phoff > file_size || (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize) > file_size) {
         terminal_printf("[Process] load_elf: Program header table out of bounds.\n");
         goto cleanup_load_elf;
     }
 
     *entry_point = ehdr->e_entry;
     terminal_printf("  ELF Entry Point: 0x%x\n", (unsigned)*entry_point);
 
     Elf32_Phdr *phdr_table = (Elf32_Phdr *)(file_data + ehdr->e_phoff);
     uintptr_t highest_addr_loaded = 0;
 
     // --- Iterate through Program Headers (Segments) ---
     for (Elf32_Half i = 0; i < ehdr->e_phnum; i++) {
         Elf32_Phdr *phdr = &phdr_table[i];
 
         if (phdr->p_type != PT_LOAD) { continue; } // Skip non-loadable segments
         if (phdr->p_memsz == 0) { continue; }      // Skip zero-size segments
 
         terminal_printf("  Segment %d: VAddr=0x%x, MemSz=%u, FileSz=%u, Offset=0x%x, Flags=%c%c%c\n",
             (int)i, (unsigned)phdr->p_vaddr, (unsigned)phdr->p_memsz, (unsigned)phdr->p_filesz, (unsigned)phdr->p_offset,
             (phdr->p_flags & PF_X) ? 'X' : '-',
             (phdr->p_flags & PF_W) ? 'W' : '-',
             (phdr->p_flags & PF_R) ? 'R' : '-');
 
         // --- Validate Segment Sizes and Offsets ---
         if (phdr->p_filesz > phdr->p_memsz) {
             terminal_printf("   -> Error: FileSz > MemSz.\n");
             goto cleanup_load_elf;
         }
         // Check segment offset and file size against the loaded file data size
         if (phdr->p_offset > file_size || phdr->p_filesz > (file_size - phdr->p_offset)) {
             terminal_printf("   -> Error: Segment file range [0x%x-0x%x) out of bounds (file size %u).\n",
                             (unsigned)phdr->p_offset, (unsigned)(phdr->p_offset + phdr->p_filesz), (unsigned)file_size);
             goto cleanup_load_elf;
         }
 
         // --- Calculate Page-Aligned Virtual Address Range ---
         uintptr_t vm_start = PAGE_ALIGN_DOWN(phdr->p_vaddr);
         // Calculate end address carefully to avoid overflow
         uintptr_t seg_mem_end_addr = phdr->p_vaddr + phdr->p_memsz;
         if (seg_mem_end_addr < phdr->p_vaddr) seg_mem_end_addr = UINTPTR_MAX; // Overflow guard
         uintptr_t vm_end = PAGE_ALIGN_UP(seg_mem_end_addr);
         if (vm_end == 0 && seg_mem_end_addr > 0) vm_end = UINTPTR_MAX; // Overflow guard for alignment
         if (vm_start >= vm_end) continue; // Skip if segment results in zero virtual pages
 
 
         // --- Determine VMA and Page Flags ---
         uint32_t vma_flags  = VM_READ | VM_USER | VM_ANONYMOUS; // Assume readable, user, anonymous by default
         uint32_t page_prot = PAGE_PRESENT | PAGE_USER;         // Base page flags
 
         if (phdr->p_flags & PF_W) {
             vma_flags  |= VM_WRITE;
             page_prot |= PAGE_RW;
         }
         if (phdr->p_flags & PF_X) {
             vma_flags |= VM_EXEC;
             // NX Handling: Only set PAGE_NX_BIT if segment is NOT executable AND NX is supported
         } else if (g_nx_supported) {
             page_prot |= PAGE_NX_BIT; // Mark non-executable pages as NX
         }
 
         terminal_printf("   -> VMA [0x%x - 0x%x), VMA Flags=0x%x, PageProt=0x%x\n",
                         (unsigned)vm_start, (unsigned)vm_end, (unsigned)vma_flags, (unsigned)page_prot);
 
         // --- Insert the VMA ---
         vma = insert_vma(mm, vm_start, vm_end, vma_flags, page_prot, NULL, 0);
         if (!vma) {
             terminal_printf("   -> Error: Failed to insert VMA for segment %d.\n", (int)i);
             goto cleanup_load_elf; // mm struct might be inconsistent now, difficult recovery
         }
         vma = NULL; // Reset vma pointer, ownership transferred to mm struct
 
         // --- Allocate Frames, Copy Data, and Map Pages ---
         terminal_printf("   -> Mapping and populating pages...\n");
         for (uintptr_t page_v = vm_start; page_v < vm_end; page_v += PAGE_SIZE) {
 
             // 1. Allocate physical frame
             phys_page = frame_alloc();
             if (!phys_page) {
                 terminal_printf("   -> Error: Out of physical frames at V=0x%x.\n", (unsigned)page_v);
                 goto cleanup_load_elf; // Need to clean up partially created process
             }
 
             // 2. Determine ELF copy/zero overlap for this specific page
             uintptr_t copy_start_vaddr = (page_v > phdr->p_vaddr) ? page_v : phdr->p_vaddr;
             uintptr_t copy_end_vaddr_file = phdr->p_vaddr + phdr->p_filesz;
              if (copy_end_vaddr_file < phdr->p_vaddr) copy_end_vaddr_file = UINTPTR_MAX; // Overflow check
             uintptr_t copy_end_vaddr_page = page_v + PAGE_SIZE;
              if (copy_end_vaddr_page < page_v) copy_end_vaddr_page = UINTPTR_MAX; // Overflow check
 
             uintptr_t actual_copy_end_vaddr = (copy_end_vaddr_page < copy_end_vaddr_file) ? copy_end_vaddr_page : copy_end_vaddr_file;
 
             size_t copy_size_this_page = (actual_copy_end_vaddr > copy_start_vaddr) ? (actual_copy_end_vaddr - copy_start_vaddr) : 0;
             size_t file_buffer_offset = (copy_start_vaddr - phdr->p_vaddr) + phdr->p_offset;
 
             // Determine zero padding needed *within this page*
             size_t zero_padding_this_page = 0;
             uintptr_t zero_start_vaddr = copy_start_vaddr + copy_size_this_page;
             uintptr_t zero_end_vaddr_seg = phdr->p_vaddr + phdr->p_memsz;
              if (zero_end_vaddr_seg < phdr->p_vaddr) zero_end_vaddr_seg = UINTPTR_MAX; // Overflow check
             uintptr_t zero_end_vaddr_page = page_v + PAGE_SIZE;
              if (zero_end_vaddr_page < page_v) zero_end_vaddr_page = UINTPTR_MAX; // Overflow check
 
             uintptr_t actual_zero_end_vaddr = (zero_end_vaddr_page < zero_end_vaddr_seg) ? zero_end_vaddr_page : zero_end_vaddr_seg;
 
             if (actual_zero_end_vaddr > zero_start_vaddr) {
                 zero_padding_this_page = actual_zero_end_vaddr - zero_start_vaddr;
             }
 
             // Clamp to ensure total doesn't exceed page size (shouldn't happen with correct logic)
             if (copy_size_this_page + zero_padding_this_page > PAGE_SIZE) {
                  terminal_printf("   -> Error: Internal calculation error: copy+zero > PAGE_SIZE for V=0x%x\n", (unsigned)page_v);
                  put_frame(phys_page); // Free the allocated frame
                  phys_page = 0;
                  goto cleanup_load_elf;
             }
 
 
             // 3. Copy ELF data + zero BSS portion for this page
             if (copy_elf_segment_data(phys_page,
                                       file_data,
                                       file_buffer_offset,
                                       copy_size_this_page,
                                       zero_padding_this_page) != 0)
             {
                 terminal_printf("   -> Error: copy_elf_segment_data failed at V=0x%x.\n", (unsigned)page_v);
                 put_frame(phys_page); // Free the allocated frame
                 phys_page = 0;
                 goto cleanup_load_elf;
             }
 
             // 4. Map the populated frame into the process's page directory
             int map_res = paging_map_single_4k(mm->pgd_phys,
                                                page_v,
                                                phys_page,
                                                page_prot); // Use calculated page protection flags
             if (map_res != 0) {
                 terminal_printf("   -> Error: paging_map_single_4k for V=0x%x failed (code=%d).\n",
                                 (unsigned)page_v, map_res);
                 put_frame(phys_page); // Free the frame we couldn't map
                 phys_page = 0;
                 goto cleanup_load_elf;
             }
             phys_page = 0; // Reset tracker, ownership transferred to page tables
         } // End loop for pages within segment
 
         // Update highest address loaded
         uintptr_t current_segment_end = phdr->p_vaddr + phdr->p_memsz;
         if (current_segment_end < phdr->p_vaddr) current_segment_end = UINTPTR_MAX; // Overflow check
         if (current_segment_end > highest_addr_loaded) {
             highest_addr_loaded = current_segment_end;
         }
     } // End loop through program headers
 
     // Initialize program break (brk) to the page boundary after the highest loaded address
     *initial_brk = PAGE_ALIGN_UP(highest_addr_loaded);
     if (*initial_brk == 0 && highest_addr_loaded > 0) *initial_brk = UINTPTR_MAX; // Handle alignment overflow
 
     terminal_printf("  ELF load complete. initial_brk=0x%x\n", (unsigned)*initial_brk);
     result = 0; // Success
 
 cleanup_load_elf:
     if (file_data) {
         kfree(file_data);
     }
     // If an error occurred after allocating phys_page but before mapping it
     if (phys_page != 0) {
         put_frame(phys_page);
     }
     // Note: If error occurred after mapping some pages, destroy_mm (called by caller on failure)
     // should handle unmapping those pages and freeing their frames.
     return result;
 }
 
 
 // ------------------------------------------------------------------------
 // create_user_process - Creates PCB, address space, loads ELF
 // ------------------------------------------------------------------------
 pcb_t *create_user_process(const char *path)
 {
     terminal_printf("[Process] Creating user process from '%s'.\n", path);
     pcb_t *proc           = NULL;
     uintptr_t pd_phys     = 0;
     bool kstack_allocated = false;
     bool mm_created       = false;
 
     proc = (pcb_t *)kmalloc(sizeof(pcb_t));
     if (!proc) {
         terminal_write("[Process] kmalloc PCB failed.\n");
         return NULL; // Allocation failed
     }
     memset(proc, 0, sizeof(pcb_t));
     proc->pid = next_pid++;
 
     // 1) Allocate Page Directory frame
     pd_phys = frame_alloc();
     if (!pd_phys) {
         terminal_write("[Process] frame_alloc PD failed.\n");
         goto fail_create;
     }
     proc->page_directory_phys = (uint32_t*)pd_phys;
     terminal_printf("  Allocated PD Phys: 0x%x\n", (unsigned)pd_phys);
 
     // 2) Temporarily map PD to copy kernel PDE entries & set recursive entry
     //    Use the predefined temporary mapping address from paging.h
     if (paging_map_single_4k((uint32_t*)g_kernel_page_directory_phys, // Map into kernel PD
                              (uintptr_t)TEMP_MAP_ADDR_PD_DST,       // Target VA for temp mapping
                              pd_phys,                               // Physical addr of new PD
                              PTE_KERNEL_DATA_FLAGS) != 0)           // Kernel RW flags
     {
         terminal_write("[Process] Failed to temp map new PD.\n");
         goto fail_create;
     }
     uint32_t *proc_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD_DST;
 
     // Zero the new PD frame first (important!)
     memset(proc_pd_virt, 0, PAGE_SIZE);
 
     // Copy shared kernel PDE entries
     copy_kernel_pde_entries(proc_pd_virt);
 
     // Set the recursive PDE entry for the new PD
     proc_pd_virt[RECURSIVE_PDE_INDEX] = (pd_phys & PAGING_ADDR_MASK) | PAGE_PRESENT | PAGE_RW;
 
     // Unmap the temporary mapping of the new PD
     paging_unmap_range((uint32_t*)g_kernel_page_directory_phys,
                        (uintptr_t)TEMP_MAP_ADDR_PD_DST, PAGE_SIZE);
     paging_invalidate_page((void*)TEMP_MAP_ADDR_PD_DST);
     proc_pd_virt = NULL; // Don't use this pointer anymore
 
     // 3) Allocate kernel stack (multi-page approach)
     if (!allocate_kernel_stack(proc)) {
         // allocate_kernel_stack handles its own cleanup on failure
         goto fail_create;
     }
     kstack_allocated = true; // Mark stack as successfully allocated
 
     // 4) Create mm_struct + load ELF
     proc->mm = create_mm(proc->page_directory_phys);
     if (!proc->mm) {
         terminal_write("[Process] create_mm failed.\n");
         goto fail_create;
     }
     mm_created = true;
 
     uintptr_t initial_brk_addr = 0;
     if (load_elf_and_init_memory(path, proc->mm, &proc->entry_point, &initial_brk_addr) != 0) {
         terminal_printf("[Process] load_elf failed for '%s'.\n", path);
         goto fail_create;
     }
     // Set initial program break in the mm_struct
     proc->mm->start_brk = initial_brk_addr;
     proc->mm->end_brk   = initial_brk_addr;
     // Add an initial (zero-size) heap VMA if your design requires it
     {
         uint32_t heap_flags = VM_READ | VM_WRITE | VM_USER | VM_ANONYMOUS;
         uint32_t heap_page_prot = PTE_USER_DATA_FLAGS; // Includes RW, User, Present
         if (g_nx_supported) heap_page_prot |= PAGE_NX_BIT; // Non-executable heap
 
         if (!insert_vma(proc->mm, initial_brk_addr, initial_brk_addr, heap_flags, heap_page_prot, NULL, 0)) {
              terminal_write("[Process] Warning: failed to insert zero-size heap VMA.\n");
              // Not necessarily fatal, brk syscall can create it later
         }
     }
 
 
     // 5) Insert a user stack VMA
     {
         proc->user_stack_top = (void *)USER_STACK_TOP_VIRT_ADDR; // Top address grows down
         uintptr_t stack_bottom = USER_STACK_BOTTOM_VIRT; // Bottom limit
         uintptr_t stack_top    = USER_STACK_TOP_VIRT_ADDR; // Top limit
 
         // Ensure bottom < top
         if (stack_bottom >= stack_top) {
              terminal_printf("[Process] Error: Invalid user stack definitions (bottom >= top).\n");
              goto fail_create;
         }
 
         uint32_t stk_flags   = VM_READ | VM_WRITE | VM_USER | VM_GROWS_DOWN | VM_ANONYMOUS;
         uint32_t stk_prot    = PTE_USER_DATA_FLAGS; // RW, User, Present
         if (g_nx_supported) stk_prot |= PAGE_NX_BIT; // Non-executable stack
 
         terminal_printf("USER_MAP DEBUG: Inserting User Stack VMA [0x%x - 0x%x)\n",
                         (unsigned)stack_bottom, (unsigned)stack_top);
         if (!insert_vma(proc->mm, stack_bottom, stack_top, stk_flags, stk_prot, NULL, 0)) {
             terminal_printf("[Process] Failed to insert user stack VMA.\n");
             goto fail_create;
         }
     }
 
     terminal_printf("[Process] Successfully created PCB PID %u for '%s'.\n",
                     (unsigned)proc->pid, path);
     return proc; // SUCCESS!
 
 fail_create:
     terminal_printf("[Process] Cleanup after create_user_process fail (PID %u).\n", (unsigned)(proc ? proc->pid : 0));
     if (proc) {
         // Destroy mm_struct (handles VMA cleanup and unmapping user pages)
         if (mm_created && proc->mm) {
             destroy_mm(proc->mm);
             proc->mm = NULL;
         }
 
         // Cleanup kernel stack (virtual mapping and physical frames)
         if (kstack_allocated && proc->kernel_stack_vaddr_top != NULL) {
             uintptr_t stack_top = (uintptr_t)proc->kernel_stack_vaddr_top;
             size_t stack_size = PROCESS_KSTACK_SIZE;
             uintptr_t stack_base = stack_top - stack_size;
 
             for (uintptr_t v_addr = stack_base; v_addr < stack_top; v_addr += PAGE_SIZE) {
                 uintptr_t phys_addr = 0;
                 if (paging_get_physical_address((uint32_t*)g_kernel_page_directory_phys, v_addr, &phys_addr) == 0) {
                      if (phys_addr != 0) {
                           put_frame(phys_addr);
                      }
                 }
                 paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, v_addr, PAGE_SIZE);
             }
             proc->kernel_stack_vaddr_top = NULL;
             proc->kernel_stack_phys_base = 0;
         }
 
         // Free the process page directory frame
         // Note: paging_free_user_space should be called *by destroy_mm* implicitly,
         // or explicitly BEFORE freeing the PD frame if destroy_mm doesn't handle it.
         // Assuming destroy_mm called paging_free_user_space.
         if (proc->page_directory_phys) {
             put_frame((uintptr_t)proc->page_directory_phys);
             proc->page_directory_phys = NULL;
         }
 
         // Free the PCB structure itself
         kfree(proc);
     } else {
         // If PCB allocation failed but PD was allocated
         if (pd_phys) {
             put_frame(pd_phys);
         }
     }
     return NULL; // Failure
 }
 
 // ------------------------------------------------------------------------
 // destroy_process - Frees all resources associated with a process
 // ------------------------------------------------------------------------
 void destroy_process(pcb_t *pcb)
 {
     if (!pcb) return;
 
     uint32_t pid = pcb->pid;
     terminal_printf("[Process] Destroying process PID %u.\n", (unsigned)pid);
 
     // 1) Destroy Memory Management (VMAs, user page tables, user frames)
     //    This should implicitly call paging_free_user_space internally if designed well.
     if (pcb->mm) {
         destroy_mm(pcb->mm);
         pcb->mm = NULL;
     }
 
     // 2) Free the process page directory frame itself
     //    (Ensure user space was freed first by destroy_mm/paging_free_user_space)
     if (pcb->page_directory_phys) {
         put_frame((uintptr_t)pcb->page_directory_phys);
         pcb->page_directory_phys = NULL;
     }
 
     // 3) Free the kernel stack (unmap virtual, free physical frames)
     if (pcb->kernel_stack_vaddr_top != NULL) {
         uintptr_t stack_top = (uintptr_t)pcb->kernel_stack_vaddr_top;
         size_t stack_size = PROCESS_KSTACK_SIZE;
         uintptr_t stack_base = stack_top - stack_size;
 
         terminal_printf("  Freeing kernel stack: V=[0x%x-0x%x)\n", (unsigned)stack_base, (unsigned)stack_top);
 
         for (uintptr_t v_addr = stack_base; v_addr < stack_top; v_addr += PAGE_SIZE) {
             uintptr_t phys_addr = 0;
             // Get physical address mapped at v_addr IN THE KERNEL PD
             if (paging_get_physical_address((uint32_t*)g_kernel_page_directory_phys, v_addr, &phys_addr) == 0) {
                  if (phys_addr != 0) {
                       put_frame(phys_addr); // Free the physical frame
                  }
             } else {
                  terminal_printf("  Warning: Could not get physical address for kernel stack V=0x%x during destroy.\n", (unsigned)v_addr);
             }
             // Unmap the virtual page from the kernel PD
             paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, v_addr, PAGE_SIZE);
         }
         pcb->kernel_stack_vaddr_top = NULL;
         pcb->kernel_stack_phys_base = 0;
     }
 
     // 4) Free the PCB structure itself
     kfree(pcb);
     terminal_printf("[Process] PCB PID %u resources freed.\n", (unsigned)pid);
 }