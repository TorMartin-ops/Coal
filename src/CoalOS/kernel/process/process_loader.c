/**
 * @file process_loader.c
 * @brief Process loading and initialization following Single Responsibility Principle
 * 
 * This module is responsible ONLY for:
 * - Process creation and initialization orchestration
 * - User stack setup and mapping
 * - Virtual memory area (VMA) creation for heap and stack
 * - Initial kernel stack preparation for context switching
 * - Process validation and verification
 */

#define LOG_MODULE "process_loader"

#include <kernel/process/process.h>
#include <kernel/memory/mm.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/frame.h>
#include <kernel/interfaces/logger.h>
#include <kernel/interfaces/memory_allocator.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>
#include <kernel/cpu/gdt.h>
#include <kernel/cpu/tss.h>

// Memory layout constants
#ifndef USER_SPACE_START_VIRT
#define USER_SPACE_START_VIRT 0x00001000
#endif

#ifndef KERNEL_VIRT_BASE
#define KERNEL_VIRT_BASE 0xC0000000
#endif

// User stack configuration - defined in process.h
// #define USER_STACK_PAGES 4
// #define USER_STACK_SIZE (USER_STACK_PAGES * PAGE_SIZE)
// #define USER_STACK_TOP_VIRT_ADDR (KERNEL_VIRT_BASE)
// #define USER_STACK_BOTTOM_VIRT (USER_STACK_TOP_VIRT_ADDR - USER_STACK_SIZE)

// Initial EFLAGS for user processes (IF=1, reserved bit 1=1) - defined in process.h
// #define USER_EFLAGS_DEFAULT 0x202

// Externals
extern uint32_t g_kernel_page_directory_phys;
extern uint32_t *g_kernel_page_directory_virt;
extern bool g_nx_supported;

// Debug macros
#define PROCESS_DEBUG 0
#if PROCESS_DEBUG
#define PROC_DEBUG_PRINTF(fmt, ...) LOGGER_DEBUG(LOG_MODULE, fmt, ##__VA_ARGS__)
#else
#define PROC_DEBUG_PRINTF(fmt, ...)
#endif

// Page directory constants are defined in paging.h

/**
 * @brief Copy kernel page directory entries to a new process page directory
 */
extern void copy_kernel_pde_entries(uint32_t *new_pd_virt);

// Functions below are unused - they exist in process.c
#if 0
/**
 * @brief Prepare the initial kernel stack for IRET to user mode
 */
static void prepare_initial_kernel_stack(pcb_t *proc) {
    PROC_DEBUG_PRINTF("Preparing initial kernel stack for process PID %u", proc->pid);
    KERNEL_ASSERT(proc != NULL, "prepare_initial_kernel_stack: NULL proc");
    KERNEL_ASSERT(proc->kernel_stack_vaddr_top != NULL, "prepare_initial_kernel_stack: Kernel stack top is NULL");
    KERNEL_ASSERT(proc->entry_point != 0, "prepare_initial_kernel_stack: Entry point is zero");
    KERNEL_ASSERT(proc->user_stack_top != NULL, "prepare_initial_kernel_stack: User stack top is NULL");

    // Get the top address and move down to prepare for pushes
    uint32_t *kstack_ptr = (uint32_t*)proc->kernel_stack_vaddr_top;
    PROC_DEBUG_PRINTF("Initial kstack_ptr (top) = %p", kstack_ptr);

    // Stack grows down. Push in reverse order of how IRET expects them.
    // SS, ESP, EFLAGS, CS, EIP

    // 1. Push User SS (Stack Segment)
    kstack_ptr--;
    *kstack_ptr = GDT_USER_DATA_SELECTOR | 3; // OR with RPL 3
    PROC_DEBUG_PRINTF("Pushed SS = %#x at %p", *kstack_ptr, kstack_ptr);

    // 2. Push User ESP (Stack Pointer)
    kstack_ptr--;
    *kstack_ptr = (uint32_t)proc->user_stack_top;
    PROC_DEBUG_PRINTF("Pushed ESP = %#x at %p", *kstack_ptr, kstack_ptr);

    // 3. Push EFLAGS
    kstack_ptr--;
    *kstack_ptr = USER_EFLAGS_DEFAULT;
    PROC_DEBUG_PRINTF("Pushed EFLAGS = %#x at %p", *kstack_ptr, kstack_ptr);

    // 4. Push User CS (Code Segment)
    kstack_ptr--;
    *kstack_ptr = GDT_USER_CODE_SELECTOR | 3; // OR with RPL 3
    PROC_DEBUG_PRINTF("Pushed CS = %#x at %p", *kstack_ptr, kstack_ptr);

    // 5. Push User EIP (Instruction Pointer)
    kstack_ptr--;
    *kstack_ptr = proc->entry_point;
    PROC_DEBUG_PRINTF("Pushed EIP = %#x at %p", proc->entry_point, kstack_ptr);

    // 6. Save the final Kernel Stack Pointer
    proc->kernel_esp_for_switch = (uint32_t)kstack_ptr;

    LOGGER_INFO(LOG_MODULE, "Kernel stack prepared for IRET. Final K_ESP = %#x", proc->kernel_esp_for_switch);
}

