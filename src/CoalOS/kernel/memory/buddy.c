/**
 * @file buddy.c
 * @brief Power-of-Two Buddy Allocator Implementation (Revised v4.1 - Build Fixes)
 *
 * Manages a virtually contiguous region mapped to physical memory, returning
 * VIRTUAL addresses to callers. Includes robust checks, SMP safety via spinlocks,
 * and optional debugging features (canaries, leak tracking).
 *
 * Key Improvements (v4.1):
 * - Fixed printf format specifiers for size_t/uintptr_t.
 * - Removed duplicate function definition.
 * - Corrected physical address calculation for alignment assertions.
 * - Enhanced error handling and robustness checks.
 * - Improved comments and code clarity.
 */

// === Header Includes ===
#include <kernel/memory/buddy.h>            // Public API, config constants (MIN/MAX_ORDER)
#include <kernel/memory/kmalloc_internal.h> // For DEFAULT_ALIGNMENT, ALIGN_UP
#include <kernel/drivers/display/terminal.h>         // Kernel logging
#include <kernel/core/types.h>            // Core types (uintptr_t, size_t, bool)
#include <kernel/sync/spinlock.h>         // Spinlock implementation
#include <libc/stdint.h>      // Fixed-width types, SIZE_MAX, UINTPTR_MAX
#include <kernel/memory/paging.h>           // For PAGE_SIZE, KERNEL_SPACE_VIRT_START
#include <kernel/lib/string.h>           // For memset (use kernel's version)
#include <kernel/lib/assert.h>           // For BUDDY_PANIC, BUDDY_ASSERT

// === Configuration & Constants ===

// Assertions for configuration defined in buddy.h
#ifndef MIN_ORDER
#error "MIN_ORDER is not defined (include buddy.h)"
#endif
#ifndef MAX_ORDER
#error "MAX_ORDER is not defined (include buddy.h)"
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096 // Define fallback if not in paging.h (should be there)
#endif

// Calculate the order corresponding to the page size compile-time
#if (PAGE_SIZE == 4096)
#define PAGE_ORDER 12
#elif (PAGE_SIZE == 8192)
#define PAGE_ORDER 13
// Add other page sizes if necessary
#else
#error "Unsupported PAGE_SIZE for buddy allocator PAGE_ORDER calculation."
#endif

// Compile-time checks for configuration validity
_Static_assert(PAGE_ORDER >= MIN_ORDER, "PAGE_ORDER must be >= MIN_ORDER");
_Static_assert(PAGE_ORDER <= MAX_ORDER, "PAGE_ORDER must be <= MAX_ORDER");
_Static_assert(MIN_ORDER <= MAX_ORDER, "MIN_ORDER must be <= MAX_ORDER");

// Smallest block size managed internally
#define MIN_INTERNAL_ORDER MIN_ORDER
#define MIN_BLOCK_SIZE_INTERNAL (1UL << MIN_INTERNAL_ORDER)

// --- Metadata Header (Non-Debug Builds) ---
#ifndef DEBUG_BUDDY
/**
 * @brief Header prepended to non-debug buddy allocations.
 * Stores the allocation order for use during free.
 */
typedef struct buddy_header {
    uint8_t order; // Order of the allocated block (MIN_ORDER to MAX_ORDER)
} buddy_header_t;

// Ensure header size is aligned to default alignment for safety
#define BUDDY_HEADER_SIZE ALIGN_UP(sizeof(buddy_header_t), DEFAULT_ALIGNMENT)
#else
#define BUDDY_HEADER_SIZE 0 // No header in debug builds
#endif // !DEBUG_BUDDY

// --- Panic & Assert Macros (Defined in assert.h, aliased for context) ---
#define BUDDY_PANIC KERNEL_PANIC_HALT
#define BUDDY_ASSERT KERNEL_ASSERT

// --- Free List Structure ---
/**
 * @brief Structure used to link free blocks in the free lists.
 * Placed at the beginning of each free block.
 */
typedef struct buddy_block {
    struct buddy_block *next;
} buddy_block_t;

// === Global State ===
static buddy_block_t *free_lists[MAX_ORDER + 1] = {0}; // Array of free lists per order
static uintptr_t g_heap_start_virt_addr = 0;           // Aligned VIRTUAL start address of managed heap
static uintptr_t g_heap_end_virt_addr = 0;             // VIRTUAL end address (exclusive) of managed heap
static uintptr_t g_buddy_heap_phys_start_addr = 0;     // Aligned PHYSICAL start address of managed heap
static size_t g_buddy_total_managed_size = 0;          // Total size managed by the allocator
static size_t g_buddy_free_bytes = 0;                  // Current free bytes (tracked approximately)
static spinlock_t g_buddy_lock;                        // Lock protecting allocator state

// Statistics
static uint64_t g_alloc_count = 0;
static uint64_t g_free_count = 0;
static uint64_t g_failed_alloc_count = 0;

// --- Debug Allocation Tracker ---
#ifdef DEBUG_BUDDY
#define DEBUG_CANARY_START 0xDEADBEEF
#define DEBUG_CANARY_END   0xCAFEBABE
#define MAX_TRACKER_NODES 1024 // Adjust as needed

