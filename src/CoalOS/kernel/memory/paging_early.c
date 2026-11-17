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
#include <kernel/memory/frame.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/core/log.h>
#include <kernel/lib/assert.h>
#include <kernel/lib/string.h>

#define HEAP_MAX_BYTES ((size_t)BUDDY_MAX_PAGES * PAGE_SIZE)

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

void memory_init(uintptr_t heap_phys, size_t heap_size) {
    uintptr_t heap_start = PAGE_ALIGN_UP(heap_phys);
    uintptr_t heap_end = PAGE_ALIGN_DOWN(heap_phys + heap_size);
    if (heap_end <= heap_start)
        KERNEL_PANIC_HALT("memory_init: empty heap");
    if (heap_end - heap_start > HEAP_MAX_BYTES)
        heap_end = heap_start + HEAP_MAX_BYTES;

    uintptr_t kernel_start = PAGE_ALIGN_DOWN((uintptr_t)&_kernel_start_phys);
    uintptr_t kernel_end = PAGE_ALIGN_UP((uintptr_t)&_kernel_end_phys);
    if (kernel_end > IDENTITY_MAP_SIZE)
        KERNEL_PANIC_HALT("kernel image exceeds the identity map");

    g_early_next = heap_start;
    g_early_end = heap_end;
    uint32_t *pd = (uint32_t *)early_page();

    /* Identity map of low memory (the kernel is linked and runs here); page 0
     * stays unmapped so that NULL dereferences fault. */
    early_map(pd, PAGE_SIZE, PAGE_SIZE, IDENTITY_MAP_SIZE - PAGE_SIZE);
    /* Direct map of the heap, plus aliases of the kernel image and VGA text. */
    early_map(pd, KERNEL_SPACE_VIRT_START + kernel_start, kernel_start, kernel_end - kernel_start);
    early_map(pd, VGA_VIRT_ADDR, VGA_PHYS_ADDR, PAGE_SIZE);
    early_map(pd, KERNEL_SPACE_VIRT_START + heap_start, heap_start, heap_end - heap_start);
    /* Kernel stack tables exist up front so every directory shares them. */
    for (uintptr_t va = KERNEL_STACK_VADDR_START; va < KERNEL_STACK_VADDR_END; va += 0x400000u)
        early_pte(pd, va);
    pd[RECURSIVE_PDE_INDEX] = (uintptr_t)pd | PAGE_PRESENT | PAGE_RW;

    paging_activate(pd);
    g_kernel_page_directory_phys = (uintptr_t)pd;
    g_kernel_page_directory_virt = phys_to_virt((uintptr_t)pd);

    buddy_init(g_early_next, heap_end - g_early_next);
    frame_init();
    kmalloc_init();

    KLOG_INFO(LOG_MODULE, "heap %#x-%#x, %u KiB page tables, %u KiB free",
              (unsigned)heap_start, (unsigned)heap_end,
              (unsigned)((g_early_next - heap_start) >> 10),
              (unsigned)(buddy_free_pages_count() << (PAGE_SHIFT - 10)));
}