/**
 * @brief Initialize a process page directory
 */
static error_t initialize_process_page_directory(pcb_t *proc) {
    PROC_DEBUG_PRINTF("Initializing page directory for process PID %u", proc->pid);
    
    uintptr_t pd_phys = frame_alloc();
    if (!pd_phys) {
        LOGGER_ERROR(LOG_MODULE, "Failed to allocate page directory frame for PID %u", proc->pid);
        return E_NOMEM;
    }
    
    proc->page_directory_phys = (uint32_t*)pd_phys;
    LOGGER_DEBUG(LOG_MODULE, "Allocated PD Phys: %#lx for PID %u", pd_phys, proc->pid);

    // Initialize page directory
    void* proc_pd_virt_temp = paging_temp_map(pd_phys);
    if (!proc_pd_virt_temp) {
        LOGGER_ERROR(LOG_MODULE, "Failed to temp map page directory");
        put_frame(pd_phys);
        return E_IO;
    }

    // Clear and initialize the page directory
    memset(proc_pd_virt_temp, 0, PAGE_SIZE);
    copy_kernel_pde_entries((uint32_t*)proc_pd_virt_temp);
    
    // Set up recursive page directory entry
    uint32_t recursive_flags = PAGE_PRESENT | PAGE_RW | (g_nx_supported ? PAGE_NX_BIT : 0);
    ((uint32_t*)proc_pd_virt_temp)[RECURSIVE_PDE_INDEX] = (pd_phys & PAGING_ADDR_MASK) | recursive_flags;
    
    paging_temp_unmap(proc_pd_virt_temp);
    
    PROC_DEBUG_PRINTF("Page directory initialization complete");

    // Verify the page directory was set up correctly
    void* temp_pd_check = paging_temp_map(pd_phys);
    if (temp_pd_check) {
        uint32_t process_kernel_base_pde = ((uint32_t*)temp_pd_check)[KERNEL_PDE_INDEX];
        uint32_t global_kernel_base_pde = g_kernel_page_directory_virt[KERNEL_PDE_INDEX];
        
        LOGGER_DEBUG(LOG_MODULE, "Verification: Proc PD[%d]=%#08x, Global PD[%d]=%#08x",
                    KERNEL_PDE_INDEX, process_kernel_base_pde, KERNEL_PDE_INDEX, global_kernel_base_pde);
        
        if (!(process_kernel_base_pde & PAGE_PRESENT)) {
            LOGGER_ERROR(LOG_MODULE, "Kernel Base PDE missing in process PD!");
            paging_temp_unmap(temp_pd_check);
            return E_INVAL;
        }
        paging_temp_unmap(temp_pd_check);
    } else {
        LOGGER_ERROR(LOG_MODULE, "Verification failed: Could not temp map process PD (%#lx)", pd_phys);
        return E_IO;
    }

    return E_SUCCESS;
}

