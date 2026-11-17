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

/** Entry point called by syscall.asm with the saved register frame. */
void syscall_dispatch(isr_frame_t *regs);

/** Set access and modification times of a file (utime). */
int vfs_utime(const char *path, uint32_t atime, uint32_t mtime);

#endif
