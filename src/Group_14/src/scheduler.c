#include "scheduler.h"
#include "process.h"    // For pcb_t, destroy_process
#include "kmalloc.h"    // For TCB allocation
#include "terminal.h"   // For logging
#include "string.h"     // For memset
#include "types.h"      // Common types
// GDT selectors should be defined, e.g., in gdt.h or a config header
#ifndef USER_CODE_SELECTOR
#define USER_CODE_SELECTOR   0x1B
#endif
#ifndef USER_DATA_SELECTOR
#define USER_DATA_SELECTOR   0x23
#endif

// Static variables for task list, current task, and stats
static tcb_t *task_list = NULL;
static tcb_t *current_task = NULL;
static uint32_t task_count = 0;
static uint32_t context_switches = 0;

// External assembly function for context switching
// Takes pointers to save old ESP, load new ESP, and optionally load new page directory
extern void context_switch(uint32_t **old_esp_ptr, uint32_t *new_esp, uint32_t *new_page_directory_phys);


void scheduler_init(void) {
    task_list = NULL;
    current_task = NULL;
    task_count = 0;
    context_switches = 0;
    terminal_write("[Scheduler] Initialized.\n");
}

int scheduler_add_task(pcb_t *pcb) {
    // Corrected: Check pcb->page_directory_phys instead of page_directory
    if (!pcb || !pcb->page_directory_phys || !pcb->kernel_stack_vaddr_top || !pcb->user_stack_top || !pcb->entry_point) {
        terminal_write("[Scheduler] Error: Invalid PCB provided to scheduler_add_task (check page_directory_phys, stack pointers, entry point).\n");
        return -1; // Indicate error
    }

    tcb_t *new_task = (tcb_t *)kmalloc(sizeof(tcb_t));
    if (!new_task) {
        terminal_write("[Scheduler] Error: Out of memory for TCB.\n");
        return -2; // Out of memory error
    }
    memset(new_task, 0, sizeof(tcb_t));
    new_task->process = pcb; // Link TCB to its process PCB

    // --- Set up initial kernel stack frame using VIRTUAL kernel stack pointer ---
    uint32_t *kstack_vtop = pcb->kernel_stack_vaddr_top; // Use the VIRTUAL top address

    // Stack frame for iret (bottom to top on stack):
    // 1. User SS (Data Segment Selector) - *** FIX HERE ***
    *(--kstack_vtop) = 0x23; // USER_DATA_SELECTOR (GDT Index 4 + RPL 3)

    // 2. User ESP (Initial User Stack Pointer)
    *(--kstack_vtop) = (uint32_t)pcb->user_stack_top;

    // 3. EFLAGS (Enable interrupts IF=1, IOPL=0 for user)
    *(--kstack_vtop) = 0x00000202; // Ensure IF (bit 9) is 1, IOPL (bits 12-13) is 0

    // 4. User CS (Code Segment Selector) - *** FIX HERE ***
    *(--kstack_vtop) = 0x1B; // USER_CODE_SELECTOR (GDT Index 3 + RPL 3)

    // 5. User EIP (Entry Point of the process)
    *(--kstack_vtop) = pcb->entry_point;

    // ... (rest of the stack setup for context_switch: pushad, segments, etc.) ...

    // Stack frame for context_switch restoration (popad, segments)
    // 6. General Purpose Registers (pushad order: EDI, ESI, EBP, ESP_ignore, EBX, EDX, ECX, EAX) - Initialize to 0
    *(--kstack_vtop) = 0; // EDI
    *(--kstack_vtop) = 0; // ESI
    *(--kstack_vtop) = 0; // EBP
    *(--kstack_vtop) = 0; // ESP_ignore (value ignored by popad)
    *(--kstack_vtop) = 0; // EBX
    *(--kstack_vtop) = 0; // EDX
    *(--kstack_vtop) = 0; // ECX
    *(--kstack_vtop) = 0; // EAX

    // 7. Segment Registers (order matching context_switch pushes: GS, FS, ES, DS) - Initialize to user data segment
    *(--kstack_vtop) = 0x23; // GS -> User Data
    *(--kstack_vtop) = 0x23; // FS -> User Data
    *(--kstack_vtop) = 0x23; // ES -> User Data
    *(--kstack_vtop) = 0x23; // DS -> User Data

    // 8. EFLAGS (pushed by pushfd in context_switch) - initialize state
    *(--kstack_vtop) = 0x00000202; // Initial flags for kernel mode before switch

    // Store the final virtual stack pointer in the TCB. This is where context_switch will load ESP from.
    new_task->esp = kstack_vtop;

    // --- Insert into scheduler's circular linked list ---
     if (task_list == NULL) {
        // First task
        task_list = new_task;
        current_task = new_task; // Set current_task for the first time
        new_task->next = new_task; // Point to itself
    } else {
        // Add to the end of the list
        tcb_t *temp = task_list;
        while (temp->next != task_list) {
            temp = temp->next;
        }
        temp->next = new_task;
        new_task->next = task_list; // Point back to the head
    }
    task_count++;

    terminal_printf("[Scheduler] Added task for PID %d (Kernel ESP starts at virt 0x%x).\n", pcb->pid, (uintptr_t)new_task->esp);
    return 0; // Success
}

