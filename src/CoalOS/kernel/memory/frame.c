/**
 * @file frame.c
 * @brief Physical Memory Frame Allocator (Page Allocator)
 *
 * Manages physical memory frames (pages) using a reference counting system.
 * Relies on the buddy allocator for underlying physical block allocation/deallocation.
 * Responsible for tracking usage of all physical frames in the system.
 */

// Essential Includes (Order matters for dependencies)
#include <kernel/memory/paging.h>           // Defines PAGE_SIZE, KERNEL_SPACE_VIRT_START, virt_to_phys, etc.
#include <kernel/memory/buddy.h>            // For buddy_alloc_raw, buddy_free_raw, MIN_ORDER, MAX_ORDER
#include <kernel/memory/kmalloc_internal.h> // For ALIGN_UP, DEFAULT_ALIGNMENT (Check if still needed)
#include <kernel/memory/frame.h>            // Public interface for this module
#include <kernel/drivers/display/terminal.h>         // For kernel logging (terminal_printf, etc.)
#include <kernel/sync/spinlock.h>         // For protecting shared frame allocator state
#include <libc/stdint.h>      // For SIZE_MAX, uintXX_t, UINTPTR_MAX, UINT64_MAX
#include <libc/string.h>      // For memset
#include <kernel/core/types.h>            // For uintptr_t, size_t, bool
#include <kernel/arch/multiboot2.h>
#include <kernel/lib/assert.h>           // For KERNEL_ASSERT and KERNEL_PANIC_HALT

// Forward declaration for idle task stack checking
extern void check_idle_task_stack_integrity(const char *checkpoint);

// --- Compile-time Sanity Checks ---
#ifndef PAGE_SIZE
#error "PAGE_SIZE is not defined! Ensure paging.h is included and defines it."
#endif

#ifndef KERNEL_SPACE_VIRT_START
#error "KERNEL_SPACE_VIRT_START is not defined! Ensure paging.h defines the kernel's virtual base address."
#endif

// Determine the buddy order corresponding to a single page frame
#if (PAGE_SIZE == 4096)
#define FRAME_BUDDY_ORDER 12
#elif (PAGE_SIZE == 8192)
#define FRAME_BUDDY_ORDER 13
#elif (PAGE_SIZE == 16384)
#define FRAME_BUDDY_ORDER 14
// Add other common page sizes if needed
#else
#error "Unsupported PAGE_SIZE for frame allocator buddy order calculation."
#endif

#ifndef MIN_ORDER
#error "MIN_ORDER is not defined (include buddy.h)"
#endif
#ifndef MAX_ORDER
#error "MAX_ORDER is not defined (include buddy.h)"
#endif

//----------------------------------------------------------------------------
// Internal Macros for Logging
//----------------------------------------------------------------------------
// Define FRAME_DEBUG_LEVEL to 1 or higher in your build system (e.g., CMakeLists.txt)
// to enable detailed frame allocation/free logging.
#ifndef FRAME_DEBUG_LEVEL
#define FRAME_DEBUG_LEVEL 0 // 0=Off, 1=Basic, 2=Verbose
#endif

