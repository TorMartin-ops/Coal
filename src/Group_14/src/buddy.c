#include "buddy.h"     // Includes config constants (MIN/MAX_ORDER), macros, etc.
#include "terminal.h"
#include "types.h"
#include "spinlock.h"  // Include spinlock header

// Check if MIN_BLOCK_SIZE meets alignment requirements
#if (1 << MIN_ORDER) < DEFAULT_ALIGNMENT
#error "MIN_ORDER in buddy.h is too small for DEFAULT_ALIGNMENT"
#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096 // Fallback if not defined elsewhere
#endif

// Smallest allocatable block size
#define MIN_BLOCK_SIZE (1 << MIN_ORDER)

// Buddy block structure for free lists
typedef struct buddy_block {
    struct buddy_block *next;
} buddy_block_t;

// Array of free lists, one for each order
static buddy_block_t *free_lists[MAX_ORDER + 1] = {0};

// Global statistics
static uintptr_t g_heap_start_addr = 0; // Store aligned heap start
static size_t g_buddy_total_size = 0;
static size_t g_buddy_free_bytes = 0;

// Global Lock for Buddy Allocator core logic
static spinlock_t g_buddy_lock;

// --- Optional Debug Allocation Tracker ---
#ifdef DEBUG_BUDDY
typedef struct allocation_tracker {
    void* addr;                 // Allocated pointer
    size_t allocated_size;      // Actual power-of-2 size allocated by buddy
    const char* source_file;    // File where BUDDY_ALLOC was called
    int source_line;            // Line where BUDDY_ALLOC was called
    struct allocation_tracker* next;
} allocation_tracker_t;

static allocation_tracker_t *g_alloc_tracker_list = NULL;
static spinlock_t g_alloc_tracker_lock; // Separate lock for the tracker list

// Simple allocator for tracker nodes (uses buddy itself - careful!)
// Alternatively, use a dedicated slab cache if available early enough.
static allocation_tracker_t* alloc_tracker_node() {
    // Need to call the *internal* buddy alloc to avoid recursion through macros
    // Since this is for debug, a small overhead might be acceptable.
    // Ensure size is sufficient.
    return (allocation_tracker_t*)buddy_alloc_internal(sizeof(allocation_tracker_t), __FILE__, __LINE__);
}

static void free_tracker_node(allocation_tracker_t* node) {
     // Need to call the *internal* buddy free
     buddy_free_internal(node, __FILE__, __LINE__);
}

#endif // DEBUG_BUDDY


// Helper: Rounds size up to the nearest power of two >= MIN_BLOCK_SIZE
static size_t buddy_round_up_to_power_of_two(size_t size) {
    size_t power = MIN_BLOCK_SIZE;
    while (power < size) {
        if (power > (SIZE_MAX >> 1)) return SIZE_MAX; // Overflow
        power <<= 1;
    }
    return power;
}

// Helper: Calculates the buddy order for a given power-of-two size
static int buddy_size_to_order(size_t size) {
    int order = MIN_ORDER;
    size_t block_size = MIN_BLOCK_SIZE;
    while (block_size < size) {
         if (block_size > (SIZE_MAX >> 1)) return MAX_ORDER + 1; // Overflow / Too large
         block_size <<= 1;
         order++;
         if (order > MAX_ORDER) return MAX_ORDER + 1; // Exceeded max order
    }
    return order;
}


