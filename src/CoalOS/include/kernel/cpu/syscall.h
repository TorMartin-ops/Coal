/*
 * System call layer: the Linux i386 ABI (see <libc/syscall_numbers.h>).
 *
 * One table indexed by the Linux number. Arguments arrive in EBX, ECX, EDX,
 * ESI, EDI, EBP; the result (or -errno) goes back in EAX.
 */
#ifndef KERNEL_CPU_SYSCALL_H
#define KERNEL_CPU_SYSCALL_H

#include <libc/syscall_numbers.h>
#include <kernel/cpu/isr_frame.h>
#include <kernel/cpu/linux_abi.h>

#define NR_SYSCALLS 384

void syscall_init(void);

/** Entry point called by syscall.asm with the saved register frame. */
void syscall_dispatch(isr_frame_t *regs);

/*
 * Services the system calls need that no other subsystem provides yet. Each has
 * a weak fallback in syscall_deps.c (-ENOSYS or "nothing"); the owning
 * subsystem only has to define the strong symbol.
 */

/** Set access and modification times of a file (utime). */
int vfs_utime(const char *path, uint32_t atime, uint32_t mtime);

/** Kernel log ring (dmesg): total capacity, copy of the most recent bytes, discard. */
size_t klog_ring_size(void);
size_t klog_ring_copy(char *buf, size_t len);
void   klog_ring_clear(void);

#endif