void yield(void) {
    // Simple round-robin: just call schedule
    schedule();
}

void schedule(void) {
     context_switches++; // Increment context switch counter

     // No scheduling needed if no tasks or only one task
     if (!current_task || !current_task->next || current_task->next == current_task) {
        return;
    }

    tcb_t *old_task = current_task;
    tcb_t *new_task = current_task->next;

    current_task = new_task; // Update current task pointer *before* the switch

    uint32_t *new_page_directory_phys_addr = NULL;
    // Only switch CR3 if the process context (PCB address space) is different
    if (old_task->process != new_task->process) {
        // Corrected: Use page_directory_phys
        new_page_directory_phys_addr = new_task->process->page_directory_phys;
        // terminal_printf("[Scheduler] Switching page directory to phys 0x%x for PID %d\n",
        //                (uintptr_t)new_page_directory_phys_addr, new_task->process->pid);
    } else {
        // Same process, no need to reload CR3. Pass NULL to context_switch.
        // terminal_printf("[Scheduler] Same process context (PID %d), not switching CR3.\n", new_task->process->pid);
    }

    // Perform the context switch
    // Pass address of old task's ESP field, the new task's ESP value, and the physical address of the new page directory (or NULL)
    context_switch(&(old_task->esp), new_task->esp, new_page_directory_phys_addr);

    // --- Code resumes here when this task (old_task) is switched back to ---
}

tcb_t *get_current_task(void) {
     // Returns pointer to the currently executing task's TCB
     // Note: This might be called from an ISR where current_task isn't fully updated yet.
     // Needs careful handling in concurrent environments. For simple round-robin, it's usually safe.
     return current_task;
}


// Removes the current task and schedules the next one
void remove_current_task_with_code(uint32_t code) {
    // Disable interrupts for safety while modifying scheduler structures
     asm volatile("cli");

      if (!task_list || !current_task) {
        terminal_write("[Scheduler] Error: No task to remove.\n");
        asm volatile("sti");
        return;
    }

    tcb_t *to_remove = current_task;
    pcb_t *proc_to_remove = to_remove->process; // Get PCB associated with the TCB
    terminal_printf("[Scheduler] Removing task for PID %d (exit code %d).\n", proc_to_remove ? proc_to_remove->pid : 0, code);

    // --- Remove TCB from scheduler list ---
    if (to_remove->next == to_remove) { // Is it the last task?
        task_list = NULL;
        current_task = NULL;
        task_count = 0;
        terminal_write("[Scheduler] Last task removed. System idle or halting...\n");
        // Free resources for the last task
        if (proc_to_remove) {
             destroy_process(proc_to_remove); // Free PCB, page dir, kernel stack
        }
        kfree(to_remove); // Free TCB
        // What happens now? Halt or jump to an idle task?
        // For now, halt the system after enabling interrupts.
        terminal_write("!!! System Halted !!!\n");
        asm volatile("sti");
        while (1) { __asm__ volatile("hlt"); } // Halt

    } else {
        // Find the previous task in the circular list
        tcb_t *prev = task_list;
        while (prev->next != to_remove) {
            prev = prev->next;
             if (prev == task_list && prev->next != to_remove) { // Safety check: avoid infinite loop if to_remove not found
                 terminal_write("[Scheduler] Error: Failed to find previous task during removal.\n");
                 asm volatile("sti");
                 return;
             }
        }
        // Unlink 'to_remove'
        prev->next = to_remove->next;
        if (task_list == to_remove) { // Update head if removing the head
            task_list = to_remove->next;
        }
        task_count--;

        // Select the next task to run *before* freeing resources
        current_task = to_remove->next;

        // --- Free Resources ---
        if (proc_to_remove) {
             destroy_process(proc_to_remove); // Free PCB, page dir, kernel stack
        }
        kfree(to_remove); // Free TCB

        // --- Switch to the next task ---
        // We need to switch context, but we don't have an 'old_esp_ptr' to save to,
        // as the current stack frame belongs to the task being destroyed.
        // We directly jump into the next task's context.
        terminal_printf("[Scheduler] Switching to next task (PID %d).\n", current_task->process ? current_task->process->pid : 0);
        uint32_t *dummy_esp_save_location = NULL; // No need to save ESP of the dying task
        // Corrected: Use page_directory_phys
        uint32_t *next_pd_phys = current_task->process ? current_task->process->page_directory_phys : NULL;
        // Directly switch: This assumes context_switch can handle a NULL old_esp_ptr or we modify it.
        // A safer approach might involve switching to a temporary kernel stack first.
        // For simplicity here, we call context_switch, acknowledging the first argument isn't used meaningfully.
        context_switch(&dummy_esp_save_location, current_task->esp, next_pd_phys);

        // This part should *never* be reached if context_switch works correctly
         terminal_write("[Scheduler] Error: Returned after context switch in remove_current_task!\n");
          while(1) { asm volatile("hlt"); }
    }
}

void debug_scheduler_stats(uint32_t *out_task_count, uint32_t *out_switches) {
     // Provide scheduler statistics if pointers are valid
     if (out_task_count) { *out_task_count = task_count; }
     if (out_switches) { *out_switches = context_switches; }
}