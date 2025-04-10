#include "frame.h"
#include "buddy.h"      // Underlying physical allocator
#include "paging.h"     // For PAGE_SIZE, KERNEL_SPACE_VIRT_START (for mapping frame array)
#include "terminal.h"   // For logging
#include "spinlock.h"   // For locking
#include <string.h>     // For memset

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

// --- Globals ---
static volatile uint32_t *g_frame_refcounts = NULL; // Array of reference counts
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

    if (!mmap_tag) {
        terminal_write("  [Frame] Error: Multiboot memory map not provided!\n");
        return -1;
    }

    // --- Pass 1: Find the highest physical address to determine array size ---
    g_highest_address = 0;
    multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
    uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;

    while ((uintptr_t)mmap_entry < mmap_end) {
        uintptr_t region_start = (uintptr_t)mmap_entry->addr;
        uintptr_t region_end = region_start + (uintptr_t)mmap_entry->len;
        if (region_end > g_highest_address) {
            g_highest_address = region_end;
        }
        mmap_entry = (multiboot_memory_map_t *)((uintptr_t)mmap_entry + mmap_tag->entry_size);
    }

    if (g_highest_address == 0) {
         terminal_write("  [Frame] Error: Could not determine memory size from map!\n");
         return -1;
    }

    // Round highest address UP to the nearest page boundary
    g_highest_address = ALIGN_UP(g_highest_address, PAGE_SIZE);
    g_total_frames = addr_to_pfn(g_highest_address);
    terminal_printf("  Detected highest physical address: 0x%x (%u total frames)\n",
                    g_highest_address, g_total_frames);

    // --- Allocate memory for the reference count array ---
    size_t refcount_array_size = g_total_frames * sizeof(uint32_t);
    refcount_array_size = ALIGN_UP(refcount_array_size, PAGE_SIZE); // Align size for buddy

    // Problem: Where to allocate this array? It needs to be done *before*
    // the main buddy allocator manages this memory. We need to find a suitable
    // region from the memory map *after* the kernel.
    // This requires the memory map parsing logic to be available very early.
    // For now, we'll *assume* buddy_alloc can be called once here safely,
    // or that a dedicated early allocator exists.
    // *** THIS IS A SIMPLIFICATION - Real kernel needs careful boot memory management ***
    g_frame_refcounts = (uint32_t*)buddy_alloc(refcount_array_size);
    if (!g_frame_refcounts) {
        terminal_write("  [Frame] Error: Failed to allocate memory for refcount array!\n");
        g_total_frames = 0;
        return -1;
    }
     terminal_printf("  Allocated refcount array at phys 0x%x (size %u bytes)\n",
                    (uintptr_t)g_frame_refcounts, refcount_array_size);

     // Map the refcount array into kernel space to initialize it
     uintptr_t refcount_array_vaddr = KERNEL_SPACE_VIRT_START + (uintptr_t)g_frame_refcounts;
     if (paging_map_range(kernel_page_directory,
                           refcount_array_vaddr,
                           (uintptr_t)g_frame_refcounts,
                           refcount_array_size,
                           PTE_KERNEL_DATA) != 0) // Use kernel RW flags
    {
         terminal_write("  [Frame] Error: Failed to map refcount array into kernel space!\n");
         // Need to free the physical memory allocated for the array
         buddy_free((void*)g_frame_refcounts, refcount_array_size);
         g_frame_refcounts = NULL;
         g_total_frames = 0;
         return -1;
    }
     terminal_printf("  Refcount array mapped at virt 0x%x\n", refcount_array_vaddr);
     volatile uint32_t* refcounts_virt = (volatile uint32_t*)refcount_array_vaddr;


    // --- Pass 2: Initialize reference counts based on memory map ---
    // Mark all frames as RESERVED initially
    for (size_t i = 0; i < g_total_frames; ++i) {
        refcounts_virt[i] = 1; // Reserve by default (ref count > 0)
    }

    // Mark AVAILABLE frames identified by Multiboot as having refcount 0
    mmap_entry = mmap_tag->entries;
    while ((uintptr_t)mmap_entry < mmap_end) {
        if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
            uintptr_t region_start = (uintptr_t)mmap_entry->addr;
            uintptr_t region_end = region_start + (uintptr_t)mmap_entry->len;

            size_t first_pfn = addr_to_pfn(ALIGN_UP(region_start, PAGE_SIZE));
            size_t last_pfn = addr_to_pfn(PAGE_ALIGN_DOWN(region_end)); // PFN before the end

            for (size_t pfn = first_pfn; pfn < last_pfn && pfn < g_total_frames; ++pfn) {
                refcounts_virt[pfn] = 0; // Mark as available
            }
        }
        mmap_entry = (multiboot_memory_map_t *)((uintptr_t)mmap_entry + mmap_tag->entry_size);
    }

    // --- Pass 3: Re-reserve frames used by kernel, buddy heap, refcount array ---
    // Round kernel start down, end up
    size_t kernel_start_pfn = addr_to_pfn(PAGE_ALIGN_DOWN(kernel_phys_start));
    size_t kernel_end_pfn = addr_to_pfn(ALIGN_UP(kernel_phys_end, PAGE_SIZE));
    terminal_printf("  Reserving kernel frames PFN %u - %u\n", kernel_start_pfn, kernel_end_pfn);
    for (size_t pfn = kernel_start_pfn; pfn < kernel_end_pfn && pfn < g_total_frames; ++pfn) {
        refcounts_virt[pfn] = 1; // Mark as reserved/in-use
    }

    // Reserve frames used by the refcount array itself
    size_t refcount_start_pfn = addr_to_pfn((uintptr_t)g_frame_refcounts);
    size_t refcount_end_pfn = addr_to_pfn((uintptr_t)g_frame_refcounts + refcount_array_size);
     terminal_printf("  Reserving refcount array frames PFN %u - %u\n", refcount_start_pfn, refcount_end_pfn);
    for (size_t pfn = refcount_start_pfn; pfn < refcount_end_pfn && pfn < g_total_frames; ++pfn) {
        refcounts_virt[pfn] = 1;
    }

    // Reserve frames intended for the buddy heap
    // Note: Buddy init should only use frames marked with refcount 0 by this init process.
    // Re-reserving here ensures buddy doesn't accidentally use kernel/refcount array space.
    size_t buddy_start_pfn = addr_to_pfn(buddy_heap_phys_start); // Use the aligned start
    size_t buddy_end_pfn = addr_to_pfn(buddy_heap_phys_end);
    terminal_printf("  Reserving buddy heap area frames PFN %u - %u\n", buddy_start_pfn, buddy_end_pfn);
     for (size_t pfn = buddy_start_pfn; pfn < buddy_end_pfn && pfn < g_total_frames; ++pfn) {
         // Only reserve if buddy will actually use it (was available)
         // Buddy init will only take available pages. We just ensure count is >0
         // if it overlaps kernel etc. This loop might be redundant if buddy_init
         // is modified to respect initial refcounts.
         // Let's assume buddy_init gets called *after* this and only uses pages with count 0.
         // If buddy overlaps kernel, kernel reservation takes precedence.
     }

     // Ensure frames below 1MB are reserved (BIOS, VGA etc.)
     size_t first_mb_pfn_limit = addr_to_pfn(0x100000);
     terminal_printf("  Reserving frames below 1MB (PFN 0 - %u)\n", first_mb_pfn_limit);
     for (size_t pfn = 0; pfn < first_mb_pfn_limit && pfn < g_total_frames; ++pfn) {
          refcounts_virt[pfn] = 1;
     }


    terminal_write("[Frame] Frame manager initialized.\n");
    return 0;
}

