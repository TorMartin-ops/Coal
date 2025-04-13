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
int frame_init(struct multiboot_tag_mmap *mmap_tag_virt,
               uintptr_t kernel_phys_start, uintptr_t kernel_phys_end,
               uintptr_t buddy_heap_phys_start, uintptr_t buddy_heap_phys_end)
{
    terminal_write("[Frame] Initializing physical frame manager...\n");
    spinlock_init(&g_frame_lock);

    if (!mmap_tag_virt) { /*...*/ return -1; }
    terminal_printf("  Using MMAP tag at VIRTUAL address: 0x%p\n", mmap_tag_virt);

    // --- Pass 1: Determine Physical Memory Extents (using VIRTUAL mmap_tag_virt) ---
    // (Code for Pass 1 is unchanged - calculates g_highest_address, g_total_frames)
    // ... (Pass 1 code omitted for brevity - assume it works) ...
    g_highest_address = 0; // Reset before loop
    struct multiboot_tag_mmap *mmap_tag = mmap_tag_virt;
    uintptr_t mmap_end_virt = (uintptr_t)mmap_tag + mmap_tag->size;
    multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
    if (mmap_tag->entry_size < sizeof(multiboot_memory_map_t)) return -1;
    while ((uintptr_t)mmap_entry < mmap_end_virt) {
        if ((uintptr_t)mmap_entry + mmap_tag->entry_size > mmap_end_virt) break;
        uintptr_t region_phys_end = (uintptr_t)mmap_entry->addr + (uintptr_t)mmap_entry->len;
        if (region_phys_end > g_highest_address) g_highest_address = region_phys_end;
        mmap_entry = (multiboot_memory_map_t *)((uintptr_t)mmap_entry + mmap_tag->entry_size);
    }
    if (g_highest_address == 0) return -1;
    g_highest_address = ALIGN_UP(g_highest_address, PAGE_SIZE);
    g_total_frames = addr_to_pfn(g_highest_address);
    if (g_total_frames == 0) return -1;
    terminal_printf("  Detected highest physical address: 0x%x (%u total frames)\n", g_highest_address, g_total_frames);


    // --- Allocate Memory for the Reference Count Array using Buddy ---
    size_t refcount_array_size_bytes = g_total_frames * sizeof(uint32_t);
    g_refcount_array_alloc_size = get_buddy_allocation_size(refcount_array_size_bytes);
    if (g_refcount_array_alloc_size == SIZE_MAX || g_refcount_array_alloc_size == 0) { /*...*/ return -1; }
    terminal_printf("  Attempting to allocate %u bytes (buddy size %u) for refcount array...\n", refcount_array_size_bytes, g_refcount_array_alloc_size);
    void* refcount_array_phys_ptr = BUDDY_ALLOC(g_refcount_array_alloc_size);
    if (!refcount_array_phys_ptr) {
        terminal_write("  [FATAL] Failed to allocate refcount array using buddy allocator!\n");
        return -1;
    }
    g_frame_refcounts_phys = (uintptr_t)refcount_array_phys_ptr;
    terminal_printf("  Allocated refcount array at phys 0x%x (buddy block size %u bytes)\n",
                     g_frame_refcounts_phys, g_refcount_array_alloc_size);


    // --- *** FIX: Manually Map the Refcount Array *** ---
    // Instead of calling paging_map_range, do the steps manually here,
    // using buddy_alloc directly for page tables if needed.
    terminal_write("  Manually mapping refcount array into kernel virtual space...\n");
    uintptr_t vaddr_start = KERNEL_SPACE_VIRT_START + g_frame_refcounts_phys;
    uintptr_t paddr_start = g_frame_refcounts_phys;
    size_t    map_size    = g_refcount_array_alloc_size;
    uint32_t  map_flags   = PTE_KERNEL_DATA_FLAGS; // Kernel RW

    uintptr_t vaddr_aligned = PAGE_ALIGN_DOWN(vaddr_start);
    uintptr_t paddr_aligned = PAGE_ALIGN_DOWN(paddr_start);
    // Calculate end address, align up.
    uintptr_t end_vaddr = vaddr_start + map_size;
    uintptr_t end_vaddr_aligned = ALIGN_UP(end_vaddr, PAGE_SIZE);
    if (end_vaddr_aligned < end_vaddr) end_vaddr_aligned = UINTPTR_MAX; // Overflow check

    if (!g_kernel_page_directory_virt) {
        terminal_write("  [FATAL] Kernel PD virtual address is NULL during manual map!\n");
        BUDDY_FREE(refcount_array_phys_ptr); // Free allocated physical memory
        g_frame_refcounts_phys = 0; g_refcount_array_alloc_size = 0;
        return -1;
    }

    terminal_printf("   Manual Map V=[0x%x-0x%x) to P=[0x%x...)\n", vaddr_aligned, end_vaddr_aligned, paddr_aligned);

    uintptr_t current_p = paddr_aligned;
    for (uintptr_t current_v = vaddr_aligned; current_v < end_vaddr_aligned; current_v += PAGE_SIZE) {
        uint32_t pd_idx = PDE_INDEX(current_v);
        uint32_t pt_idx = PTE_INDEX(current_v);

        // Access PDE using kernel's virtual PD pointer
        uint32_t pde = g_kernel_page_directory_virt[pd_idx];
        uintptr_t pt_phys_addr = 0;
        uint32_t* pt_virt_addr = NULL; // Virtual address of the PT (via recursive map)

        if (!(pde & PAGE_PRESENT)) {
            // Page Table not present, allocate one using BUDDY_ALLOC directly
            terminal_printf("    PDE[%d] not present for V=0x%x. Allocating PT frame...\n", pd_idx, current_v);
            void* new_pt_phys_ptr = BUDDY_ALLOC(PAGE_SIZE);
            if (!new_pt_phys_ptr) {
                 terminal_write("    [FATAL] Failed buddy_alloc for page table!\n");
                 // TODO: Need to unmap already mapped pages and free physical frames if this fails mid-way
                 BUDDY_FREE(refcount_array_phys_ptr); // Free the main array allocation
                 return -1;
            }
            pt_phys_addr = (uintptr_t)new_pt_phys_ptr;
            // Zero the new PT frame using physical addr temporarily mapped (if needed) or virtual recursive map
             uint32_t* temp_pt_map = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE)); // Will be valid *after* PDE is set
            // Set the PDE first
            g_kernel_page_directory_virt[pd_idx] = (pt_phys_addr & PAGING_ADDR_MASK) | (map_flags & PDE_FLAGS_FROM_PTE(map_flags)) | PAGE_PRESENT;
            paging_invalidate_page((void*)current_v); // Invalidate TLB for range covered by PDE
            // Now clear using recursive virtual address
            memset(temp_pt_map, 0, PAGE_SIZE);
            pt_virt_addr = temp_pt_map; // Use the recursive address
            terminal_printf("      Allocated PT at phys 0x%x, set PDE[%d]=0x%x, cleared PT.\n", pt_phys_addr, pd_idx, g_kernel_page_directory_virt[pd_idx]);
        } else {
            // Page Table already exists
            pt_phys_addr = pde & PAGING_ADDR_MASK;
            // Access PT via recursive mapping
            pt_virt_addr = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
             // Check if PDE flags need update (e.g., add RW) - Use flags needed for PTE
            uint32_t needed_pde_flags = (map_flags & PDE_FLAGS_FROM_PTE(map_flags)) | PAGE_PRESENT;
            if ((pde & needed_pde_flags) != needed_pde_flags) {
                 g_kernel_page_directory_virt[pd_idx] |= (needed_pde_flags & (PAGE_RW | PAGE_USER)); // Ensure at least RW
                 paging_invalidate_page((void*)current_v);
            }
        }

        // Set the Page Table Entry (PTE)
        if (pt_virt_addr[pt_idx] & PAGE_PRESENT) {
            // Error: Page already mapped within the refcount array's target VA range? Should not happen.
            terminal_printf("    [FATAL] PTE[%d] already present in PT for V=0x%x during refcount map!\n", pt_idx, current_v);
            // TODO: Cleanup logic needed here
            return -1;
        }
        pt_virt_addr[pt_idx] = (current_p & PAGING_ADDR_MASK) | map_flags | PAGE_PRESENT;
        paging_invalidate_page((void*)current_v); // Invalidate specific page TLB

        // Advance physical address for the next page
        current_p += PAGE_SIZE;
         if (current_p < paddr_aligned) { /* Overflow check */ break; }
    }
    // --- End Manual Map ---

    // Set the global virtual pointer to the mapped array
    // Calculate precise start based on alignment offset
    g_frame_refcounts = (volatile uint32_t*)(vaddr_aligned + (paddr_start % PAGE_SIZE));
    terminal_printf("  Refcount array mapped successfully at virt 0x%p (Points to Phys 0x%x)\n",
                     g_frame_refcounts, paddr_start);


    // --- Initialize Reference Counts ---
    // (This part can now safely access g_frame_refcounts via its virtual address)
    terminal_write("  Initializing reference counts...\n");
    size_t available_count = 0;
    for (size_t i = 0; i < g_total_frames; ++i) g_frame_refcounts[i] = 1; // Mark all reserved first
    mmap_entry = mmap_tag_virt->entries; // Use virtual pointer
    while ((uintptr_t)mmap_entry < mmap_end_virt) { // Use virtual end
        if (mmap_tag->entry_size == 0) break;
        if ((uintptr_t)mmap_entry + mmap_tag->entry_size > mmap_end_virt) break;
        if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
            uintptr_t region_phys_start = (uintptr_t)mmap_entry->addr;
            uintptr_t region_phys_end = region_phys_start + (uintptr_t)mmap_entry->len;
            size_t first_pfn = addr_to_pfn(ALIGN_UP(region_phys_start, PAGE_SIZE));
            size_t last_pfn = addr_to_pfn(PAGE_ALIGN_DOWN(region_phys_end));
            for (size_t pfn = first_pfn; pfn < last_pfn && pfn < g_total_frames; ++pfn) {
                 if (g_frame_refcounts[pfn] != 0) { available_count++; }
                 g_frame_refcounts[pfn] = 0; // Mark as free
            }
        }
        mmap_entry = (multiboot_memory_map_t *)((uintptr_t)mmap_entry + mmap_tag->entry_size);
    }
    terminal_printf("  Marked %u frames as initially available based on memory map.\n", available_count);

    // --- Reserve specific regions ---
    terminal_write("  Reserving kernel, boot, buddy, refcount, and initial PD regions...\n");
    reserve_range_helper(0x0, 0x100000, "First MB", &available_count);
    reserve_range_helper(kernel_phys_start, kernel_phys_end, "Kernel", &available_count);
    reserve_range_helper(buddy_heap_phys_start, buddy_heap_phys_end, "Buddy Heap", &available_count);
    reserve_range_helper(g_frame_refcounts_phys, g_frame_refcounts_phys + g_refcount_array_alloc_size, "Refcount Array", &available_count);
    if (g_kernel_page_directory_phys != 0) {
         reserve_range_helper(g_kernel_page_directory_phys, g_kernel_page_directory_phys + PAGE_SIZE, "Initial PD", &available_count);
    }
    // Reserve frames used for Page Tables during the manual mapping
    // TODO: Need to track PT frames allocated manually and reserve them here.

    terminal_printf("  Final available frame count after reservations: %u\n", available_count);
    if (available_count == 0) { /* Warning */ }

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