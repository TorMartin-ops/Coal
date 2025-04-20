// src/frame.c

// Includes (ensure buddy.h and paging.h are high up)
#include "paging.h"           // For PAGE_SIZE, KERNEL_SPACE_VIRT_START, PAGE_ALIGN_DOWN, etc.
#include "buddy.h"            // For buddy_alloc_raw, buddy_free_raw, MIN_ORDER, MAX_ORDER
#include "kmalloc_internal.h" // For ALIGN_UP, DEFAULT_ALIGNMENT
#include "frame.h"
#include "terminal.h"
#include "spinlock.h"
#include <libc/stdint.h>      // For SIZE_MAX, uintXX_t, UINTPTR_MAX, UINT64_MAX
#include <string.h>           // For memset
#include "types.h"            // For uintptr_t, size_t, bool
#include "multiboot2.h"       // For memory map parsing

// Make sure PAGE_SIZE is defined via paging.h
#ifndef PAGE_SIZE
#error "PAGE_SIZE not defined! Include paging.h"
#endif

// --- Define PAGE_ORDER based on PAGE_SIZE ---
#if (PAGE_SIZE == 4096)
#define PAGE_ORDER 12
#elif (PAGE_SIZE == 8192)
#define PAGE_ORDER 13
#else
#error "Unsupported PAGE_SIZE for buddy allocator PAGE_ORDER calculation."
#endif

// Use MIN_ORDER and MAX_ORDER from buddy.h
#ifndef MIN_ORDER
#error "MIN_ORDER is not defined (include buddy.h)"
#endif
#ifndef MAX_ORDER
#error "MAX_ORDER is not defined (include buddy.h)"
#endif
#define MIN_INTERNAL_ORDER MIN_ORDER
#define MIN_BLOCK_SIZE_INTERNAL (1 << MIN_INTERNAL_ORDER)

//-------------------------------------------------------------------
// Panic & Assert macros
//-------------------------------------------------------------------
#ifndef FRAME_PANIC
#define FRAME_PANIC(msg) do { \
    terminal_printf("\n[FRAME PANIC] %s at %s:%d. System Halted.\n", msg, __FILE__, __LINE__); \
    while (1) { asm volatile("cli; hlt"); } \
} while(0)
#endif

#ifndef FRAME_ASSERT
#define FRAME_ASSERT(condition, msg) do { \
    if (!(condition)) { \
        terminal_printf("\n[FRAME ASSERT FAILED] %s at %s:%d\n", msg, __FILE__, __LINE__); \
        FRAME_PANIC("Assertion failed"); \
    } \
} while(0)
#endif

//-------------------------------------------------------------------
// Globals
//-------------------------------------------------------------------
static volatile uint32_t *g_frame_refcounts = NULL;  // VIRTUAL address after mapping
static uintptr_t g_frame_refcounts_phys = 0;         // PHYSICAL address of the allocation
static size_t g_total_frames = 0;
static uintptr_t g_highest_address = 0;              // Highest physical address detected
static spinlock_t g_frame_lock;                      // Lock for the frame allocator itself
static size_t g_refcount_array_alloc_size = 0;       // Buddy-allocated size
extern uintptr_t g_kernel_page_directory_phys;       // from paging code (declared extern in paging.h)

//-------------------------------------------------------------------
// Helper Functions
//-------------------------------------------------------------------
// Convert address <-> PFN
static inline size_t addr_to_pfn(uintptr_t addr) {
    FRAME_ASSERT(PAGE_SIZE != 0, "PAGE_SIZE is zero in addr_to_pfn");
    if (addr == UINTPTR_MAX) { return (UINTPTR_MAX / PAGE_SIZE); }
    return addr / PAGE_SIZE;
}
static inline uintptr_t pfn_to_addr(size_t pfn) {
    if (pfn > (UINTPTR_MAX / PAGE_SIZE)) { return UINTPTR_MAX; }
    return (pfn * PAGE_SIZE);
}