// Use %lu for size_t/uintptr_t, %lu for uint32_t (since warning indicated it's long unsigned)
#define FRAME_PRINT(level, fmt, ...) \
    do { if (FRAME_DEBUG_LEVEL >= (level)) serial_printf(fmt, ##__VA_ARGS__); } while(0)

// Simplified FRAME_PANIC/ASSERT using the corrected KERNEL_* macros from assert.h
#define FRAME_PANIC(msg) KERNEL_PANIC_HALT("FRAME PANIC: " msg)
#define FRAME_ASSERT(condition, msg) KERNEL_ASSERT((condition), "FRAME ASSERT FAILED: " msg)


//----------------------------------------------------------------------------
// Module Globals
//----------------------------------------------------------------------------
// VIRTUAL address of the reference count array (lives in kernel heap).
static volatile uint32_t *g_frame_refcounts = NULL;
// PHYSICAL address where the reference count array was allocated.
static uintptr_t g_frame_refcounts_phys = 0;
// Total number of page frames representable by the highest physical address.
static size_t g_total_frames = 0;
// Highest physical address detected + 1 page (aligned).
static uintptr_t g_highest_address_aligned = 0;
// Spinlock protecting access to the refcount array and related globals.
static spinlock_t g_frame_lock;
// Actual size allocated by the buddy allocator for the refcount array.
static size_t g_refcount_array_alloc_size = 0;

// External dependency (provided by paging subsystem)
extern uint32_t g_kernel_page_directory_phys; // Physical address of initial PD

//----------------------------------------------------------------------------
// Internal Helper Functions
//----------------------------------------------------------------------------

/**
 * @brief Converts a physical address to its corresponding Page Frame Number (PFN).
 * @param addr Physical address.
 * @return PFN, or potentially SIZE_MAX/PFN_MAX on overflow (though unlikely with uintptr_t).
 */
static inline size_t addr_to_pfn(uintptr_t addr) {
    // Use KERNEL_ASSERT for internal sanity checks now
    KERNEL_ASSERT(PAGE_SIZE != 0, "PAGE_SIZE cannot be zero");
    // Basic division, relies on PAGE_SIZE being a power of 2.
    return addr / PAGE_SIZE;
}

/**
 * @brief Converts a Page Frame Number (PFN) to its corresponding physical address.
 * @param pfn Page Frame Number.
 * @return Physical address, or potentially UINTPTR_MAX on overflow.
 */
static inline uintptr_t pfn_to_addr(size_t pfn) {
    // Check for potential multiplication overflow before performing it.
    if (pfn > (UINTPTR_MAX / PAGE_SIZE)) {
        FRAME_PRINT(1, "[PFN to Addr WARN] PFN %lu too large, would overflow uintptr_t\n", (unsigned long)pfn);
        return UINTPTR_MAX; // Indicate overflow
    }
    return pfn * PAGE_SIZE;
}

/**
 * @brief Calculates the required buddy block size for a given allocation request.
 * Ensures the size is a power of 2 within the buddy system's limits.
 * NOTE: This is a simplified version, ensure it matches buddy.c's logic if complex.
 * @param total_size The desired allocation size in bytes.
 * @return The appropriate power-of-2 buddy block size, or SIZE_MAX on error/overflow.
 */
static size_t get_required_buddy_allocation_size(size_t total_size) {
    if (total_size == 0) return 0;

    const size_t min_block = (1UL << MIN_ORDER); // Use unsigned long literal for shifts
    const size_t max_block = (1UL << MAX_ORDER);

    // Ensure we request at least the minimum block size
    if (total_size < min_block) total_size = min_block;

    // Find the next power of 2 >= total_size
    size_t power_of_2 = min_block;
    while (power_of_2 < total_size) {
        // Check for overflow before shifting
        if (power_of_2 > (SIZE_MAX >> 1)) return SIZE_MAX;
        power_of_2 <<= 1;
    }

    // Ensure the required size doesn't exceed the maximum allowable block size
    if (power_of_2 > max_block) return SIZE_MAX;

    return power_of_2;
}

/**
 * @brief Marks a physical memory range as reserved in the refcount array.
 * Used during initialization to prevent allocation of critical areas.
 * @param start Physical start address of the range.
 * @param end Physical end address of the range (exclusive).
 * @param name Descriptive name of the reserved region for logging.
 */
static void mark_reserved_range(uintptr_t start, uintptr_t end, const char* name) {
    FRAME_ASSERT(g_frame_refcounts != NULL, "Refcount array must be allocated before marking reserved");
    // Allow start == end for edge cases, but not start > end
    KERNEL_ASSERT(start <= end, "Reserved range start must be less than or equal to end");
    if (start == end) return; // Nothing to reserve

    uintptr_t aligned_start = PAGE_ALIGN_DOWN(start);
    uintptr_t aligned_end   = ALIGN_UP(end, PAGE_SIZE);

    // Handle potential overflow with ALIGN_UP
    if (aligned_end < end) {
        FRAME_PRINT(1, "[Mark Reserved WARN] Alignment overflow for end address %#lx\n", (unsigned long)end);
        aligned_end = UINTPTR_MAX; // Align up to the maximum possible address
    }

    size_t start_pfn = addr_to_pfn(aligned_start);
    size_t end_pfn   = addr_to_pfn(aligned_end);

    serial_printf("      Reserving %-12s: PFNs [%6lu - %6lu) Addr [%#010lx - %#010lx)\n",
                      name, (unsigned long)start_pfn, (unsigned long)end_pfn,
                      (unsigned long)aligned_start, (unsigned long)aligned_end);

    // Clamp end_pfn to the actual total number of frames we manage
    if (end_pfn > g_total_frames) {
        FRAME_PRINT(1, "[Mark Reserved INFO] Clamping end PFN from %lu to %lu for %s\n",
                      (unsigned long)end_pfn, (unsigned long)g_total_frames, name);
        end_pfn = g_total_frames;
    }

    if (start_pfn >= end_pfn) return; // Nothing to reserve in the valid range

    // Atomically mark each frame as reserved (refcount=1)
    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);
    for (size_t pfn = start_pfn; pfn < end_pfn; ++pfn) {
        // Double-check bounds *inside* the loop before array access
        if (pfn < g_total_frames) {
             // Only mark if not already marked (prevents double-counting issues)
             // Although init should zero first, this is safer if called later.
            if (g_frame_refcounts[pfn] == 0) {
                g_frame_refcounts[pfn] = 1; // 1 indicates reserved/allocated
            } else {
                 // Use %lu for uint32_t based on previous compiler warning
                 FRAME_PRINT(1, "[Mark Reserved WARN] PFN %lu for %s already has refcount %lu\n",
                                (unsigned long)pfn, name, (unsigned long)g_frame_refcounts[pfn]);
            }
        } else {
            // This should technically not happen if end_pfn was clamped correctly, but assert defensively.
            FRAME_ASSERT(false, "PFN calculation error during reservation - PFN exceeded total frames");
        }
    }
    spinlock_release_irqrestore(&g_frame_lock, irq_flags);
}

//----------------------------------------------------------------------------
// Initialization Function: frame_init
//----------------------------------------------------------------------------

int frame_init(struct multiboot_tag_mmap *mmap_tag_virt,
    uintptr_t kernel_phys_start, uintptr_t kernel_phys_end,
    uintptr_t buddy_heap_phys_start, uintptr_t buddy_heap_phys_end)
{
terminal_write("[Frame] Initializing physical frame manager...\n");
spinlock_init(&g_frame_lock);

// --- Step 1: Validate Multiboot Memory Map ---
KERNEL_ASSERT(mmap_tag_virt != NULL, "Multiboot MMAP tag is NULL");
KERNEL_ASSERT(mmap_tag_virt->type == MULTIBOOT_TAG_TYPE_MMAP, "Invalid Multiboot tag type for MMAP");
KERNEL_ASSERT(mmap_tag_virt->entry_size >= sizeof(multiboot_memory_map_t), "MMAP entry size too small");
FRAME_PRINT(1, "   Using MMAP tag @ VIRT=%#lx (Size=%u, EntrySize=%u, Ver=%u)\n",
     (unsigned long)mmap_tag_virt, (unsigned)mmap_tag_virt->size,
     (unsigned)mmap_tag_virt->entry_size, (unsigned)mmap_tag_virt->entry_version);

// --- Step 2: Determine Total Physical Memory Span ---
uintptr_t highest_detected_addr = 0;
uintptr_t mmap_base_virt = (uintptr_t)mmap_tag_virt->entries;
uintptr_t mmap_end_virt = (uintptr_t)mmap_tag_virt + mmap_tag_virt->size;

for (uintptr_t entry_ptr = mmap_base_virt;
entry_ptr + mmap_tag_virt->entry_size <= mmap_end_virt; // Ensure entry fits within tag bounds
entry_ptr += mmap_tag_virt->entry_size)
{
multiboot_memory_map_t *entry = (multiboot_memory_map_t *)entry_ptr;
uint64_t region_start = entry->addr;
uint64_t region_len   = entry->len;
uint64_t region_end   = region_start + region_len;

// Check for overflow in region_end calculation
if (region_end < region_start) region_end = UINT64_MAX;

// Track the highest address seen across all regions
if (region_end > highest_detected_addr) {
 // Clamp to uintptr_t max if necessary
 highest_detected_addr = (region_end > UINTPTR_MAX) ? UINTPTR_MAX : (uintptr_t)region_end;
}
}

KERNEL_ASSERT(highest_detected_addr > 0, "Failed to detect highest physical address from MMAP");

// Align the highest address *up* to the next page boundary to get the total span
g_highest_address_aligned = ALIGN_UP(highest_detected_addr, PAGE_SIZE);
if (g_highest_address_aligned < highest_detected_addr) { // Handle overflow from ALIGN_UP
g_highest_address_aligned = UINTPTR_MAX;
}

g_total_frames = addr_to_pfn(g_highest_address_aligned);
KERNEL_ASSERT(g_total_frames > 0, "Calculated total frame count is zero");

serial_printf("   Detected highest physical address (aligned up): %#lx (%lu total frames potentially addressable)\n",
         (unsigned long)g_highest_address_aligned, (unsigned long)g_total_frames);

// --- Step 3: Allocate Physical Memory for Reference Count Array ---
// Check for overflow before multiplying
if (g_total_frames > (SIZE_MAX / sizeof(uint32_t))) {
FRAME_PANIC("Refcount array size calculation overflows size_t");
}
size_t refcount_array_size_bytes = g_total_frames * sizeof(uint32_t);

g_refcount_array_alloc_size = get_required_buddy_allocation_size(refcount_array_size_bytes);
if (g_refcount_array_alloc_size == SIZE_MAX || g_refcount_array_alloc_size == 0) {
FRAME_PANIC("Failed to determine valid buddy block size for refcount array");
}

// Determine the required buddy order for the allocation
int required_order = 0; // Start from MIN_ORDER implicitly
size_t block_size = (1UL << MIN_ORDER);
while(block_size < g_refcount_array_alloc_size && required_order <= MAX_ORDER) {
if (block_size > (SIZE_MAX >> 1)) { required_order = -1; break; } // Prevent overflow
block_size <<= 1;
required_order++;
}
// Adjust required_order as it represents the exponent (MIN_ORDER is order 0 conceptually for loop)
required_order += MIN_ORDER;

if (required_order > MAX_ORDER || required_order < MIN_ORDER) {
FRAME_PANIC("Could not determine valid buddy order for refcount array");
}

serial_printf("   Attempting buddy_alloc_raw(order=%d) for %lu bytes refcount array (fits in block size %lu)...\n",
         required_order, (unsigned long)refcount_array_size_bytes, (unsigned long)g_refcount_array_alloc_size);

// Allocate using the raw buddy function (returns virtual address)
void* refcount_array_virt_ptr = buddy_alloc_raw(required_order);
if (!refcount_array_virt_ptr) { FRAME_PANIC("buddy_alloc_raw failed for refcount array"); }

// --- Step 4: Calculate Physical Address & Verify Alignment ---
// This conversion assumes the buddy heap lives in the direct-mapped kernel virtual space
KERNEL_ASSERT(KERNEL_SPACE_VIRT_START != 0, "KERNEL_SPACE_VIRT_START must be defined for virt->phys conversion");
// ***** IMPORTANT: Ensure this virt->phys conversion is correct for your buddy heap location *****
// If buddy heap is NOT simply offset from KERNEL_SPACE_VIRT_START, this needs adjustment.
// Based on boot log (`buddy_heap_phys_start` = 0x800000, `buddy_heap_virt_start` = 0xc0800000),
// the offset seems to be `0xc0800000 - 0x800000 = 0xC0000000`, which matches KERNEL_SPACE_VIRT_START.
// If the buddy allocator can return memory outside its initial designated heap (unlikely but possible
// depending on implementation), this conversion might be unsafe. Assuming it stays within its mapped region:
g_frame_refcounts_phys = (uintptr_t)refcount_array_virt_ptr - KERNEL_SPACE_VIRT_START;


serial_printf("   Refcount array allocated: VIRT=%#lx -> PHYS=%#lx (Buddy Block Size=%lu)\n",
         (unsigned long)refcount_array_virt_ptr, (unsigned long)g_frame_refcounts_phys,
         (unsigned long)g_refcount_array_alloc_size);

// Critical check: The physical frame allocator *needs* the refcount array to be page-aligned.
// The buddy allocator should guarantee this for allocations >= PAGE_SIZE.
KERNEL_ASSERT((g_frame_refcounts_phys % PAGE_SIZE) == 0, "Physical address for refcount array is not page-aligned! Buddy error?");

// --- Step 5: Use the VIRTUAL address for access and Initialize ---
g_frame_refcounts = (volatile uint32_t*)refcount_array_virt_ptr;
serial_printf("   Using VIRT address %#lx for refcount array access.\n", (unsigned long)g_frame_refcounts);

terminal_write("   Initializing reference counts (zeroing, then marking reserved regions)...\n");
serial_printf("        Zeroing refcount array (%lu actual bytes) @ VIRT=%#lx...\n",
         (unsigned long)refcount_array_size_bytes, (unsigned long)g_frame_refcounts);
memset((void*)g_frame_refcounts, 0, refcount_array_size_bytes);
terminal_write("        Refcount array zeroed.\n");

// Mark known reserved physical memory regions
terminal_write("        Marking known reserved physical memory regions...\n");
mark_reserved_range(0x0, 0x100000, "Low 1MB"); // Includes BIOS, VGA, etc.
mark_reserved_range(kernel_phys_start, kernel_phys_end, "Kernel Image");
if (g_kernel_page_directory_phys != 0) { // g_kernel_page_directory_phys is set in paging_init stage 1
mark_reserved_range(g_kernel_page_directory_phys, g_kernel_page_directory_phys + PAGE_SIZE, "Initial PD");
}
// Add any other known hardware regions or special areas here if necessary.

// ***** ADDED: Mark the refcount array's own physical memory as reserved *****
if (g_frame_refcounts_phys != 0 && g_refcount_array_alloc_size > 0) {
mark_reserved_range(g_frame_refcounts_phys,
                  g_frame_refcounts_phys + g_refcount_array_alloc_size,
                  "Refcount Arr");
}
// ***** Optional: Mark the gap between kernel end and buddy start if significant *****
// uintptr_t kernel_or_pd_end = (g_kernel_page_directory_phys > kernel_phys_end) ?
//                             (g_kernel_page_directory_phys + PAGE_SIZE) : kernel_phys_end;
// kernel_or_pd_end = ALIGN_UP(kernel_or_pd_end, PAGE_SIZE);
// if (buddy_heap_phys_start > kernel_or_pd_end) {
//     mark_reserved_range(kernel_or_pd_end, buddy_heap_phys_start, "Buddy Gap");
// }

// --- Step 6: Final Sanity Check (Optional but Recommended) ---
FRAME_PRINT(1, "   Verifying available frame count post-init (based on MMAP & reservations)...\n");
size_t available_count = 0;
size_t usable_buddy_frames = 0;
mmap_base_virt = (uintptr_t)mmap_tag_virt->entries; // Reset pointer

for (uintptr_t entry_ptr = mmap_base_virt;
entry_ptr + mmap_tag_virt->entry_size <= mmap_end_virt;
entry_ptr += mmap_tag_virt->entry_size)
{
multiboot_memory_map_t *entry = (multiboot_memory_map_t *)entry_ptr;
if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
 uint64_t r_start64 = entry->addr;
 uint64_t r_end64 = r_start64 + entry->len;
 if (r_end64 < r_start64) r_end64 = UINT64_MAX; // Overflow check

 // Align region boundaries to pages for frame counting
 uintptr_t first_addr = ALIGN_UP((uintptr_t)r_start64, PAGE_SIZE);
 uintptr_t last_addr = PAGE_ALIGN_DOWN((uintptr_t)r_end64);

  if (first_addr < last_addr) { // Ensure valid range after alignment
      size_t first_pfn = addr_to_pfn(first_addr);
      size_t last_pfn = addr_to_pfn(last_addr);

      for (size_t pfn = first_pfn; pfn < last_pfn && pfn < g_total_frames; ++pfn) {
          // Check bounds before accessing array!
          if (pfn < g_total_frames) {
              if (g_frame_refcounts[pfn] == 0) {
                  available_count++;
                  // Check if this available frame falls within the buddy heap's *physical* range
                  uintptr_t current_addr = pfn_to_addr(pfn);
                  if (current_addr >= buddy_heap_phys_start && current_addr < buddy_heap_phys_end) {
                      usable_buddy_frames++;
                  }
              }
          } else { FRAME_ASSERT(false, "PFN out of range during available count check"); }
      }
  }
}
}
// Use %lu for size_t
serial_printf("   Sanity Check: Found %lu frames marked available (refcount=0).\n", (unsigned long)available_count);
serial_printf("                 Of those, %lu frames fall within the physical buddy heap range [%#lx - %#lx).\n",
         (unsigned long)usable_buddy_frames, buddy_heap_phys_start, buddy_heap_phys_end);
if (available_count == 0 && g_total_frames > 256) {
terminal_write("   [WARNING] Zero available frames detected after initialization!\n");
}

terminal_write("[Frame] Frame manager initialization complete.\n");
return 0; // Success
}

