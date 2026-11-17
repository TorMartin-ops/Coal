/**
 * @file boot_info.c
 * @brief Usable RAM from the loader's structure: Multiboot2 (GRUB) or PVH start info (qemu -kernel,
 *        Firecracker, Cloud Hypervisor).
 */
#include <kernel/core/boot_info.h>
#include <kernel/arch/multiboot2.h>
#include <kernel/memory/paging.h>
#include <kernel/lib/assert.h>

#define PVH_START_MAGIC     0x336ec578u
#define PVH_MEMMAP_RAM      1u
#define LOW_MEM_END         0x100000ul      /* RAM below 1 MiB is left to the firmware and the boot code */
#define MB2_INFO_MAX        0x100000u

struct pvh_start_info {                     /* hvm_start_info, version 1 */
    uint32_t magic, version, flags, nr_modules;
    uint64_t modlist_paddr, cmdline_paddr, rsdp_paddr, memmap_paddr;
    uint32_t memmap_entries, reserved;
};

struct pvh_memmap_entry {
    uint64_t addr, size;
    uint32_t type, reserved;
};

extern uint8_t _kernel_start_phys[], _kernel_end_phys[];

/* Removes [s, e) from every region, splitting one when the hole lies inside it. */
static void carve(struct boot_memmap *m, uintptr_t s, uintptr_t e) {
    for (unsigned i = 0; i < m->count; i++) {
        struct mem_region *r = &m->r[i];
        if (e <= r->start || s >= r->end)
            continue;
        if (s > r->start && e < r->end) {
            if (m->count == BOOT_MAX_REGIONS)
                KERNEL_PANIC_HALT("boot_info: too many memory regions");
            m->r[m->count++] = (struct mem_region){ e, r->end };
            r->end = s;
        } else if (s <= r->start && e >= r->end) {
            *r = m->r[--m->count];
            i--;
        } else if (s <= r->start) {
            r->start = e;
        } else {
            r->end = s;
        }
    }
}

static void add_ram(struct boot_memmap *m, uint64_t start, uint64_t end) {
    if (start < LOW_MEM_END)
        start = LOW_MEM_END;
    start = ALIGN_UP(start, PAGE_SIZE);
    end = ALIGN_DOWN(end, PAGE_SIZE);
    if (start >= end)
        return;
    if (m->count == BOOT_MAX_REGIONS)
        KERNEL_PANIC_HALT("boot_info: too many memory regions");
    m->r[m->count++] = (struct mem_region){ start, end };
}

/* Returns the extent of the structure in *size. */
static void read_multiboot2(uintptr_t info, struct boot_memmap *m, size_t *size) {
    uint32_t total = *(uint32_t *)phys_to_virt(info);
    if (total < 16 || total > MB2_INFO_MAX)
        KERNEL_PANIC_HALT("bad Multiboot2 info");
    *size = total;

    uintptr_t base = (uintptr_t)phys_to_virt(info);
    for (struct multiboot_tag *t = (void *)(base + 8); (uintptr_t)t < base + total && t->type != MULTIBOOT_TAG_TYPE_END;
         t = (void *)((uintptr_t)t + ALIGN_UP(t->size, 8))) {
        if (t->type != MULTIBOOT_TAG_TYPE_MMAP)
            continue;
        struct multiboot_tag_mmap *mm = (void *)t;
        for (uintptr_t e = (uintptr_t)mm->entries; e + mm->entry_size <= (uintptr_t)mm + mm->size; e += mm->entry_size) {
            multiboot_memory_map_t *me = (void *)e;
            if (me->type == MULTIBOOT_MEMORY_AVAILABLE)
                add_ram(m, me->addr, me->addr + me->len);
        }
        return;
    }
}

static void read_pvh(uintptr_t info, struct boot_memmap *m, size_t *size) {
    struct pvh_start_info *si = phys_to_virt(info);
    if (si->version < 1 || !si->memmap_paddr)
        KERNEL_PANIC_HALT("PVH start info has no memory map");
    struct pvh_memmap_entry *e = phys_to_virt(si->memmap_paddr);

    for (uint32_t i = 0; i < si->memmap_entries; i++)
        if (e[i].type == PVH_MEMMAP_RAM)
            add_ram(m, e[i].addr, e[i].addr + e[i].size);
    carve(m, si->memmap_paddr, si->memmap_paddr + si->memmap_entries * sizeof(*e));
    *size = sizeof(*si);
}

void boot_info_memmap(uint32_t magic, uintptr_t info_phys, struct boot_memmap *out) {
    size_t size;

    out->count = 0;
    if (magic == MULTIBOOT2_BOOTLOADER_MAGIC)
        read_multiboot2(info_phys, out, &size);
    else if (magic == PVH_START_MAGIC)
        read_pvh(info_phys, out, &size);
    else
        KERNEL_PANIC_HALT("unknown boot protocol");

    carve(out, info_phys, ALIGN_UP(info_phys + size, PAGE_SIZE));
    carve(out, (uintptr_t)_kernel_start_phys, ALIGN_UP((uintptr_t)_kernel_end_phys, PAGE_SIZE));
    if (!out->count)
        KERNEL_PANIC_HALT("no usable memory");
}