/**
 * @brief Structure to track allocations in debug builds.
 */
typedef struct allocation_tracker {
    void* user_addr;                 // Address returned to the user
    void* block_addr;                // Actual start address of the buddy block
    size_t block_size;               // Size of the buddy block
    int    order;                    // Order of the buddy block
    const char* source_file;         // File where allocation occurred
    int source_line;                 // Line where allocation occurred
    struct allocation_tracker* next; // Link for active/free lists
} allocation_tracker_t;

static allocation_tracker_t g_tracker_nodes[MAX_TRACKER_NODES]; // Static pool
static allocation_tracker_t *g_free_tracker_nodes = NULL;      // List of free tracker nodes
static allocation_tracker_t *g_active_allocations = NULL;      // List of active allocations
static spinlock_t g_alloc_tracker_lock;                        // Lock for tracker lists

/** @brief Initializes the debug tracker node pool. */
static void init_tracker_pool() {
    spinlock_init(&g_alloc_tracker_lock);
    g_free_tracker_nodes = NULL;
    g_active_allocations = NULL;
    for (int i = 0; i < MAX_TRACKER_NODES; ++i) {
        g_tracker_nodes[i].next = g_free_tracker_nodes;
        g_free_tracker_nodes = &g_tracker_nodes[i];
    }
}

/** @brief Allocates a tracker node from the free pool. Returns NULL if pool is empty. */
static allocation_tracker_t* alloc_tracker_node() {
    allocation_tracker_t* node = NULL;
    uintptr_t tracker_irq_flags = spinlock_acquire_irqsave(&g_alloc_tracker_lock);
    if (g_free_tracker_nodes) {
        node = g_free_tracker_nodes;
        g_free_tracker_nodes = node->next;
        node->next = NULL;
    }
    spinlock_release_irqrestore(&g_alloc_tracker_lock, tracker_irq_flags);
    if (!node) {
        serial_printf("[Buddy Debug] Warning: Allocation tracker pool exhausted!\n");
    }
    return node;
}

/** @brief Returns a tracker node to the free pool. */
static void free_tracker_node(allocation_tracker_t* node) {
    if (!node) return;
    uintptr_t tracker_irq_flags = spinlock_acquire_irqsave(&g_alloc_tracker_lock);
    node->next = g_free_tracker_nodes;
    g_free_tracker_nodes = node;
    spinlock_release_irqrestore(&g_alloc_tracker_lock, tracker_irq_flags);
}

/** @brief Adds a tracker node to the active allocations list. */
static void add_active_allocation(allocation_tracker_t* tracker) {
    if (!tracker) return;
    uintptr_t tracker_irq_flags = spinlock_acquire_irqsave(&g_alloc_tracker_lock);
    tracker->next = g_active_allocations;
    g_active_allocations = tracker;
    spinlock_release_irqrestore(&g_alloc_tracker_lock, tracker_irq_flags);
}

/** @brief Removes and returns the tracker node corresponding to user_addr. Returns NULL if not found. */
static allocation_tracker_t* remove_active_allocation(void* user_addr) {
    allocation_tracker_t* found_tracker = NULL;
    allocation_tracker_t** prev_next_ptr = &g_active_allocations;
    uintptr_t tracker_irq_flags = spinlock_acquire_irqsave(&g_alloc_tracker_lock);
    allocation_tracker_t* current = g_active_allocations;
    while (current) {
        if (current->user_addr == user_addr) {
            found_tracker = current;
            *prev_next_ptr = current->next; // Unlink
            break;
        }
        prev_next_ptr = &current->next;
        current = current->next;
    }
    spinlock_release_irqrestore(&g_alloc_tracker_lock, tracker_irq_flags);
    return found_tracker;
}
#endif // DEBUG_BUDDY


// === Internal Helper Functions ===

/**
 * @brief Calculates the required block order for a given user size.
 * Considers header overhead (in non-debug builds) and ensures the
 * resulting block can accommodate the request.
 * @param user_size The size requested by the caller.
 * @return The buddy order required, or MAX_ORDER + 1 if the size is too large.
 */
static int buddy_required_order(size_t user_size) {
    size_t required_total_size = user_size + BUDDY_HEADER_SIZE;

    // Check for overflow when adding header size
    if (required_total_size < user_size) {
        return MAX_ORDER + 1;
    }

    // Ensure minimum block size is met
    if (required_total_size < MIN_BLOCK_SIZE_INTERNAL) {
        required_total_size = MIN_BLOCK_SIZE_INTERNAL;
    }

    // Find the smallest power-of-2 block size that fits
    size_t block_size = MIN_BLOCK_SIZE_INTERNAL;
    int order = MIN_INTERNAL_ORDER;

    while (block_size < required_total_size) {
        // Check for overflow before shifting
        if (block_size > (SIZE_MAX >> 1)) {
            return MAX_ORDER + 1; // Cannot represent next block size
        }
        block_size <<= 1;
        order++;
        if (order > MAX_ORDER) {
            return MAX_ORDER + 1; // Exceeded maximum supported order
        }
    }
    return order;
}

/**
 * @brief Calculates the virtual address of the buddy block for a given block address and order.
 * @param block_addr Virtual address of the block.
 * @param order The order of the block.
 * @return Virtual address of the buddy block.
 */
