/**
 * @file paging_fault.c
 * @brief Page-fault entry from the ISR (user-mode faults only; kernel-mode
 *        faults are handled by the exception-table fixup in isr_pf.asm).
 */

#define LOG_MODULE "pf"

#include <kernel/memory/paging.h>
#include <kernel/memory/mm.h>
#include <kernel/process/process.h>
#include <kernel/process/scheduler.h>
#include <kernel/core/log.h>
#include <kernel/lib/assert.h>

#define EXIT_SIGSEGV 139    /* 128 + SIGSEGV, as a shell would report it */
#define EXIT_SIGKILL 137

void vm_fault_kill_current(uintptr_t fault_addr, uintptr_t fault_eip, int vm_fault_result) {
    pcb_t *p = get_current_process();
    KLOG_WARN(LOG_MODULE, "PID %u: %s at %#x (eip %#x), terminating",
              p ? (unsigned)p->pid : 0u,
              vm_fault_result == VM_FAULT_OOM ? "out of memory" : "segmentation fault",
              (unsigned)fault_addr, (unsigned)fault_eip);
    remove_current_task_with_code(vm_fault_result == VM_FAULT_OOM ? EXIT_SIGKILL : EXIT_SIGSEGV);
    KERNEL_PANIC_HALT("vm_fault_kill_current returned");
}

void page_fault_handler(registers_t *regs) {
    uintptr_t addr;
    asm volatile("mov %%cr2, %0" : "=r"(addr));

    if (!(regs->err_code & PAGE_FAULT_USER))
        KERNEL_PANIC_HALT("page fault in kernel mode");

    pcb_t *p = get_current_process();
    int r = (p && p->mm) ? mm_fault(p->mm, addr, regs->err_code) : VM_FAULT_SEGV;
    if (r != VM_FAULT_OK)
        vm_fault_kill_current(addr, regs->eip, r);
}