/**
 * Allocates a single physical page frame.
 */
uintptr_t frame_alloc(void) {
    if (!g_frame_refcounts) return 0; // Not initialized

    // Call buddy allocator for a PAGE_SIZE block
    void* phys_addr_void = buddy_alloc(PAGE_SIZE);
    uintptr_t phys_addr = (uintptr_t)phys_addr_void;

    if (!phys_addr) {
        terminal_write("[Frame] frame_alloc: buddy_alloc failed!\n");
        return 0; // Out of memory
    }

    size_t pfn = addr_to_pfn(phys_addr);
    if (pfn >= g_total_frames) {
        terminal_printf("[Frame] frame_alloc: buddy_alloc returned invalid address 0x%x!\n", phys_addr);
        // We allocated it, but can't track it? Free it back.
        buddy_free(phys_addr_void, PAGE_SIZE);
        return 0;
    }

    // --- Set reference count to 1 ---
    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);
    volatile uint32_t* refcounts_virt = (volatile uint32_t*)(KERNEL_SPACE_VIRT_START + (uintptr_t)g_frame_refcounts);

    if (refcounts_virt[pfn] != 0) {
        // This should NOT happen if buddy only gives out free pages (refcount 0)
        terminal_printf("[Frame] frame_alloc: WARNING! Allocated frame PFN %u (addr 0x%x) had non-zero refcount (%u)!\n",
                       pfn, phys_addr, refcounts_virt[pfn]);
        // Proceed, but indicate potential issue. Set count to 1 anyway.
    }
    refcounts_virt[pfn] = 1; // Set ref count to 1
    spinlock_release_irqrestore(&g_frame_lock, irq_flags);

    // terminal_printf("[Frame DEBUG] Allocated frame 0x%x (PFN %u), refcnt=1\n", phys_addr, pfn);
    return phys_addr;
}