static inline uintptr_t get_buddy_addr(uintptr_t block_addr, int order) {
    // XORing with the block size gives the buddy's address
    uintptr_t buddy_offset = (uintptr_t)1 << order;
    return block_addr ^ buddy_offset;
}

/**
 * @brief Adds a block (given by its virtual address) to the appropriate free list.
 * @param block_ptr Virtual address of the block to add.
 * @param order The order of the block being added.
 * @note Assumes the buddy lock is held by the caller.
 */
static void add_block_to_free_list(void *block_ptr, int order) {
    BUDDY_ASSERT(order >= MIN_INTERNAL_ORDER && order <= MAX_ORDER, "Invalid order in add_block_to_free_list");
    BUDDY_ASSERT(block_ptr != NULL, "Adding NULL block to free list");

    buddy_block_t *block = (buddy_block_t*)block_ptr;
    block->next = free_lists[order];
    free_lists[order] = block;
}

/**
 * @brief Removes a specific block (given by its virtual address) from its free list.
 * @param block_ptr Virtual address of the block to remove.
 * @param order The order of the block to remove.
 * @return true if found and removed, false otherwise.
 * @note Assumes the buddy lock is held by the caller.
 */
static bool remove_block_from_free_list(void *block_ptr, int order) {
    BUDDY_ASSERT(order >= MIN_INTERNAL_ORDER && order <= MAX_ORDER, "Invalid order in remove_block_from_free_list");
    BUDDY_ASSERT(block_ptr != NULL, "Removing NULL block from free list");

    buddy_block_t **prev_next_ptr = &free_lists[order];
    buddy_block_t *current = free_lists[order];

    while (current) {
        if (current == (buddy_block_t*)block_ptr) {
            *prev_next_ptr = current->next; // Unlink
            return true;
        }
        prev_next_ptr = &current->next;
        current = current->next;
    }
    return false; // Not found
}

/**
 * @brief Converts a block size (must be power of 2 >= MIN_BLOCK_SIZE_INTERNAL) to its buddy order.
 * @param block_size The size of the block.
 * @return The buddy order, or MAX_ORDER + 1 if size is invalid or out of range.
 */
static int __attribute__((unused)) buddy_block_size_to_order(size_t block_size) {
    // Check if size is a power of 2
    if (block_size < MIN_BLOCK_SIZE_INTERNAL || (block_size & (block_size - 1)) != 0) {
        return MAX_ORDER + 1; // Not a valid power-of-2 block size or too small
    }

    int order = 0;
    size_t size = 1;
    while (size < block_size) {
        // Check for potential overflow before shifting (unlikely with size_t but safe)
        if (size > (SIZE_MAX >> 1)) return MAX_ORDER + 1;
        size <<= 1;
        order++;
        if (order > MAX_ORDER) {
            return MAX_ORDER + 1; // Exceeded maximum supported order
        }
    }

    // Ensure the calculated order is within the valid range
    if (order < MIN_INTERNAL_ORDER) {
        // This should only happen if block_size was valid power-of-2 but somehow < MIN_BLOCK_SIZE_INTERNAL
        return MAX_ORDER + 1;
    }
    return order;
}


// === Initialization ===

/**
 * @brief Initializes the buddy allocator system.
 *
 * Sets up the free lists and populates them based on the provided memory region.
 * Assumes the physical memory region is already mapped contiguously into the
 * kernel's higher-half virtual address space.
 *
 * @param heap_region_phys_start_ptr Physical start address of the memory region.
 * @param region_size Size of the memory region in bytes.
 */
