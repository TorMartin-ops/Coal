/**
 * @file paging_fault.h
 * @brief Page-fault handling.
 *
 * User faults are resolved by mm_fault() (demand paging, stack growth,
 * copy-on-write). A fault that cannot be resolved never panics the kernel: the
 * offending process gets SIGSEGV (SIGKILL when out of memory).
 */
#ifndef PAGING_FAULT_H
#define PAGING_FAULT_H

#include <kernel/core/types.h>

/* Hardware error code bits */
#define PAGE_FAULT_PRESENT  (1u << 0)  /* protection violation (else not present) */
#define PAGE_FAULT_WRITE    (1u << 1)
#define PAGE_FAULT_USER     (1u << 2)

/* Results of mm_fault() */
#define VM_FAULT_OK    0
#define VM_FAULT_SEGV (-1)  /* invalid address or permission: the process deserves SIGSEGV */
#define VM_FAULT_OOM  (-2)  /* out of memory while resolving the fault */

struct isr_frame;

/** Page-fault interrupt handler: user faults go to mm_fault(); kernel faults need an exception-table fixup. */
void page_fault_handler(struct isr_frame *regs);

#endif /* PAGING_FAULT_H */