// Basic power-of-2 buddy size rounding (must match buddy.c)
static size_t get_buddy_allocation_size(size_t total_size) {
    if (total_size == 0) return 0;
    const size_t min_block = (1 << MIN_ORDER);
    const size_t max_block = (1 << MAX_ORDER);
    size_t min_practical_block = (min_block > DEFAULT_ALIGNMENT) ? min_block : DEFAULT_ALIGNMENT;

    if (total_size < min_practical_block) total_size = min_practical_block;
    size_t power_of_2 = min_practical_block;
    while (power_of_2 < total_size) {
        if (power_of_2 > (SIZE_MAX >> 1)) return SIZE_MAX;
        power_of_2 <<= 1;
    }
    if (power_of_2 > max_block) return SIZE_MAX;
    return power_of_2;
}

// Reserve a range
static void mark_reserved_range(uintptr_t start, uintptr_t end, const char* name) {
    FRAME_ASSERT(g_frame_refcounts != NULL, "Refcount array not yet allocated");
    uintptr_t aligned_start = PAGE_ALIGN_DOWN(start);
    uintptr_t aligned_end   = ALIGN_UP(end, PAGE_SIZE);
    if (aligned_end < end) aligned_end = UINTPTR_MAX;
    size_t start_pfn = addr_to_pfn(aligned_start);
    size_t end_pfn   = addr_to_pfn(aligned_end);
    terminal_printf("      Reserving %s PFNs [%u - %u) Addr [0x%x - 0x%x)\n",
                    name, (unsigned)start_pfn, (unsigned)end_pfn,
                    (unsigned)aligned_start, (unsigned)aligned_end);
    if (end_pfn > g_total_frames) end_pfn = g_total_frames;
    if (start_pfn >= end_pfn) return;
    for (size_t pfn = start_pfn; pfn < end_pfn; ++pfn) {
        // Check bounds just in case PFN calculation had issues
        if (pfn < g_total_frames) {
             g_frame_refcounts[pfn] = 1;
        } else {
            terminal_printf("[Mark Reserved] Warning: Calculated PFN %lu exceeds g_total_frames %lu\n", (unsigned long)pfn, (unsigned long)g_total_frames);
        }
    }
}

