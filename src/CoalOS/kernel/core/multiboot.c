/**
 * @file multiboot.c
 * @brief Multiboot2 boot information lookup
 */
#include <kernel/core/multiboot.h>

/* Sanity bound; real structures are a few hundred bytes. */
#define MULTIBOOT_INFO_MAX_SIZE 0x100000u

uint32_t multiboot_info_size(uintptr_t info_addr) {
    if (info_addr == 0) return 0;
    uint32_t total_size = *(volatile uint32_t *)info_addr;
    if (total_size < 8 || total_size > MULTIBOOT_INFO_MAX_SIZE) return 0;
    return total_size;
}

struct multiboot_tag *multiboot_find_tag(uintptr_t info_addr, uint16_t type) {
    uint32_t total_size = multiboot_info_size(info_addr);
    if (total_size == 0) return NULL;

    uintptr_t info_end = info_addr + total_size;
    struct multiboot_tag *tag = (struct multiboot_tag *)(info_addr + 8);

    while ((uintptr_t)tag + sizeof(struct multiboot_tag) <= info_end &&
           tag->type != MULTIBOOT_TAG_TYPE_END) {
        uintptr_t tag_addr = (uintptr_t)tag;
        if (tag->size < sizeof(struct multiboot_tag) || tag_addr + tag->size > info_end) {
            return NULL;
        }
        if (tag->type == type) return tag;
        tag = (struct multiboot_tag *)(tag_addr + ((tag->size + 7) & ~7u));
    }
    return NULL;
}
