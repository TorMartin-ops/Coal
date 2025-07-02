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

// Error constants for process groups and sessions
#ifndef ESRCH
#define ESRCH E_NOTFOUND
#endif
#ifndef EPERM  
#define EPERM E_PERM
#endif
#ifndef EINVAL
#define EINVAL E_INVAL
#endif
#ifndef ENOTTY
#define ENOTTY E_IO
#endif
 
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
 bool allocate_kernel_stack(pcb_t *proc);
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
  bool allocate_kernel_stack(pcb_t *proc)
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
// process_create - Simple PCB allocation and initialization
// ------------------------------------------------------------------------
/**
 * @brief Allocates and initializes a basic PCB structure
 * @param name Process name (for debugging)
 * @return Pointer to the newly allocated PCB, or NULL on failure
 */
pcb_t* process_create(const char* name) {
    pcb_t* proc = (pcb_t*)kmalloc(sizeof(pcb_t));
    if (!proc) {
        serial_printf("[Process] Failed to allocate PCB\n");
        return NULL;
    }
    
    memset(proc, 0, sizeof(pcb_t));
    proc->pid = next_pid++;
    proc->state = PROC_INITIALIZING;
    
    // Initialize spinlock
    spinlock_init(&proc->fd_table_lock);
    
    // Initialize process hierarchy and process groups/sessions
    process_init_hierarchy(proc);
    process_init_pgrp_session(proc, NULL); // NULL parent = new session leader
    
    serial_printf("[Process] Created PCB for '%s' with PID %u\n", name ? name : "unnamed", proc->pid);
    return proc;
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
 
 // copy_elf_segment_data is now unused (ELF loading moved to elf_loader.c)
#if 0
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
     void* temp_vaddr = paging_temp_map(frame_paddr);
 
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
#endif // 0 - End of unused copy_elf_segment_data
 
 
// load_elf_and_init_memory is defined in elf_loader.c
extern int load_elf_and_init_memory(const char *path,
                                    mm_struct_t *mm,
                                    uint32_t *entry_point,
                                    uintptr_t *initial_brk);
 
 
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
     proc_pd_virt_temp = paging_temp_map(pd_phys);
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
      void* temp_pd_check = paging_temp_map(pd_phys);
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
     void* temp_stack_map = paging_temp_map(initial_stack_phys_frame);
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

//============================================================================
// Process Hierarchy Management for Linux Compatibility
//============================================================================

/**
 * @brief Establishes parent-child relationship between processes.
 */
void process_add_child(pcb_t *parent, pcb_t *child) {
    if (!parent || !child) return;
    
    uintptr_t flags = spinlock_acquire_irqsave(&parent->children_lock);
    
    // Set up parent-child relationship
    child->parent = parent;
    child->ppid = parent->pid;
    
    // Add child to parent's children list (insert at head)
    child->sibling = parent->children;
    parent->children = child;
    
    spinlock_release_irqrestore(&parent->children_lock, flags);
    
    serial_printf("[Process] Established parent-child relationship: PID %u -> PID %u\n",
                  parent->pid, child->pid);
}

/**
 * @brief Removes a child from parent's children list.
 */
void process_remove_child(pcb_t *parent, pcb_t *child) {
    if (!parent || !child) return;
    
    uintptr_t flags = spinlock_acquire_irqsave(&parent->children_lock);
    
    // Find and remove child from siblings list
    if (parent->children == child) {
        // Child is first in the list
        parent->children = child->sibling;
    } else {
        // Search for child in the list
        pcb_t *current = parent->children;
        while (current && current->sibling != child) {
            current = current->sibling;
        }
        if (current) {
            current->sibling = child->sibling;
        }
    }
    
    // Clear child's family pointers
    child->parent = NULL;
    child->ppid = 0;
    child->sibling = NULL;
    
    spinlock_release_irqrestore(&parent->children_lock, flags);
    
    serial_printf("[Process] Removed child PID %u from parent PID %u\n",
                  child->pid, parent->pid);
}

/**
 * @brief Finds a child process by PID.
 */
pcb_t *process_find_child(pcb_t *parent, uint32_t child_pid) {
    if (!parent) return NULL;
    
    uintptr_t flags = spinlock_acquire_irqsave(&parent->children_lock);
    
    pcb_t *current = parent->children;
    while (current) {
        if (current->pid == child_pid) {
            spinlock_release_irqrestore(&parent->children_lock, flags);
            return current;
        }
        current = current->sibling;
    }
    
    spinlock_release_irqrestore(&parent->children_lock, flags);
    return NULL;
}

/**
 * @brief Marks a process as exited and notifies parent.
 */
void process_exit_with_status(pcb_t *proc, uint32_t exit_status) {
    if (!proc) return;
    
    proc->exit_status = exit_status;
    proc->has_exited = true;
    proc->state = PROC_ZOMBIE;
    
    serial_printf("[Process] PID %u exited with status %u\n", proc->pid, exit_status);
    
    // If process has children, reparent them to init process (PID 1)
    if (proc->children) {
        // TODO: Find init process and reparent children
        // For now, just orphan them
        uintptr_t flags = spinlock_acquire_irqsave(&proc->children_lock);
        pcb_t *child = proc->children;
        while (child) {
            pcb_t *next_child = child->sibling;
            child->parent = NULL;
            child->ppid = 1; // Reparent to init
            child->sibling = NULL;
            child = next_child;
        }
        proc->children = NULL;
        spinlock_release_irqrestore(&proc->children_lock, flags);
        
        serial_printf("[Process] Reparented children of PID %u to init\n", proc->pid);
    }
    
    // TODO: Wake up parent if it's waiting in waitpid()
}

/**
 * @brief Reaps zombie children and cleans up their resources.
 */
int process_reap_child(pcb_t *parent, int child_pid, int *status) {
    if (!parent) return -ESRCH;
    
    uintptr_t flags = spinlock_acquire_irqsave(&parent->children_lock);
    
    pcb_t *child_to_reap = NULL;
    
    if (child_pid == -1) {
        // Wait for any child - find first zombie
        pcb_t *current = parent->children;
        while (current) {
            if (current->has_exited) {
                child_to_reap = current;
                break;
            }
            current = current->sibling;
        }
    } else {
        // Wait for specific child
        child_to_reap = process_find_child(parent, child_pid);
        if (child_to_reap && !child_to_reap->has_exited) {
            child_to_reap = NULL; // Child exists but hasn't exited
        }
    }
    
    if (!child_to_reap) {
        spinlock_release_irqrestore(&parent->children_lock, flags);
        return child_pid == -1 ? -ECHILD : -ECHILD; // No zombie children
    }
    
    // Copy exit status
    if (status) {
        *status = child_to_reap->exit_status;
    }
    
    uint32_t reaped_pid = child_to_reap->pid;
    
    // Remove from children list
    if (parent->children == child_to_reap) {
        parent->children = child_to_reap->sibling;
    } else {
        pcb_t *current = parent->children;
        while (current && current->sibling != child_to_reap) {
            current = current->sibling;
        }
        if (current) {
            current->sibling = child_to_reap->sibling;
        }
    }
    
    spinlock_release_irqrestore(&parent->children_lock, flags);
    
    // Clean up child resources
    serial_printf("[Process] Reaping zombie child PID %u (exit status %u)\n",
                  reaped_pid, child_to_reap->exit_status);
    
    // TODO: Call destroy_process(child_to_reap) to free all resources
    // destroy_process(child_to_reap);
    
    return reaped_pid;
}

/**
 * @brief Initializes process hierarchy fields in a new PCB.
 */
void process_init_hierarchy(pcb_t *proc) {
    if (!proc) return;
    
    proc->ppid = 0;
    proc->parent = NULL;
    proc->children = NULL;
    proc->sibling = NULL;
    proc->exit_status = 0;
    proc->has_exited = false;
    spinlock_init(&proc->children_lock);
}

//============================================================================
// Process Groups and Sessions Management
//============================================================================

void process_init_pgrp_session(pcb_t *proc, pcb_t *parent) {
    if (!proc) return;
    
    proc->pid_namespace = 0; // Default namespace
    
    if (!parent) {
        // Init process - create new session and process group
        proc->sid = proc->pid;
        proc->pgid = proc->pid;
        proc->session_leader = proc;
        proc->pgrp_leader = proc;
        proc->is_session_leader = true;
        proc->is_pgrp_leader = true;
        proc->controlling_terminal = NULL;
        proc->has_controlling_tty = false;
        proc->tty_pgrp = proc->pid;
    } else {
        // Inherit from parent
        proc->sid = parent->sid;
        proc->pgid = parent->pgid;
        proc->session_leader = parent->session_leader;
        proc->pgrp_leader = parent->pgrp_leader;
        proc->is_session_leader = false;
        proc->is_pgrp_leader = false;
        proc->controlling_terminal = parent->controlling_terminal;
        proc->has_controlling_tty = parent->has_controlling_tty;
        proc->tty_pgrp = parent->tty_pgrp;
        
        // Add to parent's process group
        if (parent->pgrp_leader) {
            process_join_pgrp(proc, parent->pgrp_leader);
        }
    }
    
    proc->pgrp_next = NULL;
    proc->pgrp_prev = NULL;
    
    serial_printf("[Process] Initialized PID %u: SID=%u, PGID=%u, Session Leader=%s, PGrp Leader=%s\n",
                  proc->pid, proc->sid, proc->pgid,
                  proc->is_session_leader ? "Yes" : "No",
                  proc->is_pgrp_leader ? "Yes" : "No");
}

int process_setsid(pcb_t *proc) {
    if (!proc) return -ESRCH;
    
    // Cannot create session if already a process group leader
    if (proc->is_pgrp_leader) {
        return -EPERM;
    }
    
    // Leave current process group
    process_leave_pgrp(proc);
    
    // Create new session and process group
    proc->sid = proc->pid;
    proc->pgid = proc->pid;
    proc->session_leader = proc;
    proc->pgrp_leader = proc;
    proc->is_session_leader = true;
    proc->is_pgrp_leader = true;
    
    // Lose controlling terminal
    proc->controlling_terminal = NULL;
    proc->has_controlling_tty = false;
    proc->tty_pgrp = proc->pid;
    
    serial_printf("[Process] PID %u created new session (SID=%u)\n", proc->pid, proc->sid);
    return proc->sid;
}

uint32_t process_getsid(pcb_t *proc) {
    if (!proc) return 0;
    return proc->sid;
}

int process_setpgid(pcb_t *proc, uint32_t pgid) {
    if (!proc) return -ESRCH;
    
    // If pgid is 0, use process PID
    if (pgid == 0) {
        pgid = proc->pid;
    }
    
    // Cannot change pgid if session leader
    if (proc->is_session_leader) {
        return -EPERM;
    }
    
    // Find the target process group leader
    pcb_t *new_pgrp_leader = NULL;
    if (pgid == proc->pid) {
        // Creating new process group with self as leader
        new_pgrp_leader = proc;
    } else {
        // TODO: Find process group leader by PGID
        // For now, simplified implementation
        new_pgrp_leader = proc->pgrp_leader;
    }
    
    if (!new_pgrp_leader) {
        return -ESRCH;
    }
    
    // Leave current process group
    process_leave_pgrp(proc);
    
    // Join new process group
    int result = process_join_pgrp(proc, new_pgrp_leader);
    if (result == 0) {
        proc->pgid = pgid;
        if (pgid == proc->pid) {
            proc->is_pgrp_leader = true;
            proc->pgrp_leader = proc;
        }
        
        serial_printf("[Process] PID %u joined process group %u\n", proc->pid, pgid);
    }
    
    return result;
}

uint32_t process_getpgid(pcb_t *proc) {
    if (!proc) return 0;
    return proc->pgid;
}

int process_join_pgrp(pcb_t *proc, pcb_t *pgrp_leader) {
    if (!proc || !pgrp_leader) return -EINVAL;
    
    // Ensure both processes are in same session
    if (proc->sid != pgrp_leader->sid) {
        return -EPERM;
    }
    
    // Add to process group linked list
    proc->pgrp_leader = pgrp_leader;
    proc->pgrp_next = pgrp_leader->pgrp_next;
    proc->pgrp_prev = pgrp_leader;
    
    if (pgrp_leader->pgrp_next) {
        pgrp_leader->pgrp_next->pgrp_prev = proc;
    }
    pgrp_leader->pgrp_next = proc;
    
    proc->pgid = pgrp_leader->pgid;
    proc->is_pgrp_leader = (proc == pgrp_leader);
    
    serial_printf("[Process] PID %u joined process group led by PID %u\n", 
                  proc->pid, pgrp_leader->pid);
    
    return 0;
}

void process_leave_pgrp(pcb_t *proc) {
    if (!proc) return;
    
    // Remove from process group linked list
    if (proc->pgrp_prev) {
        proc->pgrp_prev->pgrp_next = proc->pgrp_next;
    }
    if (proc->pgrp_next) {
        proc->pgrp_next->pgrp_prev = proc->pgrp_prev;
    }
    
    // If this was the process group leader, transfer leadership
    if (proc->is_pgrp_leader && proc->pgrp_next) {
        pcb_t *new_leader = proc->pgrp_next;
        new_leader->is_pgrp_leader = true;
        new_leader->pgrp_leader = new_leader;
        
        // Update all processes in the group
        pcb_t *current = new_leader;
        while (current) {
            current->pgrp_leader = new_leader;
            current->pgid = new_leader->pid;
            current = current->pgrp_next;
        }
        
        serial_printf("[Process] PID %u transferred process group leadership to PID %u\n",
                      proc->pid, new_leader->pid);
    }
    
    proc->pgrp_next = NULL;
    proc->pgrp_prev = NULL;
    proc->pgrp_leader = proc; // Self-reference
    proc->is_pgrp_leader = true;
    proc->pgid = proc->pid;
    
    serial_printf("[Process] PID %u left process group\n", proc->pid);
}

int process_tcsetpgrp(pcb_t *proc, uint32_t pgid) {
    if (!proc) return -ESRCH;
    
    // Only session leader can change foreground process group
    if (!proc->is_session_leader) {
        return -EPERM;
    }
    
    // Must have controlling terminal
    if (!proc->has_controlling_tty) {
        return -ENOTTY;
    }
    
    // TODO: Validate that pgid exists in this session
    
    proc->tty_pgrp = pgid;
    
    serial_printf("[Process] Session leader PID %u set foreground group to %u\n", 
                  proc->pid, pgid);
    
    return 0;
}

int process_tcgetpgrp(pcb_t *proc) {
    if (!proc) return -ESRCH;
    
    if (!proc->has_controlling_tty) {
        return -ENOTTY;
    }
    
    return proc->tty_pgrp;
}
 
 