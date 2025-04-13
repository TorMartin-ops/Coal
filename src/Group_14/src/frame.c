// src/frame.c

#include "kmalloc_internal.h"
#include "frame.h"
#include "buddy.h"      // Needs buddy_alloc, buddy_free, MIN_BLOCK_SIZE, MAX_ORDER, DEFAULT_ALIGNMENT
#include "paging.h"     // Needs paging_map_range, PAGE_SIZE, KERNEL_SPACE_VIRT_START, PTE_KERNEL_DATA
#include "terminal.h"   // Needs terminal_printf, terminal_write
#include "spinlock.h"   // Needs spinlock_t, spinlock_init, etc.
#include <libc/stdint.h> // Needs UINT32_MAX, SIZE_MAX
#include <string.h>      // Needs memset
#include "types.h"      // Needs uintptr_t, size_t, bool

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

// --- Globals ---
static volatile uint32_t *g_frame_refcounts = NULL; // Array of reference counts (Points to virtual address after mapping)
static uintptr_t g_frame_refcounts_phys = 0;   // Physical address of the refcount array
static size_t g_total_frames = 0;           // Total number of physical frames detected
static uintptr_t g_highest_address = 0;     // Highest physical address detected
static spinlock_t g_frame_lock;             // Lock for the refcount array
// Keep track of the actual size allocated by buddy for the refcount array
static size_t g_refcount_array_alloc_size = 0;

// --- Helpers ---
static inline size_t addr_to_pfn(uintptr_t addr) {
    return addr / PAGE_SIZE;
}
static inline uintptr_t pfn_to_addr(size_t pfn) {
    return pfn * PAGE_SIZE;
}

// Calculates the expected power-of-two size buddy_alloc will return
static size_t get_buddy_allocation_size(size_t total_size) {
    if (total_size == 0) return 0;

    // Determine minimum practical block size considering alignment
    size_t min_practical_block = (MIN_BLOCK_SIZE > DEFAULT_ALIGNMENT) ? MIN_BLOCK_SIZE : DEFAULT_ALIGNMENT;
    if (total_size < min_practical_block) total_size = min_practical_block;

    // Find the next power of 2 >= total_size
    size_t power_of_2 = min_practical_block;
    while (power_of_2 < total_size) {
        // Check for overflow before shifting
        if (power_of_2 > (SIZE_MAX >> 1)) return SIZE_MAX;
        power_of_2 <<= 1;
    }

    // Ensure the size doesn't exceed the maximum manageable block size
    if (power_of_2 > ((size_t)1 << MAX_ORDER)) {
         return SIZE_MAX; // Indicate too large
    }
    return power_of_2;
}
// --- End Replicated Logic ---


// *** Static C helper function to replace the C++ lambda ***
/**
 * @brief Marks a physical memory range as reserved in the refcount array.
 * Decrements the available_count if frames within the range were previously free.
 *
 * @param start_phys Physical start address of the range.
 * @param end_phys Physical end address (exclusive) of the range.
 * @param name Descriptive name for logging purposes.
 * @param available_count Pointer to the counter of available frames (will be decremented).
 */