/* buddy_init */
void buddy_init(void *heap, size_t size) {
    terminal_printf("[Buddy] Initializing with heap @ 0x%x, size %u bytes.\n", (uintptr_t)heap, size);
    for (int i = MIN_ORDER; i <= MAX_ORDER; i++) {
        free_lists[i] = NULL;
    }

    // Initialize locks
    spinlock_init(&g_buddy_lock);
    #ifdef DEBUG_BUDDY
    spinlock_init(&g_alloc_tracker_lock);
    g_alloc_tracker_list = NULL;
    #endif

    // Align heap start address up to the greater of MIN_BLOCK_SIZE or DEFAULT_ALIGNMENT
    size_t required_alignment = (MIN_BLOCK_SIZE > DEFAULT_ALIGNMENT) ? MIN_BLOCK_SIZE : DEFAULT_ALIGNMENT;
    uintptr_t heap_addr = (uintptr_t)heap;
    uintptr_t aligned_addr = ALIGN_UP(heap_addr, required_alignment);
    size_t adjustment = aligned_addr - heap_addr;

    if (size <= adjustment) {
        terminal_write("[Buddy] Error: No space left after alignment adjustment.\n");
        g_buddy_total_size = 0; g_buddy_free_bytes = 0; g_heap_start_addr = 0;
        return;
    }
    heap = (void*)aligned_addr;
    size -= adjustment;
    g_heap_start_addr = aligned_addr; // Store aligned start

    // Find the largest power-of-two block that fits in the adjusted size
    int order = MAX_ORDER;
    size_t block_size = (size_t)1 << order;
    while (order > MIN_ORDER && block_size > size) {
        order--;
        block_size >>= 1;
    }

    if (block_size < MIN_BLOCK_SIZE || order < MIN_ORDER) {
         terminal_printf("[Buddy] Error: Aligned heap size (%u bytes) too small for MIN_ORDER (%d).\n",
                        size, MIN_ORDER);
         g_buddy_total_size = 0; g_buddy_free_bytes = 0; g_heap_start_addr = 0;
         return;
    }

    // Initialize with the largest fitting block
    g_buddy_total_size = block_size; // Only manage the largest power-of-two block
    g_buddy_free_bytes = g_buddy_total_size;

    buddy_block_t *initial_block = (buddy_block_t*)heap;
    initial_block->next = NULL;
    free_lists[order] = initial_block;

    terminal_printf("[Buddy] Init done. Aligned Base: 0x%x, Managed Size: 0x%x bytes (Order %d), Free: %u bytes\n",
                    g_heap_start_addr, g_buddy_total_size, order, g_buddy_free_bytes);
}

#ifdef DEBUG_BUDDY
/* buddy_alloc_internal (Debug Version) */
void *buddy_alloc_internal(size_t size, const char* file, int line) {
    if (size == 0) return NULL;

    size_t req_alloc_size = buddy_round_up_to_power_of_two(size);
    if (req_alloc_size == SIZE_MAX) { /* ... handle overflow ... */ return NULL; }
    int req_order = buddy_size_to_order(req_alloc_size);
    if (req_order > MAX_ORDER) { /* ... handle too large ... */ return NULL; }

    // --- Acquire Global Buddy Lock ---
    uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);

    // Find smallest available block order >= required order
    int order = req_order;
    while (order <= MAX_ORDER && free_lists[order] == NULL) {
        order++;
    }

    void *allocated_ptr = NULL;
    if (order <= MAX_ORDER) {
        // Found a suitable block
        buddy_block_t *block = free_lists[order];
        free_lists[order] = block->next; // Remove from list

        // Split block down to required order
        while (order > req_order) {
            order--;
            size_t half_size = (size_t)1 << order;
            buddy_block_t *buddy = (buddy_block_t*)((uint8_t*)block + half_size);
            // Add buddy to lower order free list
            buddy->next = free_lists[order];
            free_lists[order] = buddy;
        }
        g_buddy_free_bytes -= req_alloc_size;
        allocated_ptr = (void*)block;
    } // else: No suitable block found (out of memory)

    // --- Release Global Buddy Lock ---
    spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);

    // --- Debug Tracking (if allocation successful) ---
    if (allocated_ptr) {
        allocation_tracker_t* tracker = alloc_tracker_node(); // Allocate tracker node
        if (tracker) {
            tracker->addr = allocated_ptr;
            tracker->allocated_size = req_alloc_size; // Store actual allocated size
            tracker->source_file = file;
            tracker->source_line = line;

            // Add to tracker list (protected by separate lock)
            uintptr_t tracker_irq_flags = spinlock_acquire_irqsave(&g_alloc_tracker_lock);
            tracker->next = g_alloc_tracker_list;
            g_alloc_tracker_list = tracker;
            spinlock_release_irqrestore(&g_alloc_tracker_lock, tracker_irq_flags);
        } else {
             terminal_printf("[Buddy DEBUG] Warning: Failed to allocate tracker node for alloc at %s:%d\n", file, line);
             // Allocation still succeeded, but won't be tracked
        }
    } else {
         terminal_printf("[Buddy] Out of memory for size %u (order %d) requested at %s:%d\n", size, req_order, file, line);
    }

    return allocated_ptr;
}

