/**
 * @file process_memory.c
 * @brief Process memory management including kernel stack allocation
 * 
 * Handles kernel stack allocation, deallocation, and memory setup for processes.
 * Separated from PCB management to follow Single Responsibility Principle.
 */

#include <kernel/process/process.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/frame.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>
#include <kernel/cpu/tss.h>

// Constants for kernel stack management
#ifndef KERNEL_STACK_VIRT_START
#define KERNEL_STACK_VIRT_START 0xE0000000
#endif
#ifndef KERNEL_STACK_VIRT_END
#define KERNEL_STACK_VIRT_END   0xF0000000
#endif

// Simple linear allocator for kernel stack virtual addresses
// WARNING: Placeholder only! Not suitable for production/SMP. Needs proper allocator.
static uintptr_t g_next_kernel_stack_virt_base = KERNEL_STACK_VIRT_START;
// TODO: Replace with a proper kernel virtual address space allocator (e.g., using VMAs)

// External globals
extern uint32_t g_kernel_page_directory_phys;
extern bool g_nx_supported;

/**
 * @brief Allocates physical frames and maps them into the kernel's address space
 * to serve as the kernel stack for a given process.
 * @param proc Pointer to the PCB to setup the kernel stack for.
 * @return true on success, false on failure.
 */
bool allocate_kernel_stack(pcb_t *proc)
{
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
    }
    proc->kernel_stack_phys_base = (uint32_t)phys_frames[0];
    // *** GUARD PAGE FIX: Update log message ***
    serial_printf("  Successfully allocated %lu physical frames (incl. guard) for kernel stack.\n", (unsigned long)allocated_count);

    // 2. Allocate Virtual Range (Allocate for num_pages_with_guard)
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
    // *** GUARD PAGE FIX: Loop over all allocated frames ***
    for (size_t i = 0; i < num_pages_with_guard; i++) {
        uintptr_t target_vaddr = kstack_virt_base + (i * PAGE_SIZE);
        uintptr_t phys_addr    = phys_frames[i];
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

    return true; // Success
}

/**
 * @brief Frees kernel stack resources for a process
 * @param proc Process whose kernel stack should be freed
 */
void free_kernel_stack(pcb_t *proc)
{
    if (!proc || proc->kernel_stack_vaddr_top == NULL) {
        return;
    }

    // kernel_stack_vaddr_top points to the end of the *usable* stack (e.g., 0xe0004000)
    uintptr_t stack_top_usable = (uintptr_t)proc->kernel_stack_vaddr_top;
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
                serial_printf("[Process] Warning: Kernel stack V=%p maps to P=0?\n", (void*)v_addr);
            }
        } else {
             serial_printf("[Process] Warning: Failed to get physical addr for kernel stack V=%p during cleanup.\n", (void*)v_addr);
        }
    }
    serial_write("  Kernel stack frames (incl. guard) freed.\n");

    // NOTE: We do NOT unmap kernel stack from the kernel page directory
    // Kernel stacks are in kernel space and the mappings are shared across all processes
    // Just free the physical frames above, don't unmap the virtual addresses
    serial_write("  Kernel stack physical frames freed (virtual mappings remain in kernel PD).\n");

    proc->kernel_stack_vaddr_top = NULL;
    proc->kernel_stack_phys_base = 0;
}

//============================================================================
// New Standardized Memory Management API Implementation
//============================================================================

