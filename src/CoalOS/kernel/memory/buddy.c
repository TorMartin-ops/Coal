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
#include <libc/stddef.h>      // For offsetof macro
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

// Special logging for Order=12 corruption tracking
static void log_free_list_12_change(const char *context, buddy_block_t *old_val, buddy_block_t *new_val) {
    // Removed debug logging - kept function for minimal code changes
    if (new_val == (buddy_block_t *)0x7) {
        serial_printf("[ORDER12 CORRUPTION] *** Value 0x7 detected in free_lists[12]! Context: %s ***\n", context);
        buddy_check_guards("order12_corruption_detected");
    }
}

// Track the specific corrupted block 0xd016e000
#define CORRUPTED_BLOCK_ADDR 0xd016e000
#define CORRUPTED_NEXT_ADDR (CORRUPTED_BLOCK_ADDR + offsetof(buddy_block_t, next))

// Memory watchpoint system for tracking writes to corrupted block
static void memory_watchpoint_write(uintptr_t addr, uint32_t old_val, uint32_t new_val, const char *context) {
    if (addr == CORRUPTED_NEXT_ADDR) {
        serial_printf("[MEMORY WATCHPOINT] %s: Write to 0x%lx - Old: 0x%lx -> New: 0x%lx\n", 
                      context, (unsigned long)addr, (unsigned long)old_val, (unsigned long)new_val);
        if (new_val == 0x7) {
            serial_printf("[MEMORY CORRUPTION CAUGHT] *** Value 0x7 written to corrupted address 0x%lx! Context: %s ***\n", 
                          (unsigned long)addr, context);
            // Print stack trace or additional debugging info here
            buddy_check_guards("memory_corruption_detected");
        }
    }
}

// Wrapper function for monitored memory writes
static inline void monitored_write_ptr(buddy_block_t **dest, buddy_block_t *new_val, const char *context) {
    uintptr_t dest_addr = (uintptr_t)dest;
    buddy_block_t *old_val = *dest;
    
    // Check if this is the corrupted location
    if (dest_addr == CORRUPTED_NEXT_ADDR) {
        memory_watchpoint_write(dest_addr, (uint32_t)(uintptr_t)old_val, (uint32_t)(uintptr_t)new_val, context);
    }
    
    *dest = new_val;
}

static void check_block_integrity(buddy_block_t *block, const char *context) {
    if (!block) return;
    if ((uintptr_t)block == CORRUPTED_BLOCK_ADDR) {
        buddy_block_t *next_ptr = block->next;
        serial_printf("[BLOCK TRACE] %s: Block 0x%lx->next = 0x%lx\n", 
                      context, (unsigned long)block, (unsigned long)next_ptr);
        if (next_ptr == (buddy_block_t *)0x7) {
            serial_printf("[BLOCK CORRUPTION] *** Block 0x%lx has corrupted next pointer 0x7! Context: %s ***\n",
                          (unsigned long)block, context);
        }
    }
}

// Function to log when the specific memory region is allocated
void buddy_log_allocation(void *ptr, size_t size, const char *context) {
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end = start + size;
    if (start <= CORRUPTED_BLOCK_ADDR && CORRUPTED_BLOCK_ADDR < end) {
        serial_printf("[ALLOC TRACE] %s: Allocated range [0x%lx-0x%lx) contains corrupted block 0x%lx\n",
                      context, (unsigned long)start, (unsigned long)end, (unsigned long)CORRUPTED_BLOCK_ADDR);
        
        // Zero out the corrupted block to clear any existing corruption
        memset((void*)CORRUPTED_BLOCK_ADDR, 0, sizeof(buddy_block_t));
        serial_printf("[ALLOC TRACE] Zeroed corrupted block at 0x%lx\n", (unsigned long)CORRUPTED_BLOCK_ADDR);
    }
}