void buddy_init(void *heap_region_phys_start_ptr, size_t region_size) {
    uintptr_t heap_region_phys_start = (uintptr_t)heap_region_phys_start_ptr;
    serial_printf("[Buddy] Initializing...\n");
    serial_printf(" Input Region Phys Start: %#lx, Size: %lu bytes\n", (unsigned long)heap_region_phys_start, region_size); // Use %lu for size_t

    // 1. Basic Sanity Checks
    if (heap_region_phys_start == 0 || region_size < MIN_BLOCK_SIZE_INTERNAL) {
        BUDDY_PANIC("Invalid region parameters for buddy_init");
    }
    if (KERNEL_SPACE_VIRT_START == 0) {
        BUDDY_PANIC("KERNEL_SPACE_VIRT_START is not defined or zero");
    }
    if (MAX_ORDER >= (sizeof(uintptr_t) * 8)) { // Check if MAX_ORDER is too large
        BUDDY_PANIC("MAX_ORDER too large for address space");
    }

    // 2. Initialize Locks and Free Lists
    for (int i = 0; i <= MAX_ORDER; i++) free_lists[i] = NULL;
    spinlock_init(&g_buddy_lock);
    #ifdef DEBUG_BUDDY
    init_tracker_pool();
    #endif

    // 3. Calculate Aligned Physical and Corresponding Virtual Start
    // Align the start address UP to the largest possible block size boundary
    size_t max_block_alignment = (size_t)1 << MAX_ORDER;
    g_buddy_heap_phys_start_addr = ALIGN_UP(heap_region_phys_start, max_block_alignment);
    size_t adjustment = g_buddy_heap_phys_start_addr - heap_region_phys_start;

    // Check if enough space remains after alignment
    if (adjustment >= region_size || (region_size - adjustment) < MIN_BLOCK_SIZE_INTERNAL) {
        // Use %lu for size_t
        serial_printf("[Buddy] Error: Not enough space in region after aligning start to %lu bytes.\n", max_block_alignment);
        g_heap_start_virt_addr = 0; g_heap_end_virt_addr = 0; // Mark as uninitialized
        return;
    }

    size_t available_size = region_size - adjustment;
    g_heap_start_virt_addr = KERNEL_SPACE_VIRT_START + g_buddy_heap_phys_start_addr;

    // Check for virtual address overflow on start address calculation
    if (g_heap_start_virt_addr < KERNEL_SPACE_VIRT_START || g_heap_start_virt_addr < g_buddy_heap_phys_start_addr) {
         BUDDY_PANIC("Virtual heap start address overflowed or invalid");
    }

    g_heap_end_virt_addr = g_heap_start_virt_addr; // Tentative end, adjusted below
    // Use %lx for uintptr_t addresses
    serial_printf("  Aligned Phys Start: 0x%lx, Corresponding Virt Start: 0x%lx\n", g_buddy_heap_phys_start_addr, g_heap_start_virt_addr);
    // Use %lu for size_t
    serial_printf("  Available Size after alignment: %lu bytes\n", available_size);

    // 4. Populate Free Lists with Initial Blocks (using VIRTUAL addresses)
    g_buddy_total_managed_size = 0;
    g_buddy_free_bytes = 0;
    uintptr_t current_virt_addr = g_heap_start_virt_addr;
    size_t remaining_size = available_size;

    // Loop through remaining space, adding largest possible aligned blocks
    while (remaining_size >= MIN_BLOCK_SIZE_INTERNAL) {
        int order = MAX_ORDER;
        size_t block_size = (size_t)1 << order;

        // Find largest block that fits and maintains alignment relative to heap start
        while (order >= MIN_INTERNAL_ORDER) {
            block_size = (size_t)1 << order;
            // Check if block fits AND if current address is aligned for this block size
            // relative to the start of the managed heap.
            if (block_size <= remaining_size &&
                ((current_virt_addr - g_heap_start_virt_addr) % block_size == 0))
            {
                break; // Found suitable block order
            }
            order--;
        }

        if (order < MIN_INTERNAL_ORDER) {
            break; // Cannot fit even the smallest block at the current address
        }

        // Add the block to the free list for its order
        add_block_to_free_list((void*)current_virt_addr, order);

        // Update tracking variables
        g_buddy_total_managed_size += block_size;
        g_buddy_free_bytes += block_size;
        current_virt_addr += block_size;
        remaining_size -= block_size;

        // Check for virtual address overflow during loop
        if (current_virt_addr < g_heap_start_virt_addr) {
            serial_printf("[Buddy] Warning: Virtual address wrapped during init loop. Halting population.\n");
            break;
        }
    }

    // Set the final virtual end address based on the blocks actually added
    g_heap_end_virt_addr = g_heap_start_virt_addr + g_buddy_total_managed_size;
    if (g_heap_end_virt_addr < g_heap_start_virt_addr) { // Handle overflow
        g_heap_end_virt_addr = UINTPTR_MAX;
    }

    // Use %lx for uintptr_t addresses
    serial_printf("[Buddy] Init done. Managed VIRT Range: [0x%lx - 0x%lx)\n", g_heap_start_virt_addr, g_heap_end_virt_addr);
    // Use %lu for size_t
    serial_printf("  Total Managed: %lu bytes, Initially Free: %lu bytes\n", g_buddy_total_managed_size, g_buddy_free_bytes);
    if (remaining_size > 0) {
        // Use %lu for size_t
        serial_printf("  (Note: %lu bytes unused at end of region due to alignment/size)\n", remaining_size);
    }
}


// === Allocation ===

/**
 * @brief Internal implementation for buddy allocation. Finds/splits blocks.
 * @param requested_order The desired block order.
 * @param file Source file name (for debug builds).
 * @param line Source line number (for debug builds).
 * @return Virtual address of the allocated block, or NULL on failure.
 * @note Assumes the buddy lock is held by the caller.
 */