/* buddy_free_internal (Debug Version) */
void buddy_free_internal(void *ptr, const char* file, int line) {
    if (!ptr) return;

    // --- Debug Tracking Verification ---
    allocation_tracker_t* found_tracker = NULL;
    allocation_tracker_t** prev_tracker_ptr = &g_alloc_tracker_list;
    size_t allocated_size = 0; // Size to use for buddy free logic

    uintptr_t tracker_irq_flags = spinlock_acquire_irqsave(&g_alloc_tracker_lock);
    allocation_tracker_t* current_tracker = g_alloc_tracker_list;
    while(current_tracker) {
        if (current_tracker->addr == ptr) {
            found_tracker = current_tracker;
            *prev_tracker_ptr = current_tracker->next; // Remove from list
            allocated_size = found_tracker->allocated_size;
            break;
        }
        prev_tracker_ptr = &current_tracker->next;
        current_tracker = current_tracker->next;
    }
    spinlock_release_irqrestore(&g_alloc_tracker_lock, tracker_irq_flags);

    if (!found_tracker) {
        terminal_printf("[Buddy] Error: Attempt to free untracked or already freed pointer 0x%x at %s:%d\n", (uintptr_t)ptr, file, line);
        // Optionally panic or handle error
        return;
    }

    // Free the tracker node itself *after* extracting necessary info
    free_tracker_node(found_tracker); // Uses buddy_alloc_internal implicitly

    // --- Actual Buddy Free Logic ---
    int order = buddy_size_to_order(allocated_size); // Get order from tracked size
    if (order > MAX_ORDER) {
         terminal_printf("[Buddy] Error: Invalid tracked size %u for ptr 0x%x freed at %s:%d\n", allocated_size, (uintptr_t)ptr, file, line);
         return; // Should not happen if tracking is correct
    }

    uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
    uintptr_t addr = (uintptr_t)ptr;

    // Coalescing loop (same logic as non-debug version)
    while (order < MAX_ORDER) {
        uintptr_t buddy_addr = addr ^ ((uintptr_t)1 << order);
        buddy_block_t *prev = NULL; buddy_block_t *curr = free_lists[order]; bool merged = false;
        while (curr) {
            if ((uintptr_t)curr == buddy_addr) {
                if (prev) prev->next = curr->next; else free_lists[order] = curr->next;
                if (buddy_addr < addr) addr = buddy_addr;
                order++; merged = true; break;
            }
            prev = curr; curr = curr->next;
        }
        if (!merged) break;
    }

    // Add final block to free list
    buddy_block_t *final_block = (buddy_block_t*)addr;
    final_block->next = free_lists[order];
    free_lists[order] = final_block;
    g_buddy_free_bytes += allocated_size; // Add back the actual allocated size

    spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);
}

