/**
 * @file pvh.c
 * @brief PVH boot: builds a Multiboot2 information block (command line and memory map) from the
 *        hvm_start_info a PVH loader hands over, so the rest of the boot code sees one format.
 *
 * Runs before paging is on, on a private stack, and may only use memory inside the kernel image.
 */
#include <kernel/arch/multiboot2.h>
#include <libc/stdint.h>
#include <libc/stddef.h>

#define HVM_START_MAGIC 0x336ec578u
#define HVM_MEMMAP_RAM  1u

struct hvm_start_info {
    uint32_t magic, version, flags, nr_modules;
    uint64_t modlist_paddr, cmdline_paddr, rsdp_paddr;
    uint64_t memmap_paddr;              /* version 1 and later */
    uint32_t memmap_entries, reserved;
};

struct hvm_memmap_entry {
    uint64_t addr, size;
    uint32_t type, reserved;
};

#define INFO_MAX     2048
#define MMAP_ENTRIES 32
#define CMDLINE_MAX  256

/* In .bss, so inside the kernel image and inside the identity-mapped first 4 MiB. */
static uint8_t g_info[INFO_MAX] __attribute__((aligned(8)));

static uint8_t *put(uint8_t *p, uint32_t v) {
    *(uint32_t *)p = v;
    return p + 4;
}

uint32_t pvh_build_info(const struct hvm_start_info *si) {
    uint8_t *p = g_info + 8;            /* total_size and reserved come last */

    if (si->magic != HVM_START_MAGIC) return 0;     /* the boot code then stops: not a Multiboot2 loader */

    if (si->cmdline_paddr) {
        const char *cmd = (const char *)(uintptr_t)si->cmdline_paddr;
        uint32_t n = 0;
        while (n < CMDLINE_MAX - 1 && cmd[n]) n++;
        p = put(p, MULTIBOOT_TAG_TYPE_CMDLINE);
        p = put(p, 8 + n + 1);
        for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)cmd[i];
        p[n] = 0;
        p += (n + 1 + 7) & ~7u;
    }
    if (si->version >= 1 && si->memmap_paddr) {
        const struct hvm_memmap_entry *e = (const struct hvm_memmap_entry *)(uintptr_t)si->memmap_paddr;
        uint32_t n = si->memmap_entries < MMAP_ENTRIES ? si->memmap_entries : MMAP_ENTRIES;
        p = put(p, MULTIBOOT_TAG_TYPE_MMAP);
        p = put(p, 16 + n * (uint32_t)sizeof(multiboot_memory_map_t));
        p = put(p, sizeof(multiboot_memory_map_t));
        p = put(p, 0);                  /* entry_version */
        for (uint32_t i = 0; i < n; i++) {
            multiboot_memory_map_t *m = (multiboot_memory_map_t *)p;
            m->addr = e[i].addr;
            m->len = e[i].size;
            m->type = e[i].type == HVM_MEMMAP_RAM ? MULTIBOOT_MEMORY_AVAILABLE : MULTIBOOT_MEMORY_RESERVED;
            m->zero = 0;
            p += sizeof(*m);
        }
        p = (uint8_t *)(((uintptr_t)p + 7) & ~7u);
    }
    p = put(p, MULTIBOOT_TAG_TYPE_END);
    p = put(p, 8);
    put(g_info, (uint32_t)(p - g_info));
    put(g_info + 4, 0);
    return (uint32_t)(uintptr_t)g_info;
}
