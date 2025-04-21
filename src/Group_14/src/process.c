/**
 * @file process.c
 * @brief Process Management Implementation (Revised)
 *
 * Handles creation, destruction, and management of process control blocks (PCBs)
 * and their associated memory structures (mm_struct). Includes ELF loading,
 * kernel/user stack setup, and initial user context preparation for IRET.
 */

 #include "process.h"
 #include "mm.h"               // For mm_struct, vma_struct, create_mm, destroy_mm, insert_vma, find_vma, handle_vma_fault
 #include "kmalloc.h"          // For kmalloc, kfree
 #include "paging.h"           // For paging functions, flags, constants, registers_t
 #include "terminal.h"         // For kernel logging
 #include "types.h"            // Core type definitions
 #include "string.h"           // For memset, memcpy
 #include "scheduler.h"        // For get_current_task() etc. (Adapt based on scheduler API)
 #include "read_file.h"        // For read_file() helper
 #include "frame.h"            // For frame_alloc, put_frame
 #include "kmalloc_internal.h" // For ALIGN_UP (used by PAGE_ALIGN_UP in paging.h)
 #include "elf.h"              // ELF header definitions
 #include <libc/stddef.h>      // For NULL
 #include "assert.h"           // For KERNEL_ASSERT
 #include "gdt.h"              // For GDT_USER_CODE_SELECTOR, GDT_USER_DATA_SELECTOR
 #include "tss.h"              // For tss_set_kernel_stack // <-- Already included, no change needed here.

 // ------------------------------------------------------------------------
 // Definitions & Constants
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

 // Initial EFLAGS for user processes (IF=1, reserved bit 1=1)
 #define USER_EFLAGS_DEFAULT 0x202

 // Simple debug print macro (can be enabled/disabled)
 #define PROCESS_DEBUG 1 // Set to 0 to disable debug prints
 #if PROCESS_DEBUG
 // Note: Ensure terminal_printf handles %p correctly (it should, as %p calls _format_number with base 16)
 #define PROC_DEBUG_PRINTF(fmt, ...) terminal_printf("[Process DEBUG %s:%d] " fmt, __func__, __LINE__, ##__VA_ARGS__)
 #else
 #define PROC_DEBUG_PRINTF(fmt, ...) // Do nothing
 #endif


 // ------------------------------------------------------------------------
 // Externals & Globals
 // ------------------------------------------------------------------------
 extern uint32_t g_kernel_page_directory_phys; // Physical address of kernel's page directory
 extern bool g_nx_supported;                   // NX support flag

 // Process ID counter - NEEDS LOCKING FOR SMP
 static uint32_t next_pid = 1;
 // TODO: Add spinlock_t g_pid_lock;

 // Simple linear allocator for kernel stack virtual addresses
 // WARNING: Placeholder only! Not suitable for production/SMP. Needs proper allocator.
 static uintptr_t g_next_kernel_stack_virt_base = KERNEL_STACK_VIRT_START;
 // TODO: Replace with a proper kernel virtual address space allocator (e.g., using VMAs)

 // ------------------------------------------------------------------------
 // Local Prototypes
 // ------------------------------------------------------------------------
 static bool allocate_kernel_stack(pcb_t *proc);
 static int load_elf_and_init_memory(const char *path, mm_struct_t *mm, uint32_t *entry_point, uintptr_t *initial_brk);
 static int copy_elf_segment_data(uintptr_t frame_paddr, const uint8_t* file_data_buffer, size_t file_buffer_offset, size_t size_to_copy, size_t zero_padding);
 static void prepare_initial_kernel_stack(pcb_t *proc);
 extern void copy_kernel_pde_entries(uint32_t *new_pd_virt); // From paging.c

 // ------------------------------------------------------------------------
 // allocate_kernel_stack - Allocates and maps kernel stack pages
 // ------------------------------------------------------------------------
 /**
  * @brief Allocates physical frames and maps them into the kernel's address space
  * to serve as the kernel stack for a given process.
  * @param proc Pointer to the PCB to setup the kernel stack for.
  * @return true on success, false on failure.
  */
  static bool allocate_kernel_stack(pcb_t *proc)
 {
      PROC_DEBUG_PRINTF("Enter\n");
      KERNEL_ASSERT(proc != NULL, "allocate_kernel_stack: NULL proc");

      size_t stack_alloc_size = PROCESS_KSTACK_SIZE;
      if (stack_alloc_size == 0 || (stack_alloc_size % PAGE_SIZE) != 0) { /* ... error handling ... */ return false; }
      size_t num_pages = stack_alloc_size / PAGE_SIZE;
      terminal_printf("  Allocating %lu pages (%lu bytes) for kernel stack...\n", (unsigned long)num_pages, (unsigned long)stack_alloc_size);

      uintptr_t *phys_frames = kmalloc(num_pages * sizeof(uintptr_t));
      if (!phys_frames) { /* ... error handling ... */ return false; }
      memset(phys_frames, 0, num_pages * sizeof(uintptr_t));
      PROC_DEBUG_PRINTF("phys_frames array allocated at %p\n", phys_frames);

      // 1. Allocate Physical Frames
      size_t allocated_count = 0;
      for (allocated_count = 0; allocated_count < num_pages; allocated_count++) {
          phys_frames[allocated_count] = frame_alloc();
          if (!phys_frames[allocated_count]) { /* ... error handling & cleanup ... */ kfree(phys_frames); return false; }
          PROC_DEBUG_PRINTF("Allocated frame %lu: P=%#lx\n", (unsigned long)allocated_count, (unsigned long)phys_frames[allocated_count]);
      }
      proc->kernel_stack_phys_base = (uint32_t)phys_frames[0];
      terminal_printf("  Successfully allocated %lu physical frames for kernel stack.\n", (unsigned long)allocated_count);

      // 2. Allocate Virtual Range
      PROC_DEBUG_PRINTF("Allocating virtual range...\n");
      uintptr_t kstack_virt_base = g_next_kernel_stack_virt_base;
      uintptr_t kstack_virt_end = kstack_virt_base + stack_alloc_size;
      if (kstack_virt_base < KERNEL_STACK_VIRT_START || kstack_virt_end > KERNEL_STACK_VIRT_END || kstack_virt_end <= kstack_virt_base) { /* ... error handling & cleanup ... */ kfree(phys_frames); return false; }
      g_next_kernel_stack_virt_base = kstack_virt_end;
      KERNEL_ASSERT((kstack_virt_base % PAGE_SIZE) == 0, "Kernel stack virt base not page aligned");
      terminal_printf("  Allocated kernel stack VIRTUAL range: [%#lx - %#lx)\n", (unsigned long)kstack_virt_base, (unsigned long)kstack_virt_end);

      // 3. Map Physical Frames to Virtual Range in Kernel Page Directory
      PROC_DEBUG_PRINTF("Mapping physical frames to virtual range...\n");
      for (size_t i = 0; i < num_pages; i++) {
          uintptr_t target_vaddr = kstack_virt_base + (i * PAGE_SIZE);
          uintptr_t phys_addr    = phys_frames[i];
          PROC_DEBUG_PRINTF("Mapping page %lu: V=%p -> P=%#lx\n", (unsigned long)i, (void*)target_vaddr, (unsigned long)phys_addr);
          int map_res = paging_map_single_4k((uint32_t*)g_kernel_page_directory_phys, target_vaddr, phys_addr, PTE_KERNEL_DATA_FLAGS);
          if (map_res != 0) { /* ... error handling & cleanup ... */ kfree(phys_frames); return false; }
      }

      // <<< --- ADD KERNEL STACK WRITE TEST --- >>>
      PROC_DEBUG_PRINTF("Performing kernel stack write test V=[%p - %p)...\n", (void*)kstack_virt_base, (void*)kstack_virt_end);
      volatile uint32_t *stack_bottom_ptr = (volatile uint32_t *)kstack_virt_base;
      volatile uint32_t *stack_top_word_ptr = (volatile uint32_t *)(kstack_virt_end - sizeof(uint32_t));
      uint32_t test_value = 0xDEADBEEF;
      uint32_t read_back1 = 0, read_back2 = 0;

      terminal_printf("  Writing test value to stack bottom: %p\n", (void*)stack_bottom_ptr);
      *stack_bottom_ptr = test_value; // Write to lowest address
      read_back1 = *stack_bottom_ptr; // Read back

      terminal_printf("  Writing test value to stack top word: %p\n", (void*)stack_top_word_ptr);
      *stack_top_word_ptr = test_value; // Write to highest address (word)
      read_back2 = *stack_top_word_ptr; // Read back

      if (read_back1 != test_value || read_back2 != test_value) {
          terminal_printf("  KERNEL STACK WRITE TEST FAILED! Read back %#lx and %#lx (expected %#lx)\n",
                          (unsigned long)read_back1, (unsigned long)read_back2, (unsigned long)test_value);
          // Cleanup frames and mappings before returning false
          terminal_printf("  Unmapping failed stack range V=[%p-%p)\n", (void*)kstack_virt_base, (void*)kstack_virt_end);
          paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, kstack_virt_base, stack_alloc_size);
          for(size_t i=0; i<num_pages; ++i) { put_frame(phys_frames[i]); }
          kfree(phys_frames);
          g_next_kernel_stack_virt_base = kstack_virt_base; // Roll back VA allocator
          return false; // Indicate failure
      }
      PROC_DEBUG_PRINTF("Kernel stack write test PASSED.\n");
      // <<< --- END KERNEL STACK WRITE TEST --- >>>


      // Store the top virtual address (highest address + 1, suitable for stack pointer init)
      proc->kernel_stack_vaddr_top = (uint32_t*)kstack_virt_end;

      kfree(phys_frames); // Free the temporary array holding physical frame addresses

      terminal_printf("  Kernel stack mapped: PhysBase=%#lx, VirtBase=%#lx, VirtTop=%p\n",
                      (unsigned long)proc->kernel_stack_phys_base,
                      (unsigned long)kstack_virt_base,
                      (void*)proc->kernel_stack_vaddr_top);

      // Explicitly update TSS ESP0 whenever a kernel stack is allocated
      terminal_printf("  Updating TSS esp0 = %p\n", (void*)proc->kernel_stack_vaddr_top);
      tss_set_kernel_stack((uint32_t)proc->kernel_stack_vaddr_top);

      PROC_DEBUG_PRINTF("Exit OK\n");
      return true; // Success
 }

 // ------------------------------------------------------------------------
 // get_current_process - Retrieve PCB of the currently running process
 // ------------------------------------------------------------------------
 /**
  * @brief Gets the PCB of the currently running process.
  * @note Relies on the scheduler providing the current task/thread control block.
  * @return Pointer to the current PCB, or NULL if no process context is active.
  */
 pcb_t* get_current_process(void)
 {
      // Assumes scheduler maintains the current task control block (TCB)
      tcb_t* current_tcb = get_current_task(); // Assuming get_current_task() exists and returns tcb_t*
      if (current_tcb && current_tcb->process) {
          return current_tcb->process;
      }
      return NULL; // Early boot or kernel thread context
 }

 // ------------------------------------------------------------------------
 // copy_elf_segment_data - Populate a physical frame with ELF data
 // ------------------------------------------------------------------------
 /**
  * @brief Copies data from an ELF file buffer into a physical memory frame,
  * handling zero-padding (BSS). Uses temporary kernel mapping.
  * @param frame_paddr Physical address of the destination frame (must be page-aligned).
  * @param file_data_buffer Pointer to the buffer containing the entire ELF file data.
  * @param file_buffer_offset Offset within file_data_buffer where data for this frame starts.
  * @param size_to_copy Number of bytes to copy from the file buffer.
  * @param zero_padding Number of bytes to zero-fill after the copied data.
  * @return 0 on success, -1 on failure (e.g., temp mapping failed).
  */
 static int copy_elf_segment_data(uintptr_t frame_paddr,
                                  const uint8_t* file_data_buffer,
                                  size_t file_buffer_offset,
                                  size_t size_to_copy,
                                  size_t zero_padding)
 {
      PROC_DEBUG_PRINTF("Enter P=%#lx, offset=%lu, copy=%lu, zero=%lu\n", (unsigned long)frame_paddr, (unsigned long)file_buffer_offset, (unsigned long)size_to_copy, (unsigned long)zero_padding);
      KERNEL_ASSERT(frame_paddr != 0 && (frame_paddr % PAGE_SIZE) == 0, "copy_elf_segment_data: Invalid physical address");
      KERNEL_ASSERT(size_to_copy + zero_padding <= PAGE_SIZE, "ELF copy + zero exceeds frame");

      // Temporarily map the physical frame into kernel space
      PROC_DEBUG_PRINTF("Calling paging_temp_map_vaddr for P=%#lx\n", (unsigned long)frame_paddr);
      void* temp_vaddr = paging_temp_map(frame_paddr, PTE_KERNEL_DATA_FLAGS);

      if (temp_vaddr == NULL) {
          terminal_printf("[Process] copy_elf_segment_data: ERROR: paging_temp_map_vaddr failed (paddr=%#lx).\n", (unsigned long)frame_paddr);
          return -1;
      }
      PROC_DEBUG_PRINTF("paging_temp_map_vaddr returned V=%p\n", temp_vaddr);

      // Copy data from ELF file buffer
      if (size_to_copy > 0) {
          PROC_DEBUG_PRINTF("memcpy: dst=%p, src=%p + %lu, size=%lu\n", temp_vaddr, file_data_buffer, (unsigned long)file_buffer_offset, (unsigned long)size_to_copy);
          KERNEL_ASSERT(file_data_buffer != NULL, "copy_elf_segment_data: NULL file_data_buffer");
          memcpy(temp_vaddr, file_data_buffer + file_buffer_offset, size_to_copy);
      }
      // Zero out the BSS portion within this page
      if (zero_padding > 0) {
          PROC_DEBUG_PRINTF("memset: dst=%p + %lu, val=0, size=%lu\n", temp_vaddr, (unsigned long)size_to_copy, (unsigned long)zero_padding);
          memset((uint8_t*)temp_vaddr + size_to_copy, 0, zero_padding);
      }

      // Unmap the specific temporary kernel mapping
     PROC_DEBUG_PRINTF("Calling paging_temp_unmap_vaddr for V=%p", temp_vaddr);
     paging_temp_unmap(temp_vaddr);
     PROC_DEBUG_PRINTF("Exit OK");
     return 0; // success
 }


 // ------------------------------------------------------------------------
 // load_elf_and_init_memory - Load ELF, setup VMAs, populate pages
 // ------------------------------------------------------------------------
 /**
  * @brief Loads an ELF executable from a file buffer, validates it, creates
  * Virtual Memory Areas (VMAs) for LOAD segments, allocates physical frames,
  * copies segment data, and maps the frames into the process's address space.
  * @param path Path to the executable (used for logging).
  * @param mm Pointer to the process's memory management structure.
  * @param entry_point Output parameter for the ELF entry point virtual address.
  * @param initial_brk Output parameter for the initial program break address (end of loaded data).
  * @return 0 on success, negative error code on failure.
  */
 /*
 Okay, let's analyze these logs carefully.

 Kernel Stack Write Test: The log clearly shows:
 
 [Process DEBUG allocate_kernel_stack:139] Performing kernel stack write test V=[0xe0000000 - 0xe0004000)...
   Writing test value to stack bottom: 0xe0000000
   Writing test value to stack top word: 0xe0003ffc
 [Process DEBUG allocate_kernel_stack:164] Kernel stack write test PASSED.
 This is excellent news! It confirms that the kernel stack pages (0xe0000000 to 0xe0004000) are correctly mapped and writable in the kernel's page directory at the time the process is created. This rules out a fundamental mapping issue with the kernel stack itself being the cause of the triple fault.
 
 TSS esp0 Check: The check just before enabling interrupts confirms the value is correct:
 
 [Kernel] Final check: TSS ESP0 = 0xe0004000 before enabling interrupts.
 Silent Crash: The kernel still crashes silently immediately after sti. No serial debug characters ('S', 'P', 'G', 'D', 'T', '#', 'N') appeared.
 
 Conclusion from Logs:
 
 Since the kernel stack is verified writable, and the TSS esp0 is correct right before sti, and none of the interrupt/exception handlers are even being entered (no serial output), the most likely remaining culprit is an immediate fault during the iret instruction in jump_to_user_mode that the CPU cannot handle, leading to a triple fault.
 
 Why would iret fault?
 
 Page Fault on EIP: The instruction pointer (EIP = 0x8048080) pushed onto the stack points to the user code entry point. If the page containing this address (0x8048000) is not mapped as Present and User-accessible with Execute permission in the process's page directory (PD Phys=0x10135000), the CPU will fault immediately when trying to fetch the first instruction after iret.
 General Protection Fault: Less likely given the values, but potential issues with CS (0x1B), SS (0x23), or EFLAGS (0x202) could cause a #GP.
 Revisiting the ELF Loading Logs:
 
 Let's look very closely at the ELF loading section again from the latest log:
 
 [Process DEBUG load_elf_and_init_memory:359]  Segment 0: Type=1
   Segment 0: VAddr=0x8048000, MemSz=168, FileSz=168, Offset=0x1000, Flags=R-X
   -> VMA [0x8048000 - 0x8049000), VMA Flags=0xa5, PageProt=0x5  // <<< PROBLEM HERE
   -> Mapping and populating pages...
 [Process DEBUG load_elf_and_init_memory:414]    Processing page V=0x08048000...
 [Process DEBUG load_elf_and_init_memory:421]     Allocated frame P=0x10009000
 ...
 [Process DEBUG load_elf_and_init_memory:462]    Mapping V=0x08048000 -> P=0x10009000 with prot 0x5 // <<< PROBLEM HERE
 This is the smoking gun! The log confirms that the fix applied to process.c to correct the page_prot calculation was somehow lost or reverted in the code you compiled for this run. It's still mapping the executable code segment (R-X) with protection flags 0x5 (PAGE_PRESENT | PAGE_USER), which is User Read-Only. It's missing the necessary permissions for execution.
 
 The CPU attempts the iret, tries to fetch the instruction at EIP = 0x8048080, sees the page is present and user-accessible but not executable (or maybe even not readable depending on exact CPU behavior with these flags), triggers a Page Fault (#PF), tries to invoke the #PF handler, fails because of the stack issue (now we suspect this secondary failure), causing a Double Fault, which then fails again leading to a Triple Fault.
 
 Solution:
 
 We need to re-apply the fix to process.c in the load_elf_and_init_memory function to ensure the page_prot variable is calculated correctly based on the ELF segment flags (PF_R, PF_W, PF_X) and NX support.
 
 Let me provide the corrected process.c again, ensuring the fix is present.
 */

