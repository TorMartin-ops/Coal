/**
 * @file multiboot.h
 * @brief Multiboot2 boot information lookup
 */
#ifndef KERNEL_CORE_MULTIBOOT_H
#define KERNEL_CORE_MULTIBOOT_H

#include <kernel/arch/multiboot2.h>
#include <libc/stdint.h>
#include <libc/stddef.h>

/**
 * Finds the first tag of the given type in the boot information at
 * info_addr. The structure must be readable at that address: GRUB places it
 * in low memory, which is identity-mapped both before and after paging is
 * enabled. Returns NULL if the tag is absent or the structure is malformed.
 */
struct multiboot_tag *multiboot_find_tag(uintptr_t info_addr, uint16_t type);

/** Total size in bytes of the boot information, or 0 if it looks invalid. */
uint32_t multiboot_info_size(uintptr_t info_addr);

#endif
