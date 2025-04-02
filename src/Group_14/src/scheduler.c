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

// ... (task_list, current_task, stats, context_switch extern remain same) ...
static tcb_t *task_list = NULL;
static tcb_t *current_task = NULL;
static uint32_t task_count = 0;
static uint32_t context_switches = 0;
extern void context_switch(uint32_t **old_esp_ptr, uint32_t *new_esp, uint32_t *new_page_directory);


void scheduler_init(void) {
    // ... (same as before) ...
    task_list = NULL;
    current_task = NULL;
    task_count = 0;
    context_switches = 0;
    terminal_write("[Scheduler] Initialized.\n");
}

int scheduler_add_task(pcb_t *pcb) {
    if (!pcb || !pcb->page_directory || !pcb->kernel_stack_vaddr_top || !pcb->user_stack_top || !pcb->entry_point) {
        terminal_write("[Scheduler] Error: Invalid PCB provided to scheduler_add_task (check stack pointers).\n");
        return -1;
    }

    tcb_t *new_task = (tcb_t *)kmalloc(sizeof(tcb_t));
    if (!new_task) { /* ... error handling ... */ terminal_write("[Scheduler] Error: Out of memory for TCB.\n"); return -2; }
    memset(new_task, 0, sizeof(tcb_t));
    new_task->process = pcb;

    // --- Set up initial kernel stack frame using VIRTUAL kernel stack pointer ---
    uint32_t *kstack_vtop = pcb->kernel_stack_vaddr_top; // Use the VIRTUAL top address

    // 1. User SS
    *(--kstack_vtop) = USER_DATA_SELECTOR;
    // 2. User ESP
    *(--kstack_vtop) = (uint32_t)pcb->user_stack_top;
    // 3. EFLAGS (IF=1, IOPL=0)
    *(--kstack_vtop) = 0x00000202;
    // 4. User CS
    *(--kstack_vtop) = USER_CODE_SELECTOR;
    // 5. User EIP (Entry Point)
    *(--kstack_vtop) = pcb->entry_point;

    // 6. General Purpose Registers (pushad order: EDI, ESI, EBP, ESP_ignore, EBX, EDX, ECX, EAX)
    *(--kstack_vtop) = 0; // EDI
    *(--kstack_vtop) = 0; // ESI
    *(--kstack_vtop) = 0; // EBP
    *(--kstack_vtop) = 0; // ESP_ignore
    *(--kstack_vtop) = 0; // EBX
    *(--kstack_vtop) = 0; // EDX
    *(--kstack_vtop) = 0; // ECX
    *(--kstack_vtop) = 0; // EAX

    // 7. Segment Registers (order: GS, FS, ES, DS)
    *(--kstack_vtop) = USER_DATA_SELECTOR; // GS
    *(--kstack_vtop) = USER_DATA_SELECTOR; // FS
    *(--kstack_vtop) = USER_DATA_SELECTOR; // ES
    *(--kstack_vtop) = USER_DATA_SELECTOR; // DS

    // 8. EFLAGS (pushed by pushfd) - Already pushed as part of iret frame? Check context_switch.
    // The iret frame above includes EFLAGS. context_switch pushes EFLAGS *again*.
    // We need consistency. Let's assume context_switch expects the iret frame *plus*
    // the segment registers *plus* the pushad registers *plus* EFLAGS from pushfd.
    // So, push EFLAGS again here matching pushfd in context_switch.
    // *(--kstack_vtop) = 0x00000202; // Push EFLAGS again for popfd

    // Store the final virtual stack pointer in the TCB
    new_task->esp = kstack_vtop;

    // --- Insert into scheduler list (same as before) ---
     if (task_list == NULL) {
        task_list = new_task;
        current_task = new_task;
        new_task->next = new_task;
    } else {
        tcb_t *temp = task_list;
        while (temp->next != task_list) {
            temp = temp->next;
        }
        temp->next = new_task;
        new_task->next = task_list;
    }
    task_count++;

    terminal_printf("[Scheduler] Added task for PID %d (Kernel ESP starts at virt 0x%x).\n", pcb->pid, new_task->esp);
    return 0;
}

void yield(void) {
    // ... (same as before) ...
    schedule();
}

void schedule(void) {
    // ... (context_switches increment, check for no switch needed same as before) ...
     context_switches++;
     if (!current_task || !current_task->next || current_task->next == current_task) {
        return;
    }

    tcb_t *old_task = current_task;
    tcb_t *new_task = current_task->next;
    current_task = new_task; // Update current task pointer *before* switch

    uint32_t *new_page_directory = NULL;
    // Only switch CR3 if the process context (PCB) is different
    if (old_task->process != new_task->process) {
        new_page_directory = new_task->process->page_directory;
         terminal_printf("[Scheduler] Switching page directory to 0x%x for PID %d\n", new_page_directory, new_task->process->pid);
    } else {
         // Same process, no need to reload CR3
          // terminal_printf("[Scheduler] Same process context (PID %d), not switching CR3.\n", new_task->process->pid);
    }

    // Pass the virtual kernel stack pointer and physical page directory address
    context_switch(&(old_task->esp), new_task->esp, new_page_directory);
}

tcb_t *get_current_task(void) {
    // ... (same as before) ...
     return current_task;
}

// task_exit_cleanup remains the same, calling remove_current_task_with_code

void remove_current_task_with_code(uint32_t code) {
    // ... (Disable interrupts, check task_list/current_task same as before) ...
     asm volatile("cli");
      if (!task_list || !current_task) {
        terminal_write("[Scheduler] Error: No task to remove.\n");
        asm volatile("sti");
        return;
    }

    tcb_t *to_remove = current_task;
    pcb_t *proc_to_remove = to_remove->process;
    terminal_printf("[Scheduler] Removing task for PID %d (exit code %d).\n", proc_to_remove->pid, code);

    // --- Remove TCB from scheduler list ---
    if (to_remove->next == to_remove) { // Last task
        task_list = NULL;
        current_task = NULL;
        task_count = 0;
        terminal_write("[Scheduler] Last task removed. Halting system.\n");
        destroy_process(proc_to_remove);
        kfree(to_remove, sizeof(tcb_t));
        // asm volatile("sti"); // Enable interrupts before halting?
        while (1) { __asm__ volatile("hlt"); }
    } else {
        tcb_t *prev = task_list;
        while (prev->next != to_remove) { /* ... find prev ... */ prev = prev->next; }
        prev->next = to_remove->next;
        if (task_list == to_remove) { task_list = to_remove->next; }
        task_count--;

        current_task = to_remove->next; // Select next task *before* freeing

        // --- Free Resources ---
        destroy_process(proc_to_remove); // Free PCB, page dir, kernel stack
        kfree(to_remove, sizeof(tcb_t)); // Free TCB

        // --- Switch to the next task ---
        terminal_printf("[Scheduler] Switching to next task (PID %d).\n", current_task->process->pid);
        uint32_t *dummy_esp_save_location; // Dummy location, old ESP not needed
        context_switch(&dummy_esp_save_location, current_task->esp, current_task->process->page_directory);

        // Should not be reached
         terminal_write("[Scheduler] Error: Returned after context switch in remove task!\n");
          while(1) { asm volatile("hlt"); }
    }
}

void debug_scheduler_stats(uint32_t *out_task_count, uint32_t *out_switches) {
    // ... (same as before) ...
     if (out_task_count) { *out_task_count = task_count; }
     if (out_switches) { *out_switches = context_switches; }
}