//-------------------------------------------------------------------
// frame_init
//-------------------------------------------------------------------
int frame_init(struct multiboot_tag_mmap *mmap_tag_virt,
               uintptr_t kernel_phys_start, uintptr_t kernel_phys_end,
               uintptr_t buddy_heap_phys_start, uintptr_t buddy_heap_phys_end)
{
    terminal_write("[Frame] Initializing physical frame manager (World Class Edition v4)...\n");
    spinlock_init(&g_frame_lock);

    // 1. Validate multiboot map
    if (!mmap_tag_virt) { FRAME_PANIC("Multiboot MMAP tag virtual address is NULL!"); }
    if (mmap_tag_virt->size < (sizeof(struct multiboot_tag_mmap) + sizeof(multiboot_memory_map_t)) ||
        mmap_tag_virt->entry_size < sizeof(multiboot_memory_map_t)) {
        FRAME_PANIC("Multiboot MMAP tag structure invalid (size or entry_size)");
    }
    terminal_printf("  Using MMAP tag at VIRTUAL address: 0x%p\n", mmap_tag_virt);

    // 2. Determine total physical memory span
    g_highest_address = 0;
    uintptr_t mmap_end_virt = (uintptr_t)mmap_tag_virt + mmap_tag_virt->size;
    multiboot_memory_map_t *mmap_entry = mmap_tag_virt->entries;
    while ((uintptr_t)mmap_entry < mmap_end_virt) {
        if (mmap_tag_virt->entry_size == 0) { break; } // Safety break
        if ((uintptr_t)mmap_entry + mmap_tag_virt->entry_size > mmap_end_virt) { break; } // Bounds check

        uint64_t r_start64 = mmap_entry->addr; uint64_t r_len64 = mmap_entry->len;
        uint64_t r_end64 = r_start64 + r_len64; if (r_end64 < r_start64) r_end64 = UINT64_MAX;
        uintptr_t r_phys_end_ptr = (r_end64 > UINTPTR_MAX) ? UINTPTR_MAX : (uintptr_t)r_end64;
        if (r_phys_end_ptr > g_highest_address) g_highest_address = r_phys_end_ptr;
        mmap_entry = (multiboot_memory_map_t *)((uintptr_t)mmap_entry + mmap_tag_virt->entry_size);
    }

    if (g_highest_address == 0) { FRAME_PANIC("Failed to determine highest physical address from MMAP!"); }
    uintptr_t aligned_highest = ALIGN_UP(g_highest_address, PAGE_SIZE); if (aligned_highest == 0 && g_highest_address > 0) aligned_highest = UINTPTR_MAX;
    g_highest_address = aligned_highest;
    g_total_frames = addr_to_pfn(g_highest_address); if (g_total_frames == 0) { FRAME_PANIC("Total frame count is zero!"); }
    terminal_printf(" Detected highest physical address (aligned): %#lx (%lu total frames)\n", (unsigned long)g_highest_address, (unsigned long)g_total_frames);
    // 3. Allocate physical memory for refcount array
    if (g_total_frames > (SIZE_MAX / sizeof(uint32_t))) { FRAME_PANIC("Refcount array size calculation overflows size_t!"); }
    size_t refcount_array_size_bytes = g_total_frames * sizeof(uint32_t);
    g_refcount_array_alloc_size = get_buddy_allocation_size(refcount_array_size_bytes);
    if (g_refcount_array_alloc_size == SIZE_MAX || g_refcount_array_alloc_size == 0) { FRAME_PANIC("Failed to determine valid buddy block size for refcount array!"); }
    terminal_printf("  Attempting to allocate %u bytes (fits in buddy block size %u) for refcount array...\n",
                    (unsigned)refcount_array_size_bytes, (unsigned)g_refcount_array_alloc_size);

    // Call buddy_alloc_raw (determine order first)
    int required_order = -1;
    size_t temp_size   = (1 << MIN_ORDER); int temp_order = MIN_ORDER;
    while (temp_order <= MAX_ORDER) { if (temp_size >= g_refcount_array_alloc_size) { required_order = temp_order; break; } if (temp_size > (SIZE_MAX >> 1)) break; temp_size <<= 1; temp_order++; }
    if (required_order == -1) { FRAME_PANIC("Could not determine required order for refcount array"); }

    // Use the raw allocation function
    void* refcount_array_virt_ptr = buddy_alloc_raw(required_order);
    if (!refcount_array_virt_ptr) { FRAME_PANIC("buddy_alloc_raw failed for refcount array!"); }

    // Calculate Physical Address & Verify Alignment
    g_frame_refcounts_phys = (uintptr_t)refcount_array_virt_ptr - KERNEL_SPACE_VIRT_START;
    terminal_printf("  Allocated refcount array at VIRT 0x%p, corresponding PHYS 0x%x (Size: %u)\n",
                    refcount_array_virt_ptr, (unsigned)g_frame_refcounts_phys, (unsigned)g_refcount_array_alloc_size);
    if ((g_frame_refcounts_phys % PAGE_SIZE) != 0) { FRAME_PANIC("Physical address for refcount array is not page-aligned! (Buddy internal error)"); }
    else { terminal_printf("  Physical address 0x%x is confirmed page-aligned (%u).\n", (unsigned)g_frame_refcounts_phys, PAGE_SIZE); }

    // 4. Use the VIRTUAL address for access
    g_frame_refcounts = (volatile uint32_t*)refcount_array_virt_ptr;
    terminal_printf("  Using VIRT address 0x%p for refcount array access.\n", g_frame_refcounts);

    // 5. Initialize Reference Counts
    terminal_write("  Initializing reference counts (zeroing, then marking reserved)...\n");
    terminal_printf("   Zeroing refcount array (%u bytes) at virt 0x%p...\n", (unsigned)refcount_array_size_bytes, g_frame_refcounts);
    memset((void*)g_frame_refcounts, 0, refcount_array_size_bytes);
    terminal_write("   Refcount array zeroed.\n");

    terminal_write("   Marking reserved physical regions (excluding buddy heap/refcount array itself)...\n");
    uintptr_t kernel_phys_aligned_start = PAGE_ALIGN_DOWN(kernel_phys_start); /*...*/ uintptr_t kernel_phys_aligned_end = ALIGN_UP(kernel_phys_end, PAGE_SIZE);
    uintptr_t pd_phys_aligned_start = 0; /*...*/ uintptr_t pd_phys_aligned_end = 0; if (g_kernel_page_directory_phys != 0) { pd_phys_aligned_start = PAGE_ALIGN_DOWN(g_kernel_page_directory_phys); pd_phys_aligned_end = ALIGN_UP(g_kernel_page_directory_phys + PAGE_SIZE, PAGE_SIZE); }
    if (kernel_phys_aligned_end < kernel_phys_end) kernel_phys_aligned_end = UINTPTR_MAX; /*...*/ if (pd_phys_aligned_end < (g_kernel_page_directory_phys + PAGE_SIZE) && g_kernel_page_directory_phys != 0) pd_phys_aligned_end = UINTPTR_MAX;
    terminal_printf("    Physical Ranges Marked Reserved:\n");
    mark_reserved_range(0x0, 0x100000, "First MB");
    mark_reserved_range(kernel_phys_aligned_start, kernel_phys_aligned_end, "Kernel Image");
    if (pd_phys_aligned_end > 0) { mark_reserved_range(pd_phys_aligned_start, pd_phys_aligned_end, "Initial PD"); }
    // Buddy Heap and Refcount Array are NOT marked reserved here

    // 6. Final Sanity Check - Count available frames
    terminal_write("  Verifying available frame count based on MMAP and reservations...\n");
    size_t available_count = 0;
    mmap_entry = mmap_tag_virt->entries;
    while ((uintptr_t)mmap_entry < mmap_end_virt) {
        // --- FIX: Separate checks with curly braces for clarity ---
        if (mmap_tag_virt->entry_size == 0) { break; }
        if ((uintptr_t)mmap_entry + mmap_tag_virt->entry_size > mmap_end_virt) { break; }
        // --- End Fix ---
        if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
            uint64_t r_start64 = mmap_entry->addr; uint64_t r_end64 = r_start64 + mmap_entry->len; if (r_end64 < r_start64) r_end64 = UINT64_MAX;
            uintptr_t first_addr = ALIGN_UP((uintptr_t)r_start64, PAGE_SIZE); uintptr_t last_addr = PAGE_ALIGN_DOWN((uintptr_t)r_end64);
            if (first_addr < last_addr) {
                size_t first_pfn = addr_to_pfn(first_addr); size_t last_pfn = addr_to_pfn(last_addr);
                for (size_t pfn = first_pfn; (pfn < last_pfn) && (pfn < g_total_frames); ++pfn) {
                     // Check bounds before accessing array
                     if (pfn < g_total_frames && g_frame_refcounts[pfn] == 0) {
                         available_count++;
                     }
                 }
            }
        }
        mmap_entry = (multiboot_memory_map_t *)((uintptr_t)mmap_entry + mmap_tag_virt->entry_size);
    }
    terminal_printf("  Final available frame count: %u\n", (unsigned)available_count);
    if (available_count == 0 && g_total_frames > 256) { terminal_write("  [WARNING] Zero available frames detected after initialization!\n"); }

    terminal_write("[Frame] Frame manager initialization complete.\n");
    return 0; // success
}

