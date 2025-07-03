/**
 * @file scheduler_context.c
 * @brief Context Switching and CPU State Management
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Handles low-level context switching between tasks, idle mode
 * management, and CPU state transitions. Focuses purely on the mechanics
 * of context switching without scheduling decisions.
 */

//============================================================================
// Includes
//============================================================================
#include <kernel/process/scheduler_context.h>
#include <kernel/cpu/tss.h>
#include <kernel/cpu/gdt.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/lib/assert.h>
#include <kernel/lib/string.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

//============================================================================
// External Assembly Functions
//============================================================================
extern void simple_switch(context_t *old_esp, context_t new_esp);
extern void jump_to_user_mode(uint32_t kernel_esp);
extern uint32_t* setup_idle_context(uint32_t* stack_top, void (*idle_func)(void));

//============================================================================
// Context Switching Configuration
//============================================================================
#define IDLE_TASK_PID 0

// Logging Macros
#define SCHED_INFO(fmt, ...)  serial_printf("[Context INFO ] " fmt "\n", ##__VA_ARGS__)
#define SCHED_DEBUG(fmt, ...) serial_printf("[Context DEBUG] " fmt "\n", ##__VA_ARGS__)
#define SCHED_ERROR(fmt, ...) serial_printf("[Context ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

//============================================================================
// Idle Task Management
//============================================================================
static tcb_t g_idle_task_tcb;
static pcb_t g_idle_task_pcb;

static __attribute__((noreturn)) void kernel_idle_task_loop(void) {
    SCHED_INFO("Idle task started (PID %lu). Entering HLT loop.", (unsigned long)IDLE_TASK_PID);
    
    // Ensure segment registers are properly set
    asm volatile(
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        : : : "ax"
    );

    uint32_t loop_count = 0;
    while (1) {
        loop_count++;
        
        // Ensure segments remain valid
        asm volatile(
            "mov $0x10, %%ax\n"
            "mov %%ax, %%ds\n"
            "mov %%ax, %%es\n"
            "mov %%ax, %%fs\n"
            "mov %%ax, %%gs\n"
            : : : "ax"
        );
        
        // Periodically check and fix segments
        if ((loop_count & 0x3F) == 0) { // Every 64 iterations
            uint32_t ds_check;
            asm volatile("mov %%ds, %0" : "=r"(ds_check));
            if ((ds_check & 0xFFFF) != 0x10) {
                serial_printf("[Idle ERROR] DS corrupted! DS=0x%x at loop %u\n", 
                             ds_check & 0xFFFF, loop_count);
                // Fix it
                asm volatile(
                    "push %%eax\n"
                    "mov $0x10, %%ax\n"
                    "mov %%ax, %%ds\n"
                    "mov %%ax, %%es\n"
                    "pop %%eax\n"
                    : : : "memory"
                );
            }
        }
        
        // Memory barrier
        asm volatile("mfence" ::: "memory");

        // Enable interrupts and halt
        asm volatile ("sti; hlt");
    }
}

void scheduler_context_init_idle_task(void) {
    SCHED_DEBUG("Initializing idle task...");
    
    // Initialize idle PCB
    memset(&g_idle_task_pcb, 0, sizeof(pcb_t));
    g_idle_task_pcb.pid = IDLE_TASK_PID;
    g_idle_task_pcb.page_directory_phys = (uint32_t*)g_kernel_page_directory_phys;
    KERNEL_ASSERT(g_idle_task_pcb.page_directory_phys != NULL, "Kernel PD phys NULL during idle init");
    g_idle_task_pcb.entry_point = (uintptr_t)kernel_idle_task_loop;

    // Allocate idle task stack from kernel heap
    size_t alloc_size = PROCESS_KSTACK_SIZE + 256; // Extra space for alignment and guards
    void *idle_stack_buffer = kmalloc(alloc_size);
    if (!idle_stack_buffer) {
        KERNEL_PANIC_HALT("Failed to allocate idle task stack!");
    }
    
    // Clear the entire allocation
    memset(idle_stack_buffer, 0, alloc_size);
    
    // Align to 16-byte boundary with some padding
    uintptr_t idle_stack_base = ((uintptr_t)idle_stack_buffer + 63) & ~15;
    uintptr_t idle_stack_top = idle_stack_base + PROCESS_KSTACK_SIZE;
    
    // Add guard pattern at the base of the stack
    uint32_t *guard_base = (uint32_t*)idle_stack_base;
    for (int i = 0; i < 16; i++) {
        guard_base[i] = 0xDEADBEEF;
    }
    
    // Zero the usable stack region (skip guard area)
    serial_printf("[Context DEBUG] Zeroing idle task stack region: V=[%p - %p)\n",
                  (void*)(idle_stack_base + 64), (void*)idle_stack_top);
    memset((void*)(idle_stack_base + 64), 0, PROCESS_KSTACK_SIZE - 64);
    
    uintptr_t stack_top_virt_addr = idle_stack_top;
    g_idle_task_pcb.kernel_stack_vaddr_top = (uint32_t*)stack_top_virt_addr;

    // Log the allocated stack location
    serial_printf("[Context DEBUG] Idle stack allocated at virt %p-%p\n", 
                  (void*)idle_stack_base, (void*)idle_stack_top);

    // Initialize idle TCB
    memset(&g_idle_task_tcb, 0, sizeof(tcb_t));
    g_idle_task_tcb.process = &g_idle_task_pcb;
    g_idle_task_tcb.pid = IDLE_TASK_PID;
    g_idle_task_tcb.state = TASK_READY;
    g_idle_task_tcb.in_run_queue = false;
    g_idle_task_tcb.has_run = true;  // Mark as already run
    
    // Initialize priority for idle task
    g_idle_task_tcb.base_priority = 3; // Lowest priority
    g_idle_task_tcb.effective_priority = 3;
    g_idle_task_tcb.blocking_task = NULL;
    g_idle_task_tcb.blocked_tasks_head = NULL;
    g_idle_task_tcb.blocked_tasks_next = NULL;
    g_idle_task_tcb.priority = 3;

    // Use assembly helper to create the stack layout
    uint32_t *stack_top = (uint32_t*)stack_top_virt_addr;
    uint32_t *context_ptr = setup_idle_context(stack_top, kernel_idle_task_loop);
    
    g_idle_task_tcb.context = context_ptr;
    
    SCHED_DEBUG("Idle task context initialized with stack at %p, EIP=%p", 
                (void*)g_idle_task_tcb.context, (void*)kernel_idle_task_loop);
}

