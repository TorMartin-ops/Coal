/**
 * @file process.c
 * @brief Process Management Implementation (Fixed Compiler Errors)
 *
 * Handles creation, destruction, and management of process control blocks (PCBs)
 * and their associated memory structures (mm_struct). Includes ELF loading,
 * kernel/user stack setup, file descriptor management, and initial user context
 * preparation for IRET.
 */

 #include <kernel/process/process.h>
 #include <kernel/memory/mm.h>             // For mm_struct, vma_struct, create_mm, destroy_mm, insert_vma, find_vma, handle_vma_fault
 #include <kernel/memory/kmalloc.h>        // For kmalloc, kfree
 #include <kernel/memory/paging.h>         // For paging functions, flags, constants, registers_t, PAGE_SIZE, KERNEL_VIRT_BASE
 #include <kernel/drivers/display/terminal.h>       // For kernel logging
 #include <kernel/core/types.h>          // Core type definitions
 #include <kernel/lib/string.h>         // For memset, memcpy
 #include <kernel/process/scheduler.h>      // For get_current_task() etc. (Adapt based on scheduler API)
 #include <kernel/fs/vfs/read_file.h>      // For read_file() helper
 #include <kernel/memory/frame.h>          // For frame_alloc, put_frame
 #include <kernel/memory/kmalloc_internal.h> // For ALIGN_UP (used by PAGE_ALIGN_UP in paging.h)
 #include <kernel/process/elf.h>            // ELF header definitions
 #include <libc/stddef.h>    // For NULL
 #include <kernel/lib/assert.h>         // For KERNEL_ASSERT
 #include <kernel/cpu/gdt.h>            // For GDT_USER_CODE_SELECTOR, GDT_USER_DATA_SELECTOR
 #include <kernel/cpu/tss.h>            // For tss_set_kernel_stack
 #include <kernel/fs/vfs/sys_file.h>       // For sys_close() definition and sys_file_t type <-- Added include
 #include <kernel/fs/vfs/fs_limits.h>      // For MAX_FD <-- Added include
 #include <kernel/fs/vfs/fs_errno.h>       // For error codes (ENOENT, ENOEXEC, ENOMEM, EIO) <-- Added include
 #include <kernel/fs/vfs/vfs.h>            // For vfs_close (used in process_close_fds fallback)
 #include <kernel/drivers/display/serial.h>
 
 // Forward declaration for idle task stack checking
 extern void check_idle_task_stack_integrity(const char *checkpoint);
 
 // ------------------------------------------------------------------------
 // Definitions & Constants
 // ------------------------------------------------------------------------
 
 // Define USER_SPACE_START_VIRT if not defined elsewhere (e.g., paging.h)
 // This should be the lowest valid user virtual address.
 #ifndef USER_SPACE_START_VIRT
 #define USER_SPACE_START_VIRT 0x00001000 // Example: Assume user space starts above page 0
 #endif
 
 // Define KERNEL_VIRT_BASE if not defined elsewhere (e.g., paging.h)
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
 #define PROCESS_DEBUG 0 // Set to 0 to disable debug prints
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
 
 // --- FD Management Function Prototypes (implementation below) ---
 void process_init_fds(pcb_t *proc);
 void process_close_fds(pcb_t *proc);
 
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
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Enter\n", __func__, __LINE__);
     KERNEL_ASSERT(proc != NULL, "allocate_kernel_stack: NULL proc");

     size_t usable_stack_size = PROCESS_KSTACK_SIZE; // Defined in process.h (e.g., 16384)
     if (usable_stack_size == 0 || (usable_stack_size % PAGE_SIZE) != 0) {
        serial_printf("[Process] ERROR: Invalid PROCESS_KSTACK_SIZE (%lu).\n", (unsigned long)usable_stack_size);
        return false;
     }
     size_t num_usable_pages = usable_stack_size / PAGE_SIZE;
     // *** GUARD PAGE FIX: Allocate one extra page ***
     size_t num_pages_with_guard = num_usable_pages + 1;
     size_t total_alloc_size = num_pages_with_guard * PAGE_SIZE; // Total size including guard

     // *** GUARD PAGE FIX: Ensure log message reflects the extra page ***
     serial_printf("  Allocating %lu pages + 1 guard page (%lu bytes total) for kernel stack...\n",
                     (unsigned long)num_usable_pages, (unsigned long)total_alloc_size);

     // Use kmalloc for the temporary array holding physical frame addresses
     uintptr_t *phys_frames = kmalloc(num_pages_with_guard * sizeof(uintptr_t));
     if (!phys_frames) {
        serial_write("[Process] ERROR: kmalloc failed for phys_frames array.\n");
        return false;
     }
     memset(phys_frames, 0, num_pages_with_guard * sizeof(uintptr_t));
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] phys_frames array allocated at %p\n", __func__, __LINE__, phys_frames);

     // 1. Allocate Physical Frames (Allocate num_pages_with_guard)
     size_t allocated_count = 0;
     for (allocated_count = 0; allocated_count < num_pages_with_guard; allocated_count++) {
         phys_frames[allocated_count] = frame_alloc();
         if (phys_frames[allocated_count] == 0) { // frame_alloc returns 0 on failure
             serial_printf("[Process] ERROR: frame_alloc failed for frame %lu.\n", (unsigned long)allocated_count);
             // Free already allocated frames before returning
             for(size_t j = 0; j < allocated_count; ++j) { put_frame(phys_frames[j]); }
             kfree(phys_frames);
             return false;
         }
         PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Allocated frame %lu: P=%#lx\n", __func__, __LINE__, (unsigned long)allocated_count, (unsigned long)phys_frames[allocated_count]);
     }
     proc->kernel_stack_phys_base = (uint32_t)phys_frames[0];
     // *** GUARD PAGE FIX: Update log message ***
     serial_printf("  Successfully allocated %lu physical frames (incl. guard) for kernel stack.\n", (unsigned long)allocated_count);

     // 2. Allocate Virtual Range (Allocate for num_pages_with_guard)
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Allocating virtual range...\n", __func__, __LINE__);
     // TODO: Add locking for g_next_kernel_stack_virt_base in SMP
     uintptr_t kstack_virt_base = g_next_kernel_stack_virt_base;
     // *** GUARD PAGE FIX: Use total size including guard ***
     uintptr_t kstack_virt_end_with_guard = kstack_virt_base + total_alloc_size;
     // Check for overflow and exceeding bounds
     if (kstack_virt_base < KERNEL_STACK_VIRT_START || kstack_virt_end_with_guard > KERNEL_STACK_VIRT_END || kstack_virt_end_with_guard <= kstack_virt_base) {
        serial_printf("[Process] ERROR: Kernel stack virtual address range invalid or exhausted [%#lx - %#lx).\n",
                        (unsigned long)kstack_virt_base, (unsigned long)kstack_virt_end_with_guard);
        // Free allocated physical frames
        for(size_t i=0; i<num_pages_with_guard; ++i) { put_frame(phys_frames[i]); }
        kfree(phys_frames);
        return false;
     }
     g_next_kernel_stack_virt_base = kstack_virt_end_with_guard; // Advance allocator
     KERNEL_ASSERT((kstack_virt_base % PAGE_SIZE) == 0, "Kernel stack virt base not page aligned");
     // *** GUARD PAGE FIX: Update log message ***
     serial_printf("  Allocated kernel stack VIRTUAL range (incl. guard): [%#lx - %#lx)\n",
                     (unsigned long)kstack_virt_base, (unsigned long)kstack_virt_end_with_guard);

     // 3. Map Physical Frames to Virtual Range in Kernel Page Directory
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Mapping physical frames to virtual range...\n", __func__, __LINE__);
     // *** GUARD PAGE FIX: Loop over all allocated frames ***
     for (size_t i = 0; i < num_pages_with_guard; i++) {
         uintptr_t target_vaddr = kstack_virt_base + (i * PAGE_SIZE);
         uintptr_t phys_addr    = phys_frames[i];
         PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Mapping page %lu: V=%p -> P=%#lx\n", __func__, __LINE__, (unsigned long)i, (void*)target_vaddr, (unsigned long)phys_addr);
         // Ensure kernel paging globals are set
         if (!g_kernel_page_directory_phys) {
            serial_printf("[Process] ERROR: Kernel page directory physical address not set for mapping.\n");
            for(size_t j = 0; j < num_pages_with_guard; ++j) { if (phys_frames[j] != 0) put_frame(phys_frames[j]); }
            kfree(phys_frames);
            g_next_kernel_stack_virt_base = kstack_virt_base; // Roll back VA allocator
            return false;
         }
         int map_res = paging_map_single_4k((uint32_t*)g_kernel_page_directory_phys, target_vaddr, phys_addr, PTE_KERNEL_DATA_FLAGS);
         if (map_res != 0) {
            serial_printf("[Process] ERROR: Failed to map kernel stack page %lu (V=%p -> P=%#lx), error %d.\n",
                            (unsigned long)i, (void*)target_vaddr, (unsigned long)phys_addr, map_res);
            // Unmap already mapped pages and free all physical frames
            paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, kstack_virt_base, i * PAGE_SIZE); // Unmap successful ones
            for(size_t j = 0; j < num_pages_with_guard; ++j) { // Free all allocated frames
                if (phys_frames[j] != 0) put_frame(phys_frames[j]);
            }
            kfree(phys_frames);
            g_next_kernel_stack_virt_base = kstack_virt_base; // Roll back VA allocator
            return false;
         }
     }

     // 4. Perform Kernel Stack Write Test (on usable range only)
     // *** GUARD PAGE FIX: Test only the usable part ***
     uintptr_t usable_stack_end = kstack_virt_base + usable_stack_size;
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Performing kernel stack write test V=[%p - %p)...\n", __func__, __LINE__, (void*)kstack_virt_base, (void*)usable_stack_end);
     volatile uint32_t *stack_bottom_ptr = (volatile uint32_t *)kstack_virt_base;
     volatile uint32_t *stack_top_word_ptr = (volatile uint32_t *)(usable_stack_end - sizeof(uint32_t)); // Test highest usable word
     uint32_t test_value = 0xDEADBEEF;
     uint32_t read_back1 = 0, read_back2 = 0;

     serial_printf("  Writing test value to stack bottom: %p\n", (void*)stack_bottom_ptr);
     *stack_bottom_ptr = test_value;
     read_back1 = *stack_bottom_ptr;

     serial_printf("  Writing test value to stack top word: %p\n", (void*)stack_top_word_ptr);
     *stack_top_word_ptr = test_value;
     read_back2 = *stack_top_word_ptr;

     if (read_back1 != test_value || read_back2 != test_value) {
         serial_printf("  KERNEL STACK WRITE TEST FAILED! Read back %#lx and %#lx (expected %#lx)\n",
                         (unsigned long)read_back1, (unsigned long)read_back2, (unsigned long)test_value);
         serial_printf("  Unmapping failed stack range V=[%p-%p)\n", (void*)kstack_virt_base, (void*)kstack_virt_end_with_guard);
         // *** GUARD PAGE FIX: Unmap total size ***
         paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, kstack_virt_base, total_alloc_size);
         // *** GUARD PAGE FIX: Free all frames ***
         for(size_t i=0; i<num_pages_with_guard; ++i) { if(phys_frames[i]) put_frame(phys_frames[i]); }
         kfree(phys_frames);
         g_next_kernel_stack_virt_base = kstack_virt_base; // Roll back VA allocator
         return false;
     }
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Kernel stack write test PASSED (usable range).\n", __func__, __LINE__);

     // 5. *** Set kernel_stack_vaddr_top to the end of the USABLE stack size ***
     // This value is used for TSS.esp0 and stack preparation logic.
     proc->kernel_stack_vaddr_top = (uint32_t*)usable_stack_end; // e.g., 0xe0004000

     kfree(phys_frames); // Free the temporary array

     // *** GUARD PAGE FIX: Update log message ***
     serial_printf("  Kernel stack mapped (incl. guard): PhysBase=%#lx, VirtBase=%#lx, Usable VirtTop=%p\n",
                     (unsigned long)proc->kernel_stack_phys_base,
                     (unsigned long)kstack_virt_base,
                     (void*)proc->kernel_stack_vaddr_top);

     // 6. Update TSS ESP0 using the (unchanged) value of kernel_stack_vaddr_top
     serial_printf("  Updating TSS esp0 = %p\n", (void*)proc->kernel_stack_vaddr_top);
     tss_set_kernel_stack((uint32_t)proc->kernel_stack_vaddr_top);

     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Exit OK\n", __func__, __LINE__);
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
      // Needs adaptation based on your actual scheduler implementation.
      tcb_t* current_tcb = get_current_task(); // Assuming get_current_task() exists and returns tcb_t*
      if (current_tcb && current_tcb->process) { // Assuming tcb_t has a 'process' field pointing to pcb_t
          return current_tcb->process;
      }
      // Could be running early boot code or a kernel-only thread without a full PCB
      return NULL;
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
 
     // Sanity check the input parameters
     if (!file_data_buffer && size_to_copy > 0) {
         serial_printf("[Process] copy_elf_segment_data: ERROR: NULL file_data_buffer with non-zero copy size.\n");
         return -1;
     }

     if (file_buffer_offset > 0 && !file_data_buffer) {
         serial_printf("[Process] copy_elf_segment_data: ERROR: Non-zero file offset with NULL file_data_buffer.\n");
         return -1;
     }

     // Temporarily map the physical frame into kernel space
     PROC_DEBUG_PRINTF("Calling paging_temp_map for P=%#lx\n", (unsigned long)frame_paddr);
     // Use PTE_KERNEL_DATA_FLAGS to ensure it's writable by kernel
     void* temp_vaddr = paging_temp_map(frame_paddr, PTE_KERNEL_DATA_FLAGS);
 
     if (temp_vaddr == NULL) {
         serial_printf("[Process] copy_elf_segment_data: ERROR: paging_temp_map failed (paddr=%#lx).\n", (unsigned long)frame_paddr);
         return -1;
     }
     PROC_DEBUG_PRINTF("paging_temp_map returned V=%p\n", temp_vaddr);
 
     // Copy data from ELF file buffer - with added validation
     if (size_to_copy > 0) {
         PROC_DEBUG_PRINTF("memcpy: dst=%p, src=%p + %lu, size=%lu\n", temp_vaddr, file_data_buffer, (unsigned long)file_buffer_offset, (unsigned long)size_to_copy);
         KERNEL_ASSERT(file_data_buffer != NULL, "copy_elf_segment_data: NULL file_data_buffer");
         
         // Extra debug to see what we're copying - print first few bytes
         if (PROCESS_DEBUG && size_to_copy >= 4) {
             const uint8_t* src = file_data_buffer + file_buffer_offset;
             PROC_DEBUG_PRINTF("First 4 bytes: %02x %02x %02x %02x\n", 
                             src[0], src[1], src[2], src[3]);
         }
         
         // Do the actual copy
         memcpy(temp_vaddr, file_data_buffer + file_buffer_offset, size_to_copy);
         
         // Verify the copy succeeded
         if (PROCESS_DEBUG && size_to_copy >= 4) {
             const uint8_t* dst = (const uint8_t*)temp_vaddr;
             PROC_DEBUG_PRINTF("Verify first 4 bytes: %02x %02x %02x %02x\n", 
                             dst[0], dst[1], dst[2], dst[3]);
         }
     }
     
     // Zero out the BSS portion within this page
     if (zero_padding > 0) {
         PROC_DEBUG_PRINTF("memset: dst=%p + %lu, val=0, size=%lu\n", temp_vaddr, (unsigned long)size_to_copy, (unsigned long)zero_padding);
         memset((uint8_t*)temp_vaddr + size_to_copy, 0, zero_padding);
     }
 
     // Unmap the specific temporary kernel mapping
     PROC_DEBUG_PRINTF("Calling paging_temp_unmap for V=%p\n", temp_vaddr);
     paging_temp_unmap(temp_vaddr);
     PROC_DEBUG_PRINTF("Exit OK\n");
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
 static int load_elf_and_init_memory(const char *path,
                                    mm_struct_t *mm,
                                    uint32_t *entry_point,
                                    uintptr_t *initial_brk)
 {
      PROC_DEBUG_PRINTF("Enter path='%s', mm=%p", path ? path : "<NULL>", mm);
      KERNEL_ASSERT(path != NULL && mm != NULL && entry_point != NULL && initial_brk != NULL, "load_elf: Invalid arguments");

      size_t file_size = 0;
      uint8_t *file_data = NULL;
      uintptr_t phys_page = 0; // Tracks a potentially allocated frame that needs freeing on error
      int result = -1; // Default to error

      // 1. Read ELF file using the read_file helper
      PROC_DEBUG_PRINTF("Reading file '%s'", path);
      file_data = (uint8_t*)read_file(path, &file_size); // read_file allocates buffer
      if (!file_data) {
          serial_printf("[Process] load_elf: ERROR: Failed to read file '%s'.\n", path);
          result = -ENOENT; // File not found or read error
          goto cleanup_load_elf;
      }
      PROC_DEBUG_PRINTF("File read: size=%lu bytes, buffer=%p", (unsigned long)file_size, file_data);
      if (file_size < sizeof(Elf32_Ehdr)) {
          serial_printf("[Process] load_elf: ERROR: File '%s' is too small to be an ELF file (size %lu).\n", path, (unsigned long)file_size);
          result = -ENOEXEC; // Exec format error
          goto cleanup_load_elf;
      }

      // 2. Parse and Validate ELF Header
      PROC_DEBUG_PRINTF("Parsing ELF header...");
      Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_data;

      // Check ELF Magic Number, Class, Type, Machine, Program Header Table validity
      if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
          ehdr->e_ident[EI_MAG2] != ELFMAG2 || ehdr->e_ident[EI_MAG3] != ELFMAG3) {
          serial_printf("[Process] load_elf: ERROR: Invalid ELF magic number for '%s'.\n", path);
          result = -ENOEXEC; goto cleanup_load_elf;
      }
      if (ehdr->e_ident[EI_CLASS] != ELFCLASS32 || ehdr->e_type != ET_EXEC || ehdr->e_machine != EM_386) {
           serial_printf("[Process] load_elf: ERROR: ELF file '%s' is not a 32-bit i386 executable.\n", path);
           result = -ENOEXEC; goto cleanup_load_elf;
      }
       if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0 ||
           (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize) > file_size) {
           serial_printf("[Process] load_elf: ERROR: Invalid program header table in '%s'.\n", path);
           result = -ENOEXEC; goto cleanup_load_elf;
       }

      *entry_point = ehdr->e_entry;
      serial_printf("  ELF Entry Point: %#lx\n", (unsigned long)*entry_point);

      // 3. Process Program Headers (Segments)
      PROC_DEBUG_PRINTF("Processing %u program headers...", (unsigned)ehdr->e_phnum);
      Elf32_Phdr *phdr_table = (Elf32_Phdr *)(file_data + ehdr->e_phoff);
      uintptr_t highest_addr_loaded = 0;

      for (Elf32_Half i = 0; i < ehdr->e_phnum; i++) {
          Elf32_Phdr *phdr = &phdr_table[i];
          PROC_DEBUG_PRINTF(" Segment %u: Type=%lu", (unsigned)i, (unsigned long)phdr->p_type);

          // Only process PT_LOAD segments with a non-zero memory size
          if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) {
              PROC_DEBUG_PRINTF("   -> Skipping (Not PT_LOAD or memsz is 0)");
              continue;
          }

          // Validate segment addresses and sizes (remain the same)
          if (phdr->p_vaddr < USER_SPACE_START_VIRT || phdr->p_vaddr >= KERNEL_VIRT_BASE) {
              serial_printf("  -> Error: Segment %d VAddr %#lx out of user space bounds for '%s'.\n", (int)i, (unsigned long)phdr->p_vaddr, path);
              result = -ENOEXEC; goto cleanup_load_elf;
          }
          if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr || phdr->p_vaddr + phdr->p_memsz > KERNEL_VIRT_BASE) {
              serial_printf("  -> Error: Segment %d memory range [%#lx-%#lx) invalid for '%s'.\n", (int)i, (unsigned long)phdr->p_vaddr, (unsigned long)(phdr->p_vaddr + phdr->p_memsz), path);
              result = -ENOEXEC; goto cleanup_load_elf;
          }
           if (phdr->p_filesz > phdr->p_memsz) {
               serial_printf("  -> Error: Segment %d filesz (%lu) > memsz (%lu) for '%s'.\n", (int)i, (unsigned long)phdr->p_filesz, (unsigned long)phdr->p_memsz, path);
               result = -ENOEXEC; goto cleanup_load_elf;
           }
           if (phdr->p_offset + phdr->p_filesz < phdr->p_offset || phdr->p_offset + phdr->p_filesz > file_size) {
               serial_printf("  -> Error: Segment %d file range [%#lx-%#lx) exceeds file size (%lu) for '%s'.\n", (int)i, (unsigned long)phdr->p_offset, (unsigned long)(phdr->p_offset + phdr->p_filesz), (unsigned long)file_size, path);
               result = -ENOEXEC; goto cleanup_load_elf;
           }

          serial_printf("  Segment %d: VAddr=%#lx, MemSz=%lu, FileSz=%lu, Offset=%#lx, Flags=%c%c%c",
                          (int)i, (unsigned long)phdr->p_vaddr, (unsigned long)phdr->p_memsz, (unsigned long)phdr->p_filesz, (unsigned long)phdr->p_offset,
                          (phdr->p_flags & PF_R) ? 'R' : '-',
                          (phdr->p_flags & PF_W) ? 'W' : '-',
                          (phdr->p_flags & PF_X) ? 'X' : '-');

          // Calculate page-aligned virtual memory range for the VMA
          uintptr_t vm_start = PAGE_ALIGN_DOWN(phdr->p_vaddr);
          uintptr_t vm_end = PAGE_ALIGN_UP(phdr->p_vaddr + phdr->p_memsz);
          if (vm_end <= vm_start) continue; // Skip zero-sized VMA after alignment

          // --- FIXED: Correct VMA and Page Flag Derivation ---
          uint32_t vma_flags = VM_USER | VM_ANONYMOUS; // Base VMA flags (ANONYMOUS assumes no file backing *yet*)
          uint32_t page_prot = PAGE_PRESENT | PAGE_USER; // Base Page flags

          // Set VMA flags based on ELF permissions
          if (phdr->p_flags & PF_R) vma_flags |= VM_READ;
          if (phdr->p_flags & PF_W) vma_flags |= VM_WRITE;
          if (phdr->p_flags & PF_X) vma_flags |= VM_EXEC;

          // Set Page flags based on ELF permissions
          if (phdr->p_flags & PF_W) {
              page_prot |= PAGE_RW; // Writable page
          }
          // Set NX bit only if segment is NOT executable and NX is supported
          if (!(phdr->p_flags & PF_X) && g_nx_supported) {
              page_prot |= PAGE_NX_BIT;
          }
          // --- END FIX ---

          serial_printf("  -> VMA [%#lx - %#lx), VMA Flags=%#x, PageProt=%#x",
                          (unsigned long)vm_start, (unsigned long)vm_end, (unsigned)vma_flags, (unsigned)page_prot);

          // Insert VMA for this segment
          // TODO: If implementing file-backed pages later, pass file pointer and offset here
          if (!insert_vma(mm, vm_start, vm_end, vma_flags, page_prot, NULL, 0)) {
              serial_printf("[Process] load_elf: ERROR: Failed to insert VMA for segment %d of '%s'.\n", (int)i, path);
              result = -ENOMEM; // VMA insertion likely failed due to memory
              goto cleanup_load_elf;
          }

          // Allocate frames, populate data, and map pages for this segment
          serial_printf("  -> Mapping and populating pages...");
          for (uintptr_t page_v = vm_start; page_v < vm_end; page_v += PAGE_SIZE) {
              PROC_DEBUG_PRINTF("    Processing page V=%p...", (void*)page_v);
              phys_page = frame_alloc(); // Allocate a physical frame
              if (!phys_page) {
                  serial_printf("[Process] load_elf: ERROR: Failed to allocate frame for V=%p in '%s'.\n", (void*)page_v, path);
                  result = -ENOMEM;
                  goto cleanup_load_elf; // phys_page != 0 will be handled in cleanup
              }
              PROC_DEBUG_PRINTF("     Allocated frame P=%#lx", (unsigned long)phys_page);

              // Calculate data/padding for this specific page (logic remains the same)
              size_t copy_size_this_page = 0;
              size_t zero_padding_this_page = 0;
              size_t file_buffer_offset = 0;
              uintptr_t file_copy_start_vaddr = phdr->p_vaddr;
              uintptr_t file_copy_end_vaddr = phdr->p_vaddr + phdr->p_filesz;
              uintptr_t page_start_vaddr = page_v;
              uintptr_t page_end_vaddr = page_v + PAGE_SIZE;
              uintptr_t copy_v_start = MAX(page_start_vaddr, file_copy_start_vaddr);
              uintptr_t copy_v_end = MIN(page_end_vaddr, file_copy_end_vaddr);
              copy_size_this_page = (copy_v_end > copy_v_start) ? (copy_v_end - copy_v_start) : 0;
              if (copy_size_this_page > 0) {
                  file_buffer_offset = (copy_v_start - phdr->p_vaddr) + phdr->p_offset;
                  if (file_buffer_offset + copy_size_this_page > file_size || file_buffer_offset < phdr->p_offset) {
                      serial_printf("[Process] load_elf: ERROR: File offset calculation error for V=%p in '%s'.\n", (void*)page_v, path);
                      result = -ENOEXEC; goto cleanup_load_elf;
                  }
              }
              uintptr_t mem_end_vaddr = phdr->p_vaddr + phdr->p_memsz;
              uintptr_t zero_v_start = page_start_vaddr + copy_size_this_page;
              uintptr_t zero_v_end = MIN(page_end_vaddr, mem_end_vaddr);
              zero_padding_this_page = (zero_v_end > zero_v_start) ? (zero_v_end - zero_v_start) : 0;
              if (copy_size_this_page + zero_padding_this_page > PAGE_SIZE) {
                  serial_printf("[Process] load_elf: ERROR: Calculated copy+zero size (%lu) exceeds PAGE_SIZE for V=%p in '%s'.\n",
                                  (unsigned long)(copy_size_this_page + zero_padding_this_page), (void*)page_v, path);
                  result = -ENOEXEC; goto cleanup_load_elf;
              }
              PROC_DEBUG_PRINTF("     CopySize=%lu, ZeroPadding=%lu, FileOffset=%lu", (unsigned long)copy_size_this_page, (unsigned long)zero_padding_this_page, (unsigned long)file_buffer_offset);

              // Populate the allocated physical frame
              if (copy_elf_segment_data(phys_page, file_data, file_buffer_offset, copy_size_this_page, zero_padding_this_page) != 0) {
                  serial_printf("[Process] load_elf: ERROR: copy_elf_segment_data failed for V=%p in '%s'.\n", (void*)page_v, path);
                  result = -EIO; // Indicate an I/O related error during population
                  goto cleanup_load_elf; // phys_page will be handled in cleanup
              }

              // Map the populated frame into the process's address space using the derived page protection flags
              PROC_DEBUG_PRINTF("     Mapping V=%p -> P=%#lx with prot %#x", (void*)page_v, (unsigned long)phys_page, (unsigned)page_prot);
              int map_res = paging_map_single_4k(mm->pgd_phys, page_v, phys_page, page_prot);
              if (map_res != 0) {
                  serial_printf("[Process] load_elf: ERROR: paging_map_single_4k failed for V=%p -> P=%#lx in '%s' (err %d).\n",
                                  (void*)page_v, (unsigned long)phys_page, path, map_res);
                  put_frame(phys_page); // Free the frame since mapping failed
                  phys_page = 0; // Reset tracker
                  result = -ENOMEM; // Mapping failure likely due to page table allocation
                  goto cleanup_load_elf;
              }
              // Mapping successful, frame ownership transferred to page tables. Reset tracker.
              phys_page = 0;
          } // End loop for pages within a segment

          // Update the highest virtual address loaded so far
          uintptr_t current_segment_end = phdr->p_vaddr + phdr->p_memsz;
          if (current_segment_end < phdr->p_vaddr) current_segment_end = UINTPTR_MAX; // Handle overflow
          if (current_segment_end > highest_addr_loaded) {
              highest_addr_loaded = current_segment_end;
          }
          PROC_DEBUG_PRINTF("  Segment %u processed. highest_addr_loaded=%#lx", (unsigned)i, (unsigned long)highest_addr_loaded);
      } // End loop through segments

      // 4. Set Initial Program Break (end of loaded data, page-aligned up)
      *initial_brk = PAGE_ALIGN_UP(highest_addr_loaded);
      if (*initial_brk < highest_addr_loaded && highest_addr_loaded != 0) *initial_brk = UINTPTR_MAX; // Handle overflow
      serial_printf("  ELF load complete. initial_brk=%#lx\n", (unsigned long)*initial_brk);
      result = 0; // Success

  cleanup_load_elf:
      PROC_DEBUG_PRINTF("Cleanup: result=%d", result);
      if (file_data) { kfree(file_data); } // Free the buffer allocated by read_file
      if (phys_page != 0) { // Free frame if allocation failed mid-segment
         PROC_DEBUG_PRINTF("  Freeing dangling phys_page P=%#lx", (unsigned long)phys_page);
          put_frame(phys_page);
      }
      PROC_DEBUG_PRINTF("Exit result=%d", result);
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
  * kernel stack pointer in proc->kernel_esp_for_switch.
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
 
      // Stack grows down. Push in reverse order of how IRET expects them.
      // SS, ESP, EFLAGS, CS, EIP
 
      // 1. Push User SS (Stack Segment)
      //    Use the User Data Selector from GDT, ensuring RPL=3.
      kstack_ptr--; // Decrement stack pointer first
      *kstack_ptr = GDT_USER_DATA_SELECTOR | 3; // OR with RPL 3
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
      *kstack_ptr = GDT_USER_CODE_SELECTOR | 3; // OR with RPL 3
      PROC_DEBUG_PRINTF("Pushed CS = %#lx at %p\n", (unsigned long)*kstack_ptr, kstack_ptr);
 
      // 5. Push User EIP (Instruction Pointer)
      //    This is the program's entry point obtained from the ELF header.
      kstack_ptr--;
      *kstack_ptr = proc->entry_point;
      PROC_DEBUG_PRINTF("Pushed EIP = %#lx at %p\n", (unsigned long)proc->entry_point, kstack_ptr);
 
      // --- Optional: Push initial general-purpose register values (often zero) ---
      // If your context switch code expects these to be popped by 'popa' during the
      // *very first* switch TO this process, push placeholders here.
      // If the initial entry uses only IRET (like jump_to_user_mode), this is not strictly needed.
      // Assuming jump_to_user_mode only uses IRET, we don't push GP regs here.
 
 
      // 6. Save the final Kernel Stack Pointer
      //    This ESP value is what the kernel should load right before executing IRET
      //    (or what context_switch should restore) to jump to the user process for the first time.
      //    Store it in the PCB/TCB field used by your context switch mechanism.
      proc->kernel_esp_for_switch = (uint32_t)kstack_ptr; // This points to the last pushed value (EIP)
 
      serial_printf("  Kernel stack prepared for IRET. Final K_ESP = %#lx\n", (unsigned long)proc->kernel_esp_for_switch);
      PROC_DEBUG_PRINTF("Exit\n");
 }
 
 
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
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Enter path='%s'\n", __func__, __LINE__, path ? path : "<NULL>");
     KERNEL_ASSERT(path != NULL, "create_user_process: NULL path");
     serial_printf("[Process] Creating user process from '%s'.\n", path);

     pcb_t *proc = NULL;
     uintptr_t pd_phys = 0;
     void* proc_pd_virt_temp = NULL;
     bool pd_mapped_temp = false;
     bool initial_stack_mapped = false;
     uintptr_t initial_stack_phys_frame = 0;
     int ret_status = 0; // Track status for cleanup message

     // === Declare local variables needed for ELF loading and verification ===
     uint32_t entry_point = 0;           // <<< FIXED: Declare entry_point
     uintptr_t initial_brk = 0;          // <<< FIXED: Declare initial_brk
     bool mapping_error = false;        // <<< FIXED: Declare mapping_error
     // =======================================================================

     // --- Step 1: Allocate PCB ---
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Step 1: Allocate PCB\n", __func__, __LINE__);
     proc = (pcb_t *)kmalloc(sizeof(pcb_t));
     if (!proc) {
         serial_write("[Process] ERROR: kmalloc PCB failed.\n");
         return NULL;
     }
     memset(proc, 0, sizeof(pcb_t));
     proc->pid = next_pid++; // TODO: Lock this for SMP
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] PCB allocated at %p, PID=%lu\n", __func__, __LINE__, proc, (unsigned long)proc->pid);

     // === Step 1.5: Initialize File Descriptors and Lock ===
     process_init_fds(proc);
     // =======================================================

     // --- Step 2: Allocate Page Directory Frame ---
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Step 2: Allocate Page Directory Frame\n", __func__, __LINE__);
     pd_phys = frame_alloc();
     if (!pd_phys) {
         serial_printf("[Process] ERROR: frame_alloc PD failed for PID %lu.\n", (unsigned long)proc->pid);
         ret_status = -ENOMEM;
         goto fail_create;
     }
     proc->page_directory_phys = (uint32_t*)pd_phys;
     serial_printf("  Allocated PD Phys: %#lx for PID %lu\n", (unsigned long)pd_phys, (unsigned long)proc->pid);

     // --- Step 3: Initialize Page Directory ---
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Step 3: Initialize Page Directory (PD Phys=%#lx)\n", __func__, __LINE__, (unsigned long)pd_phys);
     // ... (Temp map PD, memset, copy_kernel_pde_entries, set recursive entry, unmap temp PD) ...
     proc_pd_virt_temp = paging_temp_map(pd_phys, PTE_KERNEL_DATA_FLAGS);
     if (!proc_pd_virt_temp) { /* ... error handling ... */ ret_status = -EIO; goto fail_create; }
     pd_mapped_temp = true;
     memset(proc_pd_virt_temp, 0, PAGE_SIZE);
     copy_kernel_pde_entries((uint32_t*)proc_pd_virt_temp);
     uint32_t recursive_flags = PAGE_PRESENT | PAGE_RW | (g_nx_supported ? PAGE_NX_BIT : 0);
     ((uint32_t*)proc_pd_virt_temp)[RECURSIVE_PDE_INDEX] = (pd_phys & PAGING_ADDR_MASK) | recursive_flags;
     paging_temp_unmap(proc_pd_virt_temp);
     pd_mapped_temp = false;
     proc_pd_virt_temp = NULL;
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d]   PD Initialization complete.\n", __func__, __LINE__);

     // ... (Verification block - now uses declared `mapping_error`) ...
      PROC_DEBUG_PRINTF("[Process DEBUG %s:%d]   Verifying copied kernel PDE entries...\n", __func__, __LINE__);
      // ... (rest of verification block using temp_pd_check...)
      // It should correctly set `mapping_error = true;` on failure within this block.
      void* temp_pd_check = paging_temp_map(pd_phys, PTE_KERNEL_READONLY_FLAGS);
       if (temp_pd_check) {
           uint32_t process_kernel_base_pde = ((uint32_t*)temp_pd_check)[KERNEL_PDE_INDEX];
           uint32_t global_kernel_base_pde = g_kernel_page_directory_virt[KERNEL_PDE_INDEX];
           serial_printf("  Verification: Proc PD[768]=%#08lx, Global PD[768]=%#08lx (Kernel Base PDE)\n",
                           (unsigned long)process_kernel_base_pde, (unsigned long)global_kernel_base_pde);
           if (!(process_kernel_base_pde & PAGE_PRESENT)) {
               serial_printf("  [FATAL VERIFICATION ERROR] Kernel Base PDE missing in process PD!\n");
               mapping_error = true; // Set flag on error
           }
           paging_temp_unmap(temp_pd_check);
       } else {
           serial_printf("  Verification FAILED: Could not temp map process PD (%#lx) for checking.\n", (unsigned long)pd_phys);
           mapping_error = true; // Set flag on error
           ret_status = -EIO; // Set error code if temp map failed directly
       }
       // Check the flag AFTER unmapping
       if (mapping_error) {
           if (ret_status == 0) ret_status = -EINVAL; // Set a generic error if only PDE check failed
           goto fail_create;
       }


     // --- Step 4: Allocate Kernel Stack ---
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Step 4: Allocate Kernel Stack\n", __func__, __LINE__);
     if (!allocate_kernel_stack(proc)) { ret_status = -ENOMEM; goto fail_create; }

     // --- Step 5: Create Memory Management structure ---
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Step 5: Create mm_struct\n", __func__, __LINE__);
     proc->mm = create_mm(proc->page_directory_phys);
     if (!proc->mm) { ret_status = -ENOMEM; goto fail_create; }

     // --- Step 6: Load ELF executable ---
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Step 6: Load ELF '%s'\n", __func__, __LINE__, path);
     // <<< FIXED: Use the now-declared variables >>>
     int load_res = load_elf_and_init_memory(path, proc->mm, &entry_point, &initial_brk);
      if (load_res != 0) {
          serial_printf("[Process] ERROR: Failed to load ELF '%s' (Error code %d).\n", path, load_res);
          ret_status = load_res;
          goto fail_create;
      }
      proc->entry_point = entry_point; // Assign result to PCB member
      if (proc->mm) { proc->mm->start_brk = proc->mm->end_brk = initial_brk; } // Assign result
      else { /* Should not happen */ ret_status = -EINVAL; goto fail_create; }


     // --- Step 7: Setup standard VMAs ---
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Step 7: Setup standard VMAs\n", __func__, __LINE__);
     // ... (Insert Heap and Stack VMAs) ...
      uintptr_t heap_start = proc->mm->end_brk;
      KERNEL_ASSERT(heap_start < USER_STACK_BOTTOM_VIRT, "Heap start overlaps user stack area");
      uint32_t heap_page_prot = PAGE_PRESENT | PAGE_RW | PAGE_USER | (g_nx_supported ? PAGE_NX_BIT : 0);
      if (!insert_vma(proc->mm, heap_start, heap_start, VM_READ | VM_WRITE | VM_USER | VM_ANONYMOUS, heap_page_prot, NULL, 0)) {
           /* ... error handling ... */ ret_status = -ENOMEM; goto fail_create;
      }
      serial_printf("  Initial Heap VMA placeholder added: [%#lx - %#lx)\n", (unsigned long)heap_start, (unsigned long)heap_start);
      uint32_t stack_page_prot = PAGE_PRESENT | PAGE_RW | PAGE_USER | (g_nx_supported ? PAGE_NX_BIT : 0);
      if (!insert_vma(proc->mm, USER_STACK_BOTTOM_VIRT, USER_STACK_TOP_VIRT_ADDR, VM_READ | VM_WRITE | VM_USER | VM_GROWS_DOWN | VM_ANONYMOUS, stack_page_prot, NULL, 0)) {
        /* ... error handling ... */ ret_status = -ENOMEM; goto fail_create;
      }
      serial_printf("  User Stack VMA added: [%#lx - %#lx)\n", (unsigned long)USER_STACK_BOTTOM_VIRT, (unsigned long)USER_STACK_TOP_VIRT_ADDR);

     // --- Step 8: Allocate and Map Initial User Stack Page ---
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Step 8: Allocate initial user stack page\n", __func__, __LINE__);
     initial_stack_phys_frame = frame_alloc();
     if (!initial_stack_phys_frame) { /* ... error handling ... */ ret_status = -ENOMEM; goto fail_create; }
     uintptr_t initial_user_stack_page_vaddr = USER_STACK_TOP_VIRT_ADDR - PAGE_SIZE;
     int map_res = paging_map_single_4k(proc->page_directory_phys, initial_user_stack_page_vaddr, initial_stack_phys_frame, stack_page_prot);
     if (map_res != 0) { /* ... error handling ... */ ret_status = -EIO; goto fail_create; }
     initial_stack_mapped = true;
     proc->user_stack_top = (void*)USER_STACK_TOP_VIRT_ADDR;
     serial_printf("  Initial user stack page allocated (P=%#lx) and mapped (V=%p). User ESP set to %p.\n",
                     (unsigned long)initial_stack_phys_frame, (void*)initial_user_stack_page_vaddr, proc->user_stack_top);
     // ... (Zero out stack page) ...
     void* temp_stack_map = paging_temp_map(initial_stack_phys_frame, PTE_KERNEL_DATA_FLAGS);
      if (temp_stack_map) { memset(temp_stack_map, 0, PAGE_SIZE); paging_temp_unmap(temp_stack_map); }
      else { /* ... warning ... */ }

     // --- Step 8.5: Verify EIP/ESP Mappings ---
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d]   Verifying EIP and ESP mappings/flags in Proc PD P=%#lx...\n", __func__, __LINE__, (unsigned long)proc->page_directory_phys);
     // ... (Actual verification logic block - unchanged, uses `mapping_error`) ...
      // Verify EIP page
      uintptr_t eip_vaddr = proc->entry_point;
      uintptr_t eip_page_vaddr = PAGE_ALIGN_DOWN(eip_vaddr);
      uintptr_t eip_phys = 0;
      uint32_t eip_pte_flags = 0;
      int eip_check_res = paging_get_physical_address_and_flags(proc->page_directory_phys, eip_page_vaddr, &eip_phys, &eip_pte_flags);
      if (eip_check_res != 0 || eip_phys == 0) { /* ... set mapping_error ... */ mapping_error = true; }
      else { /* ... check flags, set mapping_error if bad ... */
          bool flags_ok = (eip_pte_flags & PAGE_PRESENT) && (eip_pte_flags & PAGE_USER);
          // Add NX check if needed
          if (!flags_ok) mapping_error = true;
      }
      // Verify ESP page
      uintptr_t esp_page_vaddr_check = USER_STACK_TOP_VIRT_ADDR - PAGE_SIZE;
      uintptr_t esp_phys = 0;
      uint32_t esp_pte_flags = 0;
      int esp_check_res = paging_get_physical_address_and_flags(proc->page_directory_phys, esp_page_vaddr_check, &esp_phys, &esp_pte_flags);
      if (esp_check_res != 0 || esp_phys == 0) { /* ... set mapping_error ... */ mapping_error = true; }
      else { /* ... check flags, set mapping_error if bad ... */
          bool flags_ok = (esp_pte_flags & PAGE_PRESENT) && (esp_pte_flags & PAGE_USER) && (esp_pte_flags & PAGE_RW);
          if (!flags_ok) mapping_error = true;
      }
      // Check overall flag
      if (mapping_error) {
          ret_status = -EFAULT;
          serial_printf("[Process] Aborting process creation due to mapping/flags verification failure.\n");
          goto fail_create;
      }
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d]   User EIP and ESP mapping & flags verification passed.\n", __func__, __LINE__);


     // --- Step 9: Prepare Initial Kernel Stack for IRET ---
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Step 9: Prepare initial kernel stack for IRET\n", __func__, __LINE__);
     prepare_initial_kernel_stack(proc);

     // --- SUCCESS ---
     serial_printf("[Process] Successfully created PCB PID %lu structure for '%s'.\n",
                     (unsigned long)proc->pid, path);
     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Exit OK (proc=%p)\n", __func__, __LINE__, proc);
     return proc; // Return the prepared PCB

 fail_create:
     // --- Cleanup on Failure ---
     serial_printf("[Process] Cleanup after create_user_process failed (PID %lu, Status %d).\n",
                     (unsigned long)(proc ? proc->pid : 0), ret_status);
     // ... (Cleanup logic as before, calling destroy_process if proc is valid) ...
      if (pd_mapped_temp && proc_pd_virt_temp != NULL) {
          PROC_DEBUG_PRINTF("[Process DEBUG %s:%d]   Cleaning up dangling temporary PD mapping %p\n", __func__, __LINE__, proc_pd_virt_temp);
          paging_temp_unmap(proc_pd_virt_temp);
      }
      if (initial_stack_phys_frame != 0 && !initial_stack_mapped) {
           PROC_DEBUG_PRINTF("[Process DEBUG %s:%d]   Freeing unmapped initial user stack frame P=%#lx\n", __func__, __LINE__, (unsigned long)initial_stack_phys_frame);
           put_frame(initial_stack_phys_frame);
           initial_stack_phys_frame = 0;
           initial_stack_mapped = false;
      }
      if (proc) {
          PROC_DEBUG_PRINTF("[Process DEBUG %s:%d]   Calling destroy_process for partially created PID %lu\n", __func__, __LINE__, (unsigned long)proc->pid);
          destroy_process(proc); // Handles FDs, MM, kernel stack, PD frame, PCB kfree
      } else {
          if (pd_phys) { // Only free PD frame if PCB wasn't allocated but PD was
               PROC_DEBUG_PRINTF("[Process DEBUG %s:%d]   Freeing PD frame P=%#lx (PCB allocation failed)\n", __func__, __LINE__, (unsigned long)pd_phys);
              put_frame(pd_phys);
          }
      }

     PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Exit FAIL (NULL)\n", __func__, __LINE__);
     return NULL; // Indicate failure
 }
 
 // ------------------------------------------------------------------------
 // destroy_process - Frees all process resources
 // ------------------------------------------------------------------------
 /**
  * @brief Destroys a process and frees all associated resources.
  * Closes files, frees memory space (VMAs, page tables, frames via mm_struct),
  * kernel stack (frames and kernel mapping), page directory frame, and the PCB structure itself.
  * @warning Ensure the process is removed from the scheduler and is not running
  * before calling this function to avoid use-after-free issues.
  * @param pcb Pointer to the PCB of the process to destroy.
  */
  void destroy_process(pcb_t *pcb)
  {
       if (!pcb) return;
 
       uint32_t pid = pcb->pid; // Store PID for logging before freeing PCB
       // Using serial write assuming terminal might rely on functioning process/memory
       serial_printf("[destroy_process] Enter for PID %lu\n", (unsigned long)pid);
 
       PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Enter PID=%lu\n", __func__, __LINE__, (unsigned long)pid);
       serial_printf("[Process] Destroying process PID %lu.\n", (unsigned long)pid);
 
       // 1. Close All Open File Descriptors
       serial_write("[destroy_process] Step 1: Closing FDs...\n");
       check_idle_task_stack_integrity("destroy_process: Before close_fds");
       // Assuming process_close_fds(pcb) function exists and works
       process_close_fds(pcb);
       check_idle_task_stack_integrity("destroy_process: After close_fds");
       serial_write("[destroy_process] Step 1: FDs closed.\n");
 
       // 2. Destroy Memory Management structure (handles user space VMAs, page tables, frames)
       serial_write("[destroy_process] Step 2: Destroying MM (user space memory)...\n");
       check_idle_task_stack_integrity("destroy_process: Before destroy_mm");
       if (pcb->mm) {
           PROC_DEBUG_PRINTF("[Process DEBUG %s:%d]   Destroying mm_struct %p...\n", __func__, __LINE__, pcb->mm);
           // Assuming destroy_mm frees user pages, user page tables, VMA structs, and the mm_struct itself.
           // It should NOT typically free the page directory frame itself.
           destroy_mm(pcb->mm);
           pcb->mm = NULL;
       } else {
           PROC_DEBUG_PRINTF("[Process DEBUG %s:%d]   No mm_struct found to destroy.\n", __func__, __LINE__);
       }
       check_idle_task_stack_integrity("destroy_process: After destroy_mm");
       serial_write("[destroy_process] Step 2: MM destroyed.\n");
 
       // 3. Free Kernel Stack (Including Guard Page)
       serial_write("[destroy_process] Step 3: Freeing Kernel Stack (incl. guard)...\n");
       check_idle_task_stack_integrity("destroy_process: Before kernel stack free");
       if (pcb->kernel_stack_vaddr_top != NULL) {
           // kernel_stack_vaddr_top points to the end of the *usable* stack (e.g., 0xe0004000)
           uintptr_t stack_top_usable = (uintptr_t)pcb->kernel_stack_vaddr_top;
           size_t usable_stack_size = PROCESS_KSTACK_SIZE;
           // *** GUARD PAGE FIX: Calculate total size including guard ***
           size_t total_stack_size = usable_stack_size + PAGE_SIZE;
           size_t num_pages_with_guard = total_stack_size / PAGE_SIZE;
           uintptr_t stack_base = stack_top_usable - usable_stack_size; // Base of usable stack (e.g., 0xe0000000)
 
           serial_printf("  Freeing kernel stack (incl. guard): V=[%p-%p)\n",
                           (void*)stack_base, (void*)(stack_base + total_stack_size));
 
           // Free physical frames (Iterate over usable + guard page)
           serial_write("  Freeing kernel stack frames (incl. guard)...\n");
           for (size_t i = 0; i < num_pages_with_guard; ++i) { // *** GUARD PAGE FIX: Loop limit ***
               uintptr_t v_addr = stack_base + (i * PAGE_SIZE);
               uintptr_t phys_addr = 0;
               // Get physical address from KERNEL page directory
               if (g_kernel_page_directory_phys &&
                   paging_get_physical_address((uint32_t*)g_kernel_page_directory_phys, v_addr, &phys_addr) == 0)
               {
                   if (phys_addr != 0) {
                       put_frame(phys_addr); // Free the physical frame
                   } else {
                       serial_printf("[destroy_process] Warning: Kernel stack V=%p maps to P=0?\n", (void*)v_addr);
                   }
               } else {
                    serial_printf("[destroy_process] Warning: Failed to get physical addr for kernel stack V=%p during cleanup.\n", (void*)v_addr);
               }
           }
           serial_write("  Kernel stack frames (incl. guard) freed.\n");
 
           // NOTE: We do NOT unmap kernel stack from the kernel page directory
           // Kernel stacks are in kernel space and the mappings are shared across all processes
           // Just free the physical frames above, don't unmap the virtual addresses
           serial_write("  Kernel stack physical frames freed (virtual mappings remain in kernel PD).\n");
 
           pcb->kernel_stack_vaddr_top = NULL;
           pcb->kernel_stack_phys_base = 0;
       } else {
            PROC_DEBUG_PRINTF("[Process DEBUG %s:%d]   No kernel stack allocated or already freed.\n", __func__, __LINE__);
       }
       check_idle_task_stack_integrity("destroy_process: After kernel stack free");
        serial_write("[destroy_process] Step 3: Kernel Stack freed.\n");
 
       // 4. Free the process's Page Directory frame
       //    (Assumes destroy_mm does NOT free the PD frame itself)
       serial_write("[destroy_process] Step 4: Freeing Page Directory Frame...\n");
       check_idle_task_stack_integrity("destroy_process: Before PD frame free");
       if (pcb->page_directory_phys) {
           // Sanity check: mm should be NULL now if destroy_mm was called.
           if (pcb->mm) {
                serial_printf("[destroy_process] Warning: mm_struct is not NULL before freeing PD? Check destroy_mm.\n");
           }
           serial_printf("  Freeing process PD frame: P=%p\n", (void*)pcb->page_directory_phys);
           put_frame((uintptr_t)pcb->page_directory_phys);
           pcb->page_directory_phys = NULL;
       } else {
           PROC_DEBUG_PRINTF("[Process DEBUG %s:%d]   No Page Directory allocated or already freed.\n", __func__, __LINE__);
       }
       check_idle_task_stack_integrity("destroy_process: After PD frame free");
       serial_write("[destroy_process] Step 4: Page Directory Frame freed.\n");
 
       // 5. Free the PCB structure itself
       serial_write("[destroy_process] Step 5: Freeing PCB structure...\n");
       check_idle_task_stack_integrity("destroy_process: Before kfree(pcb)");
       kfree(pcb); // Free the memory allocated for the pcb_t struct
       check_idle_task_stack_integrity("destroy_process: After kfree(pcb)");
       serial_write("[destroy_process] Step 5: PCB structure freed.\n");
 
       serial_printf("[Process] PCB PID %lu resources freed.\n", (unsigned long)pid);
       serial_printf("[destroy_process] Exit for PID %lu\n", (unsigned long)pid);
       PROC_DEBUG_PRINTF("[Process DEBUG %s:%d] Exit PID=%lu\n", __func__, __LINE__, (unsigned long)pid);
  }
 
 // ------------------------------------------------------------------------
 // Process File Descriptor Management Implementations
 // ------------------------------------------------------------------------
 
 /**
  * @brief Initializes the file descriptor table for a new process.
  * Sets all entries to NULL, indicating no files are open.
  * Should be called during process creation after the PCB is allocated.
  *
  * @param proc Pointer to the new process's PCB.
  */
  void process_init_fds(pcb_t *proc) {
    // Use KERNEL_ASSERT for critical preconditions
    KERNEL_ASSERT(proc != NULL, "Cannot initialize FDs for NULL process");

    // Initialize the spinlock associated with this process's FD table
    spinlock_init(&proc->fd_table_lock);

    // Zero out the file descriptor table array.
    // While locking isn't strictly needed here if called only from the
    // single thread creating the process before it runs, it's harmless
    // and good defensive practice.
    uintptr_t irq_flags = spinlock_acquire_irqsave(&proc->fd_table_lock);
    memset(proc->fd_table, 0, sizeof(proc->fd_table));
    spinlock_release_irqrestore(&proc->fd_table_lock, irq_flags);

    // --- Optional: Initialize Standard I/O Descriptors ---
    // If your kernel provides standard I/O handles (e.g., via a console device driver),
    // you would allocate sys_file_t structures for them and place them in fd_table[0], [1], [2] here.
    // This requires interacting with your device/console driver API.
    // Example Placeholder:
    // assign_standard_io_fds(proc); // Hypothetical function
    // ----------------------------------------------------
}
 
 /**
  * @brief Closes all open file descriptors for a terminating process.
  * Iterates through the process's FD table and calls sys_close() for each open file.
  * Should be called during process termination *before* freeing the PCB memory.
  *
  * @param proc Pointer to the terminating process's PCB.
  */
  void process_close_fds(pcb_t *proc) {
    KERNEL_ASSERT(proc != NULL, "Cannot close FDs for NULL process");
    serial_printf("[Proc %lu] Closing all file descriptors...\n", (unsigned long)proc->pid);

    // Acquire the lock for the FD table of the process being destroyed.
    // Even though the process isn't running, the reaper (e.g., idle task)
    // needs exclusive access during cleanup.
    uintptr_t irq_flags = spinlock_acquire_irqsave(&proc->fd_table_lock);

    // Iterate through the entire file descriptor table
    for (int fd = 0; fd < MAX_FD; fd++) {
        sys_file_t *sf = proc->fd_table[fd];

        if (sf != NULL) { // Check if the file descriptor is currently open
            serial_printf("  [Proc %lu] Closing fd %d (sys_file_t* %p, vfs_file* %p)\n",
                           (unsigned long)proc->pid, fd, sf, sf->vfs_file);

            // Clear the FD table entry FIRST while holding the lock
            proc->fd_table[fd] = NULL;

            // Release the lock *before* calling potentially blocking/complex operations
            // like vfs_close or kfree. This minimizes lock contention, although
            // in this specific cleanup context it might be less critical.
            spinlock_release_irqrestore(&proc->fd_table_lock, irq_flags);

            // --- Perform cleanup outside the FD table lock ---
            // Call VFS close (safe to call now that FD entry is clear)
            int vfs_ret = vfs_close(sf->vfs_file); // vfs_close handles freeing sf->vfs_file->data and the vnode
            if (vfs_ret < 0) {
                serial_printf("   [Proc %lu] Warning: vfs_close for fd %d returned error %d.\n",
                               (unsigned long)proc->pid, fd, vfs_ret);
            }
            // Free the sys_file structure itself
            kfree(sf);
            // --- End cleanup outside lock ---

            // Re-acquire the lock to continue the loop safely
            irq_flags = spinlock_acquire_irqsave(&proc->fd_table_lock);

        } // end if (sf != NULL)
    } // end for

    // Release the lock after the loop finishes
    spinlock_release_irqrestore(&proc->fd_table_lock, irq_flags);

    serial_printf("[Proc %lu] All FDs processed for closing.\n", (unsigned long)proc->pid);
}
 
 