//-------------------------------------------------------------------
// frame_alloc, get_frame, put_frame, get_frame_refcount
//-------------------------------------------------------------------
uintptr_t frame_alloc(void) {
    // Use defined PAGE_ORDER
    int page_req_order = PAGE_ORDER;

    // Call buddy_alloc_raw (handles locking internally)
    void* block_virt = buddy_alloc_raw(page_req_order);
    if (!block_virt) { terminal_write("[Frame Alloc] Buddy allocation failed!\n"); return 0; }

    uintptr_t block_phys = (uintptr_t)block_virt - KERNEL_SPACE_VIRT_START;
    FRAME_ASSERT((block_phys % PAGE_SIZE) == 0, "frame_alloc got non-page-aligned phys address from buddy_alloc_raw");

    size_t pfn = addr_to_pfn(block_phys);
    FRAME_ASSERT(pfn < g_total_frames, "PFN out of range in frame_alloc");

    // Update refcount (use frame lock)
    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);

    terminal_printf("[Frame Alloc DEBUG] Checking PFN %u (Phys: 0x%x). Current refcount = %u\n",
                    (unsigned)pfn, (unsigned)block_phys, (unsigned)g_frame_refcounts[pfn]);

    FRAME_ASSERT(g_frame_refcounts[pfn] == 0, "Allocating frame that already has non-zero refcount!");

    g_frame_refcounts[pfn] = 1; // Mark as allocated (refcount 1)
    spinlock_release_irqrestore(&g_frame_lock, irq_flags);

    return block_phys;
}