/* buddy_dump_leaks (Debug Version) */
void buddy_dump_leaks(void) {
    terminal_write("\n--- Buddy Allocator Leak Check ---\n");
    uintptr_t tracker_irq_flags = spinlock_acquire_irqsave(&g_alloc_tracker_lock);
    allocation_tracker_t* current = g_alloc_tracker_list;
    int leak_count = 0;
    size_t leak_bytes = 0;
    if (!current) {
        terminal_write("No active allocations tracked. No leaks detected.\n");
    } else {
        terminal_write("Detected potential memory leaks (unfreed blocks):\n");
        while(current) {
            terminal_printf("  - Addr: 0x%x, Size: %u bytes, Allocated at: %s:%d\n",
                            (uintptr_t)current->addr, current->allocated_size,
                            current->source_file ? current->source_file : "<unknown>",
                            current->source_line);
            leak_count++;
            leak_bytes += current->allocated_size;
            current = current->next;
        }
        terminal_printf("Total Leaks: %d blocks, %u bytes\n", leak_count, leak_bytes);
    }
     terminal_write("----------------------------------\n");
    spinlock_release_irqrestore(&g_alloc_tracker_lock, tracker_irq_flags);
}

#else // !DEBUG_BUDDY

/* buddy_alloc (Non-Debug Version) */
void *buddy_alloc(size_t size) {
     if (size == 0) return NULL;

    size_t req_size = buddy_round_up_to_power_of_two(size);
    if (req_size == SIZE_MAX) { return NULL; } // Overflow check
    int req_order = buddy_size_to_order(req_size);
    if (req_order > MAX_ORDER) { return NULL; } // Size too large

    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_buddy_lock); // Acquire lock

    int order = req_order;
    while (order <= MAX_ORDER && free_lists[order] == NULL) {
        order++;
    }

    void *allocated_ptr = NULL;
    if (order <= MAX_ORDER) {
        buddy_block_t *block = free_lists[order];
        free_lists[order] = block->next;
        while (order > req_order) {
            order--;
            size_t half_size = (size_t)1 << order;
            buddy_block_t *buddy = (buddy_block_t*)((uint8_t*)block + half_size);
            buddy->next = free_lists[order];
            free_lists[order] = buddy;
        }
        g_buddy_free_bytes -= req_size;
        allocated_ptr = (void*)block;
    }

    spinlock_release_irqrestore(&g_buddy_lock, irq_flags); // Release lock
    return allocated_ptr;
}

/* buddy_free (Non-Debug Version) */
void buddy_free(void *ptr, size_t size) { // Still requires size for non-debug version
     if (!ptr || size == 0) return;

    size_t block_size = buddy_round_up_to_power_of_two(size);
    if (block_size == SIZE_MAX) { return; } // Overflow check
    int order = buddy_size_to_order(block_size);
    if (order > MAX_ORDER) { return; } // Invalid size

    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_buddy_lock); // Acquire lock

    uintptr_t addr = (uintptr_t)ptr;

     // Alignment check (optional)
    if (addr & (block_size - 1)) {
        terminal_printf("[Buddy] Error: Freeing misaligned pointer 0x%x for size %u (order %d).\n", addr, block_size, order);
        spinlock_release_irqrestore(&g_buddy_lock, irq_flags);
        return;
    }

    // Coalescing loop
    while (order < MAX_ORDER) {
        uintptr_t buddy_addr = addr ^ ((uintptr_t)1 << order);
        buddy_block_t *prev = NULL; buddy_block_t *curr = free_lists[order]; bool merged = false;
        while (curr) {
            if ((uintptr_t)curr == buddy_addr) {
                if (prev) prev->next = curr->next; else free_lists[order] = curr->next;
                if (buddy_addr < addr) addr = buddy_addr;
                order++; merged = true; break;
            }
            prev = curr; curr = curr->next;
        }
        if (!merged) break;
    }

    // Add final block to free list
    buddy_block_t *final_block = (buddy_block_t*)addr;
    final_block->next = free_lists[order];
    free_lists[order] = final_block;
    g_buddy_free_bytes += block_size;

    spinlock_release_irqrestore(&g_buddy_lock, irq_flags); // Release lock
}

#endif // DEBUG_BUDDY


/* buddy_free_space */
size_t buddy_free_space(void) {
    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
    size_t free_bytes = g_buddy_free_bytes;
    spinlock_release_irqrestore(&g_buddy_lock, irq_flags);
    return free_bytes;
}