//============================================================================
// Context Switching Implementation
//============================================================================

void scheduler_context_switch(tcb_t *old_task, tcb_t *new_task) {
    KERNEL_ASSERT(new_task && new_task->process && new_task->process->page_directory_phys, 
                  "Invalid new task");
    
    SCHED_DEBUG("Context switch: PID %lu -> PID %lu",
                old_task ? old_task->pid : (uint32_t)-1, new_task->pid);
    
    // Set up kernel stack for new task
    uintptr_t new_kernel_stack_top_vaddr = (new_task->pid == IDLE_TASK_PID)
        ? (uintptr_t)g_idle_task_pcb.kernel_stack_vaddr_top
        : (uintptr_t)new_task->process->kernel_stack_vaddr_top;
    tss_set_kernel_stack((uint32_t)new_kernel_stack_top_vaddr);
    
    // Check if page directory needs switching
    bool pd_needs_switch = (!old_task || !old_task->process || 
                           old_task->process->page_directory_phys != new_task->process->page_directory_phys);

    // Handle first-time task execution
    if (!new_task->has_run && new_task->pid != IDLE_TASK_PID) {
        new_task->has_run = true;
        
        if (new_task->process->is_kernel_task) {
            SCHED_DEBUG("First run for kernel task PID %lu", new_task->pid);
            
            // Switch page directory if needed
            if (pd_needs_switch) {
                asm volatile("mov %0, %%cr3" : : "r" (new_task->process->page_directory_phys) : "memory");
            }
            
            // For kernel tasks, start directly in kernel mode
            void (*kernel_task_entry)(void) = (void (*)(void))new_task->process->entry_point;
            kernel_task_entry();
            
            KERNEL_PANIC_HALT("Kernel task returned unexpectedly!");
        } else {
            SCHED_DEBUG("First run for user process PID %lu", new_task->pid);
            
            // Switch page directory if needed
            if (pd_needs_switch) {
                asm volatile("mov %0, %%cr3" : : "r" (new_task->process->page_directory_phys) : "memory");
            }
            
            // For user processes, use IRET to transition to user mode
            jump_to_user_mode((uint32_t)new_task->context);
        }
    } else {
        // Mark idle task as having run
        if (!new_task->has_run && new_task->pid == IDLE_TASK_PID) {
            new_task->has_run = true;
        }
        
        SCHED_DEBUG("Context switch between existing tasks");
        
        // Handle page directory switching if needed
        if (pd_needs_switch) {
            asm volatile("mov %0, %%cr3" : : "r" (new_task->process->page_directory_phys) : "memory");
        }
        
        // Idle task should never reach here due to checks in core scheduler
        if (new_task->pid == IDLE_TASK_PID) {
            KERNEL_PANIC_HALT("CRITICAL BUG: Idle task reached context switch code!");
        }
        
        // Normal context switch for user/kernel tasks
        simple_switch(old_task ? &(old_task->context) : NULL, new_task->context);
    }
}

void scheduler_context_enter_idle_mode(void) {
    SCHED_INFO("No runnable tasks. Entering HLT-based idle mode directly.");
    
    // Call idle function directly - this is how modern OSes handle idle
    // This avoids the problematic context switch to idle task
    kernel_idle_task_loop(); // This never returns
    
    // Should never reach here
    KERNEL_PANIC_HALT("Idle loop returned unexpectedly!");
}

//============================================================================
// Idle Task Access
//============================================================================
tcb_t* scheduler_context_get_idle_task(void) {
    return &g_idle_task_tcb;
}

void scheduler_context_check_idle_integrity(const char *checkpoint) {
    // Simple integrity check - verify idle task exists and is in valid state
    if (g_idle_task_tcb.state != TASK_ZOMBIE) {
        SCHED_DEBUG("Idle task integrity check passed for %s", checkpoint);
    }
}