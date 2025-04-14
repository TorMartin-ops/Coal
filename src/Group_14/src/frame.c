// src/frame.c

// Includes remain the same...
#include "kmalloc_internal.h"
#include "frame.h"
#include "buddy.h"
#include "paging.h"
#include "terminal.h"
#include "spinlock.h"
#include <libc/stdint.h>
#include <stdint.h>
#include <string.h>
#include "types.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

// Globals remain the same...
static volatile uint32_t *g_frame_refcounts = NULL;
static uintptr_t g_frame_refcounts_phys = 0;
static size_t g_total_frames = 0;
static uintptr_t g_highest_address = 0;
static spinlock_t g_frame_lock;
static size_t g_refcount_array_alloc_size = 0;

// Helpers remain the same...
static inline size_t addr_to_pfn(uintptr_t addr) {
    if (PAGE_SIZE == 0) return 0;
    if (addr == UINTPTR_MAX) { return (UINTPTR_MAX / PAGE_SIZE); }
    return addr / PAGE_SIZE;
}
static inline uintptr_t pfn_to_addr(size_t pfn) {
     if (pfn > (UINTPTR_MAX / PAGE_SIZE)) { return UINTPTR_MAX; }
    return pfn * PAGE_SIZE;
}
static size_t get_buddy_allocation_size(size_t total_size) {
    if (total_size == 0) return 0;
    size_t min_practical_block = (MIN_BLOCK_SIZE > DEFAULT_ALIGNMENT) ? MIN_BLOCK_SIZE : DEFAULT_ALIGNMENT;
    if (total_size < min_practical_block) total_size = min_practical_block;
    size_t power_of_2 = min_practical_block;
    while (power_of_2 < total_size) {
        if (power_of_2 > (SIZE_MAX >> 1)) return SIZE_MAX;
        power_of_2 <<= 1;
    }
    if (power_of_2 > ((size_t)1 << MAX_ORDER)) { return SIZE_MAX; }
    return power_of_2;
}
// --- End Helpers ---


// --- Forward Declaration for Static Helper --- <<< ADDED
static void mark_reserved_range(uintptr_t start, uintptr_t end, const char* name);
// --- End Forward Declaration ---


/**
 * frame_init
 * Initializes the physical frame reference counting system.
 * PRECONDITIONS: Buddy Allocator MUST be initialized. Paging MUST be active.
 */