//----------------------------------------------------------------------------
// Core Allocation/Deallocation Functions
//----------------------------------------------------------------------------

/**
 * @brief Allocates a single physical page frame.
 * @return The physical address of the allocated frame, or 0 on failure.
 */
uintptr_t frame_alloc(void) {
    const int frame_req_order = FRAME_BUDDY_ORDER; // Use defined constant

    FRAME_PRINT(2, "[Frame Alloc] Requesting order %d from buddy allocator...\n", frame_req_order);
    void* block_virt = buddy_alloc_raw(frame_req_order);
    FRAME_PRINT(2, "[Frame Alloc] buddy_alloc_raw returned VIRT=%p\n", block_virt);

    if (!block_virt) {
        FRAME_PRINT(0, "[Frame Alloc ERR] Buddy allocation failed (out of memory?)!\n");
        return 0; // Indicate failure
    }

    // --- Convert Virtual to Physical and Validate ---
    FRAME_ASSERT(KERNEL_SPACE_VIRT_START != 0, "KERNEL_SPACE_VIRT_START is not defined/zero");
    uintptr_t virt_addr_received = (uintptr_t)block_virt;
    uintptr_t assumed_kernel_start = KERNEL_SPACE_VIRT_START; // Capture for logging
    uintptr_t block_phys = virt_addr_received - assumed_kernel_start;

    FRAME_PRINT(1, "[Frame Alloc DBG] Received VIRT=%#lx, Assumed KERN_START=%#lx -> Calculated PHYS=%#lx\n",
                  (unsigned long)virt_addr_received, (unsigned long)assumed_kernel_start, (unsigned long)block_phys);

    // Assert alignment *after* calculating physical address
    FRAME_ASSERT((block_phys % PAGE_SIZE) == 0, "Buddy returned non-page-aligned physical address");

    // --- Calculate PFN and Check Boundary ---
    size_t pfn = addr_to_pfn(block_phys);
    size_t current_g_total_frames = g_total_frames; // Capture current boundary value

    FRAME_PRINT(1, "[Frame Alloc DBG] Calculated PFN=%lu, Checking against g_total_frames=%lu\n",
                  (unsigned long)pfn, (unsigned long)current_g_total_frames);

    // Perform the critical boundary check
    FRAME_ASSERT(pfn < current_g_total_frames, "Calculated PFN is out of range!");

    // --- Update Reference Count Atomically ---
    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);

    // Check PFN bounds *again* just before array access (defense in depth)
    if (pfn >= current_g_total_frames) {
        spinlock_release_irqrestore(&g_frame_lock, irq_flags);
        // Use KERNEL_PANIC_HALT directly as FRAME_PANIC might cause issues here
        KERNEL_PANIC_HALT("FRAME PANIC: PFN out of bounds right before refcount access!");
    }

    FRAME_PRINT(2, "[Frame Alloc] PFN=%lu (Phys=%#lx), Reading refcount...\n", (unsigned long)pfn, (unsigned long)block_phys);
    uint32_t current_refcount = g_frame_refcounts[pfn]; // Read current refcount
    // Use %lu for uint32_t (aka unsigned long)
    FRAME_PRINT(2, "[Frame Alloc] PFN=%lu, Current refcount=%lu\n", (unsigned long)pfn, (unsigned long)current_refcount);

    // This assertion is vital: We should only be allocating frames that are currently free (refcount 0).
    if (current_refcount != 0) {
        serial_printf("[Frame ERROR] PFN %lu has refcount %lu, expected 0!\n", 
                      (unsigned long)pfn, (unsigned long)current_refcount);
        serial_printf("[Frame ERROR] Physical addr 0x%lx, Virtual addr received 0x%lx\n",
                      (unsigned long)block_phys, (unsigned long)virt_addr_received);
        spinlock_release_irqrestore(&g_frame_lock, irq_flags);
        return 0; // Return failure instead of crashing
    }

    g_frame_refcounts[pfn] = 1; // Mark as allocated (set refcount to 1)
    FRAME_PRINT(1, "[Frame Alloc] PFN=%lu, Refcount set to 1.\n", (unsigned long)pfn);

    spinlock_release_irqrestore(&g_frame_lock, irq_flags);

    return block_phys; // Return the physical address
}