static void* buddy_alloc_impl(int requested_order, const char* file, int line) {
    //serial_printf("[Buddy Alloc Impl] Enter: Request order %d. File: %s Line: %d\n", requested_order, file, line); // LOG ENTRY

    // Find the smallest available block order >= requested_order
    int order = requested_order;
    while (order <= MAX_ORDER) {
        if (free_lists[order] != NULL) {
            break; // Found a suitable free list
        }
        order++;
    }

    if (order > MAX_ORDER) { // Out of memory
        g_failed_alloc_count++;
        #ifdef DEBUG_BUDDY
        serial_printf("[Buddy OOM @ %s:%d] Order %d requested, no suitable blocks found.\n", file, line, requested_order);
        #else
        // serial_printf("[Buddy OOM] Order %d requested.\n", requested_order); // Optional non-debug log
        #endif
        serial_printf("[Buddy Alloc Impl] Exit: OOM for order %d. Returning NULL.\n", requested_order); // LOG OOM EXIT
        return NULL;
    }

    // Remove block from the found free list
    buddy_block_t *block = free_lists[order];
    free_lists[order] = block->next; // Dequeue

    // Split the block down to the requested order if necessary
    while (order > requested_order) {
        order--; // Go down one order
        size_t half_block_size = (size_t)1 << order;
        // Calculate the address of the buddy (the upper half)
        uintptr_t buddy_addr = (uintptr_t)block + half_block_size;
        // Add the buddy (upper half) to the free list of the smaller order
        add_block_to_free_list((void*)buddy_addr, order);
        // Continue splitting the lower half (pointed to by 'block')
    }

    // Update statistics
    size_t allocated_block_size = (size_t)1 << requested_order;
    g_buddy_free_bytes -= allocated_block_size;
    g_alloc_count++;

    // --- Physical Address Alignment Assertion ---
    // This check ensures that if we allocate a page-sized block or larger,
    // the corresponding physical address is page-aligned.
    #ifdef PAGE_ORDER
    if (requested_order >= PAGE_ORDER) {
        uintptr_t block_addr_virt = (uintptr_t)block;

        // --- CORRECTED Physical Address Calculation ---
        // Calculate offset within the managed virtual heap
        uintptr_t offset_in_heap = block_addr_virt - g_heap_start_virt_addr;
        // Add offset to the physical start address of the heap
        uintptr_t physical_addr = g_buddy_heap_phys_start_addr + offset_in_heap;

        // Debug print before assertion
        // Use %lx for addresses, %u for integers, %lu for size_t
        // serial_printf("[Buddy Assert Check] Order=%d, Virt=0x%lx, Phys=0x%lx, PhysStart=0x%lx, PSize=%lu\n",
        //                 requested_order, block_addr_virt, physical_addr, g_buddy_heap_phys_start_addr, (unsigned long)PAGE_SIZE);

        // Assert physical alignment
        BUDDY_ASSERT((physical_addr % PAGE_SIZE) == 0,
                     "Buddy returned non-page-aligned PHYS block for page-sized request!");

        // Assert virtual alignment (should be guaranteed by buddy logic & init alignment)
        BUDDY_ASSERT((block_addr_virt % PAGE_SIZE) == 0,
                     "Buddy returned non-page-aligned VIRTUAL block for page-sized request!");
    }
    #endif

    //serial_printf("[Buddy Alloc Impl] Exit: Allocated block %p for order %d.\n", (void*)block, requested_order); // LOG SUCCESS EXIT
    // Return the VIRTUAL address of the allocated block
    return (void*)block;
}


// === Free ===

/**
 * @brief Internal implementation for freeing a buddy block. Handles coalescing.
 * @param block_addr_virt Virtual address of the block to free.
 * @param block_order Order of the block being freed.
 * @param file Source file name (for debug builds).
 * @param line Source line number (for debug builds).
 * @note Assumes the buddy lock is held by the caller.
 */
static void buddy_free_impl(void *block_addr_virt, int block_order, const char* file, int line) {
    uintptr_t addr_virt = (uintptr_t)block_addr_virt;
    size_t block_size = (size_t)1 << block_order;

    // --- Basic Validity Checks ---
    BUDDY_ASSERT(block_order >= MIN_INTERNAL_ORDER && block_order <= MAX_ORDER, "Invalid order in buddy_free_impl");
    BUDDY_ASSERT(addr_virt >= g_heap_start_virt_addr && addr_virt < g_heap_end_virt_addr, "Address outside heap in buddy_free_impl");
    // Check alignment relative to the start of the managed heap
    BUDDY_ASSERT(((addr_virt - g_heap_start_virt_addr) % block_size) == 0, "Address not aligned to block size relative to heap start in buddy_free_impl");
    // ---------------------------------

    // Coalescing loop: Try to merge with buddy until max order or buddy is not free
    while (block_order < MAX_ORDER) {
        uintptr_t buddy_addr_virt = get_buddy_addr(addr_virt, block_order);

        // Boundary check for buddy address
        if (buddy_addr_virt < g_heap_start_virt_addr || buddy_addr_virt >= g_heap_end_virt_addr) {
             break; // Buddy is outside the managed heap, cannot coalesce
        }

        // Attempt to find and remove the buddy from its free list
        if (remove_block_from_free_list((void*)buddy_addr_virt, block_order)) {
            // Buddy was free! Merge them.
            // The new, larger block starts at the lower of the two addresses.
            if (buddy_addr_virt < addr_virt) {
                addr_virt = buddy_addr_virt;
            }
            // Increase order for the next level of potential coalescing
            block_order++;
            #ifdef DEBUG_BUDDY
            // use %lx for address
            // serial_printf("  [Buddy Free Debug %s:%d] Coalesced order %d->%d V=0x%lx\n", file, line, block_order-1, block_order, addr_virt);
            #endif
        } else {
            break; // Buddy not found in free list, cannot coalesce further
        }
    }

    // Add the final (potentially coalesced) block to the appropriate free list
    add_block_to_free_list((void*)addr_virt, block_order);

    // Update statistics (add back size of the *originally* freed block)
    // Note: block_size was calculated based on the initial block_order passed in.
    g_buddy_free_bytes += block_size;
    g_free_count++;
}