int frame_init(struct multiboot_tag_mmap *mmap_tag_virt,
               uintptr_t kernel_phys_start, uintptr_t kernel_phys_end,
               uintptr_t buddy_heap_phys_start, uintptr_t buddy_heap_phys_end)
{
    terminal_write("[Frame] Initializing physical frame manager...\n");
    spinlock_init(&g_frame_lock);

    if (!mmap_tag_virt) { /* ... fatal error ... */ return -1; }
    terminal_printf("  Using MMAP tag at VIRTUAL address: 0x%p\n", mmap_tag_virt);

    // --- Pass 1: Determine Physical Memory Extents ---
    // (Code unchanged - assumes it works correctly now)
    g_highest_address = 0;
    struct multiboot_tag_mmap *mmap_tag = mmap_tag_virt;
    uintptr_t mmap_end_virt = (uintptr_t)mmap_tag + mmap_tag->size;
    multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
    if (mmap_tag->entry_size < sizeof(multiboot_memory_map_t)) return -1;
    while ((uintptr_t)mmap_entry < mmap_end_virt) {
        if ((uintptr_t)mmap_entry + mmap_tag->entry_size > mmap_end_virt) break;
        uint64_t region_start_64 = mmap_entry->addr;
        uint64_t region_len_64 = mmap_entry->len;
        uint64_t region_phys_end_64 = region_start_64 + region_len_64;
        if (region_phys_end_64 < region_start_64) region_phys_end_64 = UINT64_MAX;
        uintptr_t region_phys_end_ptr = (uintptr_t)region_phys_end_64;
        if (region_phys_end_64 > UINTPTR_MAX) region_phys_end_ptr = UINTPTR_MAX;
        if (region_phys_end_ptr > g_highest_address) g_highest_address = region_phys_end_ptr;
        mmap_entry = (multiboot_memory_map_t *)((uintptr_t)mmap_entry + mmap_tag->entry_size);
    }
    if (g_highest_address == 0) return -1;
    if (g_highest_address > UINTPTR_MAX - PAGE_SIZE + 1) {
        g_highest_address = UINTPTR_MAX;
        terminal_write("  [Warning] Highest physical address close to max, clamping to UINTPTR_MAX.\n");
    } else {
        g_highest_address = ALIGN_UP(g_highest_address, PAGE_SIZE);
    }
    if (g_highest_address == 0) return -1;
    g_total_frames = addr_to_pfn(g_highest_address);
    if (g_total_frames == 0) return -1;
    terminal_printf("  Detected highest physical address (aligned): 0x%x (%u total frames)\n", g_highest_address, g_total_frames);

    // --- Allocate Memory for the Reference Count Array ---
    // (Code unchanged)
    size_t refcount_array_size_bytes = g_total_frames * sizeof(uint32_t);
    if (g_total_frames > SIZE_MAX / sizeof(uint32_t)) return -1;
    g_refcount_array_alloc_size = get_buddy_allocation_size(refcount_array_size_bytes);
    if (g_refcount_array_alloc_size == SIZE_MAX || g_refcount_array_alloc_size == 0) return -1;
    terminal_printf("  Attempting to allocate %u bytes (buddy size %u) for refcount array...\n", refcount_array_size_bytes, g_refcount_array_alloc_size);
    void* refcount_array_phys_ptr = BUDDY_ALLOC(g_refcount_array_alloc_size);
    if (!refcount_array_phys_ptr) return -1;
    g_frame_refcounts_phys = (uintptr_t)refcount_array_phys_ptr;
    terminal_printf("  Allocated refcount array at phys 0x%x (buddy block size %u bytes)\n", g_frame_refcounts_phys, g_refcount_array_alloc_size);

    // --- Map the Refcount Array into Kernel Virtual Space ---
    // (Code unchanged)
    terminal_write("  Mapping refcount array into kernel virtual space using paging_map_range...\n");
    uintptr_t vaddr_start = KERNEL_SPACE_VIRT_START + g_frame_refcounts_phys;
    uintptr_t paddr_start = g_frame_refcounts_phys;
    size_t    map_size    = g_refcount_array_alloc_size;
    uint32_t  map_flags   = PTE_KERNEL_DATA_FLAGS;
    uintptr_t vaddr_aligned_start = PAGE_ALIGN_DOWN(vaddr_start);
    uintptr_t paddr_aligned_start = PAGE_ALIGN_DOWN(paddr_start);
    size_t map_aligned_size = ALIGN_UP(vaddr_start + map_size, PAGE_SIZE) - vaddr_aligned_start;
     if (map_aligned_size < map_size) { BUDDY_FREE(refcount_array_phys_ptr); return -1; }
    if (paging_map_range((uint32_t*)g_kernel_page_directory_phys, vaddr_aligned_start, paddr_aligned_start, map_aligned_size, map_flags) != 0) {
        BUDDY_FREE(refcount_array_phys_ptr); return -1;
    }
    g_frame_refcounts = (volatile uint32_t*)(vaddr_aligned_start + (paddr_start % PAGE_SIZE));
    terminal_printf("  Refcount array mapped successfully at virt 0x%p (Points to Phys 0x%x)\n", g_frame_refcounts, paddr_start);


    // --- REVISED: Initialize Reference Counts ---
    // (Logic remains same as previous attempt, uses helper function now)
    terminal_write("  Initializing reference counts (marking all free, then reserving known regions)...\n");
    size_t available_count = 0;

    // Step 1: Mark ALL frames as potentially available (refcount = 0).
    terminal_printf("   Zeroing refcount array (size: %u bytes) at virt 0x%p...\n", refcount_array_size_bytes, g_frame_refcounts);
    memset((void*)g_frame_refcounts, 0, refcount_array_size_bytes);
    terminal_write("   Refcount array zeroed.\n");

    // Step 2: Explicitly mark known RESERVED regions by setting refcount = 1.
    terminal_write("   Marking reserved regions...\n");
    // Calculate aligned ranges
    uintptr_t kernel_phys_aligned_start     = PAGE_ALIGN_DOWN(kernel_phys_start);
    uintptr_t kernel_phys_aligned_end       = ALIGN_UP(kernel_phys_end, PAGE_SIZE);
    uintptr_t buddy_heap_phys_aligned_start = PAGE_ALIGN_DOWN(buddy_heap_phys_start);
    uintptr_t buddy_heap_phys_aligned_end   = ALIGN_UP(buddy_heap_phys_end, PAGE_SIZE);
    uintptr_t refcount_phys_aligned_start   = PAGE_ALIGN_DOWN(g_frame_refcounts_phys);
    uintptr_t refcount_phys_aligned_end     = ALIGN_UP(g_frame_refcounts_phys + g_refcount_array_alloc_size, PAGE_SIZE);
    uintptr_t pd_phys_aligned_start         = 0;
    uintptr_t pd_phys_aligned_end           = 0;
    if (g_kernel_page_directory_phys != 0) {
        pd_phys_aligned_start = PAGE_ALIGN_DOWN(g_kernel_page_directory_phys);
        pd_phys_aligned_end = ALIGN_UP(g_kernel_page_directory_phys + PAGE_SIZE, PAGE_SIZE);
    }
    // Clamp end addresses if they overflowed
    if (kernel_phys_aligned_end < kernel_phys_end) kernel_phys_aligned_end = UINTPTR_MAX;
    if (buddy_heap_phys_aligned_end < buddy_heap_phys_end) buddy_heap_phys_aligned_end = UINTPTR_MAX;
    if (refcount_phys_aligned_end < (g_frame_refcounts_phys + g_refcount_array_alloc_size)) refcount_phys_aligned_end = UINTPTR_MAX;
    if (pd_phys_aligned_end < (g_kernel_page_directory_phys + PAGE_SIZE) && g_kernel_page_directory_phys != 0) pd_phys_aligned_end = UINTPTR_MAX;

    // Print the ranges being reserved
    terminal_printf("    Calculated Reserved Ranges (Physical):\n");
    terminal_printf("      First MB:      [0x%x - 0x%x)\n", (uintptr_t)0x0, (uintptr_t)0x100000);
    terminal_printf("      Kernel:        [0x%x - 0x%x)\n", kernel_phys_aligned_start, kernel_phys_aligned_end);
    terminal_printf("      Buddy Heap:    [0x%x - 0x%x)\n", buddy_heap_phys_aligned_start, buddy_heap_phys_aligned_end);
    terminal_printf("      Refcount Array:[0x%x - 0x%x)\n", refcount_phys_aligned_start, refcount_phys_aligned_end);
    if (pd_phys_aligned_end > 0) {
        terminal_printf("      Initial PD:    [0x%x - 0x%x)\n", pd_phys_aligned_start, pd_phys_aligned_end);
    }

    // Call the helper function to mark ranges
    mark_reserved_range(0x0, 0x100000, "First MB");
    mark_reserved_range(kernel_phys_aligned_start, kernel_phys_aligned_end, "Kernel");
    mark_reserved_range(buddy_heap_phys_aligned_start, buddy_heap_phys_aligned_end, "Buddy Heap");
    mark_reserved_range(refcount_phys_aligned_start, refcount_phys_aligned_end, "Refcount Array");
    if (pd_phys_aligned_end > 0) {
        mark_reserved_range(pd_phys_aligned_start, pd_phys_aligned_end, "Initial PD");
    }

    // Step 3: Iterate through MMAP Available regions AGAIN to count available frames.
    terminal_write("   Counting available frames based on MMAP and reservations...\n");
    available_count = 0; // Reset counter
    mmap_entry = mmap_tag_virt->entries; // Reset MMAP pointer
    while ((uintptr_t)mmap_entry < mmap_end_virt) {
        if (mmap_tag->entry_size == 0) break;
        if ((uintptr_t)mmap_entry + mmap_tag->entry_size > mmap_end_virt) break;

        if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
            uint64_t region_phys_start_64 = mmap_entry->addr;
            uint64_t region_len_64 = mmap_entry->len;
            uint64_t region_phys_end_64 = region_phys_start_64 + region_len_64;
             if (region_phys_end_64 < region_phys_start_64) region_phys_end_64 = UINT64_MAX;

            uintptr_t first_usable_addr = ALIGN_UP((uintptr_t)region_phys_start_64, PAGE_SIZE);
            uintptr_t last_usable_addr = PAGE_ALIGN_DOWN((uintptr_t)region_phys_end_64);

            // *** Debug print for MMAP region being processed ***
            // terminal_printf("    Processing MMAP Available Region: Phys [0x%llx - 0x%llx) -> Usable PFNs [%u - %u)\n",
            //                 region_phys_start_64, region_phys_end_64,
            //                 addr_to_pfn(first_usable_addr), addr_to_pfn(last_usable_addr));


             if (first_usable_addr < last_usable_addr) {
                 size_t first_pfn = addr_to_pfn(first_usable_addr);
                 size_t last_pfn = addr_to_pfn(last_usable_addr); // Exclusive end PFN

                 for (size_t pfn = first_pfn; pfn < last_pfn && pfn < g_total_frames; ++pfn) {
                     // Only count frames that were NOT marked as reserved in Step 2
                     if (g_frame_refcounts[pfn] == 0) {
                         available_count++;
                         // *** Debug print for frame counted as available ***
                         // if (available_count < 10 || available_count % 1000 == 0) {
                         //    terminal_printf("      PFN %u (0x%x): Counted as AVAILABLE\n", pfn, pfn_to_addr(pfn));
                         // }
                     } else {
                         // *** Debug print for frame skipped (already marked reserved) ***
                         // if (pfn < first_pfn + 5 || pfn % 512 == 0) {
                         //     terminal_printf("      PFN %u (0x%x): Skipping count (already reserved)\n", pfn, pfn_to_addr(pfn));
                         // }
                     }
                 }
             }
        }
        mmap_entry = (multiboot_memory_map_t *)((uintptr_t)mmap_entry + mmap_tag->entry_size);
    }
    // --- End REVISED Initialization ---


    terminal_printf("  Final available frame count after initialisation: %u\n", available_count);
    if (available_count == 0 && g_total_frames > 1024) {
        terminal_write("  [WARNING] No physical frames reported available after reservations!\n");
     } else if (available_count == 0) {
         terminal_write("  [WARNING] Frame count is zero, system might be low on memory or reservations overlap significantly.\n");
     }

    terminal_write("[Frame] Frame manager initialized.\n");
    return 0; // Success
}