static void reserve_range_helper(uintptr_t start_phys, uintptr_t end_phys, const char* name, size_t* available_count) {
    // Align start down and end up to encompass full pages
    size_t start_pfn = addr_to_pfn(PAGE_ALIGN_DOWN(start_phys));
    size_t end_pfn = addr_to_pfn(ALIGN_UP(end_phys, PAGE_SIZE)); // Exclusive end

    terminal_printf("    Reserving %s PFNs [%u - %u)\n", name, start_pfn, end_pfn);

    // Check bounds before looping
    if (end_pfn > g_total_frames) {
        terminal_printf("      [Warning] Reservation range for %s exceeds total frames (%u > %u). Clamping.\n", name, end_pfn, g_total_frames);
        end_pfn = g_total_frames;
    }
    if (start_pfn >= g_total_frames) {
         terminal_printf("      [Warning] Reservation start PFN for %s is out of bounds (%u >= %u). Skipping.\n", name, start_pfn, g_total_frames);
         return; // Start is already past the end
    }


    // Lock might be needed here if other threads could modify counts concurrently,
    // but during single-threaded init, it's likely safe without it.
    // Add lock/unlock around the loop if necessary in the future.
    for (size_t pfn = start_pfn; pfn < end_pfn; ++pfn) {
        // Safely check bounds again inside loop just in case (though outer check should suffice)
        // if (pfn >= g_total_frames) break;

        if (g_frame_refcounts[pfn] == 0) {
            if (*available_count > 0) { // Prevent underflow if called incorrectly
                 (*available_count)--; // Decrement if it was previously marked available
            } else {
                 // This indicates a potential logic error if available_count goes negative
                 terminal_printf("      [Warning] available_count tried to go below zero while reserving PFN %u for %s!\n", pfn, name);
            }
        }
        g_frame_refcounts[pfn] = 1; // Mark as reserved (refcount 1)
    }
}
// *** END NEW HELPER ***


/**
 * frame_init
 * Initializes the physical frame reference counting system.
 * PRECONDITIONS: Buddy Allocator MUST be initialized. Paging MUST be active.
 */
