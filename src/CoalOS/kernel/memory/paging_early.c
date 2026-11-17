/**
 * @file paging_early.c
 * @brief Boot-time memory bring-up (runs with paging off).
 *
 * The boot page tables are carved from the start of the heap region with a
 * bump allocator. The buddy allocator is started afterwards on the remainder,
 * so it never owns a frame that holds a live page table.
 */

#define LOG_MODULE "mem"

#include <kernel/memory/paging.h>
#include <kernel/memory/buddy.h>
#include <kernel/memory/kmap.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/core/log.h>
#include <kernel/lib/assert.h>
#include <kernel/lib/string.h>

#define PAGE_PSE        0x080u                 /* 4 MiB page (PDE only) */
#define BIG_PAGE        0x400000u
#define KMAP_BYTES      (KMAP_SLOTS * PAGE_SIZE)
#define REGION_MAX      0xFFFFF000u            /* 32-bit physical addressing: stop below 4 GiB */

extern uint8_t _kernel_start_phys;
extern uint8_t _kernel_end_phys;

static uintptr_t g_early_next;
static uintptr_t g_early_end;

static uintptr_t early_page(void) {
    if (g_early_next + PAGE_SIZE > g_early_end)
        KERNEL_PANIC_HALT("early page tables: out of memory");
    uintptr_t page = g_early_next;
    g_early_next += PAGE_SIZE;
    memset((void *)page, 0, PAGE_SIZE);
    return page;
}

/* Walker for the boot directory; tables are addressed physically. */
static uint32_t *early_pte(uint32_t *pd, uintptr_t vaddr) {
    uint32_t *pde = &pd[PDE_INDEX(vaddr)];
    if (!(*pde & PAGE_PRESENT))
        *pde = early_page() | PAGE_PRESENT | PAGE_RW;
    return (uint32_t *)(*pde & PAGING_ADDR_MASK) + PTE_INDEX(vaddr);
}

static void early_map(uint32_t *pd, uintptr_t vaddr, uintptr_t paddr, size_t bytes) {
    for (size_t off = 0; off < bytes; off += PAGE_SIZE)
        *early_pte(pd, vaddr + off) = (paddr + off) | PTE_KERNEL_DATA_FLAGS;
}

/* Direct-map [start, end) of physical memory, using 4 MiB pages where aligned. */
static void early_map_direct(uint32_t *pd, uintptr_t start, uintptr_t end) {
    uintptr_t big_lo = ALIGN_UP(start, BIG_PAGE), big_hi = ALIGN_DOWN(end, BIG_PAGE);
    if (big_lo >= big_hi) {
        early_map(pd, KERNEL_SPACE_VIRT_START + start, start, end - start);
        return;
    }
    early_map(pd, KERNEL_SPACE_VIRT_START + start, start, big_lo - start);
    for (uintptr_t p = big_lo; p < big_hi; p += BIG_PAGE)
        pd[PDE_INDEX(KERNEL_SPACE_VIRT_START + p)] = p | PTE_KERNEL_DATA_FLAGS | PAGE_PSE;
    early_map(pd, KERNEL_SPACE_VIRT_START + big_hi, big_hi, end - big_hi);
}

void memory_init(uintptr_t heap_phys, size_t heap_size) {
    uintptr_t heap_start = PAGE_ALIGN_UP(heap_phys);
    uint64_t end64 = (uint64_t)heap_phys + heap_size;
    uintptr_t heap_end = end64 > REGION_MAX ? REGION_MAX : PAGE_ALIGN_DOWN((uintptr_t)end64);
    if (heap_end <= heap_start)
        KERNEL_PANIC_HALT("memory_init: empty heap");
    uintptr_t low_end = heap_end < HIGHMEM_START_PHYS ? heap_end : HIGHMEM_START_PHYS;

    uintptr_t kernel_start = PAGE_ALIGN_DOWN((uintptr_t)&_kernel_start_phys);
    uintptr_t kernel_end = PAGE_ALIGN_UP((uintptr_t)&_kernel_end_phys);
    if (kernel_end > IDENTITY_MAP_SIZE)
        KERNEL_PANIC_HALT("kernel image exceeds the identity map");

    g_early_next = heap_start;
    g_early_end = low_end;
    uint32_t *pd = (uint32_t *)early_page();

    /* Identity map of low memory (the kernel is linked and runs here); page 0
     * stays unmapped so that NULL dereferences fault. */
    early_map(pd, PAGE_SIZE, PAGE_SIZE, IDENTITY_MAP_SIZE - PAGE_SIZE);
    /* Direct map of lowmem, plus aliases of the kernel image and VGA text. */
    early_map(pd, KERNEL_SPACE_VIRT_START + kernel_start, kernel_start, kernel_end - kernel_start);
    early_map(pd, VGA_VIRT_ADDR, VGA_PHYS_ADDR, PAGE_SIZE);
    early_map_direct(pd, heap_start, low_end);
    /* Kernel stack and kmap tables exist up front so every directory shares them. */
    for (uintptr_t va = KERNEL_STACK_VADDR_START; va < KERNEL_STACK_VADDR_END; va += BIG_PAGE)
        early_pte(pd, va);
    early_pte(pd, KMAP_VADDR_START);
    early_pte(pd, KERNEL_MMIO_VADDR_START);
    pd[RECURSIVE_PDE_INDEX] = (uintptr_t)pd | PAGE_PRESENT | PAGE_RW;

    /* Page descriptors for the whole region, in lowmem right after the tables. */
    struct page *pages = (struct page *)g_early_next;
    size_t meta_bytes = PAGE_ALIGN_UP(buddy_meta_bytes(heap_end - heap_start));
    if (g_early_next + meta_bytes > g_early_end)
        KERNEL_PANIC_HALT("memory_init: out of memory for page descriptors");
    g_early_next += meta_bytes;

    paging_activate(pd);
    g_kernel_page_directory_phys = (uintptr_t)pd;
    g_kernel_page_directory_virt = phys_to_virt((uintptr_t)pd);

    buddy_init(g_early_next, low_end, heap_end, phys_to_virt((uintptr_t)pages));
    kmalloc_init();

    KLOG_INFO(LOG_MODULE, "lowmem %u MiB, highmem %u MiB, %u KiB boot structures, %u MiB free",
              (unsigned)(buddy_total_pages(ZONE_NORMAL) >> 8), (unsigned)(buddy_total_pages(ZONE_HIGH) >> 8),
              (unsigned)((g_early_next - heap_start) >> 10),
              (unsigned)((buddy_free_pages_count(ZONE_NORMAL) + buddy_free_pages_count(ZONE_HIGH)) >> 8));
}