/**
 * @brief Setup standard VMAs (heap and stack) for a process
 */
static error_t setup_standard_vmas(pcb_t *proc) {
    PROC_DEBUG_PRINTF("Setting up standard VMAs for process PID %u", proc->pid);
    
    if (!proc->mm) {
        LOGGER_ERROR(LOG_MODULE, "Process mm_struct is NULL");
        return E_INVAL;
    }

    // Setup heap VMA (initially empty, starting at end_brk)
    uintptr_t heap_start = proc->mm->end_brk;
    KERNEL_ASSERT(heap_start < USER_STACK_BOTTOM_VIRT, "Heap start overlaps user stack area");
    
    uint32_t heap_page_prot = PAGE_PRESENT | PAGE_RW | PAGE_USER | (g_nx_supported ? PAGE_NX_BIT : 0);
    if (!insert_vma(proc->mm, heap_start, heap_start, 
                   VM_READ | VM_WRITE | VM_USER | VM_ANONYMOUS, heap_page_prot, NULL, 0)) {
        LOGGER_ERROR(LOG_MODULE, "Failed to insert heap VMA");
        return E_NOMEM;
    }
    
    LOGGER_DEBUG(LOG_MODULE, "Initial Heap VMA placeholder added: [%#lx - %#lx)",
                heap_start, heap_start);

    // Setup user stack VMA
    uint32_t stack_page_prot = PAGE_PRESENT | PAGE_RW | PAGE_USER | (g_nx_supported ? PAGE_NX_BIT : 0);
    if (!insert_vma(proc->mm, USER_STACK_BOTTOM_VIRT, USER_STACK_TOP_VIRT_ADDR,
                   VM_READ | VM_WRITE | VM_USER | VM_GROWS_DOWN | VM_ANONYMOUS, 
                   stack_page_prot, NULL, 0)) {
        LOGGER_ERROR(LOG_MODULE, "Failed to insert user stack VMA");
        return E_NOMEM;
    }
    
    LOGGER_DEBUG(LOG_MODULE, "User Stack VMA added: [%#lx - %#lx)",
                USER_STACK_BOTTOM_VIRT, USER_STACK_TOP_VIRT_ADDR);

    return E_SUCCESS;
}

/**
 * @brief Allocate and map initial user stack page
 */
static error_t setup_initial_user_stack(pcb_t *proc) {
    PROC_DEBUG_PRINTF("Setting up initial user stack for process PID %u", proc->pid);
    
    // Allocate physical frame for initial stack page
    uintptr_t initial_stack_phys_frame = frame_alloc();
    if (!initial_stack_phys_frame) {
        LOGGER_ERROR(LOG_MODULE, "Failed to allocate initial user stack frame");
        return E_NOMEM;
    }

    // Map the stack page
    uintptr_t initial_user_stack_page_vaddr = USER_STACK_TOP_VIRT_ADDR - PAGE_SIZE;
    uint32_t stack_page_prot = PAGE_PRESENT | PAGE_RW | PAGE_USER | (g_nx_supported ? PAGE_NX_BIT : 0);
    
    int map_res = paging_map_single_4k(proc->page_directory_phys, 
                                      initial_user_stack_page_vaddr, 
                                      initial_stack_phys_frame, 
                                      stack_page_prot);
    if (map_res != 0) {
        LOGGER_ERROR(LOG_MODULE, "Failed to map initial user stack page");
        put_frame(initial_stack_phys_frame);
        return E_IO;
    }

    // Set user stack top
    proc->user_stack_top = (void*)USER_STACK_TOP_VIRT_ADDR;
    
    LOGGER_INFO(LOG_MODULE, "Initial user stack page allocated (P=%#lx) and mapped (V=%p). User ESP set to %p",
               initial_stack_phys_frame, (void*)initial_user_stack_page_vaddr, proc->user_stack_top);

    // Zero out the stack page
    void* temp_stack_map = paging_temp_map(initial_stack_phys_frame);
    if (temp_stack_map) {
        memset(temp_stack_map, 0, PAGE_SIZE);
        paging_temp_unmap(temp_stack_map);
    } else {
        LOGGER_WARN(LOG_MODULE, "Could not zero initial user stack page");
    }

    return E_SUCCESS;
}

