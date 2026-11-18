/**
 * @file paging_fault.c
 * @brief Page-fault entry from the ISR (user-mode faults only; kernel-mode
 *        faults are handled by the exception-table fixup).
 */

#define LOG_MODULE "pf"

#include <kernel/memory/paging.h>
#include <kernel/memory/mm.h>
#include <kernel/process/process.h>
#include <kernel/process/signal.h>
#include <kernel/core/log.h>
#include <kernel/lib/assert.h>
#include <kernel/cpu/idt.h>
#include <kernel/cpu/exception_table.h>
#include <kernel/cpu/msr.h>
#include <kernel/memory/kfence.h>

void page_fault_handler(isr_frame_t *regs) {
    uintptr_t addr = read_cr2();

    if (!(regs->err_code & PAGE_FAULT_USER)) {
#ifdef COAL_DEBUG_KERNEL
        if (kfence_handle_fault(addr, regs))
            return;                             /* reported; the access goes on */
#endif
        uint64_t fixup = find_exception_fixup(regs->rip);
        if (!fixup) kernel_fault_panic(regs);
        regs->rip = fixup;                      /* a user access inside a copy loop: the copy reports a short count */
        return;
    }

    pcb_t *p = get_current_process();
    int r = (p && p->mm) ? mm_fault(p->mm, addr, regs->err_code) : VM_FAULT_SEGV;
    if (r == VM_FAULT_OOM) {
        signal_force(SIGKILL);                  /* mm_fault() already ran the OOM killer and logged the victim */
    } else if (r != VM_FAULT_OK) {
        KLOG_WARN(LOG_MODULE, "PID %u: segmentation fault at %#lx (rip %#lx)", (unsigned)p->pid,
                  (unsigned long)addr, (unsigned long)regs->rip);
        signal_force(SIGSEGV);
    }
}