/**
 * @brief Decrements the reference count for a physical page frame.
 * If the count reaches zero, the frame is freed back to the buddy allocator.
 * @param phys_addr The physical address of the frame to release/decrement.
 * Must be page-aligned.
 */
void put_frame(uintptr_t phys_addr) {
    // Ensure the address is page-aligned before proceeding.
    FRAME_ASSERT((phys_addr % PAGE_SIZE) == 0, "put_frame called with non-page-aligned address");
    if ((phys_addr % PAGE_SIZE) != 0) {
         FRAME_PRINT(0, "[Put Frame WARN] Received non-aligned address %#lx, aligning down to %#lx\n",
                       (unsigned long)phys_addr, (unsigned long)PAGE_ALIGN_DOWN(phys_addr));
         phys_addr = PAGE_ALIGN_DOWN(phys_addr); // Align down for safety
    }

    size_t pfn = addr_to_pfn(phys_addr);

    // Basic check: Don't try to operate on PFNs outside our known range.
    if (pfn >= g_total_frames) {
         FRAME_PRINT(0, "[Put Frame ERR] Attempted to free PFN %lu (from Phys %#lx) which is out of range (max %lu)\n",
                       (unsigned long)pfn, (unsigned long)phys_addr, (unsigned long)g_total_frames);
         KERNEL_ASSERT(false, "put_frame called with PFN out of range"); // Trigger assert/panic
         return;
    }

    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);

    // Check PFN bounds again inside lock
    if (pfn >= g_total_frames) {
        spinlock_release_irqrestore(&g_frame_lock, irq_flags);
        KERNEL_PANIC_HALT("FRAME PANIC: PFN out of bounds in put_frame before refcount access");
    }

    uint32_t current_refcount = g_frame_refcounts[pfn];
    // Use %lu for uint32_t
    FRAME_PRINT(2, "[Put Frame] PFN=%lu (Phys=%#lx), Current refcount=%lu\n",
                  (unsigned long)pfn, (unsigned long)phys_addr, (unsigned long)current_refcount);

    // Critical check: Ensure we are not decrementing a count that's already zero (double free).
    if (current_refcount == 0) {
        spinlock_release_irqrestore(&g_frame_lock, irq_flags);
        FRAME_PANIC("Double free detected in put_frame!");
        return; // Should not be reached if PANIC halts
    }

    // Decrement the reference count
    g_frame_refcounts[pfn]--;
    uint32_t new_refcount = g_frame_refcounts[pfn]; // Read back the new value

    // Use %lu for uint32_t
    FRAME_PRINT(1, "[Put Frame] PFN=%lu, Decremented refcount to %lu.\n", (unsigned long)pfn, (unsigned long)new_refcount);

    // If the reference count is now zero, free the frame back to the buddy system
    if (new_refcount == 0) {
        // Convert physical address back to the virtual address the buddy system expects
        uintptr_t virt_addr = phys_addr + KERNEL_SPACE_VIRT_START;
        // Check for overflow during virt conversion
        if (virt_addr < phys_addr || virt_addr < KERNEL_SPACE_VIRT_START) {
             spinlock_release_irqrestore(&g_frame_lock, irq_flags);
             FRAME_PANIC("Virtual address overflow during put_frame phys->virt conversion!");
        }

        // Release the frame lock *before* calling the buddy allocator,
        // as the buddy allocator has its own internal locking.
        spinlock_release_irqrestore(&g_frame_lock, irq_flags);

        FRAME_PRINT(2, "[Put Frame] Refcount is zero, freeing VIRT=%p (order %d) to buddy system.\n",
                      (void*)virt_addr, FRAME_BUDDY_ORDER);
        
        // Check idle task stack before buddy_free_raw
        if (pfn == 0x10002 || pfn == 0x10071 || pfn == 0x10074) {
            // These are important PFNs - PD frames or user stack frames
            serial_printf("[Put Frame WARN] About to free important PFN %lu (Phys=%#lx, Virt=%#lx)\n",
                          (unsigned long)pfn, (unsigned long)phys_addr, (unsigned long)virt_addr);
            check_idle_task_stack_integrity("put_frame: Before buddy_free (important frame)");
        }
        
        buddy_free_raw((void*)virt_addr, FRAME_BUDDY_ORDER);
        
        // Check idle task stack after buddy_free_raw
        if (pfn == 0x10002 || pfn == 0x10071 || pfn == 0x10074) {
            check_idle_task_stack_integrity("put_frame: After buddy_free (important frame)");
        }

    } else {
        // Frame is still referenced by others, just release the lock
        spinlock_release_irqrestore(&g_frame_lock, irq_flags);
    }
}