/**
 * @brief Verify EIP and ESP mappings are correct
 */
static error_t verify_process_mappings(pcb_t *proc) {
    PROC_DEBUG_PRINTF("Verifying EIP and ESP mappings for process PID %u", proc->pid);
    
    bool mapping_error = false;

    // Verify EIP page
    uintptr_t eip_vaddr = proc->entry_point;
    uintptr_t eip_page_vaddr = PAGE_ALIGN_DOWN(eip_vaddr);
    uintptr_t eip_phys = 0;
    uint32_t eip_pte_flags = 0;
    
    int eip_check_res = paging_get_physical_address_and_flags(proc->page_directory_phys, 
                                                             eip_page_vaddr, &eip_phys, &eip_pte_flags);
    if (eip_check_res != 0 || eip_phys == 0) {
        LOGGER_ERROR(LOG_MODULE, "EIP page %#lx not mapped for PID %u", eip_page_vaddr, proc->pid);
        mapping_error = true;
    } else {
        bool flags_ok = (eip_pte_flags & PAGE_PRESENT) && (eip_pte_flags & PAGE_USER);
        if (!flags_ok) {
            LOGGER_ERROR(LOG_MODULE, "EIP page %#lx has incorrect flags %#x for PID %u", 
                        eip_page_vaddr, eip_pte_flags, proc->pid);
            mapping_error = true;
        }
    }

    // Verify ESP page
    uintptr_t esp_page_vaddr_check = USER_STACK_TOP_VIRT_ADDR - PAGE_SIZE;
    uintptr_t esp_phys = 0;
    uint32_t esp_pte_flags = 0;
    
    int esp_check_res = paging_get_physical_address_and_flags(proc->page_directory_phys, 
                                                             esp_page_vaddr_check, &esp_phys, &esp_pte_flags);
    if (esp_check_res != 0 || esp_phys == 0) {
        LOGGER_ERROR(LOG_MODULE, "ESP page %#lx not mapped for PID %u", esp_page_vaddr_check, proc->pid);
        mapping_error = true;
    } else {
        bool flags_ok = (esp_pte_flags & PAGE_PRESENT) && (esp_pte_flags & PAGE_USER) && (esp_pte_flags & PAGE_RW);
        if (!flags_ok) {
            LOGGER_ERROR(LOG_MODULE, "ESP page %#lx has incorrect flags %#x for PID %u", 
                        esp_page_vaddr_check, esp_pte_flags, proc->pid);
            mapping_error = true;
        }
    }

    if (mapping_error) {
        LOGGER_ERROR(LOG_MODULE, "Process mapping verification failed for PID %u", proc->pid);
        return E_FAULT;
    }

    PROC_DEBUG_PRINTF("User EIP and ESP mapping & flags verification passed");
    return E_SUCCESS;
}
#endif // 0 - End of unused functions

// create_user_process is defined in process.c

/**
 * @brief Initialize process loading subsystem
 */
error_t process_loader_init(void) {
    LOGGER_INFO(LOG_MODULE, "Process loader initialized");
    return E_SUCCESS;
}

/**
 * @brief Cleanup process loading subsystem
 */
void process_loader_cleanup(void) {
    LOGGER_INFO(LOG_MODULE, "Process loader cleaned up");
}