// === Functions: frame_alloc, get_frame, put_frame, get_frame_refcount ===
// (Unchanged)
// ... (rest of file remains the same) ...
uintptr_t frame_alloc(void) { /* ... */ }
void get_frame(uintptr_t phys_addr) { /* ... */ }
void put_frame(uintptr_t phys_addr) { /* ... */ }
int get_frame_refcount(uintptr_t phys_addr) { /* ... */ }


// --- Definition of Static Helper Function ---
// (Moved from within frame_init to file scope)
static void mark_reserved_range(uintptr_t start, uintptr_t end, const char* name) {
    size_t start_pfn = addr_to_pfn(start);
    size_t end_pfn = addr_to_pfn(end); // End PFN is exclusive
    terminal_printf("      Reserving %s PFNs [%u - %u) Addr [0x%x - 0x%x)\n", name, start_pfn, end_pfn, start, end);
    if (end_pfn > g_total_frames) end_pfn = g_total_frames; // Clamp end
    if (start_pfn >= end_pfn) return; // Skip if range is invalid or empty
    for (size_t pfn = start_pfn; pfn < end_pfn; ++pfn) {
        if (pfn < g_total_frames) { // Ensure index is within bounds
            g_frame_refcounts[pfn] = 1; // Mark as reserved
        }
    }
}
// --- End Static Helper Function Definition ---