int frame_init(struct multiboot_tag_mmap *mmap_tag,
               uintptr_t kernel_phys_start, uintptr_t kernel_phys_end,
               uintptr_t buddy_heap_phys_start, uintptr_t buddy_heap_phys_end)
{
    terminal_write("[Frame] Initializing physical frame manager...\n");
    spinlock_init(&g_frame_lock); // Initialize lock for frame operations
    if (!mmap_tag) {
        terminal_write("  [FATAL] Multiboot memory map tag is NULL!\n");
        return -1;
    }

    // --- Pass 1: Determine Physical Memory Extents ---
    g_highest_address = 0;
    multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
    uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;
    while ((uintptr_t)mmap_entry < mmap_end) {
        if (mmap_tag->entry_size == 0) {
            terminal_write("  [Frame FATAL] MMAP entry size is zero!\n");
            return -1;
        }
        uintptr_t region_end = (uintptr_t)mmap_entry->addr + (uintptr_t)mmap_entry->len;
        if (region_end > g_highest_address) {
            g_highest_address = region_end;
        }
        mmap_entry = (multiboot_memory_map_t *)((uintptr_t)mmap_entry + mmap_tag->entry_size);
    }
    if (g_highest_address == 0) {
        terminal_write("  [FATAL] Could not determine highest physical address.\n");
        return -1;
    }
    g_highest_address = ALIGN_UP(g_highest_address, PAGE_SIZE);
    g_total_frames = addr_to_pfn(g_highest_address);
    if (g_total_frames == 0) {
         terminal_write("  [FATAL] Total number of frames calculated is zero.\n");
         return -1;
    }
    terminal_printf("  Detected highest physical address: 0x%x (%u total frames)\n",
                     g_highest_address, g_total_frames);
    // --- End Pass 1 ---


    // --- Allocate Memory for the Reference Count Array ---
    size_t refcount_array_size_bytes = g_total_frames * sizeof(uint32_t);
    g_refcount_array_alloc_size = get_buddy_allocation_size(refcount_array_size_bytes);
    if (g_refcount_array_alloc_size == SIZE_MAX || g_refcount_array_alloc_size == 0) {
        terminal_printf("  [FATAL] Calculated refcount array size is invalid or too large (%u bytes required).\n", refcount_array_size_bytes);
        return -1;
    }

    terminal_printf("  [Frame] Attempting to allocate %u bytes (requires buddy size %u) for refcount array...\n", refcount_array_size_bytes, g_refcount_array_alloc_size);
    void* refcount_array_phys_ptr = BUDDY_ALLOC(g_refcount_array_alloc_size);

    if (!refcount_array_phys_ptr) {
        terminal_write("  [FATAL] Failed to allocate refcount array using buddy allocator!\n");
        g_refcount_array_alloc_size = 0;
        return -1;
    }
    g_frame_refcounts_phys = (uintptr_t)refcount_array_phys_ptr;
    terminal_printf("  Allocated refcount array at phys 0x%x (buddy block size %u bytes)\n",
                     g_frame_refcounts_phys, g_refcount_array_alloc_size);

    // --- Map the Refcount Array into Kernel Virtual Address Space ---
    uintptr_t refcount_array_vaddr = KERNEL_SPACE_VIRT_START + g_frame_refcounts_phys;
    terminal_printf("  [Frame] Mapping refcount array: Phys 0x%x -> Virt 0x%x (Size %u)\n",
                     g_frame_refcounts_phys, refcount_array_vaddr, g_refcount_array_alloc_size);

    extern uint32_t g_kernel_page_directory_phys; // Get active PD phys address
    if (!g_kernel_page_directory_phys) {
         terminal_write("  [FATAL] Kernel page directory physical address not set globally!\n");
         BUDDY_FREE(refcount_array_phys_ptr);
         g_frame_refcounts_phys = 0; g_refcount_array_alloc_size = 0;
         return -1;
    }

    if (paging_map_range((uint32_t*)g_kernel_page_directory_phys,
                         refcount_array_vaddr, g_frame_refcounts_phys,
                         g_refcount_array_alloc_size, PTE_KERNEL_DATA_FLAGS) != 0)
    {
        terminal_write("  [FATAL] Failed to map refcount array into kernel space!\n");
        BUDDY_FREE(refcount_array_phys_ptr);
        g_frame_refcounts_phys = 0; g_refcount_array_alloc_size = 0;
        return -1;
    }
    g_frame_refcounts = (volatile uint32_t*)refcount_array_vaddr;
    terminal_printf("  Refcount array mapped successfully at virt 0x%x\n", refcount_array_vaddr);

    // --- Initialize Reference Counts ---
    terminal_write("  [Frame] Initializing reference counts...\n");
    size_t available_count = 0; // Local variable to track count during init
    // Mark all initially as reserved (refcount = 1)
    // Access using virtual pointer g_frame_refcounts
    for (size_t i = 0; i < g_total_frames; ++i) {
        g_frame_refcounts[i] = 1;
    }

    // Mark available regions as free (refcount = 0), update available_count
    mmap_entry = mmap_tag->entries; // Reset entry pointer
    while ((uintptr_t)mmap_entry < mmap_end) {
        if (mmap_tag->entry_size == 0) break;
        if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
          uintptr_t region_start = (uintptr_t)mmap_entry->addr;
          uintptr_t region_end = region_start + (uintptr_t)mmap_entry->len;
          size_t first_pfn = addr_to_pfn(ALIGN_UP(region_start, PAGE_SIZE));
          size_t last_pfn = addr_to_pfn(PAGE_ALIGN_DOWN(region_end)); // Exclusive end PFN

          for (size_t pfn = first_pfn; pfn < last_pfn && pfn < g_total_frames; ++pfn) {
              if (g_frame_refcounts[pfn] != 0) { // Count only if changing state from reserved
                   available_count++;
              }
              g_frame_refcounts[pfn] = 0; // Mark as free
          }
        }
        mmap_entry = (multiboot_memory_map_t *)((uintptr_t)mmap_entry + mmap_tag->entry_size);
    }
    terminal_printf("  Marked %u frames as initially available based on memory map.\n", available_count);

    // Mark specific regions as RESERVED (refcount = 1) using the helper function
    terminal_write("  [Frame] Reserving kernel, boot, buddy, refcount, and initial PD regions...\n");

    // Use the C helper function, passing address of available_count
    reserve_range_helper(0x0, 0x100000, "First MB", &available_count);
    reserve_range_helper(kernel_phys_start, kernel_phys_end, "Kernel", &available_count);
    reserve_range_helper(buddy_heap_phys_start, buddy_heap_phys_end, "Buddy Heap", &available_count);
    reserve_range_helper(g_frame_refcounts_phys, g_frame_refcounts_phys + g_refcount_array_alloc_size, "Refcount Array", &available_count);
    if (g_kernel_page_directory_phys != 0) {
         reserve_range_helper(g_kernel_page_directory_phys, g_kernel_page_directory_phys + PAGE_SIZE, "Initial PD", &available_count);
         terminal_write("    (Assuming self-map PT covered by other reservations)\n");
    }

    terminal_printf("  Final available frame count after reservations: %u\n", available_count);
    if (available_count == 0) {
         terminal_write("  [Frame WARNING] No available frames found after reserving critical areas!\n");
    }

    terminal_write("[Frame] Frame manager initialized.\n");
    return 0; // Success
}


