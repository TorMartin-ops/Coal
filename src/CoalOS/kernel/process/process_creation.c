/**
 * @file process_creation.c
 * @brief High-level process creation and destruction orchestration
 * 
 * Coordinates the creation of user processes by orchestrating PCB allocation,
 * memory setup, ELF loading, and kernel stack preparation. Follows the 
 * orchestration pattern rather than doing the work directly.
 */

#include <kernel/process/process.h>
#include <kernel/memory/mm.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/frame.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/core/types.h>
#include <kernel/lib/string.h>
#include <kernel/process/scheduler.h>
#include <kernel/fs/vfs/read_file.h>
#include <kernel/memory/kmalloc_internal.h>
#include <kernel/process/elf.h>
#include <libc/stddef.h>
#include <kernel/lib/assert.h>
#include <kernel/cpu/gdt.h>
#include <kernel/cpu/tss.h>
#include <kernel/fs/vfs/sys_file.h>
#include <kernel/fs/vfs/fs_limits.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/drivers/display/serial.h>

// Define USER_SPACE_START_VIRT if not defined elsewhere (e.g., paging.h)
#ifndef USER_SPACE_START_VIRT
#define USER_SPACE_START_VIRT 0x00001000
#endif

#ifndef KERNEL_VIRT_BASE
#define KERNEL_VIRT_BASE 0xC0000000
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
#define PROCESS_DEBUG 0

#if PROCESS_DEBUG
#define PROC_DEBUG_PRINTF(fmt, ...) terminal_printf("[Process DEBUG %s:%d] " fmt, __func__, __LINE__, ##__VA_ARGS__)
#else
#define PROC_DEBUG_PRINTF(fmt, ...)
#endif

// External globals
extern uint32_t g_kernel_page_directory_phys;
extern bool g_nx_supported;
extern uint32_t *g_kernel_page_directory_virt; // Add this missing declaration

// Forward declarations
extern int load_elf_and_init_memory(const char *path, mm_struct_t *mm, uint32_t *entry_point, uintptr_t *initial_brk);
extern void copy_kernel_pde_entries(uint32_t *new_pd_virt);
extern void check_idle_task_stack_integrity(const char *checkpoint);

// Function prototypes for separated modules
extern bool allocate_kernel_stack(pcb_t *proc);
extern void free_kernel_stack(pcb_t *proc);
extern void process_init_fds(pcb_t *proc);
extern void process_close_fds(pcb_t *proc);

/**
 * @brief Prepares the kernel stack of a newly created process for the initial
 * transition to user mode via the IRET instruction.
 * @param proc Pointer to the PCB of the process.
 */