// === Public API Implementations ===

#ifdef DEBUG_BUDDY
/* buddy_alloc_internal (Debug Version Wrapper) */
void *buddy_alloc_internal(size_t size, const char* file, int line) {
    if (size == 0) return NULL;

    int req_order = buddy_required_order(size);
    if (req_order > MAX_ORDER) {
        serial_printf("[Buddy DEBUG %s:%d] Error: Size %lu too large (req order %d > max %d).\n", file, line, size, req_order, MAX_ORDER); // %lu for size_t
        // Acquire lock just to update stats safely
        uintptr_t flags = spinlock_acquire_irqsave(&g_buddy_lock);
        g_failed_alloc_count++;
        spinlock_release_irqrestore(&g_buddy_lock, flags);
        return NULL;
    }

    uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
    void *block_ptr = buddy_alloc_impl(req_order, file, line);
    spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);

    if (!block_ptr) return NULL; // buddy_alloc_impl already logged and updated stats

    size_t block_size = (size_t)1 << req_order;
    void* user_ptr = block_ptr; // In debug, user address is the block address

    // Track allocation
    allocation_tracker_t* tracker = alloc_tracker_node();
    if (!tracker) {
        serial_printf("[Buddy DEBUG %s:%d] CRITICAL: Failed to alloc tracker! Freeing block.\n", file, line);
        // Free the block we just allocated
        uintptr_t free_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
        buddy_free_impl(block_ptr, req_order, __FILE__, __LINE__); // Use internal free
        spinlock_release_irqrestore(&g_buddy_lock, free_irq_flags);
        // Adjust stats: allocation failed overall
        uintptr_t stat_flags = spinlock_acquire_irqsave(&g_buddy_lock);
        g_failed_alloc_count++; // Increment failure
        g_alloc_count--;        // Decrement success count from buddy_alloc_impl
        g_buddy_free_bytes += block_size; // Add block back to free count
        spinlock_release_irqrestore(&g_buddy_lock, stat_flags);
        return NULL;
    }

    tracker->user_addr = user_ptr;
    tracker->block_addr = block_ptr;
    tracker->block_size = block_size;
    tracker->order = req_order; // Store order
    tracker->source_file = file;
    tracker->source_line = line;
    add_active_allocation(tracker);

    // Place canaries
    if (block_size >= sizeof(uint32_t) * 2) {
        *(volatile uint32_t*)block_ptr = DEBUG_CANARY_START;
        *(volatile uint32_t*)((uintptr_t)block_ptr + block_size - sizeof(uint32_t)) = DEBUG_CANARY_END;
    } else if (block_size >= sizeof(uint32_t)) {
         *(volatile uint32_t*)block_ptr = DEBUG_CANARY_START;
    }

    return user_ptr;
}

/* buddy_free_internal (Debug Version Wrapper) */
void buddy_free_internal(void *ptr, const char* file, int line) {
    if (!ptr) return;

    uintptr_t addr = (uintptr_t)ptr;
    // Check range FIRST (user address should match block address in debug)
    // Use %lx for addresses
    if (addr < g_heap_start_virt_addr || addr >= g_heap_end_virt_addr) {
        serial_printf("[Buddy DEBUG %s:%d] Error: Freeing 0x%p outside heap [0x%lx - 0x%lx).\n",
                        file, line, ptr, g_heap_start_virt_addr, g_heap_end_virt_addr);
        BUDDY_PANIC("Freeing pointer outside heap!");
        return;
    }

    // Find and remove tracker based on user_addr (which is ptr)
    allocation_tracker_t* tracker = remove_active_allocation(ptr);
    if (!tracker) {
        serial_printf("[Buddy DEBUG %s:%d] Error: Freeing untracked/double-freed pointer 0x%p\n", file, line, ptr);
        BUDDY_PANIC("Freeing untracked pointer!");
        return;
    }

    // Use details from the tracker
    size_t block_size = tracker->block_size;
    void* block_ptr = tracker->block_addr; // Actual block start address
    int block_order = tracker->order;
    bool canary_ok = true;

    // Validate canaries based on block_ptr and block_size
    // Use %lu for size_t
    if (block_size >= sizeof(uint32_t) * 2) {
        if (*(volatile uint32_t*)block_ptr != DEBUG_CANARY_START) {
            serial_printf("[Buddy DEBUG %s:%d] CORRUPTION: Start canary fail block 0x%p (size %lu, order %d) freed from 0x%p! Alloc@ %s:%d\n",
                            file, line, block_ptr, block_size, block_order, ptr, tracker->source_file, tracker->source_line);
            canary_ok = false;
        }
        if (*(volatile uint32_t*)((uintptr_t)block_ptr + block_size - sizeof(uint32_t)) != DEBUG_CANARY_END) {
             serial_printf("[Buddy DEBUG %s:%d] CORRUPTION: End canary fail block 0x%p (size %lu, order %d) freed from 0x%p! Alloc@ %s:%d\n",
                            file, line, block_ptr, block_size, block_order, ptr, tracker->source_file, tracker->source_line);
            canary_ok = false;
        }
    } else if (block_size >= sizeof(uint32_t)) {
         if (*(volatile uint32_t*)block_ptr != DEBUG_CANARY_START) {
            serial_printf("[Buddy DEBUG %s:%d] CORRUPTION: Start canary fail (small block) block 0x%p (size %lu, order %d) freed from 0x%p! Alloc@ %s:%d\n",
                            file, line, block_ptr, block_size, block_order, ptr, tracker->source_file, tracker->source_line);
            canary_ok = false;
         }
    }

    if (!canary_ok) {
        BUDDY_PANIC("Heap corruption detected by canary!");
        // Free tracker even on panic? Maybe not, leave state for debugging.
        // free_tracker_node(tracker);
        return; // Stop here if panic doesn't halt
    }

    // Check tracked order validity
    if (block_order < MIN_INTERNAL_ORDER || block_order > MAX_ORDER) {
        serial_printf("[Buddy DEBUG %s:%d] Error: Invalid tracked block order %d for ptr 0x%p\n", file, line, block_order, ptr);
        free_tracker_node(tracker); // Still free tracker
        BUDDY_PANIC("Invalid block order in tracker");
        return;
    }

    // Perform actual free using block_ptr and block_order from tracker
    uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
    buddy_free_impl(block_ptr, block_order, file, line);
    spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);

    // Return tracker node to pool
    free_tracker_node(tracker);
}