//----------------------------------------------------------------------------
// Reference Count Management Functions
//----------------------------------------------------------------------------

/**
 * @brief Gets the current reference count of a physical page frame.
 * @param phys_addr The physical address of the frame (must be page-aligned).
 * @return The reference count, or -1 if the PFN is out of range.
 */
int get_frame_refcount(uintptr_t phys_addr) {
    FRAME_ASSERT((phys_addr % PAGE_SIZE) == 0, "get_frame_refcount requires page-aligned address");
    if ((phys_addr % PAGE_SIZE) != 0) phys_addr = PAGE_ALIGN_DOWN(phys_addr); // Align defensively

    size_t pfn = addr_to_pfn(phys_addr);

    // Check if PFN is valid before acquiring lock/accessing array
    if (pfn >= g_total_frames) {
         FRAME_PRINT(1, "[Get Refcount WARN] PFN %lu (from Phys %#lx) out of range (max %lu)\n",
                       (unsigned long)pfn, (unsigned long)phys_addr, (unsigned long)g_total_frames);
        return -1; // Indicate error
    }

    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);
    // Check bounds again inside lock
    if (pfn >= g_total_frames) {
         spinlock_release_irqrestore(&g_frame_lock, irq_flags);
         FRAME_PRINT(0, "[Get Refcount ERR] PFN %lu became invalid under lock?\n", (unsigned long)pfn);
         return -1;
    }
    // Read refcount (cast to int for return type compatibility)
    int count = (int)g_frame_refcounts[pfn];
    spinlock_release_irqrestore(&g_frame_lock, irq_flags);

    FRAME_PRINT(2, "[Get Refcount] PFN=%lu (Phys=%#lx) -> Count=%d\n", (unsigned long)pfn, (unsigned long)phys_addr, count);
    return count;
}