static void prepare_initial_kernel_stack(pcb_t *proc) {
     PROC_DEBUG_PRINTF("Enter\n");
     KERNEL_ASSERT(proc != NULL, "prepare_initial_kernel_stack: NULL proc");
     KERNEL_ASSERT(proc->kernel_stack_vaddr_top != NULL, "prepare_initial_kernel_stack: Kernel stack top is NULL");
     KERNEL_ASSERT(proc->entry_point != 0, "prepare_initial_kernel_stack: Entry point is zero");
     KERNEL_ASSERT(proc->user_stack_top != NULL, "prepare_initial_kernel_stack: User stack top is NULL");

     // Get the top address and move down to prepare for pushes
     uint32_t *kstack_ptr = (uint32_t*)proc->kernel_stack_vaddr_top;
     PROC_DEBUG_PRINTF("Initial kstack_ptr (top) = %p\n", kstack_ptr);

     // Stack grows down. Push in reverse order of how IRET expects them.
     // SS, ESP, EFLAGS, CS, EIP

     // 1. Push User SS (Stack Segment)
     kstack_ptr--;
     *kstack_ptr = GDT_USER_DATA_SELECTOR;
     PROC_DEBUG_PRINTF("Pushed SS = %#lx at %p\n", (unsigned long)*kstack_ptr, kstack_ptr);

     // 2. Push User ESP (Stack Pointer)
     kstack_ptr--;
     *kstack_ptr = (uint32_t)proc->user_stack_top;
      PROC_DEBUG_PRINTF("Pushed ESP = %#lx at %p\n", (unsigned long)*kstack_ptr, kstack_ptr);

     // 3. Push EFLAGS
     kstack_ptr--;
     *kstack_ptr = USER_EFLAGS_DEFAULT;
     PROC_DEBUG_PRINTF("Pushed EFLAGS = %#lx at %p\n", (unsigned long)*kstack_ptr, kstack_ptr);

     // 4. Push User CS (Code Segment)
     kstack_ptr--;
     *kstack_ptr = GDT_USER_CODE_SELECTOR;
     PROC_DEBUG_PRINTF("Pushed CS = %#lx at %p\n", (unsigned long)*kstack_ptr, kstack_ptr);

     // 5. Push User EIP (Instruction Pointer)
     kstack_ptr--;
     *kstack_ptr = proc->entry_point;
     PROC_DEBUG_PRINTF("Pushed EIP = %#lx at %p\n", (unsigned long)proc->entry_point, kstack_ptr);

     // 6. Save the final Kernel Stack Pointer
     proc->kernel_esp_for_switch = (uint32_t)kstack_ptr;

     serial_printf("  Kernel stack prepared for IRET. Final K_ESP = %#lx\n", (unsigned long)proc->kernel_esp_for_switch);
    
    // Debug: Print the values that will be used for IRET
    serial_printf("  [DEBUG] IRET stack values: EIP=%#lx, CS=%#lx, EFLAGS=%#lx, ESP=%#lx, SS=%#lx\n",
                  (unsigned long)kstack_ptr[0],  // EIP
                  (unsigned long)kstack_ptr[1],  // CS
                  (unsigned long)kstack_ptr[2],  // EFLAGS
                  (unsigned long)kstack_ptr[3],  // ESP
                  (unsigned long)kstack_ptr[4]); // SS
    
    // Debug: Verify entry point is mapped
    serial_printf("  [DEBUG] Entry point 0x%x should be mapped during ELF loading\n", proc->entry_point);
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
    PROC_DEBUG_PRINTF("Enter path='%s'\n", path ? path : "<NULL>");
    KERNEL_ASSERT(path != NULL, "create_user_process: NULL path");
    serial_printf("[Process] Creating user process from '%s'.\n", path);

    pcb_t *proc = NULL;
    uintptr_t pd_phys = 0;
    void* proc_pd_virt_temp = NULL;
    bool pd_mapped_temp = false;
    bool initial_stack_mapped = false;
    uintptr_t initial_stack_phys_frame = 0;
    int ret_status = 0;

    // Declare local variables needed for ELF loading and verification
    uint32_t entry_point = 0;
    uintptr_t initial_brk = 0;
    bool mapping_error = false;

    // --- Step 1: Allocate PCB ---
    PROC_DEBUG_PRINTF("Step 1: Allocate PCB\n");
    proc = (pcb_t *)kmalloc(sizeof(pcb_t));
    if (!proc) {
        serial_write("[Process] ERROR: kmalloc PCB failed.\n");
        return NULL;
    }
    memset(proc, 0, sizeof(pcb_t));
    // Use the PCB manager to allocate PID - this will be handled by process_create()
    // For now, use a temporary approach until we refactor this properly
    static uint32_t temp_next_pid = 100; // Start after PCB manager PIDs
    proc->pid = temp_next_pid++;
    PROC_DEBUG_PRINTF("PCB allocated at %p, PID=%lu\n", proc, (unsigned long)proc->pid);

    // === Step 1.5: Initialize File Descriptors and Lock ===
    process_init_fds(proc);

    // --- Step 2: Allocate Page Directory Frame ---
    PROC_DEBUG_PRINTF("Step 2: Allocate Page Directory Frame\n");
    pd_phys = frame_alloc();
    if (!pd_phys) {
        serial_printf("[Process] ERROR: frame_alloc PD failed for PID %lu.\n", (unsigned long)proc->pid);
        ret_status = -ENOMEM;
        goto fail_create;
    }
    proc->page_directory_phys = (uint32_t*)pd_phys;
    serial_printf("  Allocated PD Phys: %#lx for PID %lu\n", (unsigned long)pd_phys, (unsigned long)proc->pid);

    // --- Step 3: Initialize Page Directory ---
    PROC_DEBUG_PRINTF("Step 3: Initialize Page Directory (PD Phys=%#lx)\n", (unsigned long)pd_phys);
    proc_pd_virt_temp = paging_temp_map(pd_phys);
    if (!proc_pd_virt_temp) { ret_status = -EIO; goto fail_create; }
    pd_mapped_temp = true;
    memset(proc_pd_virt_temp, 0, PAGE_SIZE);
    copy_kernel_pde_entries((uint32_t*)proc_pd_virt_temp);
    uint32_t recursive_flags = PAGE_PRESENT | PAGE_RW | (g_nx_supported ? PAGE_NX_BIT : 0);
    ((uint32_t*)proc_pd_virt_temp)[RECURSIVE_PDE_INDEX] = (pd_phys & PAGING_ADDR_MASK) | recursive_flags;
    paging_temp_unmap(proc_pd_virt_temp);
    pd_mapped_temp = false;
    proc_pd_virt_temp = NULL;
    PROC_DEBUG_PRINTF("  PD Initialization complete.\n");

    // Verification block
     PROC_DEBUG_PRINTF("  Verifying copied kernel PDE entries...\n");
     void* temp_pd_check = paging_temp_map(pd_phys);
      if (temp_pd_check) {
          uint32_t process_kernel_base_pde = ((uint32_t*)temp_pd_check)[KERNEL_PDE_INDEX];
          uint32_t global_kernel_base_pde = g_kernel_page_directory_virt[KERNEL_PDE_INDEX];
          serial_printf("  Verification: Proc PD[768]=%#08lx, Global PD[768]=%#08lx (Kernel Base PDE)\n",
                          (unsigned long)process_kernel_base_pde, (unsigned long)global_kernel_base_pde);
          if (!(process_kernel_base_pde & PAGE_PRESENT)) {
              serial_printf("  [FATAL VERIFICATION ERROR] Kernel Base PDE missing in process PD!\n");
              mapping_error = true;
          }
          paging_temp_unmap(temp_pd_check);
      } else {
          serial_printf("  Verification FAILED: Could not temp map process PD (%#lx) for checking.\n", (unsigned long)pd_phys);
          mapping_error = true;
          ret_status = -EIO;
      }
      // Check the flag AFTER unmapping
      if (mapping_error) {
          if (ret_status == 0) ret_status = -EINVAL;
          goto fail_create;
      }

    // --- Step 4: Allocate Kernel Stack ---
    PROC_DEBUG_PRINTF("Step 4: Allocate Kernel Stack\n");
    if (!allocate_kernel_stack(proc)) { ret_status = -ENOMEM; goto fail_create; }

    // --- Step 5: Create Memory Management structure ---
    PROC_DEBUG_PRINTF("Step 5: Create mm_struct\n");
    proc->mm = create_mm(proc->page_directory_phys);
    if (!proc->mm) { ret_status = -ENOMEM; goto fail_create; }

    // --- Step 6: Load ELF executable ---
    PROC_DEBUG_PRINTF("Step 6: Load ELF '%s'\n", path);
    int load_res = load_elf_and_init_memory(path, proc->mm, &entry_point, &initial_brk);
     if (load_res != 0) {
         serial_printf("[Process] ERROR: Failed to load ELF '%s' (Error code %d).\n", path, load_res);
         ret_status = load_res;
         goto fail_create;
     }
     proc->entry_point = entry_point;
     if (proc->mm) { proc->mm->start_brk = proc->mm->end_brk = initial_brk; }
     else { ret_status = -EINVAL; goto fail_create; }

    // --- Step 7: Setup standard VMAs ---
    PROC_DEBUG_PRINTF("Step 7: Setup standard VMAs\n");
     uintptr_t heap_start = proc->mm->end_brk;
     KERNEL_ASSERT(heap_start < USER_STACK_BOTTOM_VIRT, "Heap start overlaps user stack area");
     uint32_t heap_page_prot = PAGE_PRESENT | PAGE_RW | PAGE_USER | (g_nx_supported ? PAGE_NX_BIT : 0);
     if (!insert_vma(proc->mm, heap_start, heap_start, VM_READ | VM_WRITE | VM_USER | VM_ANONYMOUS, heap_page_prot, NULL, 0)) {
          ret_status = -ENOMEM; goto fail_create;
     }
     serial_printf("  Initial Heap VMA placeholder added: [%#lx - %#lx)\n", (unsigned long)heap_start, (unsigned long)heap_start);
     uint32_t stack_page_prot = PAGE_PRESENT | PAGE_RW | PAGE_USER | (g_nx_supported ? PAGE_NX_BIT : 0);
     if (!insert_vma(proc->mm, USER_STACK_BOTTOM_VIRT, USER_STACK_TOP_VIRT_ADDR, VM_READ | VM_WRITE | VM_USER | VM_GROWS_DOWN | VM_ANONYMOUS, stack_page_prot, NULL, 0)) {
       ret_status = -ENOMEM; goto fail_create;
     }
     serial_printf("  User Stack VMA added: [%#lx - %#lx)\n", (unsigned long)USER_STACK_BOTTOM_VIRT, (unsigned long)USER_STACK_TOP_VIRT_ADDR);

    // --- Step 8: Allocate and Map Initial User Stack Page ---
    PROC_DEBUG_PRINTF("Step 8: Allocate initial user stack page\n");
    initial_stack_phys_frame = frame_alloc();
    if (!initial_stack_phys_frame) { ret_status = -ENOMEM; goto fail_create; }
    uintptr_t initial_user_stack_page_vaddr = USER_STACK_TOP_VIRT_ADDR - PAGE_SIZE;
    int map_res = paging_map_single_4k(proc->page_directory_phys, initial_user_stack_page_vaddr, initial_stack_phys_frame, stack_page_prot);
    if (map_res != 0) { ret_status = -EIO; goto fail_create; }
    initial_stack_mapped = true;
    proc->user_stack_top = (void*)USER_STACK_TOP_VIRT_ADDR;
    serial_printf("  Initial user stack page allocated (P=%#lx) and mapped (V=%p). User ESP set to %p.\n",
                    (unsigned long)initial_stack_phys_frame, (void*)initial_user_stack_page_vaddr, proc->user_stack_top);
    
    void* temp_stack_map = paging_temp_map(initial_stack_phys_frame);
     if (temp_stack_map) { memset(temp_stack_map, 0, PAGE_SIZE); paging_temp_unmap(temp_stack_map); }

    // --- Step 8.5: Verify EIP/ESP Mappings ---
    PROC_DEBUG_PRINTF("  Verifying EIP and ESP mappings/flags in Proc PD P=%#lx...\n", (unsigned long)proc->page_directory_phys);
     // Verify EIP page
     uintptr_t eip_vaddr = proc->entry_point;
     uintptr_t eip_page_vaddr = PAGE_ALIGN_DOWN(eip_vaddr);
     uintptr_t eip_phys = 0;
     uint32_t eip_pte_flags = 0;
     int eip_check_res = paging_get_physical_address_and_flags(proc->page_directory_phys, eip_page_vaddr, &eip_phys, &eip_pte_flags);
     if (eip_check_res != 0 || eip_phys == 0) { mapping_error = true; }
     else {
         bool flags_ok = (eip_pte_flags & PAGE_PRESENT) && (eip_pte_flags & PAGE_USER);
         if (!flags_ok) mapping_error = true;
     }
     // Verify ESP page
     uintptr_t esp_page_vaddr_check = USER_STACK_TOP_VIRT_ADDR - PAGE_SIZE;
     uintptr_t esp_phys = 0;
     uint32_t esp_pte_flags = 0;
     int esp_check_res = paging_get_physical_address_and_flags(proc->page_directory_phys, esp_page_vaddr_check, &esp_phys, &esp_pte_flags);
     if (esp_check_res != 0 || esp_phys == 0) { mapping_error = true; }
     else {
         bool flags_ok = (esp_pte_flags & PAGE_PRESENT) && (esp_pte_flags & PAGE_USER) && (esp_pte_flags & PAGE_RW);
         if (!flags_ok) mapping_error = true;
     }
     // Check overall flag
     if (mapping_error) {
         ret_status = -EFAULT;
         serial_printf("[Process] Aborting process creation due to mapping/flags verification failure.\n");
         goto fail_create;
     }
    PROC_DEBUG_PRINTF("  User EIP and ESP mapping & flags verification passed.\n");

    // --- Step 9: Prepare Initial Kernel Stack for IRET ---
    PROC_DEBUG_PRINTF("Step 9: Prepare initial kernel stack for IRET\n");
    prepare_initial_kernel_stack(proc);

    // --- SUCCESS ---
    serial_printf("[Process] Successfully created PCB PID %lu structure for '%s'.\n",
                    (unsigned long)proc->pid, path);
    PROC_DEBUG_PRINTF("Exit OK (proc=%p)\n", proc);
    return proc;

fail_create:
    // --- Cleanup on Failure ---
    serial_printf("[Process] Cleanup after create_user_process failed (PID %lu, Status %d).\n",
                    (unsigned long)(proc ? proc->pid : 0), ret_status);
     if (pd_mapped_temp && proc_pd_virt_temp != NULL) {
         PROC_DEBUG_PRINTF("  Cleaning up dangling temporary PD mapping %p\n", proc_pd_virt_temp);
         paging_temp_unmap(proc_pd_virt_temp);
     }
     if (initial_stack_phys_frame != 0 && !initial_stack_mapped) {
          PROC_DEBUG_PRINTF("  Freeing unmapped initial user stack frame P=%#lx\n", (unsigned long)initial_stack_phys_frame);
          put_frame(initial_stack_phys_frame);
          initial_stack_phys_frame = 0;
          initial_stack_mapped = false;
     }
     if (proc) {
         PROC_DEBUG_PRINTF("  Calling destroy_process for partially created PID %lu\n", (unsigned long)proc->pid);
         destroy_process(proc);
     } else {
         if (pd_phys) {
              PROC_DEBUG_PRINTF("  Freeing PD frame P=%#lx (PCB allocation failed)\n", (unsigned long)pd_phys);
             put_frame(pd_phys);
         }
     }

    PROC_DEBUG_PRINTF("Exit FAIL (NULL)\n");
    return NULL;
}