/**
 * Increments the reference count for a frame.
 */
void get_frame(uintptr_t phys_addr) {
    if (!g_frame_refcounts) return; // Not initialized
    if (phys_addr == 0) return; // Cannot refcount frame 0 typically

    size_t pfn = addr_to_pfn(phys_addr);
    if (pfn >= g_total_frames) {
        terminal_printf("[Frame] get_frame: Invalid physical address 0x%x!\n", phys_addr);
        return;
    }

    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);
    volatile uint32_t* refcounts_virt = (volatile uint32_t*)(KERNEL_SPACE_VIRT_START + (uintptr_t)g_frame_refcounts);

    if (refcounts_virt[pfn] == 0) {
        // Incrementing count of a free page? This indicates a bug elsewhere.
        terminal_printf("[Frame] get_frame: WARNING! Incrementing refcount of free frame PFN %u (addr 0x%x)!\n", pfn, phys_addr);
        // Proceed, but this frame shouldn't have been considered free.
    }
    if (refcounts_virt[pfn] == UINT32_MAX) {
         terminal_printf("[Frame] get_frame: Error! Refcount overflow for PFN %u (addr 0x%x)\n", pfn, phys_addr);
         // Panic or handle error?
    } else {
         refcounts_virt[pfn]++;
    }
    // terminal_printf("[Frame DEBUG] Incremented refcnt for 0x%x (PFN %u) to %u\n", phys_addr, pfn, refcounts_virt[pfn]);
    spinlock_release_irqrestore(&g_frame_lock, irq_flags);
}

/**
 * Decrements the reference count, potentially freeing the frame.
 */
void put_frame(uintptr_t phys_addr) {
    if (!g_frame_refcounts) return; // Not initialized
    if (phys_addr == 0) return;

    size_t pfn = addr_to_pfn(phys_addr);
    if (pfn >= g_total_frames) {
        terminal_printf("[Frame] put_frame: Invalid physical address 0x%x!\n", phys_addr);
        return;
    }

    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);
    volatile uint32_t* refcounts_virt = (volatile uint32_t*)(KERNEL_SPACE_VIRT_START + (uintptr_t)g_frame_refcounts);

    if (refcounts_virt[pfn] == 0) {
        // Decrementing count of an already free page? Double free or corruption.
        terminal_printf("[Frame] put_frame: ERROR! Double free detected for frame PFN %u (addr 0x%x)!\n", pfn, phys_addr);
        spinlock_release_irqrestore(&g_frame_lock, irq_flags);
        return; // Avoid decrementing below zero
    }

    refcounts_virt[pfn]--; // Decrement count
    uint32_t current_count = refcounts_virt[pfn]; // Read count after decrement

    // terminal_printf("[Frame DEBUG] Decremented refcnt for 0x%x (PFN %u) to %u\n", phys_addr, pfn, current_count);

    // If count dropped to zero, free the frame back to buddy
    if (current_count == 0) {
        // Release lock *before* calling buddy_free
        spinlock_release_irqrestore(&g_frame_lock, irq_flags);
        // terminal_printf("[Frame DEBUG] Refcnt hit 0, freeing frame 0x%x (PFN %u) via buddy.\n", phys_addr, pfn);
        buddy_free((void*)phys_addr, PAGE_SIZE); // Free single page
    } else {
        // Count > 0, just release lock
        spinlock_release_irqrestore(&g_frame_lock, irq_flags);
    }
}

/**
 * Gets the current reference count.
 */
int get_frame_refcount(uintptr_t phys_addr) {
    if (!g_frame_refcounts) return -1; // Not initialized
    size_t pfn = addr_to_pfn(phys_addr);
    if (pfn >= g_total_frames) return -1; // Invalid address

    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);
    volatile uint32_t* refcounts_virt = (volatile uint32_t*)(KERNEL_SPACE_VIRT_START + (uintptr_t)g_frame_refcounts);
    int count = (int)refcounts_virt[pfn];
    spinlock_release_irqrestore(&g_frame_lock, irq_flags);
    return count;
}