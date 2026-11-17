/**
 * @file paging_early.c
 * @brief Boot-time memory bring-up.
 *
 * The boot code (boot/boot.asm) left tables that map the first 4 GiB at PHYS_OFFSET. They are used to
 * carve the new kernel page tables and the page descriptors from the start of the largest RAM range below
 * 4 GiB with a bump allocator; the buddy allocator then starts on the rest, so it never owns a frame that
 * holds a live table.
 */

#define LOG_MODULE "mem"

#include <kernel/memory/paging.h>
#include <kernel/memory/buddy.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/cpu/msr.h>
#include <kernel/core/log.h>
#include <kernel/lib/assert.h>
#include <kernel/lib/string.h>

#define BIG_PAGE        0x200000ul              /* 2 MiB */
#define LOW_RAM_LIMIT   ZONE_HIGH_START_PHYS    /* the boot tables map this much */
#define DIRECT_MAP_MAX  (512ul << 30)           /* one PML4 entry */

extern uint8_t _kernel_text_start[], _kernel_text_end[];
extern uint8_t _kernel_rodata_start[], _kernel_rodata_end[];
extern uint8_t _kernel_data_start[], _kernel_data_end[];

static uintptr_t g_early_next;
static uintptr_t g_early_end;

static uintptr_t early_alloc(size_t bytes) {
    bytes = PAGE_ALIGN_UP(bytes);
    if (g_early_next + bytes > g_early_end)
        KERNEL_PANIC_HALT("memory_init: out of memory for the boot structures");
    uintptr_t phys = g_early_next;
    g_early_next += bytes;
    memset(phys_to_virt(phys), 0, bytes);
    return phys;
}

/* The table below `table[idx]`, created if absent. Tables are addressed through the boot direct map. */
static pte_t *early_table(pte_t *table, unsigned idx) {
    if (!(table[idx] & PAGE_PRESENT))
        table[idx] = early_alloc(PAGE_SIZE) | PAGE_PRESENT | PAGE_RW;
    return phys_to_virt(table[idx] & PAGING_ADDR_MASK);
}

/* Maps [start, end) of physical memory at PHYS_OFFSET with 2 MiB pages. */
static void map_direct(pte_t *pml4, uintptr_t start, uintptr_t end) {
    pte_t *pdpt = early_table(pml4, PML4_INDEX(PHYS_OFFSET));

    for (uintptr_t p = ALIGN_DOWN(start, BIG_PAGE); p < ALIGN_UP(end, BIG_PAGE); p += BIG_PAGE) {
        pte_t *pd = early_table(pdpt, PDPT_INDEX(p));
        pd[PD_INDEX(p)] = p | PAGE_PRESENT | PAGE_RW | PAGE_HUGE | PAGE_NX;
    }
}

/* The kernel image at -2 GiB with 4 KiB pages: text read-only and executable, data not executable. */
static void map_image(pte_t *pml4) {
    pte_t *pdpt = early_table(pml4, PML4_INDEX(KERNEL_IMAGE_VIRT));
    pte_t *pd = early_table(pdpt, PDPT_INDEX(KERNEL_IMAGE_VIRT));

    early_table(pdpt, PDPT_INDEX(KERNEL_MMIO_VADDR_START));         /* page directory of the device window */
    for (uintptr_t va = (uintptr_t)_kernel_text_start; va < (uintptr_t)_kernel_data_end; va += PAGE_SIZE) {
        uint64_t flags = va < (uintptr_t)_kernel_text_end ? PAGE_PRESENT
                       : va < (uintptr_t)_kernel_rodata_end ? PAGE_PRESENT | PAGE_NX
                       : PAGE_PRESENT | PAGE_RW | PAGE_NX;
        pte_t *pt = early_table(pd, PD_INDEX(va));
        pt[PT_INDEX(va)] = (va - KERNEL_IMAGE_VIRT) | flags;
    }
}

void memory_init(const struct boot_memmap *in) {
    struct boot_memmap map = *in;
    unsigned boot_region = 0;
    uintptr_t base = ~0ul, end = 0, best = 0;

    for (unsigned i = 0; i < map.count; i++) {
        uintptr_t top = map.r[i].end < LOW_RAM_LIMIT ? map.r[i].end : LOW_RAM_LIMIT;
        if (map.r[i].start < top && top - map.r[i].start > best) {
            best = top - map.r[i].start;
            boot_region = i;
        }
        if (map.r[i].start < base) base = map.r[i].start;
        if (map.r[i].end > end) end = map.r[i].end;
    }
    if (!best || end > DIRECT_MAP_MAX)
        KERNEL_PANIC_HALT("memory_init: no usable RAM below 4 GiB");
    g_early_next = map.r[boot_region].start;
    g_early_end = g_early_next + best;
    uintptr_t early_start = g_early_next;

    struct page *pages = phys_to_virt(early_alloc(buddy_meta_bytes(end - base)));
    uintptr_t pml4_phys = early_alloc(PAGE_SIZE);
    pte_t *pml4 = phys_to_virt(pml4_phys);

    map_direct(pml4, 0, BIG_PAGE);                      /* BIOS area and VGA text */
    for (unsigned i = 0; i < map.count; i++)
        map_direct(pml4, map.r[i].start, map.r[i].end);
    map_image(pml4);
    early_table(pml4, PML4_INDEX(KERNEL_STACK_VADDR_START));    /* shared by every address space */

    write_cr3(pml4_phys);                               /* the identity map is gone from here on */
    g_kernel_page_directory_phys = pml4_phys;
    g_kernel_page_directory_virt = phys_to_virt(pml4_phys);

    map.r[boot_region].start = g_early_next;
    buddy_init(base, end, pages);
    for (unsigned i = 0; i < map.count; i++)
        buddy_add_range(map.r[i].start, map.r[i].end);
    kmalloc_init();

    KLOG_INFO(LOG_MODULE, "%u MiB below 4 GiB, %u MiB above, %u KiB boot structures, %u MiB free",
              (unsigned)(buddy_total_pages(ZONE_LOW) >> 8), (unsigned)(buddy_total_pages(ZONE_HIGH) >> 8),
              (unsigned)((g_early_next - early_start) >> 10),
              (unsigned)((buddy_free_pages_count(ZONE_LOW) + buddy_free_pages_count(ZONE_HIGH)) >> 8));
}