error_t allocate_kernel_stack_safe(pcb_t *proc) {
    // Input validation
    if (!proc) {
        return E_INVAL;
    }

    // Check if process already has a kernel stack
    if (proc->kernel_stack_vaddr_top != NULL || proc->kernel_stack_phys_base != 0) {
        serial_printf("[Process] Process PID %u already has kernel stack allocated\n", proc->pid);
        return E_INVAL;
    }

    // Validate stack size configuration
    size_t usable_stack_size = PROCESS_KSTACK_SIZE;
    if (usable_stack_size == 0 || (usable_stack_size % PAGE_SIZE) != 0) {
        serial_printf("[Process] Invalid PROCESS_KSTACK_SIZE (%zu), must be page-aligned\n", 
                      usable_stack_size);
        return E_INVAL;
    }

    // Calculate page requirements
    size_t num_usable_pages = usable_stack_size / PAGE_SIZE;
    size_t num_pages_with_guard = num_usable_pages + 1; // Include guard page
    size_t total_alloc_size = num_pages_with_guard * PAGE_SIZE;

    serial_printf("  Allocating %zu pages + 1 guard page (%zu bytes total) for kernel stack...\n",
                  num_usable_pages, total_alloc_size);

    // Allocate array for physical frame addresses
    uintptr_t *phys_frames = kmalloc(num_pages_with_guard * sizeof(uintptr_t));
    if (!phys_frames) {
        serial_printf("[Process] Failed to allocate phys_frames array for PID %u\n", proc->pid);
        return E_NOMEM;
    }
    memset(phys_frames, 0, num_pages_with_guard * sizeof(uintptr_t));

    // Allocate physical frames
    size_t allocated_count = 0;
    for (allocated_count = 0; allocated_count < num_pages_with_guard; allocated_count++) {
        phys_frames[allocated_count] = frame_alloc();
        if (phys_frames[allocated_count] == 0) {
            serial_printf("[Process] Frame allocation failed at frame %zu for PID %u\n", 
                          allocated_count, proc->pid);
            
            // Clean up already allocated frames
            for (size_t j = 0; j < allocated_count; ++j) {
                put_frame(phys_frames[j]);
            }
            kfree(phys_frames);
            return E_NOMEM;
        }
    }

    // Allocate virtual address range
    uintptr_t kstack_virt_base = g_next_kernel_stack_virt_base;
    uintptr_t kstack_virt_end_with_guard = kstack_virt_base + total_alloc_size;
    
    // Check virtual address space exhaustion
    if (kstack_virt_base < KERNEL_STACK_VIRT_START || 
        kstack_virt_end_with_guard > KERNEL_STACK_VIRT_END || 
        kstack_virt_end_with_guard <= kstack_virt_base) {
        
        serial_printf("[Process] Kernel stack virtual address space exhausted [%#lx - %#lx)\n",
                      kstack_virt_base, kstack_virt_end_with_guard);
        
        // Clean up physical frames
        for (size_t i = 0; i < num_pages_with_guard; ++i) {
            put_frame(phys_frames[i]);
        }
        kfree(phys_frames);
        return E_NOSPC;
    }

    // Map physical frames to virtual range
    for (size_t i = 0; i < num_pages_with_guard; i++) {
        uintptr_t target_vaddr = kstack_virt_base + (i * PAGE_SIZE);
        uintptr_t phys_addr = phys_frames[i];
        
        // Check kernel page directory is available
        if (!g_kernel_page_directory_phys) {
            serial_printf("[Process] Kernel page directory not initialized\n");
            
            // Clean up
            for (size_t j = 0; j < num_pages_with_guard; ++j) {
                put_frame(phys_frames[j]);
            }
            kfree(phys_frames);
            return E_FAULT;
        }

        int map_res = paging_map_single_4k((uint32_t*)g_kernel_page_directory_phys, 
                                           target_vaddr, phys_addr, PTE_KERNEL_DATA_FLAGS);
        if (map_res != 0) {
            serial_printf("[Process] Failed to map kernel stack page %zu (V=%p -> P=%#lx), error %d\n",
                          i, (void*)target_vaddr, phys_addr, map_res);
            
            // Unmap already mapped pages and free all physical frames
            paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, 
                               kstack_virt_base, i * PAGE_SIZE);
            for (size_t j = 0; j < num_pages_with_guard; ++j) {
                put_frame(phys_frames[j]);
            }
            kfree(phys_frames);
            return E_FAULT;
        }
    }

    // Test kernel stack write (on usable range only)
    uintptr_t usable_stack_end = kstack_virt_base + usable_stack_size;
    volatile uint32_t *stack_bottom_ptr = (volatile uint32_t *)kstack_virt_base;
    volatile uint32_t *stack_top_word_ptr = (volatile uint32_t *)(usable_stack_end - sizeof(uint32_t));
    uint32_t test_value = 0xDEADBEEF;

    *stack_bottom_ptr = test_value;
    uint32_t read_back1 = *stack_bottom_ptr;
    
    *stack_top_word_ptr = test_value;
    uint32_t read_back2 = *stack_top_word_ptr;

    if (read_back1 != test_value || read_back2 != test_value) {
        serial_printf("[Process] Kernel stack write test failed! Read back %#x and %#x (expected %#x)\n",
                      read_back1, read_back2, test_value);
        
        // Clean up failed stack
        paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, 
                           kstack_virt_base, total_alloc_size);
        for (size_t i = 0; i < num_pages_with_guard; ++i) {
            put_frame(phys_frames[i]);
        }
        kfree(phys_frames);
        return E_FAULT;
    }

    // Success - update process state
    g_next_kernel_stack_virt_base = kstack_virt_end_with_guard;
    proc->kernel_stack_phys_base = (uint32_t)phys_frames[0];
    proc->kernel_stack_vaddr_top = (uint32_t*)usable_stack_end;
    
    kfree(phys_frames);

    // Update TSS ESP0
    tss_set_kernel_stack((uint32_t)proc->kernel_stack_vaddr_top);

    serial_printf("[Process] Kernel stack allocated successfully for PID %u: PhysBase=%#x, VirtTop=%p\n",
                  proc->pid, proc->kernel_stack_phys_base, (void*)proc->kernel_stack_vaddr_top);

    return E_SUCCESS;
}