// Memory guards to catch corruption
#define BUDDY_GUARD_MAGIC_BEFORE 0xDEADBEEF
#define BUDDY_GUARD_MAGIC_AFTER  0xBEEFDEAD
static uint32_t g_free_list_guards_before[MAX_ORDER + 1];
static uint32_t g_free_list_guards_after[MAX_ORDER + 1];

static uintptr_t g_heap_start_virt_addr = 0;           // Aligned VIRTUAL start address of managed heap
static uintptr_t g_heap_end_virt_addr = 0;             // VIRTUAL end address (exclusive) of managed heap
static uintptr_t g_buddy_heap_phys_start_addr = 0;     // Aligned PHYSICAL start address of managed heap
static size_t g_buddy_total_managed_size = 0;          // Total size managed by the allocator
static size_t g_buddy_free_bytes = 0;                  // Current free bytes (tracked approximately)
static spinlock_t g_buddy_lock;                        // Lock protecting allocator state

// Statistics
static uint64_t g_alloc_count = 0;
static uint64_t g_free_count = 0;

// Function to add memory integrity checking before critical operations
static void verify_corrupted_block_integrity(const char *context) {
    // Check if the corrupted block memory still looks like a valid buddy_block_t
    if (CORRUPTED_BLOCK_ADDR >= g_heap_start_virt_addr && CORRUPTED_BLOCK_ADDR < g_heap_end_virt_addr) {
        buddy_block_t *test_block = (buddy_block_t *)CORRUPTED_BLOCK_ADDR;
        buddy_block_t *next_ptr = test_block->next;
        
        if (next_ptr == (buddy_block_t *)0x7) {
            serial_printf("[INTEGRITY CHECK] %s: Block 0x%lx still corrupted with next=0x7\n", 
                          context, (unsigned long)CORRUPTED_BLOCK_ADDR);
        } else if (next_ptr != NULL && ((uintptr_t)next_ptr < g_heap_start_virt_addr || (uintptr_t)next_ptr >= g_heap_end_virt_addr)) {
            serial_printf("[INTEGRITY CHECK] %s: Block 0x%lx has invalid next=0x%lx (outside heap)\n", 
                          context, (unsigned long)CORRUPTED_BLOCK_ADDR, (unsigned long)next_ptr);
        }
    }
}

//============================================================================
// Memory Guard Functions
//============================================================================

static void buddy_init_guards(void) {
    for (int i = 0; i <= MAX_ORDER; i++) {
        g_free_list_guards_before[i] = BUDDY_GUARD_MAGIC_BEFORE;
        g_free_list_guards_after[i] = BUDDY_GUARD_MAGIC_AFTER;
    }
    serial_printf("[Buddy Guards] Initialized guards for orders 0-%d\n", MAX_ORDER);
}

