/**
 * @file vdso.h
 * @brief The vDSO pages (see kernel/cpu/vdso_abi.h): created once at boot, mapped into every program image.
 */
#ifndef KERNEL_PROCESS_VDSO_H
#define KERNEL_PROCESS_VDSO_H

#include <kernel/core/types.h>
#include <kernel/memory/mm.h>

/** Allocates the pages, copies the image and publishes the clock. Without sysenter the stub uses int 0x80. */
void vdso_init(bool sysenter);

/** Maps the time page and the vDSO read-only into mm. 0 or -ENOMEM (mm is then released by the caller). */
int vdso_map(mm_struct_t *mm);

#endif
