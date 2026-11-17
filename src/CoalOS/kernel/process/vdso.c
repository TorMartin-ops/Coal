/**
 * @file vdso.c
 * @brief Builds the vDSO pages and maps them into new address spaces.
 *
 * The two frames are shared by every process: each mapping holds a reference, so tearing down an
 * address space or forking needs no special case.
 */
#include <kernel/process/vdso.h>
#include <kernel/cpu/vdso_abi.h>
#include <kernel/drivers/timer/clock.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/lib/assert.h>
#include <kernel/lib/string.h>
#include <kernel/memory/frame.h>
#include <kernel/memory/paging.h>

extern const uint8_t vdso_image[], vdso_image_end[];     /* vdso_image.asm */

#define USER_RO (PAGE_PRESENT | PAGE_USER)

_Static_assert(VDSO_LANDING == 0xBFFFF409u, "update VDSO_LANDING in kernel/cpu/cpu_defs.inc");

static uintptr_t vvar_frame, vdso_frame;

void vdso_init(bool sysenter) {
    size_t size = (size_t)(vdso_image_end - vdso_image);
    uint8_t *code;

    KERNEL_ASSERT(size <= PAGE_SIZE, "vDSO image larger than a page");
    vvar_frame = frame_alloc_zeroed();
    vdso_frame = frame_alloc_zeroed();
    if (!vvar_frame || !vdso_frame) KERNEL_PANIC_HALT("no memory for the vDSO");

    code = phys_to_virt(vdso_frame);
    memcpy(code, vdso_image, size);
    if (!sysenter) {                    /* "mov ebp, esp; sysenter" becomes "mov ebp, [esp]; nop": the int 0x80 behind it makes the call */
        static const uint8_t int80_stub[4] = { 0x8B, 0x2C, 0x24, 0x90 };
        memcpy(code + (VDSO_PATCH - VDSO_BASE), int80_stub, sizeof int80_stub);
    }
    clock_publish(phys_to_virt(vvar_frame));
}

int vdso_map(mm_struct_t *mm) {
    if (!insert_vma(mm, VVAR_BASE, VDSO_BASE + PAGE_SIZE, VM_READ | VM_EXEC | VM_USER)) return -ENOMEM;

    get_frame(vvar_frame);
    if (paging_map_single_4k(mm->pgd_phys, VVAR_BASE, vvar_frame, USER_RO)) {
        put_frame(vvar_frame);
        return -ENOMEM;
    }
    get_frame(vdso_frame);
    if (paging_map_single_4k(mm->pgd_phys, VDSO_BASE, vdso_frame, USER_RO)) {
        put_frame(vdso_frame);
        return -ENOMEM;
    }
    return 0;
}