/* buddy_dump_leaks (Debug Version) */
void buddy_dump_leaks(void) {
    terminal_write("\n--- Buddy Allocator Leak Check ---\n");
    uintptr_t tracker_irq_flags = spinlock_acquire_irqsave(&g_alloc_tracker_lock);
    allocation_tracker_t* current = g_active_allocations;
    int leak_count = 0;
    size_t leak_bytes = 0;
    if (!current) {
        terminal_write("No active allocations tracked. No leaks detected.\n");
    } else {
        terminal_write("Detected potential memory leaks (unfreed blocks):\n");
        while(current) {
            // Use %lu for size_t
            serial_printf("  - User Addr: 0x%p, Block Addr: 0x%p, Block Size: %lu bytes (Order %d), Allocated at: %s:%d\n",
                            current->user_addr, current->block_addr, current->block_size, current->order,
                            current->source_file ? current->source_file : "<unknown>",
                            current->source_line);
            leak_count++;
            leak_bytes += current->block_size;
            current = current->next;
        }
        // Use %lu for size_t
        serial_printf("Total Leaks: %d blocks, %lu bytes (buddy block size)\n", leak_count, leak_bytes);
    }
    terminal_write("----------------------------------\n");
    spinlock_release_irqrestore(&g_alloc_tracker_lock, tracker_irq_flags);
}

#else // !DEBUG_BUDDY (Non-Debug Implementations)

/* buddy_alloc (Non-Debug Version) */
void *buddy_alloc(size_t size) {
    if (size == 0) return NULL;

    int req_order = buddy_required_order(size);
    if (req_order > MAX_ORDER) {
        // Acquire lock just to update stats safely
        uintptr_t flags = spinlock_acquire_irqsave(&g_buddy_lock);
        g_failed_alloc_count++;
        spinlock_release_irqrestore(&g_buddy_lock, flags);
        return NULL;
    }

    uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
    void *block_ptr = buddy_alloc_impl(req_order, NULL, 0); // Pass NULL file/line
    spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);

    if (!block_ptr) return NULL; // buddy_alloc_impl already updated stats

    // Prepend header
    uintptr_t block_addr = (uintptr_t)block_ptr;
    buddy_header_t* header = (buddy_header_t*)block_addr;
    header->order = (uint8_t)req_order;
    void* user_ptr = (void*)(block_addr + BUDDY_HEADER_SIZE);

    return user_ptr;
}

/* buddy_free (Non-Debug Version) */
void buddy_free(void *ptr) {
    if (!ptr) return;

    uintptr_t user_addr = (uintptr_t)ptr;

    // Enhanced debugging: Check for obviously corrupted pointers (Linux-compatible)
    if (user_addr < 0x1000) {
        serial_printf("[Buddy] Error: Freeing small pointer 0x%lx (likely corrupted/NULL-like).\n", user_addr);
        BUDDY_PANIC("Freeing corrupted pointer!");
        return;
    }

    // Check range FIRST, considering header size  
    if (user_addr < (g_heap_start_virt_addr + BUDDY_HEADER_SIZE) || user_addr >= g_heap_end_virt_addr) {
        serial_printf("[Buddy] Error: Freeing pointer 0x%lx outside heap [0x%lx - 0x%lx).\n",
                        user_addr, g_heap_start_virt_addr + BUDDY_HEADER_SIZE, g_heap_end_virt_addr);
        BUDDY_PANIC("Freeing pointer outside heap!");
        return;
    }

    // Calculate block address and get order from header
    uintptr_t block_addr = user_addr - BUDDY_HEADER_SIZE;
    buddy_header_t* header = (buddy_header_t*)block_addr;
    int block_order = header->order;

    // Validate order from header
    if (block_order < MIN_INTERNAL_ORDER || block_order > MAX_ORDER) {
        serial_printf("[Buddy] Error: Corrupted header freeing 0x%p (order %d).\n", ptr, block_order);
        BUDDY_PANIC("Corrupted buddy header!");
        return;
    }

    // Validate alignment based on order from header
    size_t block_size = (size_t)1 << block_order;
    // Use %lx for addresses
    if ((block_addr - g_heap_start_virt_addr) % block_size != 0) {
        serial_printf("[Buddy] Error: Freeing misaligned pointer 0x%p (derived block 0x%lx, order %d)\n", ptr, block_addr, block_order);
        BUDDY_PANIC("Freeing pointer yielding misaligned block!");
        return;
    }

    // Perform actual free
    uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
    buddy_free_impl((void*)block_addr, block_order, NULL, 0); // Pass NULL file/line
    spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);
}