// === Functions: frame_alloc, get_frame, put_frame, get_frame_refcount ===

uintptr_t frame_alloc(void) {
    if (!g_frame_refcounts) {
        terminal_write("[Frame] frame_alloc: Called before frame manager fully initialized!\n");
        return 0;
    }
    size_t alloc_size = PAGE_SIZE;
    void* phys_addr_void = BUDDY_ALLOC(alloc_size);
    uintptr_t phys_addr = (uintptr_t)phys_addr_void;
    if (!phys_addr) {
        terminal_write("[Frame] frame_alloc: buddy_alloc failed (Out of memory?).\n");
        return 0;
    }
    size_t pfn = addr_to_pfn(phys_addr);
    if (pfn >= g_total_frames) {
        terminal_printf("[Frame] frame_alloc: buddy_alloc returned invalid address 0x%x (PFN %u)!\n", phys_addr, pfn);
        BUDDY_FREE(phys_addr_void);
        return 0;
    }
    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);
    if (g_frame_refcounts[pfn] != 0) {
        terminal_printf("[Frame] frame_alloc: WARNING! Allocated frame PFN %u (addr 0x%x) had non-zero refcount (%u)! Overwriting to 1.\n", pfn, phys_addr, g_frame_refcounts[pfn]);
    }
    g_frame_refcounts[pfn] = 1;
    spinlock_release_irqrestore(&g_frame_lock, irq_flags);
    return phys_addr;
}

void get_frame(uintptr_t phys_addr) {
    if (!g_frame_refcounts) { return; }
    if ((phys_addr % PAGE_SIZE) != 0) { phys_addr = PAGE_ALIGN_DOWN(phys_addr); }
    if (phys_addr == 0 || phys_addr >= g_highest_address) { return; }
    size_t pfn = addr_to_pfn(phys_addr);
    if (pfn >= g_total_frames) { return; }

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

void put_frame(uintptr_t phys_addr) {
    if (!g_frame_refcounts) { return; }
    if ((phys_addr % PAGE_SIZE) != 0) { phys_addr = PAGE_ALIGN_DOWN(phys_addr); }
    if (phys_addr == 0 || phys_addr >= g_highest_address) { return; }
    size_t pfn = addr_to_pfn(phys_addr);
    if (pfn >= g_total_frames) { return; }

    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);
    if (g_frame_refcounts[pfn] == 0) {
        terminal_printf("[Frame] put_frame: ERROR! Double free detected for frame PFN %u (addr 0x%x)!\n", pfn, phys_addr);
        spinlock_release_irqrestore(&g_frame_lock, irq_flags);
        return;
    }
    g_frame_refcounts[pfn]--;
    uint32_t current_count = g_frame_refcounts[pfn];
    if (current_count == 0) {
        spinlock_release_irqrestore(&g_frame_lock, irq_flags);
        BUDDY_FREE((void*)phys_addr); // Use macro
    } else {
        spinlock_release_irqrestore(&g_frame_lock, irq_flags);
    }
}

int get_frame_refcount(uintptr_t phys_addr) {
    if (!g_frame_refcounts) return -1;
    if ((phys_addr % PAGE_SIZE) != 0) { phys_addr = PAGE_ALIGN_DOWN(phys_addr); }
    if (phys_addr >= g_highest_address) return -1;
    size_t pfn = addr_to_pfn(phys_addr);
    if (pfn >= g_total_frames) return -1;

    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);
    int count = (int)g_frame_refcounts[pfn];
    spinlock_release_irqrestore(&g_frame_lock, irq_flags);
    return count;
}