/**
 * @brief Increments the reference count for a physical page frame.
 * The frame MUST already be allocated (refcount > 0).
 * @param phys_addr The physical address of the frame to increment (must be page-aligned).
 */
void frame_incref(uintptr_t phys_addr) {
    FRAME_ASSERT((phys_addr % PAGE_SIZE) == 0, "frame_incref requires page-aligned address");
     if ((phys_addr % PAGE_SIZE) != 0) phys_addr = PAGE_ALIGN_DOWN(phys_addr); // Align defensively

    size_t pfn = addr_to_pfn(phys_addr);

    // Check PFN validity upfront
    if (pfn >= g_total_frames) {
        FRAME_PANIC("frame_incref called with PFN out of range!");
        return; // Should not be reached if PANIC halts
    }

    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_frame_lock);

    // Check bounds again inside lock
    if (pfn >= g_total_frames) {
         spinlock_release_irqrestore(&g_frame_lock, irq_flags);
         FRAME_PANIC("PFN out of bounds in frame_incref under lock!");
    }

    uint32_t old_count = g_frame_refcounts[pfn];
    // Use %lu for uint32_t
    FRAME_PRINT(2, "[Frame Incref] PFN=%lu (Phys=%#lx), Old count=%lu\n",
                  (unsigned long)pfn, (unsigned long)phys_addr, (unsigned long)old_count);

    // Critical assertions:
    FRAME_ASSERT(old_count > 0, "Incrementing refcount of a frame that is supposedly free (count was 0)!");
    FRAME_ASSERT(old_count < UINT32_MAX, "Frame reference count overflow during increment!");

    g_frame_refcounts[pfn]++;
    // Use %lu for uint32_t
    FRAME_PRINT(1, "[Frame Incref] PFN=%lu, Count %lu -> %lu\n", (unsigned long)pfn, (unsigned long)old_count, (unsigned long)g_frame_refcounts[pfn]);

    spinlock_release_irqrestore(&g_frame_lock, irq_flags);
}