/**
 * @brief Allocates a raw buddy block of the specified order. Handles locking internally.
 * FOR KERNEL INTERNAL USE ONLY (e.g., page frame allocator). No header prepended.
 * @param order The exact buddy order to allocate (MIN_ORDER to MAX_ORDER).
 * @return Virtual address of the allocated block, or NULL on failure.
 */
 void* buddy_alloc_raw(int order) {
    //serial_printf("[Buddy Raw Alloc] Requesting order %d\n", order); // LOG ENTRY
    if (order < MIN_INTERNAL_ORDER || order > MAX_ORDER) {
         serial_printf("[Buddy Raw Alloc] Error: Invalid order %d requested.\n", order);
         // Acquire lock just to update stats safely
         uintptr_t flags = spinlock_acquire_irqsave(&g_buddy_lock);
         g_failed_alloc_count++;
         spinlock_release_irqrestore(&g_buddy_lock, flags);
         return NULL; // Return NULL on invalid order
    }

    uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
    //serial_printf("[Buddy Raw Alloc] Lock acquired. Calling buddy_alloc_impl for order %d\n", order); // LOG BEFORE IMPL
    void *block_ptr = buddy_alloc_impl(order, __FILE__, __LINE__); // Pass file/line even in non-debug for OOM trace
    //serial_printf("[Buddy Raw Alloc] buddy_alloc_impl returned %p for order %d\n", block_ptr, order); // LOG AFTER IMPL
    spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);
    //serial_printf("[Buddy Raw Alloc] Lock released. Returning %p\n", block_ptr); // LOG EXIT
    return block_ptr;
}

/**
 * @brief Frees a raw buddy block of the specified order. Handles locking internally.
 * FOR KERNEL INTERNAL USE ONLY. Assumes ptr is the actual block address.
 * @param block_addr_virt Virtual address of the block to free.
 * @param order The exact buddy order of the block being freed.
 */
void buddy_free_raw(void* block_addr_virt, int order) {
     if (!block_addr_virt) return;

     // Basic validation on order and address
     if (order < MIN_INTERNAL_ORDER || order > MAX_ORDER) {
         serial_printf("[Buddy Raw Free] Error: Invalid order %d for block 0x%p.\n", order, block_addr_virt);
         BUDDY_PANIC("Invalid order in buddy_free_raw");
         return;
     }
      uintptr_t addr = (uintptr_t)block_addr_virt;
      // Use %lx for addresses
      if (addr < g_heap_start_virt_addr || addr >= g_heap_end_virt_addr) {
          serial_printf("[Buddy Raw Free] Error: Freeing 0x%p outside heap [0x%lx - 0x%lx).\n", block_addr_virt, g_heap_start_virt_addr, g_heap_end_virt_addr);
          BUDDY_PANIC("Freeing pointer outside heap in buddy_free_raw");
          return;
      }
      size_t block_size = (size_t)1 << order;
      if ((addr - g_heap_start_virt_addr) % block_size != 0) { // Check alignment relative to heap start
          serial_printf("[Buddy Raw Free] Error: Freeing misaligned pointer 0x%p for order %d.\n", block_addr_virt, order);
          BUDDY_PANIC("Freeing misaligned pointer in buddy_free_raw");
          return;
      }

     uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
     // Call internal implementation (pass dummy file/line for potential debug traces inside)
     buddy_free_impl(block_addr_virt, order, __FILE__, __LINE__);
     spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);

     // Note: buddy_free_impl already increments g_free_count
}

#endif // DEBUG_BUDDY


// === Statistics ===

/** @brief Returns the current amount of free space managed by the allocator. */
size_t buddy_free_space(void) {
    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
    size_t free_bytes = g_buddy_free_bytes;
    spinlock_release_irqrestore(&g_buddy_lock, irq_flags);
    return free_bytes;
}

/** @brief Returns the total amount of space initially managed by the allocator. */
size_t buddy_total_space(void) {
    // This value is set during init and doesn't change, no lock needed.
    return g_buddy_total_managed_size;
}

/** @brief Fills a structure with current allocator statistics. */
void buddy_get_stats(buddy_stats_t *stats) {
    if (!stats) return;
    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
    stats->total_bytes = g_buddy_total_managed_size;
    stats->free_bytes = g_buddy_free_bytes;
    stats->alloc_count = g_alloc_count;
    stats->free_count = g_free_count;
    stats->failed_alloc_count = g_failed_alloc_count;
    spinlock_release_irqrestore(&g_buddy_lock, irq_flags);
}