/**
 * @brief Destroys a process and frees all associated resources.
 * @warning Ensure the process is removed from the scheduler and is not running
 * before calling this function to avoid use-after-free issues.
 * @param pcb Pointer to the PCB of the process to destroy.
 */
 void destroy_process(pcb_t *pcb)
 {
      if (!pcb) return;

      uint32_t pid = pcb->pid;
      serial_printf("[destroy_process] Enter for PID %lu\n", (unsigned long)pid);
      PROC_DEBUG_PRINTF("Enter PID=%lu\n", (unsigned long)pid);
      serial_printf("[Process] Destroying process PID %lu.\n", (unsigned long)pid);

      // 1. Close All Open File Descriptors
      serial_write("[destroy_process] Step 1: Closing FDs...\n");
      check_idle_task_stack_integrity("destroy_process: Before close_fds");
      process_close_fds(pcb);
      check_idle_task_stack_integrity("destroy_process: After close_fds");
      serial_write("[destroy_process] Step 1: FDs closed.\n");

      // 2. Destroy Memory Management structure
      serial_write("[destroy_process] Step 2: Destroying MM (user space memory)...\n");
      check_idle_task_stack_integrity("destroy_process: Before destroy_mm");
      if (pcb->mm) {
          PROC_DEBUG_PRINTF("  Destroying mm_struct %p...\n", pcb->mm);
          destroy_mm(pcb->mm);
          pcb->mm = NULL;
      } else {
          PROC_DEBUG_PRINTF("  No mm_struct found to destroy.\n");
      }
      check_idle_task_stack_integrity("destroy_process: After destroy_mm");
      serial_write("[destroy_process] Step 2: MM destroyed.\n");

      // 3. Free Kernel Stack
      serial_write("[destroy_process] Step 3: Freeing Kernel Stack (incl. guard)...\n");
      check_idle_task_stack_integrity("destroy_process: Before kernel stack free");
      free_kernel_stack(pcb);
      check_idle_task_stack_integrity("destroy_process: After kernel stack free");
       serial_write("[destroy_process] Step 3: Kernel Stack freed.\n");

      // 4. Free the process's Page Directory frame
      serial_write("[destroy_process] Step 4: Freeing Page Directory Frame...\n");
      check_idle_task_stack_integrity("destroy_process: Before PD frame free");
      if (pcb->page_directory_phys) {
          if (pcb->mm) {
               serial_printf("[destroy_process] Warning: mm_struct is not NULL before freeing PD? Check destroy_mm.\n");
          }
          serial_printf("  Freeing process PD frame: P=%p\n", (void*)pcb->page_directory_phys);
          put_frame((uintptr_t)pcb->page_directory_phys);
          pcb->page_directory_phys = NULL;
      } else {
          PROC_DEBUG_PRINTF("  No Page Directory allocated or already freed.\n");
      }
      check_idle_task_stack_integrity("destroy_process: After PD frame free");
      serial_write("[destroy_process] Step 4: Page Directory Frame freed.\n");

      // 5. Free the PCB structure itself
      serial_write("[destroy_process] Step 5: Freeing PCB structure...\n");
      check_idle_task_stack_integrity("destroy_process: Before kfree(pcb)");
      kfree(pcb);
      check_idle_task_stack_integrity("destroy_process: After kfree(pcb)");
      serial_write("[destroy_process] Step 5: PCB structure freed.\n");

      serial_printf("[Process] PCB PID %lu resources freed.\n", (unsigned long)pid);
      serial_printf("[destroy_process] Exit for PID %lu\n", (unsigned long)pid);
      PROC_DEBUG_PRINTF("Exit PID=%lu\n", (unsigned long)pid);
 }

