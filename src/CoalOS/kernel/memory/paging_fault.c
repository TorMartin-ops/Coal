/**
 * @file paging_fault.c
 * @brief Page fault handler implementation
 * 
 * Handles page faults for demand paging, copy-on-write, and memory protection violations.
 */

#define LOG_MODULE "page_fault"

#include <kernel/memory/paging_fault.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/mm.h>
#include <kernel/process/process.h>
#include <kernel/process/scheduler.h>
#include <kernel/interfaces/logger.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/lib/assert.h>

// Page fault statistics
static struct {
    uint64_t total_faults;
    uint64_t handled_faults;
    uint64_t fatal_faults;
} g_pf_stats = {0};

/**
 * @brief Page fault handler
 */
void page_fault_handler(registers_t* regs) {
    // Get the faulting address from CR2
    uintptr_t fault_addr;
    asm volatile("mov %%cr2, %0" : "=r"(fault_addr));
    
    // Get error code details
    uint32_t error_code = regs->err_code;
    bool is_present = (error_code & PAGE_FAULT_PRESENT) != 0;
    bool is_write = (error_code & PAGE_FAULT_WRITE) != 0;
    bool is_user = (error_code & PAGE_FAULT_USER) != 0;
    bool is_reserved = (error_code & PAGE_FAULT_RESERVED) != 0;
    bool is_fetch = (error_code & PAGE_FAULT_FETCH) != 0;
    
    g_pf_stats.total_faults++;
    
    // Log the fault details
    LOGGER_DEBUG(LOG_MODULE, "Page fault at %p (EIP=%p, error=%#x)",
                 (void*)fault_addr, (void*)regs->eip, error_code);
    LOGGER_DEBUG(LOG_MODULE, "  - %s, %s, %s mode%s%s",
                 is_present ? "protection violation" : "not present",
                 is_write ? "write" : "read",
                 is_user ? "user" : "kernel",
                 is_reserved ? ", reserved bit" : "",
                 is_fetch ? ", instruction fetch" : "");
    
    // Get current process
    tcb_t* current_task = get_current_task();
    if (!current_task || !current_task->process) {
        LOGGER_ERROR(LOG_MODULE, "Page fault with no current process!");
        goto fatal;
    }
    
    pcb_t* process = current_task->process;
    
    // Check if this is a kernel mode fault
    if (!is_user && fault_addr >= KERNEL_SPACE_VIRT_START) {
        LOGGER_ERROR(LOG_MODULE, "Kernel page fault at %p", (void*)fault_addr);
        goto fatal;
    }
    
    // Find the VMA for this address
    vma_struct_t *vma = find_vma(process->mm, fault_addr);
    if (!vma) {
        // Check if this might be a stack growth situation
        // Look for a VMA just above the fault address with VM_GROWS_DOWN flag
        uintptr_t page_aligned_fault = fault_addr & ~(PAGE_SIZE - 1);
        
        // Try to find a VMA that starts within one page above the fault
        vma = find_vma(process->mm, page_aligned_fault + PAGE_SIZE);
        
        if (vma && (vma->vm_flags & VM_GROWS_DOWN)) {
            // Check if the fault is within reasonable stack growth distance
            // (typically within a few pages of the current stack bottom)
            if (fault_addr >= (vma->vm_start - (16 * PAGE_SIZE))) {
                LOGGER_DEBUG(LOG_MODULE, "Stack growth detected: extending VMA down to %p", 
                            (void*)page_aligned_fault);
                
                // Extend the VMA downward
                vma->vm_start = page_aligned_fault;
                
                // Fall through to normal fault handling which will allocate the page
            } else {
                LOGGER_ERROR(LOG_MODULE, "Stack growth too large: fault at %p, stack at %p",
                            (void*)fault_addr, (void*)vma->vm_start);
                goto fatal;
            }
        } else {
            LOGGER_ERROR(LOG_MODULE, "No VMA found for address %p", (void*)fault_addr);
            goto fatal;
        }
    }
    
    // Try to handle the fault through the memory manager
    int result = handle_vma_fault(process->mm, vma, fault_addr, error_code);
    
    if (result == 0) {
        // Successfully handled
        g_pf_stats.handled_faults++;
        LOGGER_DEBUG(LOG_MODULE, "Page fault handled successfully");
        return;
    }
    
    // Check specific error cases
    if (result == -1) {
        LOGGER_ERROR(LOG_MODULE, "No VMA found for address %p", (void*)fault_addr);
    } else if (result == -2) {
        LOGGER_ERROR(LOG_MODULE, "Access violation: %s to %s page at %p",
                     is_write ? "write" : "read",
                     is_present ? "read-only" : "unmapped",
                     (void*)fault_addr);
    } else if (result == -3) {
        LOGGER_ERROR(LOG_MODULE, "Out of memory while handling page fault");
    } else {
        LOGGER_ERROR(LOG_MODULE, "Unknown error handling page fault: %d", result);
    }
    
fatal:
    g_pf_stats.fatal_faults++;
    
    // Print detailed fault information
    terminal_printf("\n=== FATAL PAGE FAULT ===\n");
    terminal_printf("Fault Address: %p\n", (void*)fault_addr);
    terminal_printf("Instruction Pointer: %p\n", (void*)regs->eip);
    terminal_printf("Error Code: %#x\n", error_code);
    terminal_printf("  - %s\n", is_present ? "Protection Violation" : "Page Not Present");
    terminal_printf("  - %s Access\n", is_write ? "Write" : "Read");
    terminal_printf("  - %s Mode\n", is_user ? "User" : "Kernel");
    if (is_reserved) terminal_printf("  - Reserved Bit Set\n");
    if (is_fetch) terminal_printf("  - Instruction Fetch\n");
    
    if (current_task && current_task->process) {
        terminal_printf("Process: PID %u\n", current_task->process->pid);
    }
    
    // Also log to serial
    serial_printf("\n=== FATAL PAGE FAULT ===\n");
    serial_printf("Fault Address: %p\n", (void*)fault_addr);
    serial_printf("EIP: %p, Error: %#x\n", (void*)regs->eip, error_code);
    
    // Dump register state
    terminal_printf("\nRegisters:\n");
    terminal_printf("EAX=%08x EBX=%08x ECX=%08x EDX=%08x\n",
                    regs->eax, regs->ebx, regs->ecx, regs->edx);
    terminal_printf("ESI=%08x EDI=%08x EBP=%08x ESP=%08x\n",
                    regs->esi, regs->edi, regs->ebp, regs->useresp);
    terminal_printf("CS=%04x DS=%04x ES=%04x FS=%04x GS=%04x SS=%04x\n",
                    regs->cs, regs->ds, regs->es, regs->fs, regs->gs, regs->ss);
    terminal_printf("EFLAGS=%08x\n", regs->eflags);
    
    // Halt the system
    KERNEL_PANIC_HALT("Fatal page fault");
}

/**
 * @brief Get page fault statistics
 */
void page_fault_get_stats(uint64_t* total_faults, 
                         uint64_t* handled_faults,
                         uint64_t* fatal_faults) {
    if (total_faults) *total_faults = g_pf_stats.total_faults;
    if (handled_faults) *handled_faults = g_pf_stats.handled_faults;
    if (fatal_faults) *fatal_faults = g_pf_stats.fatal_faults;
}