/**
 * @file paging_fault.c
 * @brief Page-fault entry from the ISR (user-mode faults only; kernel-mode
 *        faults are handled by the exception-table fixup in isr_pf.asm).
 */

#define LOG_MODULE "pf"

#include <kernel/memory/paging.h>
#include <kernel/memory/mm.h>
#include <kernel/process/process.h>
#include <kernel/process/signal.h>
#include <kernel/core/log.h>
#include <kernel/lib/assert.h>

void page_fault_handler(registers_t *regs) {
    uintptr_t addr;
    asm volatile("mov %%cr2, %0" : "=r"(addr));

    if (!(regs->err_code & PAGE_FAULT_USER))
        KERNEL_PANIC_HALT("page fault in kernel mode");

    pcb_t *p = get_current_process();
    int r = (p && p->mm) ? mm_fault(p->mm, addr, regs->err_code) : VM_FAULT_SEGV;
    if (r != VM_FAULT_OK) {
        KLOG_WARN(LOG_MODULE, "PID %u: %s at %#x (eip %#x)", (unsigned)p->pid,
                  r == VM_FAULT_OOM ? "out of memory" : "segmentation fault",
                  (unsigned)addr, (unsigned)regs->eip);
        signal_force(r == VM_FAULT_OOM ? SIGKILL : SIGSEGV);
    }
    signal_return_to_user((isr_frame_t *)regs);
}