void get_frame(uintptr_t phys_addr) {
    if ((phys_addr % PAGE_SIZE) != 0) { phys_addr = PAGE_ALIGN_DOWN(phys_addr); }
    size_t pfn = addr_to_pfn(phys_addr);
    if (pfn >= g_total_frames) { return; }
    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);
    FRAME_ASSERT(g_frame_refcounts[pfn] > 0, "get_frame called on unallocated frame!");
    FRAME_ASSERT(g_frame_refcounts[pfn] < UINT32_MAX, "Frame reference count overflow!");
    g_frame_refcounts[pfn]++;
    spinlock_release_irqrestore(&g_frame_lock, irq_flags);
}

void put_frame(uintptr_t phys_addr) {
    if ((phys_addr % PAGE_SIZE) != 0) { phys_addr = PAGE_ALIGN_DOWN(phys_addr); }
    size_t pfn = addr_to_pfn(phys_addr);
    if (pfn >= g_total_frames) { return; }

    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);
    if (g_frame_refcounts[pfn] == 0) {
        spinlock_release_irqrestore(&g_frame_lock, irq_flags);
        FRAME_PANIC("Double free detected in put_frame!");
        return;
    }
    g_frame_refcounts[pfn]--;

    if (g_frame_refcounts[pfn] == 0) {
        uintptr_t virt_addr = phys_addr + KERNEL_SPACE_VIRT_START;
        if (virt_addr < phys_addr || virt_addr < KERNEL_SPACE_VIRT_START) { /* Handle overflow */ spinlock_release_irqrestore(&g_frame_lock, irq_flags); FRAME_PANIC("Virtual address overflow during put_frame conversion!"); return; }
        spinlock_release_irqrestore(&g_frame_lock, irq_flags); // Release frame lock before calling buddy

        int page_req_order = PAGE_ORDER;

        // Call buddy_free_raw (handles its own locking)
        buddy_free_raw((void*)virt_addr, page_req_order);

    } else {
        // Frame still in use, just release lock
        spinlock_release_irqrestore(&g_frame_lock, irq_flags);
    }
}

int get_frame_refcount(uintptr_t phys_addr) {
    if ((phys_addr % PAGE_SIZE) != 0) phys_addr = PAGE_ALIGN_DOWN(phys_addr);
    size_t pfn = addr_to_pfn(phys_addr);
    if (pfn >= g_total_frames) return -1;
    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);
    int count = (int)g_frame_refcounts[pfn];
    spinlock_release_irqrestore(&g_frame_lock, irq_flags);
    return count;
}