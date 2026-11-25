/**
 * @file kfence.h
 * @brief KFENCE-style sampling heap checker of the debug kernel.
 *
 * Every Nth small kmalloc (coal.kfence=N) is served from a pool where each object has a page of its own, next
 * to a guard page: right-aligned objects catch overflows, left-aligned ones underflows. Freed objects stay
 * unmapped until their slot is needed again, so a use after free faults. Faults in the pool are reported with the
 * allocation and free backtraces and then let through so the kernel carries on.
 */
#ifndef KERNEL_MEMORY_KFENCE_H
#define KERNEL_MEMORY_KFENCE_H

#include <kernel/core/types.h>

struct isr_frame;

#ifdef COAL_DEBUG_KERNEL

#define KFENCE_BASE  0xFFFFC00040000000ul               /* behind the kernel stack window */
#define KFENCE_SLOTS 255
#define KFENCE_SIZE  ((2ul * KFENCE_SLOTS + 1) * 4096)

extern unsigned g_kfence_interval;
extern unsigned g_kfence_countdown;

void kfence_init(void);

/** NULL: not sampled, too big, or the pool is empty. */
void *kfence_alloc(size_t size, const uintptr_t *bt, unsigned n);

static inline bool kfence_owns(const void *p) {
    return (uintptr_t)p - KFENCE_BASE < KFENCE_SIZE;
}

void kfence_free(void *p, const uintptr_t *bt, unsigned n);

/** A kernel-mode page fault: true if it hit the pool (reported, access allowed to continue). */
bool kfence_handle_fault(uintptr_t addr, const struct isr_frame *f);

#endif
#endif
