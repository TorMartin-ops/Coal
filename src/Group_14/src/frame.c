// src/frame.c

#include "frame.h"
#include "buddy.h"
#include "paging.h"
#include "terminal.h"
#include "spinlock.h"
#include <libc/stdint.h> // For UINT32_MAX
#include <string.h>      // For memset
#include "types.h"
#include "kmalloc_internal.h" // For ALIGN_UP, PAGE_ALIGN_DOWN

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

// --- Globals ---
static volatile uint32_t *g_frame_refcounts = NULL; // Array of reference counts (Points to virtual address after mapping)
static uintptr_t g_frame_refcounts_phys = 0;   // Physical address of the refcount array
static size_t g_total_frames = 0;           // Total number of physical frames detected
static uintptr_t g_highest_address = 0;     // Highest physical address detected
static spinlock_t g_frame_lock;             // Lock for the refcount array

// Helper to get PFN (Physical Frame Number) from address
static inline size_t addr_to_pfn(uintptr_t addr) {
    return addr / PAGE_SIZE;
}

// Helper to get address from PFN
static inline uintptr_t pfn_to_addr(size_t pfn) {
    return pfn * PAGE_SIZE;
}

/**
 * Initializes the physical frame reference counting system.
 * Needs careful placement to allocate the refcount array itself.
 */
int frame_init(struct multiboot_tag_mmap *mmap_tag,
               uintptr_t kernel_phys_start, uintptr_t kernel_phys_end,
               uintptr_t buddy_heap_phys_start, uintptr_t buddy_heap_phys_end)
{
    terminal_write("[Frame] Initializing physical frame manager...\n");
    spinlock_init(&g_frame_lock);
    if (!mmap_tag) { return -1; }

    // Pass 1: Find highest address
    g_highest_address = 0;
    multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
    uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;
    while ((uintptr_t)mmap_entry < mmap_end) {
        if (mmap_tag->entry_size == 0) break; // Avoid loop if bad entry size
        uintptr_t region_end = (uintptr_t)mmap_entry->addr + (uintptr_t)mmap_entry->len;
        if (region_end > g_highest_address) g_highest_address = region_end;
        mmap_entry = (multiboot_memory_map_t *)((uintptr_t)mmap_entry + mmap_tag->entry_size);
    }
    if (g_highest_address == 0) return -1;

    g_highest_address = ALIGN_UP(g_highest_address, PAGE_SIZE);
    g_total_frames = addr_to_pfn(g_highest_address);
    terminal_printf("  Detected highest physical address: 0x%x (%u total frames)\n",
                     g_highest_address, g_total_frames);

    // Allocate refcount array using buddy
    size_t refcount_array_size = g_total_frames * sizeof(uint32_t);
    refcount_array_size = ALIGN_UP(refcount_array_size, PAGE_SIZE);
    void* refcount_array_phys_ptr = buddy_alloc(refcount_array_size);
    if (!refcount_array_phys_ptr) {
        terminal_write("  [FATAL] Failed to allocate refcount array!\n");
        return -1;
    }
    g_frame_refcounts_phys = (uintptr_t)refcount_array_phys_ptr; // Store physical address
    terminal_printf("  Allocated refcount array at phys 0x%x (size %u bytes)\n",
                     g_frame_refcounts_phys, refcount_array_size);

    // Map the refcount array into kernel virtual space
    uintptr_t refcount_array_vaddr = KERNEL_SPACE_VIRT_START + g_frame_refcounts_phys;
    extern uint32_t g_kernel_page_directory_phys; // Assume this is set by paging_init
    if (paging_map_range((uint32_t*)g_kernel_page_directory_phys,
                    refcount_array_vaddr,
                    g_frame_refcounts_phys, // Physical address
                    refcount_array_size,
                    PTE_KERNEL_DATA) != 0)
    {
        terminal_write("  [FATAL] Failed to map refcount array into kernel space!\n");
        buddy_free(refcount_array_phys_ptr, refcount_array_size); // Free physical memory
        g_frame_refcounts_phys = 0;
        return -1;
    }
    g_frame_refcounts = (volatile uint32_t*)refcount_array_vaddr; // Store VIRTUAL address
    terminal_printf("  Refcount array mapped at virt 0x%x\n", refcount_array_vaddr);

    // Pass 2 & 3: Initialize counts (use virtual address)
    // Mark all as reserved initially
    for (size_t i = 0; i < g_total_frames; ++i) g_frame_refcounts[i] = 1;

    // Mark available based on memory map
    mmap_entry = mmap_tag->entries;
    while ((uintptr_t)mmap_entry < mmap_end) {
        if (mmap_tag->entry_size == 0) break;
        if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
          uintptr_t region_start = (uintptr_t)mmap_entry->addr;
          uintptr_t region_end = region_start + (uintptr_t)mmap_entry->len;
          size_t first_pfn = addr_to_pfn(ALIGN_UP(region_start, PAGE_SIZE));
          size_t last_pfn = addr_to_pfn(PAGE_ALIGN_DOWN(region_end));
          for (size_t pfn = first_pfn; pfn < last_pfn && pfn < g_total_frames; ++pfn) {
              g_frame_refcounts[pfn] = 0; // Mark as free (refcount 0)
          }
        }
        mmap_entry = (multiboot_memory_map_t *)((uintptr_t)mmap_entry + mmap_tag->entry_size);
    }

    // Re-reserve specific regions (kernel, refcount array, buddy heap, first MB)
    size_t kernel_start_pfn = addr_to_pfn(PAGE_ALIGN_DOWN(kernel_phys_start));
    size_t kernel_end_pfn = addr_to_pfn(ALIGN_UP(kernel_phys_end, PAGE_SIZE));
    for (size_t pfn = kernel_start_pfn; pfn < kernel_end_pfn && pfn < g_total_frames; ++pfn) g_frame_refcounts[pfn] = 1;

    size_t refcount_start_pfn = addr_to_pfn(g_frame_refcounts_phys);
    size_t refcount_end_pfn = addr_to_pfn(g_frame_refcounts_phys + refcount_array_size);
    for (size_t pfn = refcount_start_pfn; pfn < refcount_end_pfn && pfn < g_total_frames; ++pfn) g_frame_refcounts[pfn] = 1;

    size_t buddy_start_pfn = addr_to_pfn(PAGE_ALIGN_DOWN(buddy_heap_phys_start));
    size_t buddy_end_pfn = addr_to_pfn(ALIGN_UP(buddy_heap_phys_end, PAGE_SIZE));
    for (size_t pfn = buddy_start_pfn; pfn < buddy_end_pfn && pfn < g_total_frames; ++pfn) g_frame_refcounts[pfn] = 1;

    size_t first_mb_pfn_limit = addr_to_pfn(0x100000);
    for (size_t pfn = 0; pfn < first_mb_pfn_limit && pfn < g_total_frames; ++pfn) g_frame_refcounts[pfn] = 1;


    terminal_write("[Frame] Frame manager initialized.\n");
    return 0;
}