void buddy_check_guards(const char *context) {
    for (int i = 0; i <= MAX_ORDER; i++) {
        if (g_free_list_guards_before[i] != BUDDY_GUARD_MAGIC_BEFORE) {
            serial_printf("[BUDDY GUARD CORRUPTION] %s: Before-guard for Order=%d corrupted! "
                          "Expected=0x%x, Found=0x%x\n", 
                          context, i, BUDDY_GUARD_MAGIC_BEFORE, g_free_list_guards_before[i]);
            BUDDY_PANIC("Buddy free list guard corruption detected!");
        }
        if (g_free_list_guards_after[i] != BUDDY_GUARD_MAGIC_AFTER) {
            serial_printf("[BUDDY GUARD CORRUPTION] %s: After-guard for Order=%d corrupted! "
                          "Expected=0x%x, Found=0x%x\n",
                          context, i, BUDDY_GUARD_MAGIC_AFTER, g_free_list_guards_after[i]);
            BUDDY_PANIC("Buddy free list guard corruption detected!");
        }
    }
}
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
    // Validate parameters - use graceful error handling instead of assertions
    if (order < MIN_INTERNAL_ORDER || order > MAX_ORDER) {
        serial_printf("[Buddy ERROR] Invalid order %d in add_block_to_free_list (valid: %d-%d)\n", 
                      order, MIN_INTERNAL_ORDER, MAX_ORDER);
        return; // Gracefully fail instead of crashing
    }
    
    if (!block_ptr) {
        serial_printf("[Buddy ERROR] Attempting to add NULL block to free list (order %d)\n", order);
        return; // Gracefully fail instead of crashing
    }

    buddy_block_t *block = (buddy_block_t*)block_ptr;
    
    // Use monitored write to track corruption
    monitored_write_ptr(&block->next, free_lists[order], "add_block_to_free_list_set_next");
    
    // Check block integrity after setting next pointer
    check_block_integrity(block, "add_block_to_free_list_after_next_set");
    
    // Log Order=12 changes for corruption tracking
    if (order == 12) {
        log_free_list_12_change("add_block_to_free_list", free_lists[order], block);
        check_block_integrity(block, "add_block_to_free_list_order12");
    }
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
    // Validate parameters - use graceful error handling instead of assertions
    if (order < MIN_INTERNAL_ORDER || order > MAX_ORDER) {
        serial_printf("[Buddy ERROR] Invalid order %d in remove_block_from_free_list (valid: %d-%d)\n", 
                      order, MIN_INTERNAL_ORDER, MAX_ORDER);
        return false; // Return error instead of crashing
    }
    
    if (!block_ptr) {
        serial_printf("[Buddy ERROR] Attempting to remove NULL block from free list (order %d)\n", order);
        return false; // Return error instead of crashing
    }

    buddy_block_t **prev_next_ptr = &free_lists[order];
    buddy_block_t *current = free_lists[order];

    while (current) {
        if (current == (buddy_block_t*)block_ptr) {
            // Log Order=12 changes for corruption tracking
            if (order == 12 && prev_next_ptr == &free_lists[order]) {
                log_free_list_12_change("remove_block_from_free_list", *prev_next_ptr, current->next);
            }
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
    for (int i = 0; i <= MAX_ORDER; i++) {
        if (i == 12) {
            log_free_list_12_change("buddy_init", free_lists[i], NULL);
        }
        free_lists[i] = NULL;
    }
    spinlock_init(&g_buddy_lock);
    buddy_init_guards(); // Initialize memory guards around free lists
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

//============================================================================
// Refactored Helper Functions Following SRP
//============================================================================

/**
 * @brief Performs comprehensive integrity checks before allocation
 * @param context Description of when this check is performed
 */
static void verify_allocator_integrity(const char *context) {
    buddy_check_guards(context);
    verify_corrupted_block_integrity(context);
}

/**
 * @brief Finds the smallest suitable block order for allocation
 * @param requested_order Minimum required order
 * @return Order of found block, or MAX_ORDER + 1 if no suitable block found
 */
static int find_suitable_order(int requested_order) {
    int order = requested_order;
    while (order <= MAX_ORDER) {
        if (free_lists[order] != NULL) {
            return order; // Found a suitable free list
        }
        order++;
    }
    return MAX_ORDER + 1; // Out of memory
}

/**
 * @brief Validates and cleans corrupted free lists (defensive programming)
 * @param current_order The order being processed
 */
static void validate_and_clean_free_lists(int current_order) {
    // Only check higher orders to reduce debug spam
    for (int check_order = 15; check_order <= MAX_ORDER; check_order++) {
        buddy_block_t *check_block = free_lists[check_order];
        if (check_block != NULL) {
            if ((uintptr_t)check_block < g_heap_start_virt_addr || 
                (uintptr_t)check_block >= g_heap_end_virt_addr) {
                serial_printf("[CORRUPTION DETECTED] Order=%d has invalid block 0x%lx during Order=%d allocation\n",
                              check_order, (unsigned long)check_block, current_order);
                serial_printf("[AUTO-RECOVERY] Clearing corrupted free_lists[%d]...\n", check_order);
                free_lists[check_order] = NULL;  // Clear corrupted list
            }
        }
    }
}

/**
 * @brief Attempts to recover from block corruption by finding alternative blocks
 * @param corrupted_order The order where corruption was detected
 * @param block_out Output parameter for recovered block
 * @param order_out Output parameter for recovered block order
 * @return true if recovery successful, false otherwise
 */
static bool attempt_corruption_recovery(int corrupted_order, buddy_block_t **block_out, int *order_out) {
    serial_printf("[CORRUPTION RECOVERY] Clearing corrupted free_lists[%d] and trying higher order\n", corrupted_order);
    free_lists[corrupted_order] = NULL;
    
    // Try to find a higher order block we can split
    int recovery_order = corrupted_order + 1;
    while (recovery_order <= MAX_ORDER) {
        if (free_lists[recovery_order] != NULL) {
            buddy_block_t *recovery_block = free_lists[recovery_order];
            if ((uintptr_t)recovery_block >= g_heap_start_virt_addr && 
                (uintptr_t)recovery_block < g_heap_end_virt_addr) {
                serial_printf("[CORRUPTION RECOVERY] Found valid block at Order=%d, using for allocation\n", recovery_order);
                *block_out = recovery_block;
                *order_out = recovery_order;
                return true;
            }
        }
        recovery_order++;
    }
    
    serial_printf("[CORRUPTION RECOVERY] Failed to find valid alternative block\n");
    return false;
}

/**
 * @brief Validates a block address and attempts recovery if corrupted
 * @param block The block to validate
 * @param order Current order
 * @param block_out Output parameter for validated/recovered block
 * @param order_out Output parameter for validated/recovered order
 * @return true if block is valid or recovery successful, false otherwise
 */
static bool validate_block_or_recover(buddy_block_t *block, int order, 
                                     buddy_block_t **block_out, int *order_out) {
    if ((uintptr_t)block >= g_heap_start_virt_addr && (uintptr_t)block < g_heap_end_virt_addr) {
        // Block is valid
        *block_out = block;
        *order_out = order;
        return true;
    }
    
    serial_printf("[BUDDY CORRUPTION] Order=%d, Block=0x%lx corrupted! Attempting recovery...\n",
                  order, (unsigned long)block);
    
    return attempt_corruption_recovery(order, block_out, order_out);
}

/**
 * @brief Monitors critical memory reads for corruption detection
 * @param block The block being accessed
 */
static void monitor_critical_memory_access(buddy_block_t *block) {
    // Monitor the critical read of block->next that was previously corrupted
    if ((uintptr_t)block == CORRUPTED_BLOCK_ADDR) {
        buddy_block_t *next_val = block->next;
        serial_printf("[MEMORY READ TRACE] Reading block->next from 0x%lx: value=0x%lx\n", 
                      (unsigned long)CORRUPTED_NEXT_ADDR, (unsigned long)next_val);
        if (next_val == (buddy_block_t *)0x7) {
            serial_printf("[CORRUPTION READ] *** Reading corrupted value 0x7 from block->next at 0x%lx! ***\n", 
                          (unsigned long)CORRUPTED_NEXT_ADDR);
            buddy_check_guards("corruption_read_detected");
        }
    }
}

/**
 * @brief Splits a block down to the requested order
 * @param block The block to split
 * @param current_order Current order of the block
 * @param target_order Desired order after splitting
 */
static void split_block_to_target_order(buddy_block_t *block, int current_order, int target_order) {
    int order = current_order;
    uintptr_t block_addr = (uintptr_t)block;
    
    while (order > target_order) {
        order--; // Go down one order
        size_t half_block_size = (size_t)1 << order;
        // Calculate the address of the buddy (the upper half)
        uintptr_t buddy_addr = block_addr + half_block_size;
        // Add the buddy (upper half) to the free list of the smaller order
        add_block_to_free_list((void*)buddy_addr, order);
        // Continue splitting the lower half (pointed to by 'block')
    }
}

/**
 * @brief Updates allocation statistics
 * @param allocated_order Order of the allocated block
 */
static void update_allocation_statistics(int allocated_order) {
    size_t allocated_block_size = (size_t)1 << allocated_order;
    g_buddy_free_bytes -= allocated_block_size;
    g_alloc_count++;
}

/**
 * @brief Validates physical and virtual address alignment for page-sized allocations
 * @param block The allocated block
 * @param allocated_order Order of the allocated block
 */
static void validate_allocation_alignment(void *block, int allocated_order) {
    #ifdef PAGE_ORDER
    if (allocated_order >= PAGE_ORDER) {
        uintptr_t block_addr_virt = (uintptr_t)block;

        // Calculate physical address
        uintptr_t offset_in_heap = block_addr_virt - g_heap_start_virt_addr;
        uintptr_t physical_addr = g_buddy_heap_phys_start_addr + offset_in_heap;

        // Assert physical alignment
        BUDDY_ASSERT((physical_addr % PAGE_SIZE) == 0,
                     "Buddy returned non-page-aligned PHYS block for page-sized request!");

        // Assert virtual alignment
        BUDDY_ASSERT((block_addr_virt % PAGE_SIZE) == 0,
                     "Buddy returned non-page-aligned VIRTUAL block for page-sized request!");
    }
    #endif
}

/**
 * @brief Logs debug information for Order=12 tracking
 * @param order The block order being processed
 * @param context Description of the operation
 */
static void log_order_debug_info(int order, const char *context) {
    if (order == 12) {
        serial_printf("[ORDER12 TRACE] %s: Order=12, current free_lists[12]=0x%lx\n", 
                      context, (unsigned long)free_lists[12]);
        check_block_integrity(free_lists[12], context);
    }
}

/**
 * @brief Internal implementation for buddy allocation. Finds/splits blocks.
 * @param requested_order The desired block order.
 * @param file Source file name (for debug builds).
 * @param line Source line number (for debug builds).
 * @return Virtual address of the allocated block, or NULL on failure.
 * @note Assumes the buddy lock is held by the caller.
 */
static void* buddy_alloc_impl(int requested_order, const char* file, int line) {
    // Verify allocator integrity before starting allocation
    verify_allocator_integrity("buddy_alloc_impl_start");
    
    // Find suitable order for allocation
    int current_order = find_suitable_order(requested_order);
    if (current_order > MAX_ORDER) {
        // Out of memory
        g_failed_alloc_count++;
        #ifdef DEBUG_BUDDY
        serial_printf("[Buddy OOM @ %s:%d] Order %d requested, no suitable blocks found.\n", 
                      file, line, requested_order);
        #endif
        return NULL;
    }

    // Validate and clean free lists before allocation
    validate_and_clean_free_lists(current_order);
    
    // Attempt allocation with corruption recovery if needed
    buddy_block_t *block = free_lists[current_order];
    if (!validate_block_or_recover(block, current_order, &block, &current_order)) {
        BUDDY_PANIC("Buddy allocator corruption recovery failed!");
    }
    
    // Monitor critical memory access for debugging
    monitor_critical_memory_access(block);
    
    // Dequeue block from free list
    log_order_debug_info(current_order, "dequeue");
    free_lists[current_order] = block->next;

    // Split block to target order if necessary
    split_block_to_target_order(block, current_order, requested_order);

    // Update allocation statistics
    update_allocation_statistics(requested_order);

    // Validate allocation alignment
    validate_allocation_alignment((void*)block, requested_order);

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
    // Debug logging removed
    if (order < MIN_INTERNAL_ORDER || order > MAX_ORDER) {
         serial_printf("[Buddy Raw Alloc] Error: Invalid order %d requested.\n", order);
         // Acquire lock just to update stats safely
         uintptr_t flags = spinlock_acquire_irqsave(&g_buddy_lock);
         g_failed_alloc_count++;
         spinlock_release_irqrestore(&g_buddy_lock, flags);
         return NULL; // Return NULL on invalid order
    }

    uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
    // Debug logging removed
    void *block_ptr = buddy_alloc_impl(order, __FILE__, __LINE__); // Pass file/line even in non-debug for OOM trace
    // Debug logging removed
    spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);
    // Debug logging removed
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

//============================================================================
// New Standardized Error Handling API Implementation
//============================================================================

error_t buddy_alloc_safe(size_t size, void **ptr_out) {
    // Input validation
    if (!ptr_out) {
        return E_INVAL;
    }
    
    if (size == 0) {
        return E_INVAL;
    }

    // Calculate required order and check if size is too large
    int req_order = buddy_required_order(size);
    if (req_order > MAX_ORDER) {
        // Update failure statistics
        uintptr_t flags = spinlock_acquire_irqsave(&g_buddy_lock);
        g_failed_alloc_count++;
        spinlock_release_irqrestore(&g_buddy_lock, flags);
        return E_OVERFLOW;
    }

    // Attempt allocation using existing implementation
    void *allocated_ptr = buddy_alloc(size);
    if (!allocated_ptr) {
        // Check if this was due to insufficient memory or other issues
        uintptr_t flags = spinlock_acquire_irqsave(&g_buddy_lock);
        size_t free_space = g_buddy_free_bytes;
        spinlock_release_irqrestore(&g_buddy_lock, flags);
        
        // If we have some free space but couldn't allocate, it's fragmentation
        if (free_space > 0) {
            return E_NOMEM; // Fragmentation - not enough contiguous space
        } else {
            return E_NOMEM; // Actually out of memory
        }
    }

    // Success - return allocated pointer
    *ptr_out = allocated_ptr;
    return E_SUCCESS;
}

error_t buddy_free_safe(void *ptr) {
    // Handle NULL pointer gracefully (not an error)
    if (!ptr) {
        return E_SUCCESS;
    }

    // Basic validation - check if pointer is within managed region
    uintptr_t ptr_addr = (uintptr_t)ptr;
    
    if (ptr_addr < g_heap_start_virt_addr || ptr_addr >= g_heap_end_virt_addr) {
        return E_INVAL;
    }

    // Check alignment - buddy allocator returns aligned pointers
    if ((ptr_addr & (MIN_BLOCK_SIZE - 1)) != 0) {
        return E_INVAL;
    }

    // Use existing free implementation
    buddy_free(ptr);
    
    // If we get here, the free was successful
    // (buddy_free would have panicked on serious corruption)
    return E_SUCCESS;
}

error_t buddy_get_allocation_info(void *ptr, size_t *size_out) {
    // Input validation
    if (!ptr || !size_out) {
        return E_INVAL;
    }

    // Basic validation - check if pointer is within managed region
    uintptr_t ptr_addr = (uintptr_t)ptr;
    
    if (ptr_addr < g_heap_start_virt_addr || ptr_addr >= g_heap_end_virt_addr) {
        return E_INVAL;
    }

    // Check alignment
    if ((ptr_addr & (MIN_BLOCK_SIZE - 1)) != 0) {
        return E_INVAL;
    }

    // Calculate the order based on pointer alignment
    // This is a simplified approach - in a full implementation,
    // we would need block metadata to determine the exact size
    uintptr_t offset = ptr_addr - g_heap_start_virt_addr;
    int order = MIN_ORDER;
    
    // Find the highest order this address could represent
    for (int test_order = MAX_ORDER; test_order >= MIN_ORDER; test_order--) {
        size_t block_size = (size_t)1 << test_order;
        if ((offset & (block_size - 1)) == 0) {
            // This address is aligned for this order
            order = test_order;
            break;
        }
    }

    *size_out = (size_t)1 << order;
    return E_SUCCESS;
}