// ------------------------------------------------------------------------
 // load_elf_and_init_memory - Load ELF, setup VMAs, populate pages
 // ------------------------------------------------------------------------
 static int load_elf_and_init_memory(const char *path,
                                       mm_struct_t *mm,
                                       uint32_t *entry_point,
                                       uintptr_t *initial_brk)
 {
     PROC_DEBUG_PRINTF("Enter path='%s', mm=%p\n", path ? path : "<NULL>", mm);
     KERNEL_ASSERT(path != NULL && mm != NULL && entry_point != NULL && initial_brk != NULL, "load_elf: Invalid arguments");

     size_t file_size = 0;
     uint8_t *file_data = NULL;
     uintptr_t phys_page = 0; // Tracks a potentially allocated frame that needs freeing on error
     int result = -1; // Default to error

     // 1. Read ELF file
     PROC_DEBUG_PRINTF("Reading file '%s'\n", path);
     file_data = (uint8_t*)read_file(path, &file_size);
     if (!file_data) {
         terminal_printf("[Process] load_elf: ERROR: Failed to read file '%s'.\n", path);
         goto cleanup_load_elf;
     }
     PROC_DEBUG_PRINTF("File read: size=%lu bytes, buffer=%p\n", (unsigned long)file_size, file_data);
     if (file_size < sizeof(Elf32_Ehdr)) {
         terminal_printf("[Process] load_elf: ERROR: File '%s' is too small to be an ELF file (size %lu).\n", path, (unsigned long)file_size);
         goto cleanup_load_elf;
     }

     // 2. Parse and Validate ELF Header
     PROC_DEBUG_PRINTF("Parsing ELF header...\n");
     Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_data;

     // Basic ELF header validation (replace 'false' with actual checks)
     if (/* Add your ELF header validation logic here, e.g., magic number, class, type, machine */ false) {
         terminal_printf("[Process] load_elf: ERROR: Invalid ELF header or properties for '%s'.\n", path);
         goto cleanup_load_elf;
     }
     *entry_point = ehdr->e_entry;
     terminal_printf("  ELF Entry Point: %#lx\n", (unsigned long)*entry_point);

     // 3. Process Program Headers (Segments)
     PROC_DEBUG_PRINTF("Processing %u program headers...\n", (unsigned)ehdr->e_phnum);
     Elf32_Phdr *phdr_table = (Elf32_Phdr *)(file_data + ehdr->e_phoff);
     uintptr_t highest_addr_loaded = 0;

     for (Elf32_Half i = 0; i < ehdr->e_phnum; i++) {
         Elf32_Phdr *phdr = &phdr_table[i];
         PROC_DEBUG_PRINTF(" Segment %u: Type=%u\n", (unsigned)i, (unsigned)phdr->p_type);

         // Only process PT_LOAD segments with a non-zero memory size
         if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) {
             continue;
         }

         // Basic segment validation (replace 'false' with actual checks)
         if (/* Add your segment validation logic here, e.g., alignment, bounds */ false) {
             terminal_printf("  -> Error: Invalid segment %d geometry or placement for '%s'.\n", (int)i, path);
             goto cleanup_load_elf;
         }

         terminal_printf("  Segment %d: VAddr=%#lx, MemSz=%u, FileSz=%u, Offset=%#lx, Flags=%c%c%c\n",
                         (int)i, (unsigned long)phdr->p_vaddr, (unsigned)phdr->p_memsz, (unsigned)phdr->p_filesz, (unsigned long)phdr->p_offset,
                         (phdr->p_flags & PF_R) ? 'R' : '-',
                         (phdr->p_flags & PF_W) ? 'W' : '-',
                         (phdr->p_flags & PF_X) ? 'X' : '-');

         // Calculate page-aligned virtual memory range for the VMA
         uintptr_t vm_start = PAGE_ALIGN_DOWN(phdr->p_vaddr);
         uintptr_t vm_end = PAGE_ALIGN_UP(phdr->p_vaddr + phdr->p_memsz);
         if (vm_end <= vm_start) { // Skip if rounding results in zero size
             continue;
         }

         // *** === Page Protection Logic Updated based on Fix #1 === ***
         uint32_t vma_flags = VM_USER | VM_ANONYMOUS;    // Base VMA flags
         uint32_t page_prot = PAGE_PRESENT | PAGE_USER;  // Base Page flags (NX not set initially)

         if (phdr->p_flags & PF_R) {
             vma_flags |= VM_READ;            // read is implied, but keep for clarity
         }
         if (phdr->p_flags & PF_W) {
             vma_flags |= VM_WRITE;
             page_prot |= PAGE_RW;            // writable page
         }
         // Apply execute/NX logic based on segment flags (PF_X)
         if (phdr->p_flags & PF_X) {
             vma_flags |= VM_EXEC;            // executable VMA
             /* Leave PAGE_NX_BIT clear in page_prot so the page may execute */
             // The base 'page_prot' already lacks PAGE_NX_BIT
         } else if (g_nx_supported) { // Check g_nx_supported HERE
             // Segment is NOT executable AND NX is supported
             page_prot |= PAGE_NX_BIT;        // Mark non-executable segment with NX bit
         }
         // *** === End Updated Page Protection Logic === ***


         terminal_printf("  -> VMA [%#lx - %#lx), VMA Flags=%#x, PageProt=%#x\n", // Check PageProt value here!
                         (unsigned long)vm_start, (unsigned long)vm_end, (unsigned)vma_flags, (unsigned)page_prot);

         // Insert VMA for this segment
         if (!insert_vma(mm, vm_start, vm_end, vma_flags, page_prot, NULL, 0)) {
             terminal_printf("[Process] load_elf: ERROR: Failed to insert VMA for segment %d of '%s'.\n", (int)i, path);
             goto cleanup_load_elf;
         }

         // Allocate frames, populate data, and map pages for this segment
         terminal_printf("  -> Mapping and populating pages...\n");
         for (uintptr_t page_v = vm_start; page_v < vm_end; page_v += PAGE_SIZE) {
             PROC_DEBUG_PRINTF("   Processing page V=%p...\n", (void*)page_v);
             phys_page = frame_alloc();
             if (!phys_page) {
                 terminal_printf("[Process] load_elf: ERROR: Failed to allocate frame for V=%p in '%s'.\n", (void*)page_v, path);
                 goto cleanup_load_elf; // phys_page != 0 will be handled in cleanup
             }
             PROC_DEBUG_PRINTF("    Allocated frame P=%#lx\n", (unsigned long)phys_page);

             // Calculate how much data to copy from the file and how much to zero-pad
             size_t copy_size_this_page = 0;
             size_t zero_padding_this_page = 0;
             size_t file_buffer_offset = 0;

             uintptr_t file_copy_start_vaddr = phdr->p_vaddr; // Virtual address where file data starts
             uintptr_t file_copy_end_vaddr = phdr->p_vaddr + phdr->p_filesz; // Virtual address where file data ends
             uintptr_t page_start_vaddr = page_v; // Current page's start virtual address
             uintptr_t page_end_vaddr = page_v + PAGE_SIZE; // Current page's end virtual address

             // Calculate overlap between file data range and current page range
             uintptr_t copy_v_start = MAX(page_start_vaddr, file_copy_start_vaddr);
             uintptr_t copy_v_end = MIN(page_end_vaddr, file_copy_end_vaddr);
             copy_size_this_page = (copy_v_end > copy_v_start) ? (copy_v_end - copy_v_start) : 0;

             // Calculate offset into the file buffer for copying
             if (copy_size_this_page > 0) {
                 file_buffer_offset = (copy_v_start - phdr->p_vaddr) + phdr->p_offset;
                 // Sanity check offset
                 if (file_buffer_offset + copy_size_this_page > file_size || phdr->p_offset > file_size || file_buffer_offset < phdr->p_offset) {
                    terminal_printf("[Process] load_elf: ERROR: File offset calculation error for V=%p in '%s'.\n", (void*)page_v, path);
                    goto cleanup_load_elf;
                 }
             }

             // Calculate zero-padding size for this page (BSS)
             uintptr_t mem_end_vaddr = phdr->p_vaddr + phdr->p_memsz; // Virtual address where segment memory ends
             uintptr_t zero_v_start = page_start_vaddr + copy_size_this_page; // Where zeroing starts in this page
             uintptr_t zero_v_end = MIN(page_end_vaddr, mem_end_vaddr); // Where zeroing ends in this page
             zero_padding_this_page = (zero_v_end > zero_v_start) ? (zero_v_end - zero_v_start) : 0;


             PROC_DEBUG_PRINTF("    CopySize=%lu, ZeroPadding=%lu, FileOffset=%lu\n", (unsigned long)copy_size_this_page, (unsigned long)zero_padding_this_page, (unsigned long)file_buffer_offset);
             if (copy_size_this_page + zero_padding_this_page > PAGE_SIZE) {
                 terminal_printf("[Process] load_elf: ERROR: Calculated copy+zero size (%lu) exceeds PAGE_SIZE for V=%p in '%s'.\n",
                                 (unsigned long)(copy_size_this_page + zero_padding_this_page), (void*)page_v, path);
                 goto cleanup_load_elf;
             }

             // Populate the allocated physical frame with data
             if (copy_elf_segment_data(phys_page, file_data, file_buffer_offset, copy_size_this_page, zero_padding_this_page) != 0) {
                 terminal_printf("[Process] load_elf: ERROR: copy_elf_segment_data failed for V=%p in '%s'.\n", (void*)page_v, path);
                 goto cleanup_load_elf; // phys_page will be handled in cleanup
             }

             // Map the populated frame into the process's address space using the calculated protection flags
             PROC_DEBUG_PRINTF("    Mapping V=%p -> P=%#lx with prot %#x\n", (void*)page_v, (unsigned long)phys_page, (unsigned)page_prot); // Check prot value here
             int map_res = paging_map_single_4k(mm->pgd_phys, page_v, phys_page, page_prot);
             if (map_res != 0) {
                 terminal_printf("[Process] load_elf: ERROR: paging_map_single_4k failed for V=%p -> P=%#lx in '%s' (err %d).\n",
                                 (void*)page_v, (unsigned long)phys_page, path, map_res);
                 // Frame was allocated but mapping failed, it needs explicit freeing here
                 put_frame(phys_page);
                 phys_page = 0; // Reset tracker
                 goto cleanup_load_elf;
             }
             // Mapping successful, frame ownership transferred to page tables. Reset tracker.
             phys_page = 0;
         } // End loop for pages within a segment

         // Update the highest virtual address loaded so far
         uintptr_t current_segment_end = phdr->p_vaddr + phdr->p_memsz;
         // Handle potential overflow if segment wraps around
         if (current_segment_end < phdr->p_vaddr) current_segment_end = UINTPTR_MAX;
         if (current_segment_end > highest_addr_loaded) {
             highest_addr_loaded = current_segment_end;
         }
         PROC_DEBUG_PRINTF("  Segment %u processed. highest_addr_loaded=%#lx\n", (unsigned)i, (unsigned long)highest_addr_loaded);
     } // End loop through segments

     // 4. Set Initial Program Break (end of loaded data, page-aligned up)
     *initial_brk = PAGE_ALIGN_UP(highest_addr_loaded);
     // Handle potential overflow if highest_addr_loaded was already page aligned near the top
     if (*initial_brk < highest_addr_loaded) *initial_brk = UINTPTR_MAX;
     terminal_printf("  ELF load complete. initial_brk=%#lx\n", (unsigned long)*initial_brk);
     result = 0; // Success

 cleanup_load_elf:
     PROC_DEBUG_PRINTF("Cleanup: result=%d\n", result);
     // Free the ELF file buffer if it was allocated
     if (file_data) {
         kfree(file_data);
     }
     // Free the physical page if one was allocated but not successfully mapped
     if (phys_page != 0) {
         put_frame(phys_page);
     }
     // If loading failed, the caller (create_user_process) should handle cleanup
     // of partially created mm_struct, VMAs, page tables etc via destroy_process.
     PROC_DEBUG_PRINTF("Exit result=%d\n", result);
     return result;
 }


 // ------------------------------------------------------------------------
 // prepare_initial_kernel_stack - Sets up the kernel stack for first IRET
 // ------------------------------------------------------------------------
 /**
  * @brief Prepares the kernel stack of a newly created process for the initial
  * transition to user mode via the IRET instruction.
  * @param proc Pointer to the PCB of the process.
  * @note This function assumes proc->kernel_stack_vaddr_top points to the
  * valid top (highest address + 1) of the allocated kernel stack.
  * It pushes the necessary values for IRET and stores the final
  * kernel stack pointer in proc->kernel_esp_for_switch (ASSUMED FIELD).
  */
 static void prepare_initial_kernel_stack(pcb_t *proc) {
      PROC_DEBUG_PRINTF("Enter\n");
      KERNEL_ASSERT(proc != NULL, "prepare_initial_kernel_stack: NULL proc");
      KERNEL_ASSERT(proc->kernel_stack_vaddr_top != NULL, "prepare_initial_kernel_stack: Kernel stack top is NULL");
      KERNEL_ASSERT(proc->entry_point != 0, "prepare_initial_kernel_stack: Entry point is zero");
      KERNEL_ASSERT(proc->user_stack_top != NULL, "prepare_initial_kernel_stack: User stack top is NULL");

      // Get the top address and move down to prepare for pushes
      // The stack pointer points to the last occupied dword, so start just above the top address.
      uint32_t *kstack_ptr = (uint32_t*)proc->kernel_stack_vaddr_top;
      PROC_DEBUG_PRINTF("Initial kstack_ptr (top) = %p\n", kstack_ptr);

      // 1. Push User SS (Stack Segment)
      //    Use the User Data Selector from GDT, ensuring RPL=3.
      kstack_ptr--; // Decrement stack pointer
      *kstack_ptr = GDT_USER_DATA_SELECTOR | 3;
      PROC_DEBUG_PRINTF("Pushed SS = %#lx at %p\n", (unsigned long)*kstack_ptr, kstack_ptr);

      // 2. Push User ESP (Stack Pointer)
      //    Points to the top of the allocated user stack region.
      kstack_ptr--;
      *kstack_ptr = (uint32_t)proc->user_stack_top;
       PROC_DEBUG_PRINTF("Pushed ESP = %#lx at %p\n", (unsigned long)*kstack_ptr, kstack_ptr);

      // 3. Push EFLAGS
      //    Use default flags enabling interrupts (IF=1).
      kstack_ptr--;
      *kstack_ptr = USER_EFLAGS_DEFAULT;
      PROC_DEBUG_PRINTF("Pushed EFLAGS = %#lx at %p\n", (unsigned long)*kstack_ptr, kstack_ptr);

      // 4. Push User CS (Code Segment)
      //    Use the User Code Selector from GDT, ensuring RPL=3.
      kstack_ptr--;
      *kstack_ptr = GDT_USER_CODE_SELECTOR | 3;
      PROC_DEBUG_PRINTF("Pushed CS = %#lx at %p\n", (unsigned long)*kstack_ptr, kstack_ptr);

      // 5. Push User EIP (Instruction Pointer)
      //    This is the program's entry point obtained from the ELF header.
      kstack_ptr--;
      *kstack_ptr = proc->entry_point;
      PROC_DEBUG_PRINTF("Pushed EIP = %#lx at %p\n", (unsigned long)proc->entry_point, kstack_ptr); // Use proc->entry_point directly

      // --- Optional: Push initial general-purpose register values (often zero) ---
      // If your context switch code expects these to be popped by 'popa', push them here.
      // If the context switch only restores ESP and uses IRET, this is not needed.
      // Assuming context_switch.asm does NOT handle the initial IRET frame + popa.
      // We will assume registers start with undefined values (or zero if desired later).
      // Example if pushad state needs to be simulated:
      // kstack_ptr--; *kstack_ptr = 0; // EDI
      // kstack_ptr--; *kstack_ptr = 0; // ESI
      // kstack_ptr--; *kstack_ptr = 0; // EBP
      // kstack_ptr--; *kstack_ptr = 0; // ESP_dummy (value doesn't matter here)
      // kstack_ptr--; *kstack_ptr = 0; // EBX
      // kstack_ptr--; *kstack_ptr = 0; // EDX
      // kstack_ptr--; *kstack_ptr = 0; // ECX
      // kstack_ptr--; *kstack_ptr = 0; // EAX

      // 6. Save the final Kernel Stack Pointer
      //    This ESP value is what the kernel should load right before executing IRET
      //    to jump to the user process for the first time.
      //    Store it in the PCB/TCB field used by your context switch mechanism.
      //    *** ASSUMES pcb_t has a field named 'kernel_esp_for_switch' ***
      //    *** YOU MUST ADD THIS FIELD TO process.h ***
      proc->kernel_esp_for_switch = (uint32_t)kstack_ptr;

      terminal_printf("  Kernel stack prepared for IRET. Final K_ESP = %#lx\n", (unsigned long)proc->kernel_esp_for_switch);
      PROC_DEBUG_PRINTF("Exit\n");
 }


 // ------------------------------------------------------------------------
 // create_user_process - Main function to create a new process
 // ------------------------------------------------------------------------
 /**
  * @brief Creates a new user process by loading an ELF executable.
  * Sets up PCB, memory space (page directory, VMAs), kernel stack,
  * user stack, loads ELF segments, prepares the initial kernel stack
  * for context switching, and updates the TSS esp0 field.
  * @param path Path to the executable file.
  * @return Pointer to the newly created PCB on success, NULL on failure.
  */
 pcb_t *create_user_process(const char *path)
 {
      PROC_DEBUG_PRINTF("Enter path='%s'\n", path ? path : "<NULL>");
      KERNEL_ASSERT(path != NULL, "create_user_process: NULL path");
      terminal_printf("[Process] Creating user process from '%s'.\n", path);

      pcb_t *proc = NULL;
      uintptr_t pd_phys = 0;
      void* proc_pd_virt_temp = NULL;
      bool pd_mapped_temp = false;
      bool kstack_allocated = false;
      bool initial_stack_mapped = false;
      uintptr_t initial_stack_phys_frame = 0;
      int ret_status = -1; // Track status for cleanup message

      // --- Allocate PCB ---
      PROC_DEBUG_PRINTF("Step 1: Allocate PCB\n");
      proc = (pcb_t *)kmalloc(sizeof(pcb_t));
      if (!proc) {
          terminal_write("[Process] ERROR: kmalloc PCB failed.\n");
          return NULL;
      }
      memset(proc, 0, sizeof(pcb_t));
      // TODO: Lock PID allocation for SMP
      proc->pid = next_pid++;
      PROC_DEBUG_PRINTF("PCB allocated at %p, PID=%lu\n", proc, (unsigned long)proc->pid);
      // proc->state = PROC_INITIALIZING; // Set initial state

      // --- Allocate Page Directory ---
      PROC_DEBUG_PRINTF("Step 2: Allocate Page Directory Frame\n");
      pd_phys = frame_alloc();
      if (!pd_phys) {
          terminal_printf("[Process] ERROR: frame_alloc PD failed for PID %lu.\n", (unsigned long)proc->pid);
          ret_status = -1;
          goto fail_create;
      }
      proc->page_directory_phys = (uint32_t*)pd_phys;
      terminal_printf("  Allocated PD Phys: %#lx for PID %lu\n", (unsigned long)pd_phys, (unsigned long)proc->pid);

      // --- Initialize Page Directory ---
      PROC_DEBUG_PRINTF("Step 3: Initialize Page Directory (PD Phys=%#lx)\n", (unsigned long)pd_phys);
      PROC_DEBUG_PRINTF("  Calling paging_temp_map_vaddr...\n");
      proc_pd_virt_temp = paging_temp_map(pd_phys, PTE_KERNEL_DATA_FLAGS);
      PROC_DEBUG_PRINTF("  paging_temp_map_vaddr returned %p\n", proc_pd_virt_temp);
      if (proc_pd_virt_temp == NULL) {
          terminal_printf("[Process] ERROR: Failed to temp map new PD for PID %lu.\n", (unsigned long)proc->pid);
          ret_status = -2;
          goto fail_create;
      }
      pd_mapped_temp = true;

      PROC_DEBUG_PRINTF("  Calling memset on temp PD mapping %p...\n", proc_pd_virt_temp);
      memset(proc_pd_virt_temp, 0, PAGE_SIZE); // Zero out the new PD
      PROC_DEBUG_PRINTF("  Calling copy_kernel_pde_entries to %p...\n", proc_pd_virt_temp);
      copy_kernel_pde_entries((uint32_t*)proc_pd_virt_temp); // Copy kernel mappings
      PROC_DEBUG_PRINTF("  Setting recursive entry in temp PD mapping %p...\n", proc_pd_virt_temp);
      // Set up recursive mapping entry for the new PD itself
      ((uint32_t*)proc_pd_virt_temp)[RECURSIVE_PDE_INDEX] = (pd_phys & PAGING_ADDR_MASK) | PAGE_PRESENT | PAGE_RW | PAGE_NX_BIT; // Kernel RW, NX ok
      PROC_DEBUG_PRINTF("  Calling paging_temp_unmap_vaddr for %p...\n", proc_pd_virt_temp);
      paging_temp_unmap(proc_pd_virt_temp);
      pd_mapped_temp = false;
      proc_pd_virt_temp = NULL;
      PROC_DEBUG_PRINTF("  PD Initialization complete.\n");

      // --- Allocate Kernel Stack ---
      PROC_DEBUG_PRINTF("Step 4: Allocate Kernel Stack\n");
      kstack_allocated = allocate_kernel_stack(proc);
      if (!kstack_allocated) {
          terminal_printf("[Process] ERROR: Failed to allocate kernel stack for PID %lu.\n", (unsigned long)proc->pid);
          ret_status = -3;
          goto fail_create;
      }
      // Note: allocate_kernel_stack now handles the initial tss_set_kernel_stack call.

      // --- Create Memory Management structure ---
      PROC_DEBUG_PRINTF("Step 5: Create mm_struct\n");
      proc->mm = create_mm(proc->page_directory_phys);
      if (!proc->mm) {
          terminal_printf("[Process] ERROR: create_mm failed for PID %lu.\n", (unsigned long)proc->pid);
          ret_status = -4;
          goto fail_create;
      }
      PROC_DEBUG_PRINTF("  mm_struct created at %p\n", proc->mm);

      // --- Load ELF executable into memory space ---
      PROC_DEBUG_PRINTF("Step 6: Load ELF '%s'\n", path);
      uintptr_t initial_brk_addr = 0;
      if (load_elf_and_init_memory(path, proc->mm, &proc->entry_point, &initial_brk_addr) != 0) {
          terminal_printf("[Process] ERROR: load_elf failed for '%s', PID %lu.\n", path, (unsigned long)proc->pid);
          ret_status = -5;
          goto fail_create;
      }
      // Initialize heap break pointers
      proc->mm->start_brk = initial_brk_addr;
      proc->mm->end_brk   = initial_brk_addr;
      PROC_DEBUG_PRINTF("  ELF loaded. Entry=%#lx, Initial Brk=%#lx\n", (unsigned long)proc->entry_point, (unsigned long)initial_brk_addr);

      // --- Setup standard VMAs (Heap placeholder, User Stack) ---
      PROC_DEBUG_PRINTF("Step 7: Setup standard VMAs\n");
      // Heap VMA (initially zero size, grows with brk/sbrk)
      {
          PROC_DEBUG_PRINTF("  Setting up Heap VMA at %#lx\n", (unsigned long)initial_brk_addr);
          // Removed VM_HEAP flag as it wasn't defined/used elsewhere
          uint32_t heap_flags = VM_READ | VM_WRITE | VM_USER | VM_ANONYMOUS;
          uint32_t heap_page_prot = PTE_USER_DATA_FLAGS; // User RW-, NX
          if (!insert_vma(proc->mm, initial_brk_addr, initial_brk_addr, heap_flags, heap_page_prot, NULL, 0)) {
              terminal_printf("[Process] Warning: failed to insert zero-size heap VMA for PID %lu.\n", (unsigned long)proc->pid);
              // Non-fatal, brk might fail later
          }
      }
      // User Stack VMA
      {
          uintptr_t stack_bottom = USER_STACK_BOTTOM_VIRT; // From process.h
          uintptr_t stack_top    = USER_STACK_TOP_VIRT_ADDR; // From process.h
          KERNEL_ASSERT(stack_bottom < stack_top && stack_top <= KERNEL_VIRT_BASE && (stack_bottom % PAGE_SIZE) == 0 && (stack_top % PAGE_SIZE) == 0, "Invalid user stack definitions");

          // Removed VM_STACK flag as it wasn't defined/used elsewhere
          uint32_t stk_flags = VM_READ | VM_WRITE | VM_USER | VM_GROWS_DOWN | VM_ANONYMOUS;
          uint32_t stk_prot  = PTE_USER_DATA_FLAGS; // User RW-, NX

          terminal_printf("  Inserting User Stack VMA [%#lx - %#lx) (Grows Down) for PID %lu\n",
                          (unsigned long)stack_bottom, (unsigned long)stack_top, (unsigned long)proc->pid);
          if (!insert_vma(proc->mm, stack_bottom, stack_top, stk_flags, stk_prot, NULL, 0)) {
              terminal_printf("[Process] ERROR: Failed to insert user stack VMA for PID %lu.\n", (unsigned long)proc->pid);
              ret_status = -6;
              goto fail_create;
          }
          proc->user_stack_top = (void *)USER_STACK_TOP_VIRT_ADDR; // Store for context setup
          PROC_DEBUG_PRINTF("  User Stack VMA inserted. User Stack Top=%p\n", proc->user_stack_top);
      }

      // --- Allocate and Map Initial User Stack Page ---
      PROC_DEBUG_PRINTF("Step 8: Allocate initial user stack page\n");
      // Allocate the page just below the top address, as stack grows down.
      uintptr_t initial_stack_page_vaddr = USER_STACK_TOP_VIRT_ADDR - PAGE_SIZE;
      KERNEL_ASSERT(initial_stack_page_vaddr >= USER_STACK_BOTTOM_VIRT, "Initial stack page calculation error");

      initial_stack_phys_frame = frame_alloc();
      if (!initial_stack_phys_frame) {
          terminal_printf("[Process] ERROR: Failed to allocate initial user stack frame for PID %lu.\n", (unsigned long)proc->pid);
          ret_status = -7;
          goto fail_create;
      }
      terminal_printf("  Allocated initial user stack frame P=%#lx for V=%p\n", (unsigned long)initial_stack_phys_frame, (void*)initial_stack_page_vaddr);

      // Map the frame into the process's address space
      uint32_t stack_page_prot = PTE_USER_DATA_FLAGS; // User RW-, NX
      PROC_DEBUG_PRINTF("  Mapping initial user stack page V=%p -> P=%#lx\n", (void*)initial_stack_page_vaddr, (unsigned long)initial_stack_phys_frame);
      int map_res = paging_map_single_4k(proc->page_directory_phys,
                                         initial_stack_page_vaddr,
                                         initial_stack_phys_frame,
                                         stack_page_prot);
      if (map_res != 0) {
          terminal_printf("[Process] ERROR: Failed to map initial user stack page for PID %lu (err %d)\n", (unsigned long)proc->pid, map_res);
          initial_stack_mapped = false; // Mark as not mapped for cleanup
          ret_status = -8;
          goto fail_create;
      }
      initial_stack_mapped = true;
      // Frame ownership transferred to page tables, managed by mm_struct/VMA cleanup

      // --- Prepare Initial Kernel Stack for IRET ---
      PROC_DEBUG_PRINTF("Step 9: Prepare initial kernel stack for IRET\n");
      prepare_initial_kernel_stack(proc);

      // --- Add to Scheduler Ready Queue ---
      PROC_DEBUG_PRINTF("Step 10: Add to scheduler (Placeholder)\n");
      // This part depends heavily on your scheduler's design.
      // You might need to allocate a TCB, link it to the PCB, set the state,
      // and call the scheduler's add function.
      // Example placeholder:
      // proc->state = PROC_READY;
      // if (scheduler_add_process(proc) != 0) { // Assuming scheduler takes PCB
      //     terminal_printf("[Process] ERROR: Failed to add process PID %lu to scheduler.\n", (unsigned long)proc->pid);
      //     ret_status = -9; // Use a distinct error code
      //     goto fail_create; // Cleanup all resources if adding fails
      // }
      terminal_printf("  Process PID %lu configuration complete. Ready to be scheduled (manual step).\n", (unsigned long)proc->pid);

      // --- SUCCESS ---
      terminal_printf("[Process] Successfully created PCB PID %lu structure for '%s'.\n",
                      (unsigned long)proc->pid, path);
      PROC_DEBUG_PRINTF("Exit OK (proc=%p)\n", proc);
      return proc; // Return the prepared PCB

 fail_create:
      // --- Cleanup on Failure ---
      terminal_printf("[Process] Cleanup after create_user_process failed (PID %lu, Status %d).\n",
                      (unsigned long)(proc ? proc->pid : 0), ret_status);

      // Ensure temporary PD mapping is undone if it was active
      if (pd_mapped_temp && proc_pd_virt_temp != NULL) {
          PROC_DEBUG_PRINTF("  Cleaning up temporary PD mapping %p\n", proc_pd_virt_temp);
          paging_temp_unmap(proc_pd_virt_temp);

      }

      // Free the initial user stack physical frame ONLY if it was allocated but mapping FAILED
      // If mapping succeeded, the frame is owned by the mm_struct and cleaned up by destroy_process->destroy_mm
      if (initial_stack_phys_frame != 0 && !initial_stack_mapped) {
           terminal_printf("  Freeing unmapped initial user stack frame P=%#lx\n", (unsigned long)initial_stack_phys_frame);
          put_frame(initial_stack_phys_frame);
      }

      // Call destroy_process for comprehensive cleanup of PCB and associated resources
      if (proc) {
          // destroy_process handles freeing mm, kstack (frames+mapping), pd_phys, and pcb struct
          PROC_DEBUG_PRINTF("  Calling destroy_process for partially created PID %lu\n", (unsigned long)proc->pid);
          destroy_process(proc);
      } else {
          // If PCB allocation failed but PD was allocated
          if (pd_phys) {
               PROC_DEBUG_PRINTF("  Freeing PD frame P=%#lx (PCB allocation failed)\n", (unsigned long)pd_phys);
              put_frame(pd_phys);
          }
      }

      PROC_DEBUG_PRINTF("Exit FAIL (NULL)\n");
      return NULL; // Indicate failure
 }

 // ------------------------------------------------------------------------
 // destroy_process - Frees all process resources
 // ------------------------------------------------------------------------
 /**
  * @brief Destroys a process and frees all associated resources.
  * Frees memory space (VMAs, page tables, frames via mm_struct), kernel stack
  * (frames and kernel mapping), page directory frame, and the PCB structure itself.
  * @warning Ensure the process is removed from the scheduler and is not running
  * before calling this function to avoid use-after-free issues.
  * @param pcb Pointer to the PCB of the process to destroy.
  */
 void destroy_process(pcb_t *pcb)
 {
      if (!pcb) return;

      uint32_t pid = pcb->pid;
      PROC_DEBUG_PRINTF("Enter PID=%lu\n", (unsigned long)pid);
      terminal_printf("[Process] Destroying process PID %lu.\n", (unsigned long)pid);

      // 1. Destroy Memory Management structure and associated resources
      //    This should handle freeing all user-space page tables and frames
      //    associated with the process's VMAs.
      if (pcb->mm) {
          PROC_DEBUG_PRINTF("  Destroying mm_struct %p...\n", pcb->mm);
          destroy_mm(pcb->mm); // Assumes destroy_mm frees user pages/tables & mm struct
          pcb->mm = NULL;
      }

      // 2. Free Kernel Stack (Physical frames and Kernel Virtual Mapping)
      if (pcb->kernel_stack_vaddr_top != NULL) {
          uintptr_t stack_top = (uintptr_t)pcb->kernel_stack_vaddr_top;
          size_t stack_size = PROCESS_KSTACK_SIZE;
          uintptr_t stack_base = stack_top - stack_size;
          terminal_printf("  Freeing kernel stack: V=[%p-%p)\n", (void*)stack_base, (void*)stack_top);

          // Iterate through the kernel virtual addresses used by the stack
          for (uintptr_t v_addr = stack_base; v_addr < stack_top; v_addr += PAGE_SIZE) {
              uintptr_t phys_addr = 0;
              // Look up the physical frame mapped at this virtual address in the KERNEL page directory
              PROC_DEBUG_PRINTF("   Looking up physical address for kernel stack V=%p...\n", (void*)v_addr);
              if (paging_get_physical_address((uint32_t*)g_kernel_page_directory_phys, v_addr, &phys_addr) == 0) {
                  if (phys_addr != 0) {
                      PROC_DEBUG_PRINTF("   Found P=%#lx. Calling put_frame.\n", (unsigned long)phys_addr);
                      // Free the physical frame
                      put_frame(phys_addr);
                  } else {
                       terminal_printf("  Warning: No physical frame found for kernel stack V=%p during destroy.\n", (void*)v_addr);
                   }
              } else {
                    terminal_printf("  Warning: Could not get PTE for kernel stack V=%p during destroy.\n", (void*)v_addr);
              }
          }
          // Unmap the virtual range from the KERNEL page directory
          // This removes the PTEs/PDEs from the kernel's address space.
          PROC_DEBUG_PRINTF("  Unmapping kernel stack range V=[%p-%p) from kernel PD.\n", (void*)stack_base, (void*)stack_top);
          paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, stack_base, stack_size);

          pcb->kernel_stack_vaddr_top = NULL;
          pcb->kernel_stack_phys_base = 0;
      } else {
           PROC_DEBUG_PRINTF("  No kernel stack allocated or already freed.\n");
      }

      // 3. Free the process's Page Directory frame
      //    This MUST happen AFTER destroy_mm (which frees user page tables)
      //    and AFTER freeing the kernel stack (which uses the kernel PD).
      if (pcb->page_directory_phys) {
          terminal_printf("  Freeing process PD frame: P=%p\n", (void*)pcb->page_directory_phys);
          put_frame((uintptr_t)pcb->page_directory_phys);
          pcb->page_directory_phys = NULL;
      } else {
          PROC_DEBUG_PRINTF("  No Page Directory allocated or already freed.\n");
      }

      // 4. Free the PCB structure itself
      PROC_DEBUG_PRINTF("  Freeing PCB structure %p\n", pcb);
      // TODO: Lock PID release for SMP if necessary
      kfree(pcb);
      terminal_printf("[Process] PCB PID %lu resources freed.\n", (unsigned long)pid);
      PROC_DEBUG_PRINTF("Exit PID=%lu\n", (unsigned long)pid);
 }