/**
 * Allocates a single physical page frame.
 */
uintptr_t frame_alloc(void) {
    if (!g_frame_refcounts) return 0; // Not initialized

    void* phys_addr_void = buddy_alloc(PAGE_SIZE);
    uintptr_t phys_addr = (uintptr_t)phys_addr_void;

    if (!phys_addr) {
        return 0; // Out of memory
    }

    size_t pfn = addr_to_pfn(phys_addr);
    if (pfn >= g_total_frames) {
        terminal_printf("[Frame] frame_alloc: buddy_alloc returned invalid address 0x%x!\n", phys_addr);
        buddy_free(phys_addr_void, PAGE_SIZE);
        return 0;
    }

    // Set reference count to 1 using the *virtual* address
    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);
    if (g_frame_refcounts[pfn] != 0) {
        terminal_printf("[Frame] frame_alloc: WARNING! Allocated frame PFN %u (addr 0x%x) had non-zero refcount (%u)!\n",
                       pfn, phys_addr, g_frame_refcounts[pfn]);
    }
    g_frame_refcounts[pfn] = 1;
    spinlock_release_irqrestore(&g_frame_lock, irq_flags);

    return phys_addr;
}

/**
 * Increments the reference count for a frame.
 */
void get_frame(uintptr_t phys_addr) {
    if (!g_frame_refcounts) return;
    if (phys_addr == 0 || phys_addr >= g_highest_address) return;

    size_t pfn = addr_to_pfn(phys_addr);
    if (pfn >= g_total_frames) {
        terminal_printf("[Frame] get_frame: Invalid physical address 0x%x (PFN %u >= total %u)!\n", phys_addr, pfn, g_total_frames);
        return;
    }

    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);
    if (g_frame_refcounts[pfn] == 0) {
        terminal_printf("[Frame] get_frame: WARNING! Incrementing refcount of free frame PFN %u (addr 0x%x)!\n", pfn, phys_addr);
    }
    if (g_frame_refcounts[pfn] == UINT32_MAX) {
         terminal_printf("[Frame] get_frame: Error! Refcount overflow for PFN %u (addr 0x%x)\n", pfn, phys_addr);
    } else {
         g_frame_refcounts[pfn]++;
    }
    spinlock_release_irqrestore(&g_frame_lock, irq_flags);
}

/**
 * Decrements the reference count, potentially freeing the frame.
 */
void put_frame(uintptr_t phys_addr) {
    if (!g_frame_refcounts) return;
    if (phys_addr == 0 || phys_addr >= g_highest_address) return;

    size_t pfn = addr_to_pfn(phys_addr);
    if (pfn >= g_total_frames) {
        terminal_printf("[Frame] put_frame: Invalid physical address 0x%x (PFN %u >= total %u)!\n", phys_addr, pfn, g_total_frames);
        return;
    }

    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);
    if (g_frame_refcounts[pfn] == 0) {
        terminal_printf("[Frame] put_frame: ERROR! Double free detected for frame PFN %u (addr 0x%x)!\n", pfn, phys_addr);
        spinlock_release_irqrestore(&g_frame_lock, irq_flags);
        return; // Avoid decrementing below zero
    }

    g_frame_refcounts[pfn]--;
    uint32_t current_count = g_frame_refcounts[pfn];

    if (current_count == 0) {
        spinlock_release_irqrestore(&g_frame_lock, irq_flags); // Release lock before buddy_free
        buddy_free((void*)phys_addr, PAGE_SIZE);
    } else {
        spinlock_release_irqrestore(&g_frame_lock, irq_flags); // Count > 0, just release lock
    }
}

/**
 * Gets the current reference count.
 */
int get_frame_refcount(uintptr_t phys_addr) {
    if (!g_frame_refcounts) return -1;
    if (phys_addr >= g_highest_address) return -1;

    size_t pfn = addr_to_pfn(phys_addr);
    if (pfn >= g_total_frames) return -1;

    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);
    int count = (int)g_frame_refcounts[pfn];
    spinlock_release_irqrestore(&g_frame_lock, irq_flags);
    return count;
}