//============================================================================
// New Standardized Process Creation API Implementation
//============================================================================

error_t create_user_process_safe(const char *path, pcb_t **proc_out) {
    // Input validation
    if (!path || !proc_out) {
        return E_INVAL;
    }
    
    // Basic path validation
    size_t path_len = strlen(path);
    if (path_len == 0 || path_len >= 256) { // Reasonable path length limit
        return E_INVAL;
    }

    // Check if file exists before proceeding with expensive process creation
    int fd = sys_open(path, 0, 0); // O_RDONLY = 0, mode = 0
    if (fd < 0) {
        serial_printf("[Process] Executable file '%s' not found (open failed with %d)\n", path, fd);
        if (fd == -ENOENT) {
            return E_NOTFOUND;
        } else if (fd == -ENOMEM) {
            return E_NOMEM;
        } else {
            return E_IO;
        }
    }
    sys_close(fd); // Close the file, we just wanted to verify it exists

    // Use the legacy create_user_process implementation for now
    // TODO: This should be refactored to use the new standardized APIs internally
    pcb_t *proc = create_user_process(path);
    if (!proc) {
        // The legacy function doesn't provide specific error context
        // We have to infer the error type based on common failure modes
        serial_printf("[Process] Legacy create_user_process failed for '%s'\n", path);
        
        // Try to determine the failure reason
        // If we can't allocate even a small amount of memory, it's E_NOMEM
        void *test_alloc = kmalloc(64);
        if (!test_alloc) {
            return E_NOMEM;
        }
        kfree(test_alloc);
        
        // If memory is available but process creation failed, it's likely corruption or ELF issues
        return E_CORRUPT;
    }

    // Success
    *proc_out = proc;
    serial_printf("[Process] User process created successfully from '%s' with PID %u\n", 
                  path, proc->pid);
    return E_SUCCESS;
}