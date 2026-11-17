/**
 * @file boot_info.h
 * @brief What the boot loader tells the kernel, in one form: Multiboot2 (GRUB) and PVH (Firecracker,
 *        Cloud Hypervisor, qemu -kernel) both end up as a list of usable RAM ranges.
 */
#ifndef KERNEL_CORE_BOOT_INFO_H
#define KERNEL_CORE_BOOT_INFO_H

#include <kernel/core/types.h>

#define BOOT_MAX_REGIONS 32

struct mem_region {
    uintptr_t start, end;               /* page aligned, end exclusive */
};

struct boot_memmap {
    unsigned count;
    struct mem_region r[BOOT_MAX_REGIONS];
};

/** Reads the memory map from the loader's structure at info_phys (reachable through the direct map).
 *  The result covers RAM above 1 MiB only, without the kernel image and without the structure itself.
 *  Halts if the magic is unknown or there is no memory map. */
void boot_info_memmap(uint32_t magic, uintptr_t info_phys, struct boot_memmap *out);

/** The kernel command line from the loader's structure (NUL terminated, inside the direct map), or NULL. */
const char *boot_info_cmdline(uint32_t magic, uintptr_t info_phys);

#endif
