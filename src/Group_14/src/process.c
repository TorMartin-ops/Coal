/**
 * process.c - Process Management Implementation (Corrected Version 2)
 *
 * Handles creation, destruction, and management of process control blocks (PCBs)
 * and their associated memory structures (mm_struct). Includes ELF loading.
 */

 #include "process.h"
 #include "mm.h"               // For mm_struct, vma_struct, create_mm, destroy_mm, insert_vma
 #include "kmalloc.h"           // For kmalloc, kfree
 #include "paging.h"           // For paging functions, flags, constants (PAGE_SIZE etc.)
 #include "terminal.h"         // For kernel logging
 #include "types.h"             // Core type definitions
 #include "string.h"           // For memset, memcpy
 #include "scheduler.h"         // For get_current_task()
 #include "read_file.h"         // For read_file() helper
 // #include "buddy.h"          // No longer needed for kernel stack VA reservation
 #include "frame.h"             // For frame_alloc, put_frame
 #include "kmalloc_internal.h" // For ALIGN_UP (used by PAGE_ALIGN_UP in paging.h)
 #include "elf.h"               // ELF header definitions
 #include <libc/stddef.h>       // For NULL
 #include "assert.h"           // For KERNEL_ASSERT
 
 // ------------------------------------------------------------------------
 // Local Definitions (Ideally should be in appropriate headers)
 // ------------------------------------------------------------------------
 #ifndef KERNEL_VIRT_BASE
 #define KERNEL_VIRT_BASE 0xC0000000 // Start of kernel virtual address space
 #endif
 
 #ifndef MAX
 #define MAX(a, b) ((a) > (b) ? (a) : (b))
 #endif
 #ifndef MIN
 #define MIN(a, b) ((a) < (b) ? (a) : (b))
 #endif
 
 
 // ------------------------------------------------------------------------
 // Constants & Configuration
 // ------------------------------------------------------------------------
 
 // Define the virtual address space region reserved for kernel stacks
 // IMPORTANT: These must be defined in a header (e.g., process.h or paging.h)
 // and must not overlap with other kernel virtual memory regions (heap, etc.)
 // Example values:
 #ifndef KERNEL_STACK_VIRT_START
 #define KERNEL_STACK_VIRT_START 0xE0000000 // Example: Start of a 256MB region for stacks
 #endif
 #ifndef KERNEL_STACK_VIRT_END
 #define KERNEL_STACK_VIRT_END   0xF0000000 // Example: End of the region
 #endif
 
 // ------------------------------------------------------------------------
 // Externals & Globals
 // ------------------------------------------------------------------------
 extern uint32_t g_kernel_page_directory_phys; // Physical address of kernel's page directory (from paging.c)
 extern bool g_nx_supported;                   // NX support flag (from paging.c)
 
 // Process ID counter
 static uint32_t next_pid = 1;
 
 // Simple allocator for kernel stack virtual addresses within the dedicated range
 // NOTE: This is a placeholder. A real OS needs a proper bitmap/list allocator
 // and potentially locking if SMP is enabled.
 static uintptr_t g_next_kernel_stack_virt_base = KERNEL_STACK_VIRT_START;
 
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
  * 3. Allocates a contiguous VIRTUAL range from the dedicated kernel stack area.
  * 4. Maps the allocated physical frames into the allocated contiguous virtual range
  * within the KERNEL'S address space (kernel page directory).
  * 5. Sets pcb->kernel_stack_vaddr_top.
  */
 static bool allocate_kernel_stack(pcb_t *proc)
 {
     KERNEL_ASSERT(proc != NULL, "allocate_kernel_stack: NULL proc");
 
     size_t stack_alloc_size = PROCESS_KSTACK_SIZE;
 
     // Validate stack size configuration
     if (stack_alloc_size == 0 || (stack_alloc_size % PAGE_SIZE) != 0) {
         terminal_printf("[Process] Error: Invalid PROCESS_KSTACK_SIZE (%u bytes) - must be multiple of %u and > 0.\n",
                         (unsigned)stack_alloc_size, (unsigned)PAGE_SIZE);
         return false;
     }
 
     size_t num_pages = stack_alloc_size / PAGE_SIZE;
     terminal_printf("  Allocating %u pages (%u bytes) for kernel stack...\n", (unsigned)num_pages, (unsigned)stack_alloc_size);
 
     // --------------------------------------------------------------------
     // 1) Allocate Physical Frames
     // --------------------------------------------------------------------
     // Use a temporary array on the *current* kernel stack (caller's stack)
     // If num_pages is large, consider kmalloc, but usually it's small (e.g., 4 pages).
     // KERNEL_ASSERT(num_pages < 16, "Kernel stack size excessively large?"); // Sanity check
     if (num_pages >= 16) { // Use kmalloc for larger stacks to avoid stack overflow
          terminal_printf("[Process] Warning: Large kernel stack requested (%u pages), using kmalloc for temp frame list.\n", (unsigned)num_pages);
          // Fall through to kmalloc based allocation below
     } else {
          // Allocate on current stack if small enough
          uintptr_t phys_frames_on_stack[num_pages];
          memset(phys_frames_on_stack, 0, sizeof(phys_frames_on_stack));
 
          size_t allocated_count = 0;
          for (size_t i = 0; i < num_pages; i++) {
             uintptr_t frame_addr = frame_alloc();
             if (!frame_addr) {
                 terminal_printf("  [Process] Failed to allocate physical frame %u/%u for kernel stack (Out of Memory?).\n", (unsigned)(i + 1), (unsigned)num_pages);
                 // Cleanup previously allocated frames
                 for (size_t j = 0; j < allocated_count; j++) {
                     if (phys_frames_on_stack[j] != 0) {
                         put_frame(phys_frames_on_stack[j]);
                     }
                 }
                 return false; // Physical frame allocation failed
             }
             phys_frames_on_stack[i] = frame_addr;
             allocated_count++;
          }
          proc->kernel_stack_phys_base = (uint32_t)phys_frames_on_stack[0]; // Store base physical frame
          terminal_printf("  Successfully allocated %u physical frames for kernel stack.\n", (unsigned)allocated_count);
 
 
          // --------------------------------------------------------------------
          // 2) Allocate contiguous virtual range from dedicated kernel stack area
          // --------------------------------------------------------------------
          // --- Placeholder Virtual Allocator ---
          // NOTE: Add locking if using this in an SMP environment!
          uintptr_t kstack_virt_base = g_next_kernel_stack_virt_base;
          g_next_kernel_stack_virt_base += stack_alloc_size;
 
          // Basic check for overlap/exhaustion
          if (g_next_kernel_stack_virt_base >= KERNEL_STACK_VIRT_END || kstack_virt_base < KERNEL_STACK_VIRT_START) {
              terminal_printf("  [Process] Error: Kernel stack virtual address space exhausted or invalid range.\n");
              g_next_kernel_stack_virt_base = kstack_virt_base; // Rollback allocation pointer
 
              // Cleanup frames
              for (size_t i = 0; i < allocated_count; i++) {
                  put_frame(phys_frames_on_stack[i]);
              }
              return false;
          }
          // --- End Placeholder Virtual Allocator ---
 
          // Ensure the allocated virtual base is page-aligned (it should be if KERNEL_STACK_VIRT_START and stack_alloc_size are page-aligned)
          KERNEL_ASSERT((kstack_virt_base % PAGE_SIZE) == 0, "Kernel stack virtual base not page aligned");
 
          terminal_printf("  Allocated kernel stack VIRTUAL range: [0x%x - 0x%x)\n",
                          (unsigned)kstack_virt_base,
                          (unsigned)(kstack_virt_base + stack_alloc_size));
 
          // --------------------------------------------------------------------
          // 3) Map each physical frame into that virtual range in the Kernel PD
          // --------------------------------------------------------------------
          bool mapping_ok = true;
          for (size_t i = 0; i < num_pages; i++) {
             uintptr_t target_vaddr = kstack_virt_base + (i * PAGE_SIZE);
             uintptr_t phys_addr    = phys_frames_on_stack[i];
 
             // Map into the global kernel page directory
             int map_res = paging_map_single_4k(
                 (uint32_t*)g_kernel_page_directory_phys, // Map into KERNEL's address space
                 target_vaddr,
                 phys_addr,
                 PTE_KERNEL_DATA_FLAGS // Kernel RW, NX flags (usually stacks aren't executable)
             );
             if (map_res != 0) {
                 terminal_printf("  [Process] Failed to map kernel stack page %u (V=0x%x -> P=0x%x), code=%d.\n",
                                 (unsigned)i, (unsigned)target_vaddr, (unsigned)phys_addr, map_res);
 
                 // Unmap previously successfully mapped pages (0 to i-1) from KERNEL PD
                 if (i > 0) {
                     paging_unmap_range(
                         (uint32_t*)g_kernel_page_directory_phys,
                         kstack_virt_base,
                         i * PAGE_SIZE // Unmap only the pages successfully mapped so far
                     );
                 }
                 // Free ALL allocated physical frames (0 to allocated_count-1)
                 for (size_t j = 0; j < allocated_count; j++) {
                     put_frame(phys_frames_on_stack[j]);
                 }
                 // Rollback VA allocation
                  g_next_kernel_stack_virt_base = kstack_virt_base;
                 mapping_ok = false;
                 break; // Exit mapping loop
             }
          }
 
          if (!mapping_ok) {
              return false; // Mapping failed
          }
 
          // The top of the stack is at the high end of the virtual range
          proc->kernel_stack_vaddr_top = (uint32_t*)(kstack_virt_base + stack_alloc_size);
 
          terminal_printf("  Kernel stack mapped: PhysBase=0x%x, VirtBase=0x%x, VirtTop=0x%x\n",
                          (unsigned)proc->kernel_stack_phys_base,
                          (unsigned)kstack_virt_base,
                          (unsigned)proc->kernel_stack_vaddr_top);
 
          return true; // Success using on-stack temp array
 
     } // End of stack allocation block
 
 
     // Fallback to kmalloc based allocation if stack is large or if stack allocation block above fails
     uintptr_t *phys_frames_heap = kmalloc(num_pages * sizeof(uintptr_t));
     if (!phys_frames_heap) {
        terminal_write("  [Process] Failed to allocate temporary array (heap) for kernel stack frames.\n");
        return false;
     }
     memset(phys_frames_heap, 0, num_pages * sizeof(uintptr_t));
 
     size_t allocated_count_heap = 0;
     for (size_t i = 0; i < num_pages; i++) {
        uintptr_t frame_addr = frame_alloc();
        if (!frame_addr) {
            terminal_printf("  [Process] Failed to allocate physical frame %u/%u for kernel stack (Out of Memory?).\n", (unsigned)(i + 1), (unsigned)num_pages);
            // Cleanup previously allocated frames
            for (size_t j = 0; j < allocated_count_heap; j++) {
                if (phys_frames_heap[j] != 0) {
                    put_frame(phys_frames_heap[j]);
                }
            }
            kfree(phys_frames_heap);
            return false; // Physical frame allocation failed
        }
        phys_frames_heap[i] = frame_addr;
        allocated_count_heap++;
     }
     proc->kernel_stack_phys_base = (uint32_t)phys_frames_heap[0]; // Store base physical frame
     terminal_printf("  Successfully allocated %u physical frames for kernel stack (using heap temp).\n", (unsigned)allocated_count_heap);
 
 
     // --------------------------------------------------------------------
     // 2) Allocate contiguous virtual range from dedicated kernel stack area
     // --------------------------------------------------------------------
     // --- Placeholder Virtual Allocator ---
     // NOTE: Add locking if using this in an SMP environment!
     uintptr_t kstack_virt_base_heap = g_next_kernel_stack_virt_base;
     g_next_kernel_stack_virt_base += stack_alloc_size;
 
     // Basic check for overlap/exhaustion
     if (g_next_kernel_stack_virt_base >= KERNEL_STACK_VIRT_END || kstack_virt_base_heap < KERNEL_STACK_VIRT_START) {
         terminal_printf("  [Process] Error: Kernel stack virtual address space exhausted or invalid range.\n");
         g_next_kernel_stack_virt_base = kstack_virt_base_heap; // Rollback allocation pointer
 
         // Cleanup frames
         for (size_t i = 0; i < allocated_count_heap; i++) {
             put_frame(phys_frames_heap[i]);
         }
         kfree(phys_frames_heap);
         return false;
     }
     // --- End Placeholder Virtual Allocator ---
 
     // Ensure the allocated virtual base is page-aligned
     KERNEL_ASSERT((kstack_virt_base_heap % PAGE_SIZE) == 0, "Kernel stack virtual base not page aligned");
 
     terminal_printf("  Allocated kernel stack VIRTUAL range: [0x%x - 0x%x)\n",
                     (unsigned)kstack_virt_base_heap,
                     (unsigned)(kstack_virt_base_heap + stack_alloc_size));
 
     // --------------------------------------------------------------------
     // 3) Map each physical frame into that virtual range in the Kernel PD
     // --------------------------------------------------------------------
     bool mapping_ok_heap = true;
     for (size_t i = 0; i < num_pages; i++) {
        uintptr_t target_vaddr = kstack_virt_base_heap + (i * PAGE_SIZE);
        uintptr_t phys_addr    = phys_frames_heap[i];
 
        int map_res = paging_map_single_4k(
            (uint32_t*)g_kernel_page_directory_phys,
            target_vaddr,
            phys_addr,
            PTE_KERNEL_DATA_FLAGS
        );
        if (map_res != 0) {
            terminal_printf("  [Process] Failed to map kernel stack page %u (V=0x%x -> P=0x%x), code=%d.\n",
                            (unsigned)i, (unsigned)target_vaddr, (unsigned)phys_addr, map_res);
 
            // Unmap previously successfully mapped pages (0 to i-1)
            if (i > 0) {
                paging_unmap_range(
                    (uint32_t*)g_kernel_page_directory_phys,
                    kstack_virt_base_heap,
                    i * PAGE_SIZE
                );
            }
            // Free ALL allocated physical frames (0 to allocated_count_heap-1)
            for (size_t j = 0; j < allocated_count_heap; j++) {
                put_frame(phys_frames_heap[j]);
            }
            kfree(phys_frames_heap); // Free the temp array
            // Rollback VA allocation
             g_next_kernel_stack_virt_base = kstack_virt_base_heap;
            mapping_ok_heap = false;
            break; // Exit mapping loop
        }
     }
 
     if (!mapping_ok_heap) {
         kfree(phys_frames_heap); // Make sure temp array is freed on error
         return false; // Mapping failed
     }
 
     // The top of the stack is at the high end of the virtual range
     proc->kernel_stack_vaddr_top = (uint32_t*)(kstack_virt_base_heap + stack_alloc_size);
 
     // We don't need the list of physical frames anymore (ownership transferred to page tables)
     kfree(phys_frames_heap);
 
     terminal_printf("  Kernel stack mapped: PhysBase=0x%x, VirtBase=0x%x, VirtTop=0x%x\n",
                     (unsigned)proc->kernel_stack_phys_base,
                     (unsigned)kstack_virt_base_heap,
                     (unsigned)proc->kernel_stack_vaddr_top);
 
     return true; // Success using heap temp array
 
 }
 
 
 // ------------------------------------------------------------------------
 // get_current_process
 // ------------------------------------------------------------------------
 pcb_t* get_current_process(void)
 {
     tcb_t* current_tcb = get_current_task();
     // Check TCB first, then its associated process pointer
     if (current_tcb && current_tcb->process) {
         return current_tcb->process;
     }
     // This might happen early in boot or if kernel runs without a process context
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
     KERNEL_ASSERT(frame_paddr != 0, "copy_elf_segment_data: Zero physical address");
     KERNEL_ASSERT((frame_paddr % PAGE_SIZE) == 0, "copy_elf_segment_data: Unaligned physical address");
 
     // Use the temporary mapping function which returns the virtual address
     void* temp_vaddr = paging_temp_map(frame_paddr);
     if (temp_vaddr == NULL) {
         terminal_printf("[Process] copy_elf_segment_data: paging_temp_map failed (paddr=0x%x).\n", (unsigned)frame_paddr);
         return -1;
     }
 
     // Ensure we stay within 4KB bounds for this single page operation
     KERNEL_ASSERT(size_to_copy <= PAGE_SIZE, "ELF copy: size_to_copy > PAGE_SIZE");
     KERNEL_ASSERT(zero_padding <= PAGE_SIZE, "ELF copy: zero_padding > PAGE_SIZE");
     KERNEL_ASSERT(size_to_copy + zero_padding <= PAGE_SIZE, "ELF copy + zero exceeds 4KB frame");
 
     // Copy data from ELF buffer into the newly mapped page using the returned virtual address
     if (size_to_copy > 0) {
         // KERNEL_ASSERT(file_data_buffer != NULL, "copy_elf_segment_data: NULL file_data_buffer"); // Caller should check
         memcpy(temp_vaddr, file_data_buffer + file_buffer_offset, size_to_copy);
     }
     // Zero the remainder (BSS section within this page)
     if (zero_padding > 0) {
         memset((uint8_t*)temp_vaddr + size_to_copy, 0, zero_padding);
     }
 
     // Unmap the temporary region using the virtual address
     paging_temp_unmap(temp_vaddr);
 
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
     KERNEL_ASSERT(path != NULL, "load_elf: NULL path");
     KERNEL_ASSERT(mm != NULL, "load_elf: NULL mm_struct");
     KERNEL_ASSERT(entry_point != NULL, "load_elf: NULL entry_point pointer");
     KERNEL_ASSERT(initial_brk != NULL, "load_elf: NULL initial_brk pointer");
 
     size_t file_size       = 0;
     uint8_t *file_data     = NULL;
     vma_struct_t* vma      = NULL; // Track VMA for potential cleanup on error
     uintptr_t phys_page    = 0;    // Track allocated page for cleanup on error
     int result             = -1;   // Assume failure
 
     file_data = (uint8_t*)read_file(path, &file_size);
     if (!file_data) {
         terminal_printf("[Process] load_elf: read_file failed for '%s'.\n", path);
         goto cleanup_load_elf; // No resources allocated yet besides maybe kmalloc in read_file
     }
     if (file_size < sizeof(Elf32_Ehdr)) {
         terminal_printf("[Process] load_elf: File '%s' too small for ELF header.\n", path);
         goto cleanup_load_elf;
     }
 
     Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_data;
 
     // --- Validate ELF Header ---
     // Use standard ELF magic check
     if (memcmp(ehdr->e_ident, "\x7F" "ELF", 4) != 0) {
         terminal_printf("[Process] load_elf: Invalid ELF magic for '%s'.\n", path);
         goto cleanup_load_elf;
     }
     if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
         terminal_printf("[Process] load_elf: Not a 32-bit ELF file '%s'.\n", path);
         goto cleanup_load_elf;
     }
      if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
         terminal_printf("[Process] load_elf: Not Little Endian ELF file '%s'.\n", path);
         goto cleanup_load_elf;
     }
     if (ehdr->e_type != ET_EXEC) {
         terminal_printf("[Process] load_elf: Not an executable ELF file '%s' (type=%u).\n", path, ehdr->e_type);
          goto cleanup_load_elf;
     }
     if (ehdr->e_machine != EM_386) {
          terminal_printf("[Process] load_elf: Invalid machine type for '%s' (machine=%u).\n", path, ehdr->e_machine);
         goto cleanup_load_elf;
     }
     if (ehdr->e_version != EV_CURRENT) {
          terminal_printf("[Process] load_elf: Invalid ELF version for '%s'.\n", path);
         goto cleanup_load_elf;
     }
     if (ehdr->e_phentsize != sizeof(Elf32_Phdr)) {
         terminal_printf("[Process] load_elf: Invalid program header size for '%s'.\n", path);
         goto cleanup_load_elf;
     }
     if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
         terminal_printf("[Process] load_elf: No program headers found in '%s'.\n", path);
         goto cleanup_load_elf;
     }
     // Check that the program header table fits within the loaded file data
     if (ehdr->e_phoff > file_size || (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize) > file_size) {
         terminal_printf("[Process] load_elf: Program header table out of bounds in '%s'.\n", path);
         goto cleanup_load_elf;
     }
     if (ehdr->e_entry == 0) {
         terminal_printf("[Process] load_elf: Zero entry point in '%s'.\n", path);
         goto cleanup_load_elf;
     }
     // Basic check: Entry point should probably be within user space range
     if (ehdr->e_entry >= KERNEL_VIRT_BASE) {
         terminal_printf("[Process] load_elf: Warning: Entry point 0x%x is in kernel space for '%s'.\n", (unsigned)ehdr->e_entry, path);
         // Allow for now, but suspicious
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
 
         // Ensure segment fits within user space virtual memory limits
         if (phdr->p_vaddr >= KERNEL_VIRT_BASE || (phdr->p_vaddr + phdr->p_memsz) > KERNEL_VIRT_BASE) {
              terminal_printf("  -> Error: Segment %d VAddr range [0x%x - 0x%x) overlaps kernel space.\n",
                             (int)i, (unsigned)phdr->p_vaddr, (unsigned)(phdr->p_vaddr + phdr->p_memsz));
              goto cleanup_load_elf;
         }
          if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr) { // Check for address wrap-around
              terminal_printf("  -> Error: Segment %d VAddr range wraps around.\n", (int)i);
              goto cleanup_load_elf;
          }
 
 
         terminal_printf("  Segment %d: VAddr=0x%x, MemSz=%u, FileSz=%u, Offset=0x%x, Flags=%c%c%c\n",
                       (int)i, (unsigned)phdr->p_vaddr, (unsigned)phdr->p_memsz, (unsigned)phdr->p_filesz, (unsigned)phdr->p_offset,
                       (phdr->p_flags & PF_R) ? 'R' : '-', // Read flag corrected
                       (phdr->p_flags & PF_W) ? 'W' : '-',
                       (phdr->p_flags & PF_X) ? 'X' : '-');
 
 
         // --- Validate Segment Sizes and Offsets ---
         if (phdr->p_filesz > phdr->p_memsz) {
             terminal_printf("  -> Error: FileSz (%u) > MemSz (%u).\n", (unsigned)phdr->p_filesz, (unsigned)phdr->p_memsz);
             goto cleanup_load_elf;
         }
         // Check segment offset and file size against the loaded file data size
         if (phdr->p_offset > file_size || phdr->p_filesz > (file_size - phdr->p_offset)) {
             terminal_printf("  -> Error: Segment file range [0x%x-0x%x) out of bounds (file size %u).\n",
                             (unsigned)phdr->p_offset, (unsigned)(phdr->p_offset + phdr->p_filesz), (unsigned)file_size);
             goto cleanup_load_elf;
         }
 
         // --- Calculate Page-Aligned Virtual Address Range ---
         uintptr_t vm_start = PAGE_ALIGN_DOWN(phdr->p_vaddr);
         uintptr_t seg_mem_end_addr = phdr->p_vaddr + phdr->p_memsz;
         // Overflow check already done above
         uintptr_t vm_end = PAGE_ALIGN_UP(seg_mem_end_addr);
         // Handle PAGE_ALIGN_UP overflow to 0
         if (vm_end == 0 && seg_mem_end_addr > 0) vm_end = KERNEL_VIRT_BASE; // Align up overflow means end is max user addr
 
         if (vm_start >= vm_end) {
              terminal_printf("  -> Warning: Segment %d results in zero virtual pages, skipping.\n", (int)i);
              continue;
         }
 
 
         // --- Determine VMA and Page Flags ---
         uint32_t vma_flags  = VM_USER | VM_ANONYMOUS; // User accessible, assume anonymous initially
         uint32_t page_prot = PAGE_PRESENT | PAGE_USER;    // Base page flags
 
         if (phdr->p_flags & PF_R) { vma_flags |= VM_READ;  }
         if (phdr->p_flags & PF_W) {
             vma_flags |= VM_WRITE;
             page_prot |= PAGE_RW; // Writable page
         }
         if (phdr->p_flags & PF_X) {
             vma_flags |= VM_EXEC;
             // NX Handling: Only set PAGE_NX_BIT if segment is NOT executable AND NX is supported
         } else if (g_nx_supported) {
             page_prot |= PAGE_NX_BIT; // Mark non-executable pages as NX
         }
 
         terminal_printf("  -> VMA [0x%x - 0x%x), VMA Flags=0x%x, PageProt=0x%x\n",
                         (unsigned)vm_start, (unsigned)vm_end, (unsigned)vma_flags, (unsigned)page_prot);
 
         // --- Insert the VMA ---
         vma = insert_vma(mm, vm_start, vm_end, vma_flags, page_prot, NULL, 0);
         if (!vma) {
             terminal_printf("  -> Error: Failed to insert VMA for segment %d.\n", (int)i);
             // Note: mm struct might be inconsistent now. destroy_mm in the caller should handle this.
             goto cleanup_load_elf;
         }
         vma = NULL; // Reset vma pointer, ownership transferred to mm struct
 
         // --- Allocate Frames, Copy Data, and Map Pages ---
         terminal_printf("  -> Mapping and populating pages...\n");
         for (uintptr_t page_v = vm_start; page_v < vm_end; page_v += PAGE_SIZE) {
 
             // 1. Allocate physical frame
             phys_page = frame_alloc();
             if (!phys_page) {
                 terminal_printf("  -> Error: Out of physical frames at V=0x%x.\n", (unsigned)page_v);
                 // Note: Need to clean up partially created process via caller's destroy_mm.
                 goto cleanup_load_elf;
             }
 
             // 2. Determine ELF copy/zero overlap for this specific page
             //    Calculate the intersection of [page_v, page_v + PAGE_SIZE) with [phdr->p_vaddr, phdr->p_vaddr + phdr->p_filesz)
             uintptr_t file_copy_start_vaddr = phdr->p_vaddr;
             uintptr_t file_copy_end_vaddr   = phdr->p_vaddr + phdr->p_filesz;
             // Overflow check done previously
 
             uintptr_t page_start_vaddr = page_v;
             uintptr_t page_end_vaddr   = page_v + PAGE_SIZE;
             if (page_end_vaddr < page_v) page_end_vaddr = KERNEL_VIRT_BASE; // Handle overflow
 
             // Find intersection for copying using locally defined MAX/MIN
             uintptr_t copy_v_start = MAX(page_start_vaddr, file_copy_start_vaddr);
             uintptr_t copy_v_end   = MIN(page_end_vaddr, file_copy_end_vaddr);
 
             size_t copy_size_this_page = (copy_v_end > copy_v_start) ? (copy_v_end - copy_v_start) : 0;
             size_t file_buffer_offset = (copy_size_this_page > 0) ? (copy_v_start - phdr->p_vaddr) + phdr->p_offset : 0;
 
             // Calculate zero padding (BSS) for the rest of the page up to MemSz
             uintptr_t mem_end_vaddr = phdr->p_vaddr + phdr->p_memsz;
              if (mem_end_vaddr < phdr->p_vaddr) mem_end_vaddr = KERNEL_VIRT_BASE; // Handle overflow
 
             uintptr_t zero_v_start = page_start_vaddr + copy_size_this_page; // Start zeroing after copied data
             uintptr_t zero_v_end   = MIN(page_end_vaddr, mem_end_vaddr);      // Zero up to segment end or page end
 
             size_t zero_padding_this_page = (zero_v_end > zero_v_start) ? (zero_v_end - zero_v_start) : 0;
 
             // Final sanity check
             if (copy_size_this_page + zero_padding_this_page > PAGE_SIZE) {
                  terminal_printf("  -> Error: Internal calculation error: copy(%u)+zero(%u) > PAGE_SIZE for V=0x%x\n",
                                 (unsigned)copy_size_this_page, (unsigned)zero_padding_this_page, (unsigned)page_v);
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
                 terminal_printf("  -> Error: copy_elf_segment_data failed at V=0x%x.\n", (unsigned)page_v);
                 put_frame(phys_page); // Free the allocated frame
                 phys_page = 0;
                 goto cleanup_load_elf;
             }
 
             // 4. Map the populated frame into the process's page directory
             //    Map using the page protection flags calculated earlier for this segment
             int map_res = paging_map_single_4k(mm->pgd_phys,
                                                page_v,
                                                phys_page,
                                                page_prot);
             if (map_res != 0) {
                 terminal_printf("  -> Error: paging_map_single_4k for V=0x%x -> P=0x%x failed (code=%d).\n",
                                 (unsigned)page_v, (unsigned)phys_page, map_res);
                 put_frame(phys_page); // Free the frame we couldn't map
                 phys_page = 0;
                 // Again, rely on caller's destroy_mm to clean up partially mapped segments
                 goto cleanup_load_elf;
             }
             phys_page = 0; // Reset tracker, frame ownership transferred to page tables (via mm_struct)
         } // End loop for pages within segment
 
         // Update highest address loaded across all segments
         uintptr_t current_segment_end = phdr->p_vaddr + phdr->p_memsz;
         if (current_segment_end < phdr->p_vaddr) current_segment_end = KERNEL_VIRT_BASE; // Handle overflow
         if (current_segment_end > highest_addr_loaded) {
             highest_addr_loaded = current_segment_end;
         }
     } // End loop through program headers
 
     // Initialize program break (brk) to the page boundary after the highest loaded address
     *initial_brk = PAGE_ALIGN_UP(highest_addr_loaded);
      // Handle PAGE_ALIGN_UP overflow to 0
     if (*initial_brk == 0 && highest_addr_loaded > 0) *initial_brk = KERNEL_VIRT_BASE;
 
     terminal_printf("  ELF load complete. initial_brk=0x%x\n", (unsigned)*initial_brk);
     result = 0; // Success
 
 cleanup_load_elf:
     if (file_data) {
         kfree(file_data); // Free the buffer holding the ELF file contents
     }
     // If an error occurred after allocating phys_page but before mapping it successfully
     if (phys_page != 0) {
         put_frame(phys_page);
     }
     // Note: If error occurred after mapping some pages, destroy_mm (called by create_user_process on failure)
     // is responsible for unmapping those pages and freeing their frames via the VMA cleanup.
     return result;
 }
 
 
 // ------------------------------------------------------------------------
 // create_user_process - Creates PCB, address space, loads ELF
 // ------------------------------------------------------------------------
 pcb_t *create_user_process(const char *path)
 {
     KERNEL_ASSERT(path != NULL, "create_user_process: NULL path");
     terminal_printf("[Process] Creating user process from '%s'.\n", path);
     pcb_t *proc           = NULL;
     uintptr_t pd_phys     = 0;
     void* proc_pd_virt_temp = NULL; // To store the temporary virtual mapping address of the PD
     bool pd_mapped_temp   = false; // Tracks if PD was temporarily mapped
 
     proc = (pcb_t *)kmalloc(sizeof(pcb_t));
     if (!proc) {
         terminal_write("[Process] kmalloc PCB failed.\n");
         return NULL; // Allocation failed, no cleanup needed yet
     }
     memset(proc, 0, sizeof(pcb_t));
     proc->pid = next_pid++; // Assign PID early for logging
 
     // --- Allocate Page Directory frame ---
     pd_phys = frame_alloc();
     if (!pd_phys) {
         terminal_write("[Process] frame_alloc PD failed.\n");
         goto fail_create;
     }
     proc->page_directory_phys = (uint32_t*)pd_phys;
     terminal_printf("  Allocated PD Phys: 0x%x for PID %u\n", (unsigned)pd_phys, (unsigned)proc->pid);
 
     // --- Temporarily map PD to copy kernel PDE entries & set recursive entry ---
     proc_pd_virt_temp = paging_temp_map(pd_phys); // Pass only physical address
     if (proc_pd_virt_temp == NULL) {
         terminal_write("[Process] Failed to temp map new PD.\n");
         goto fail_create;
     }
     pd_mapped_temp = true;
 
     // Zero the new PD frame first (important!)
     memset(proc_pd_virt_temp, 0, PAGE_SIZE);
 
     // Copy shared kernel PDE entries (ensure kernel mappings are present)
     copy_kernel_pde_entries((uint32_t*)proc_pd_virt_temp);
 
     // Set the recursive PDE entry for the new PD
     ((uint32_t*)proc_pd_virt_temp)[RECURSIVE_PDE_INDEX] = (pd_phys & PAGING_ADDR_MASK) | PAGE_PRESENT | PAGE_RW;
 
     // Unmap the temporary mapping of the new PD using the virtual address returned earlier
     paging_temp_unmap(proc_pd_virt_temp);
     pd_mapped_temp = false;
     proc_pd_virt_temp = NULL; // Don't use this pointer anymore
 
     // --- Allocate kernel stack (multi-page approach) ---
     // Use the kstack_allocated flag locally to know if cleanup is needed by destroy_process
     bool kstack_allocated_local = allocate_kernel_stack(proc);
     if (!kstack_allocated_local) {
         terminal_write("[Process] Failed to allocate kernel stack.\n");
         // allocate_kernel_stack handles its own frame cleanup on failure
         goto fail_create;
     }
     // kstack_allocated_local is true now
 
     // --- Create mm_struct + load ELF ---
     proc->mm = create_mm(proc->page_directory_phys);
     if (!proc->mm) {
         terminal_write("[Process] create_mm failed.\n");
         goto fail_create; // destroy_process below will handle kstack cleanup
     }
     // mm_created flag removed
 
     uintptr_t initial_brk_addr = 0;
     if (load_elf_and_init_memory(path, proc->mm, &proc->entry_point, &initial_brk_addr) != 0) {
         terminal_printf("[Process] load_elf failed for '%s'.\n", path);
         // load_elf_and_init_memory cleans up its own file buffer.
         // destroy_process called below will clean up partially created VMAs/pages and kernel stack.
         goto fail_create;
     }
     // Set initial program break in the mm_struct
     // Ensure brk is within user space and page aligned
     KERNEL_ASSERT(initial_brk_addr < KERNEL_VIRT_BASE, "Initial BRK is in kernel space");
     KERNEL_ASSERT((initial_brk_addr % PAGE_SIZE) == 0, "Initial BRK not page aligned");
     proc->mm->start_brk = initial_brk_addr;
     proc->mm->end_brk   = initial_brk_addr;
 
     // Add an initial (zero-size) heap VMA. This helps simplify brk/sbrk later.
     {
         // Remove VM_HEAP as it's undeclared
         uint32_t heap_flags = VM_READ | VM_WRITE | VM_USER | VM_ANONYMOUS;
         uint32_t heap_page_prot = PTE_USER_DATA_FLAGS; // Includes RW, User, Present
         if (g_nx_supported) heap_page_prot |= PAGE_NX_BIT; // Non-executable heap
 
         if (!insert_vma(proc->mm, initial_brk_addr, initial_brk_addr, heap_flags, heap_page_prot, NULL, 0)) {
              terminal_write("[Process] Warning: failed to insert zero-size heap VMA.\n");
              // Not necessarily fatal, brk syscall should still work but might need extra logic
         }
     }
 
     // --- Insert a user stack VMA ---
     {
         proc->user_stack_top = (void *)USER_STACK_TOP_VIRT_ADDR; // Top address (highest usable)
         uintptr_t stack_bottom = USER_STACK_BOTTOM_VIRT; // Bottom limit (lowest accessible)
         uintptr_t stack_top    = USER_STACK_TOP_VIRT_ADDR; // Top limit (highest accessible + 1 byte)
 
         // Basic sanity checks on stack constants
         KERNEL_ASSERT(stack_bottom < stack_top, "User stack bottom >= top");
         KERNEL_ASSERT(stack_top <= KERNEL_VIRT_BASE, "User stack top overlaps kernel");
         KERNEL_ASSERT((stack_bottom % PAGE_SIZE) == 0, "User stack bottom not page aligned");
         KERNEL_ASSERT((stack_top % PAGE_SIZE) == 0, "User stack top not page aligned");
 
 
         uint32_t stk_flags   = VM_READ | VM_WRITE | VM_USER | VM_GROWS_DOWN | VM_ANONYMOUS;
         uint32_t stk_prot    = PTE_USER_DATA_FLAGS; // RW, User, Present
         if (g_nx_supported) stk_prot |= PAGE_NX_BIT; // Non-executable stack
 
         terminal_printf("  Inserting User Stack VMA [0x%x - 0x%x) (Grows Down)\n",
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
     // General cleanup for any failure during process creation
     terminal_printf("[Process] Cleanup after create_user_process fail (PID %u).\n", (unsigned)(proc ? proc->pid : 0));
 
     // Ensure temporary PD mapping is undone if it was active and failed *before* unmap
     if (pd_mapped_temp && proc_pd_virt_temp != NULL) {
          paging_temp_unmap(proc_pd_virt_temp);
     }
 
     if (proc) {
         // Call destroy_process to handle cleanup of allocated resources.
         // This simplifies the cleanup logic here significantly.
         // destroy_process handles mm_struct, PD frame, and kernel stack if allocated.
         destroy_process(proc); // destroy_process should handle NULL mm etc. gracefully
         proc = NULL; // Ensure we return NULL
     } else {
         // If PCB allocation failed, but PD frame was allocated somehow (shouldn't happen with current logic)
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
     if (!pcb) return; // Allow safe calls with NULL
 
     uint32_t pid = pcb->pid; // Get PID before potential kfree
     terminal_printf("[Process] Destroying process PID %u.\n", (unsigned)pid);
 
     // 1) Destroy Memory Management (VMAs, user page tables, user frames)
     //    This is the most complex part. destroy_mm should handle:
     //    - Iterating through all VMAs.
     //    - Unmapping all associated user pages from the process's PD.
     //    - Calling put_frame() for the physical frames backing those pages (if anonymous/COW).
     //    - Freeing allocated page table frames (but not the PD frame itself).
     //    - Freeing the VMA structures and the mm_struct itself.
     if (pcb->mm) {
         destroy_mm(pcb->mm); // Assumes destroy_mm is robust
         pcb->mm = NULL;
     }
 
     // 2) Free the kernel stack (unmap virtual from kernel PD, free physical frames)
     //    This must happen *after* context switch away from this process,
     //    but the PCB destruction might be deferred (e.g., by scheduler).
     if (pcb->kernel_stack_vaddr_top != NULL) {
         uintptr_t stack_top = (uintptr_t)pcb->kernel_stack_vaddr_top;
         // Recalculate base based on known size
         size_t stack_size = PROCESS_KSTACK_SIZE;
          KERNEL_ASSERT(stack_size > 0 && (stack_size % PAGE_SIZE) == 0, "Invalid kernel stack size");
         uintptr_t stack_base = stack_top - stack_size;
 
         terminal_printf("  Freeing kernel stack: V=[0x%x-0x%x)\n", (unsigned)stack_base, (unsigned)stack_top);
 
         for (uintptr_t v_addr = stack_base; v_addr < stack_top; v_addr += PAGE_SIZE) {
             uintptr_t phys_addr = 0;
             // Get physical address mapped at v_addr IN THE KERNEL PD
             // We need to look in the kernel's mappings, not the (potentially already freed) process PD
             if (paging_get_physical_address((uint32_t*)g_kernel_page_directory_phys, v_addr, &phys_addr) == 0) {
                  if (phys_addr != 0) {
                      // terminal_printf("  -> Freeing frame P=0x%x for V=0x%x\n", phys_addr, v_addr); // Debug
                      put_frame(phys_addr); // Free the physical frame
                  } else {
                       terminal_printf("  Warning: No physical frame found for kernel stack V=0x%x during destroy.\n", (unsigned)v_addr);
                  }
             } else {
                  terminal_printf("  Warning: Could not get PTE for kernel stack V=0x%x during destroy.\n", (unsigned)v_addr);
             }
         }
          // Unmap the entire virtual range from the kernel PD in one go
          paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, stack_base, stack_size);
 
         pcb->kernel_stack_vaddr_top = NULL;
         pcb->kernel_stack_phys_base = 0;
     }
 
     // 3) Free the process page directory frame itself
     //    This must happen *after* destroy_mm has freed all user page tables referenced by it.
     if (pcb->page_directory_phys) {
         terminal_printf("  Freeing process PD frame: P=0x%x\n", (unsigned)pcb->page_directory_phys);
         put_frame((uintptr_t)pcb->page_directory_phys);
         pcb->page_directory_phys = NULL;
     }
 
 
     // 4) Free the PCB structure itself
     //    Should be the very last step.
     kfree(pcb);
     terminal_printf("[Process] PCB PID %u resources freed.\n", (unsigned)pid);
 }