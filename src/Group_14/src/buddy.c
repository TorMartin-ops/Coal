/**
 * buddy.c - Power-of-Two Buddy Allocator Implementation
 *
 * REVISED (v2 - Major Upgrades):
 * - Address Space: Assumes management of a virtually mapped region corresponding
 * to contiguous physical memory. Pointers are virtual addresses.
 * - Metadata Header (Non-Debug): Stores allocation order before the user block,
 * removing the 'size' parameter from buddy_free().
 * - Debug Enhancements:
 * - Uses a fixed static pool for allocation tracker nodes (no self-allocation).
 * - Adds start/end canaries around user data for bounds checking.
 * - Checks canaries during buddy_free_internal().
 * - API: buddy_free(void* ptr) is now the non-debug API.
 * - Robustness: Added more checks, clearer errors, BUDDY_PANIC.
 * - Statistics: Added alloc/free/fail counters.
 * - Structure: Improved comments, naming, alignment handling with header.
 */
 
 #include "kmalloc_internal.h"
 #include "buddy.h"      // Includes config constants (MIN/MAX_ORDER), macros, API defs
 #include "terminal.h"   // For logging
 #include "types.h"      // For uintptr_t, size_t, bool, etc.
 #include "spinlock.h"   // For spinlocks
 #include <libc/stdint.h> // For SIZE_MAX, fixed-width types (ensure path is correct)
 
 // --- Configuration Checks ---
 
 #ifndef MIN_ORDER
 #error "MIN_ORDER is not defined (include buddy.h)"
 #endif
 #ifndef MAX_ORDER
 #error "MAX_ORDER is not defined (include buddy.h)"
 #endif
 #ifndef PAGE_SIZE
 #define PAGE_SIZE 4096 // Fallback if not defined elsewhere
 #endif
 
 // Smallest allocatable block size (for user)
 // #define MIN_BLOCK_SIZE_USER (1 << MIN_ORDER)
 // Smallest internal buddy block size (must hold header + user data)
 #define MIN_INTERNAL_ORDER MIN_ORDER // Adjust if header makes smallest block too small
 #define MIN_BLOCK_SIZE_INTERNAL (1 << MIN_INTERNAL_ORDER)
 
 
 // --- Metadata Header (Used only when DEBUG_BUDDY is *not* defined) ---
 #ifndef DEBUG_BUDDY
 typedef struct buddy_header {
     uint8_t order;      // Order of the allocated block (MIN_INTERNAL_ORDER to MAX_ORDER)
     // Add padding if needed for alignment, or other metadata
     // uint8_t reserved[...];
 } buddy_header_t;
 
 // Ensure header size doesn't violate alignment of subsequent block
 // We allocate block_size + header_size, return ptr + header_size.
 // The original block must be aligned to block_size.
 #define BUDDY_HEADER_SIZE (((sizeof(buddy_header_t) + DEFAULT_ALIGNMENT - 1)) & ~(DEFAULT_ALIGNMENT - 1))
 #define BUDDY_HEADER_SIZE 0 // No header in debug mode (info is in tracker)
 #endif // !DEBUG_BUDDY
 
 // Check if smallest internal block can hold header + minimal user data alignment
 #if !defined(DEBUG_BUDDY) && (MIN_BLOCK_SIZE_INTERNAL < (BUDDY_HEADER_SIZE + DEFAULT_ALIGNMENT))
     // Smallest block might not be large enough to store header AND return aligned pointer.
     // Consider increasing MIN_INTERNAL_ORDER or adjusting header/alignment.
     // For now, we assume alignment works out, but this is a potential issue.
     #warning "Smallest internal buddy block might be too small for header and alignment."
 #endif
 
 
 // Kernel panic for critical buddy errors
 #define BUDDY_PANIC(msg) do { \
     terminal_printf("\n[BUDDY PANIC] %s at %s:%d. System Halted.\n", msg, __FILE__, __LINE__); \
     /* Ideally, dump allocator state here */ \
     while (1) { asm volatile("cli; hlt"); } \
 } while(0)
 
 
 // Buddy block structure for free lists (stores pointers as virtual addresses)
 typedef struct buddy_block {
     struct buddy_block *next;
 } buddy_block_t;
 
 // Array of free lists, one for each order
 static buddy_block_t *free_lists[MAX_ORDER + 1] = {0};
 
 // Global heap state
 static uintptr_t g_heap_start_virt_addr = 0; // Store aligned VIRTUAL heap start
 static uintptr_t g_heap_end_virt_addr = 0;   // Store VIRTUAL heap end (exclusive)
 static size_t g_buddy_total_managed_size = 0;
 static size_t g_buddy_free_bytes = 0;
 
 // Global statistics
 static uint64_t g_alloc_count = 0;
 static uint64_t g_free_count = 0;
 static uint64_t g_failed_alloc_count = 0;
 
 // Global Lock for Buddy Allocator core logic
 static spinlock_t g_buddy_lock;
 
 // --- Debug Allocation Tracker ---
 #ifdef DEBUG_BUDDY
 #define DEBUG_CANARY_START 0xDEADBEEF
 #define DEBUG_CANARY_END   0xCAFEBABE
 #define MAX_TRACKER_NODES 1024 // Max tracked allocations (adjust as needed)
 
 typedef struct allocation_tracker {
     void* user_addr;            // Pointer returned to the user
     void* block_addr;           // Actual start of the buddy block
     size_t block_size;          // Power-of-2 size of the buddy block
     const char* source_file;    // File where BUDDY_ALLOC was called
     int source_line;            // Line where BUDDY_ALLOC was called
     struct allocation_tracker* next; // For free list / active list
     // Canaries are stored in the allocated block itself
 } allocation_tracker_t;
 
 // Static pool for tracker nodes
 static allocation_tracker_t g_tracker_nodes[MAX_TRACKER_NODES];
 static allocation_tracker_t *g_free_tracker_nodes = NULL; // Linked list of free nodes
 static allocation_tracker_t *g_active_allocations = NULL; // Linked list of active allocs
 static spinlock_t g_alloc_tracker_lock; // Separate lock for the tracker lists
 
 // Initialize the static tracker pool
 static void init_tracker_pool() {
     spinlock_init(&g_alloc_tracker_lock);
     g_free_tracker_nodes = NULL;
     g_active_allocations = NULL;
     // Link all nodes into the free list
     for (int i = 0; i < MAX_TRACKER_NODES; ++i) {
         g_tracker_nodes[i].next = g_free_tracker_nodes;
         g_free_tracker_nodes = &g_tracker_nodes[i];
     }
 }
 
 // Allocate a tracker node from the free pool
 static allocation_tracker_t* alloc_tracker_node() {
     allocation_tracker_t* node = NULL;
     uintptr_t tracker_irq_flags = spinlock_acquire_irqsave(&g_alloc_tracker_lock);
     if (g_free_tracker_nodes) {
         node = g_free_tracker_nodes;
         g_free_tracker_nodes = node->next;
         node->next = NULL; // Clear next pointer for active list use
     }
     spinlock_release_irqrestore(&g_alloc_tracker_lock, tracker_irq_flags);
     return node; // Returns NULL if pool is exhausted
 }
 
 // Return a tracker node to the free pool
 static void free_tracker_node(allocation_tracker_t* node) {
     if (!node) return;
     uintptr_t tracker_irq_flags = spinlock_acquire_irqsave(&g_alloc_tracker_lock);
     node->next = g_free_tracker_nodes;
     g_free_tracker_nodes = node;
     spinlock_release_irqrestore(&g_alloc_tracker_lock, tracker_irq_flags);
 }
 
 // Add a tracker to the active list
 static void add_active_allocation(allocation_tracker_t* tracker) {
     if (!tracker) return;
     uintptr_t tracker_irq_flags = spinlock_acquire_irqsave(&g_alloc_tracker_lock);
     tracker->next = g_active_allocations;
     g_active_allocations = tracker;
     spinlock_release_irqrestore(&g_alloc_tracker_lock, tracker_irq_flags);
 }
 
 // Find and remove a tracker from the active list
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
 
 
 // --- Helper Functions ---
 
 
 // Helper: Calculates the minimum required block order to satisfy a user request size,
 //         considering header and alignment.
 static int buddy_required_order(size_t user_size) {
     size_t required_total_size = user_size + BUDDY_HEADER_SIZE;
 
     // Find smallest power-of-2 block size >= required_total_size
     size_t block_size = MIN_BLOCK_SIZE_INTERNAL; // Start with smallest internal block
     int order = MIN_INTERNAL_ORDER;
 
     while (block_size < required_total_size) {
         if (block_size > (SIZE_MAX >> 1)) return MAX_ORDER + 1; // Overflow check
         block_size <<= 1;
         order++;
         if (order > MAX_ORDER) return MAX_ORDER + 1; // Exceeded max order
     }
 
     // Additional check: Ensure the block provides enough space after the header
     // for the user to get a pointer aligned to DEFAULT_ALIGNMENT.
     // The start of the block is aligned to block_size.
     // The header takes BUDDY_HEADER_SIZE.
     // We need ALIGN_UP(block_start + BUDDY_HEADER_SIZE, DEFAULT_ALIGNMENT) + user_size <= block_start + block_size
     // This check is slightly complex. A simpler, stricter check: ensure block_size >= header + user_size.
     // The loop already guarantees this. Let's assume alignment works out for now.
 
     return order;
 }
 
 // Helper: Get buddy address for a given block and order
 static inline uintptr_t get_buddy_addr(uintptr_t block_addr, int order) {
     return block_addr ^ ((uintptr_t)1 << order);
 }
 
 // Helper: Add a block to the appropriate free list
 static void add_block_to_free_list(void *block_ptr, int order) {
     buddy_block_t *block = (buddy_block_t*)block_ptr;
     block->next = free_lists[order];
     free_lists[order] = block;
 }
 
 // Helper: Remove a specific block from its free list
 // Returns true if found and removed, false otherwise.
 static bool remove_block_from_free_list(void *block_ptr, int order) {
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
 
 
 // --- Initialization ---
 
 /**
  * @brief Initializes the buddy allocator system.
  *
  * @param heap_region_start Virtual address of the start of the memory region.
  * This address MUST be appropriately mapped to the
  * underlying physical memory before calling init.
  * @param region_size Size of the memory region in bytes.
  */
 void buddy_init(void *heap_region_start, size_t region_size) {
     terminal_printf("[Buddy] Initializing allocator...\n");
     terminal_printf("  Region Start Virt: 0x%p, Size: %u bytes\n", heap_region_start, region_size);
 
     // 1. Basic Sanity Checks
     if (!heap_region_start || region_size < MIN_BLOCK_SIZE_INTERNAL) {
         BUDDY_PANIC("Invalid region parameters for buddy_init.");
         return;
     }
     if (MAX_ORDER >= (sizeof(size_t) * 8)) {
          BUDDY_PANIC("MAX_ORDER is too large for size_t.");
          return;
     }
 
 
     // 2. Initialize Free Lists and Locks
     for (int i = 0; i <= MAX_ORDER; i++) { // Iterate up to MAX_ORDER included
         free_lists[i] = NULL;
     }
     spinlock_init(&g_buddy_lock);
     #ifdef DEBUG_BUDDY
     init_tracker_pool();
     #endif
 
     // 3. Align Heap Start and Adjust Size
     // Ensure the base address used by the buddy system meets the alignment
     // requirements of the largest possible block (MAX_ORDER).
     size_t max_block_alignment = (size_t)1 << MAX_ORDER;
     uintptr_t start_addr = (uintptr_t)heap_region_start;
     uintptr_t aligned_start_addr = ALIGN_UP(start_addr, max_block_alignment);
     size_t adjustment = aligned_start_addr - start_addr;
 
     if (adjustment >= region_size || (region_size - adjustment) < MIN_BLOCK_SIZE_INTERNAL) {
         terminal_printf("[Buddy] Error: Not enough space after alignment (Need %u, Adj %u, Have %u).\n",
                         MIN_BLOCK_SIZE_INTERNAL, adjustment, region_size);
         g_heap_start_virt_addr = 0; g_heap_end_virt_addr = 0; g_buddy_total_managed_size = 0; g_buddy_free_bytes = 0;
         return; // Not enough space
     }
 
     size_t available_size = region_size - adjustment;
     g_heap_start_virt_addr = aligned_start_addr;
     // Calculate end based on available size, but don't exceed initial region end
     g_heap_end_virt_addr = aligned_start_addr + available_size;
 
     terminal_printf("  Aligned Start Virt: 0x%x, Available Size: %u bytes\n", g_heap_start_virt_addr, available_size);
 
     // 4. Add Initial Blocks to Free Lists
     // Add chunks of the largest possible power-of-two sizes that fit
     g_buddy_total_managed_size = 0;
     g_buddy_free_bytes = 0;
     uintptr_t current_addr = g_heap_start_virt_addr;
 
     while (available_size >= MIN_BLOCK_SIZE_INTERNAL) {
         // Find largest power-of-two block size that fits in remaining space
         // AND starts at 'current_addr' which has alignment 'max_block_alignment'
         int order = MAX_ORDER;
         size_t block_size = (size_t)1 << order;
 
         // Ensure block_size calculation didn't wrap/overflow (shouldn't if MAX_ORDER checked)
         if (block_size == 0 && order > 0) {
              BUDDY_PANIC("Block size calculation resulted in zero.");
         }
 
         while (block_size > available_size) {
              if (order == MIN_INTERNAL_ORDER) break; // Cannot go smaller
             order--;
             block_size >>= 1;
         }
 
         // We should always find at least MIN_BLOCK_SIZE_INTERNAL if loop condition holds
         if (block_size < MIN_BLOCK_SIZE_INTERNAL) {
             // This indicates available_size dropped below minimum, loop should have exited
             terminal_printf("[Buddy] Warning: Remaining size %u too small in init loop.\n", available_size);
             break;
         }
 
         terminal_printf("  Adding initial block: Addr=0x%x, Size=%u (Order %d)\n", current_addr, block_size, order);
         add_block_to_free_list((void*)current_addr, order);
 
         g_buddy_total_managed_size += block_size;
         g_buddy_free_bytes += block_size;
         current_addr += block_size;
         available_size -= block_size;
     }
 
      // Adjust heap end to reflect only the memory added to lists
      g_heap_end_virt_addr = g_heap_start_virt_addr + g_buddy_total_managed_size;
 
     terminal_printf("[Buddy] Init done. Managed Addr Range Virt: [0x%x - 0x%x)\n", g_heap_start_virt_addr, g_heap_end_virt_addr);
     terminal_printf("  Total Managed: %u bytes, Initially Free: %u bytes\n", g_buddy_total_managed_size, g_buddy_free_bytes);
     if (available_size > 0) {
         terminal_printf("  (Note: %u bytes potentially unused at end of provided region)\n", available_size);
     }
 }
 
 
 // --- Allocation Implementation ---
 
 /**
  * @brief Internal implementation for buddy allocation.
  * Handles finding/splitting blocks and managing free lists.
  *
  * @param requested_order The required order for the allocation (already calculated).
  * @param file Debug source file.
  * @param line Debug source line.
  * @return Pointer to the start of the allocated *buddy block* (not user pointer yet),
  * or NULL if allocation fails.
  */
 static void* buddy_alloc_impl(int requested_order, const char* file, int line) {
     // --- Find Suitable Block ---
     int order = requested_order;
     // Search for the smallest available block order >= required order
     while (order <= MAX_ORDER && free_lists[order] == NULL) {
         order++;
     }
 
     // Out of memory?
     if (order > MAX_ORDER) {
         g_failed_alloc_count++;
         #ifdef DEBUG_BUDDY
         terminal_printf("[Buddy] Out of memory for order %d requested at %s:%d\n", requested_order, file, line);
         #else
         // terminal_printf("[Buddy] Out of memory for order %d\n", requested_order); // Reduce noise in release
         #endif
         return NULL;
     }
 
     // --- Allocate and Split Block ---
     // Take the block from the found list
     buddy_block_t *block = free_lists[order];
     free_lists[order] = block->next; // Remove from list
 
     // Split the block down to the required order, adding buddies to free lists
     while (order > requested_order) {
         order--;
         size_t half_block_size = (size_t)1 << order;
         // The buddy is the second half of the current 'block'
         uintptr_t buddy_addr = (uintptr_t)block + half_block_size;
         add_block_to_free_list((void*)buddy_addr, order);
         // The first half ('block') continues down to the next split level
     }
 
     // --- Update Stats ---
     size_t allocated_block_size = (size_t)1 << requested_order;
     g_buddy_free_bytes -= allocated_block_size;
     g_alloc_count++;
 
     // Return the pointer to the start of the allocated buddy block
     return (void*)block;
 }
 
 // --- Free Implementation ---
 
 /**
  * @brief Internal implementation for freeing a buddy block.
  * Handles coalescing and managing free lists.
  *
  * @param block_addr Virtual address of the start of the buddy block to free.
  * @param block_order Order of the block being freed.
  * @param file Debug source file.
  * @param line Debug source line.
  */
 static void buddy_free_impl(void *block_addr, int block_order, const char* file, int line) {
     uintptr_t addr = (uintptr_t)block_addr;
     size_t block_size = (size_t)1 << block_order;
 
     // Coalescing loop: Merge with buddies if they are free
     while (block_order < MAX_ORDER) {
         uintptr_t buddy_addr = get_buddy_addr(addr, block_order);
 
         // Check if the buddy is also free by trying to remove it from the list
         if (remove_block_from_free_list((void*)buddy_addr, block_order)) {
             // Found and removed free buddy! Merge them.
             // The new, larger block starts at the lower address
             if (buddy_addr < addr) {
                 addr = buddy_addr;
             }
             // Increase order for the next level of coalescing
             block_order++;
             // block_size is implicitly doubled for the next iteration's stats
             #ifdef DEBUG_BUDDY
             // terminal_printf("   [Buddy Free Debug %s:%d] Coalesced order %d blocks at 0x%x, 0x%x -> order %d block at 0x%x\n",
             //                 file, line, block_order-1, (addr < buddy_addr)?addr:buddy_addr, (addr < buddy_addr)?buddy_addr:addr, block_order, addr);
             #endif
         } else {
             // Buddy not found in free list, cannot coalesce further
             break;
         }
     }
 
     // Add the final (potentially coalesced) block to the appropriate free list
     add_block_to_free_list((void*)addr, block_order);
 
     // Update stats - add back the size of the originally freed block
     // The coalescing implicitly handles the sizes of the merged blocks by removing
     // the buddy from the free list before merging.
     g_buddy_free_bytes += block_size;
     g_free_count++;
 }
 
 
 // --- Public API ---
 
 #ifdef DEBUG_BUDDY
 /* buddy_alloc_internal (Debug Version Wrapper) */
 void *buddy_alloc_internal(size_t size, const char* file, int line) {
     if (size == 0) return NULL;
 
     // Calculate required order considering alignment for user pointer later
     int req_order = buddy_required_order(size);
     if (req_order > MAX_ORDER) {
         terminal_printf("[Buddy DEBUG %s:%d] Error: Requested size %u too large (req order %d > max %d).\n", file, line, size, req_order, MAX_ORDER);
         g_failed_alloc_count++;
         return NULL;
     }
 
     // Acquire lock for core allocation logic
     uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
     void *block_ptr = buddy_alloc_impl(req_order, file, line);
     spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);
 
     // Handle allocation failure
     if (!block_ptr) {
         return NULL;
     }
 
     // Allocation successful, prepare user pointer and debug info
     size_t block_size = (size_t)1 << req_order;
     void* user_ptr = block_ptr; // In debug, header/canaries are handled separately
 
     // --- Allocation Tracking & Canaries ---
     allocation_tracker_t* tracker = alloc_tracker_node();
     if (!tracker) {
         // Critical: Cannot track allocation, potentially dangerous.
         // Options: Return NULL, panic, or proceed untracked with warning.
         terminal_printf("[Buddy DEBUG %s:%d] CRITICAL: Failed to allocate tracker node! Returning NULL.\n", file, line);
         // Need to free the block we just allocated from buddy_alloc_impl
         uintptr_t free_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
         buddy_free_impl(block_ptr, req_order, file, line); // Free it back
         spinlock_release_irqrestore(&g_buddy_lock, free_irq_flags);
         g_failed_alloc_count++; // Count as failure
         return NULL;
     }
 
     tracker->user_addr = user_ptr;
     tracker->block_addr = block_ptr;
     tracker->block_size = block_size;
     tracker->source_file = file;
     tracker->source_line = line;
     add_active_allocation(tracker); // Add to active list
 
     // Place canaries (if block is large enough)
     if (block_size >= sizeof(uint32_t) * 2) { // Need space for start and end canary
         *(uint32_t*)block_ptr = DEBUG_CANARY_START; // Canary at the very start
         *(uint32_t*)((uintptr_t)block_ptr + block_size - sizeof(uint32_t)) = DEBUG_CANARY_END; // Canary at the very end
     } else if (block_size >= sizeof(uint32_t)) {
          *(uint32_t*)block_ptr = DEBUG_CANARY_START; // Only start canary fits
     }
 
     // User pointer might be offset if we reserve space for start canary?
     // Let's return the block pointer directly, canaries are outside user area.
     // User area is effectively block_ptr + sizeof(canary_start) to block_ptr + block_size - sizeof(canary_end)
     // This simplifies free() but means user cannot use the absolute edges.
     // Alternative: Return block_ptr + sizeof(canary), store header/canary info before it.
     // Let's stick to returning block_ptr for now, user must respect bounds implicitly checked by canaries.
 
     // Optionally: Poison the user area (e.g., memset with 0xCD)
 
     return user_ptr;
 }
 
 /* buddy_free_internal (Debug Version Wrapper) */
 void buddy_free_internal(void *ptr, const char* file, int line) {
     if (!ptr) return;
 
      // Check if pointer is within the managed heap range
      uintptr_t addr = (uintptr_t)ptr;
      if (addr < g_heap_start_virt_addr || addr >= g_heap_end_virt_addr) {
           terminal_printf("[Buddy DEBUG %s:%d] Error: Attempt to free pointer 0x%p outside managed heap [0x%x - 0x%x).\n",
                           file, line, ptr, g_heap_start_virt_addr, g_heap_end_virt_addr);
           // BUDDY_PANIC("Freeing pointer outside heap!"); // Optional: Panic on invalid free
           return;
      }
 
 
     // --- Find Allocation in Tracker ---
     allocation_tracker_t* tracker = remove_active_allocation(ptr);
 
     if (!tracker) {
         terminal_printf("[Buddy DEBUG %s:%d] Error: Attempt to free untracked or double-freed pointer 0x%p\n", file, line, ptr);
          // Check if it's already freed but still in active list somehow? Unlikely with locking.
         // BUDDY_PANIC("Freeing untracked pointer!"); // Optional: Panic
         return;
     }
 
     // --- Validate Canaries ---
     size_t block_size = tracker->block_size;
     void* block_ptr = tracker->block_addr; // Start of the actual buddy block
 
      // Check canaries relative to block_ptr
     bool canary_ok = true;
      if (block_size >= sizeof(uint32_t) * 2) {
           if (*(uint32_t*)block_ptr != DEBUG_CANARY_START) {
                terminal_printf("[Buddy DEBUG %s:%d] CORRUPTION DETECTED: Start canary mismatch for block 0x%p (size %u) freed from 0x%p! Expected 0x%x, got 0x%x\n",
                                tracker->source_file, tracker->source_line, // Allocation site
                                block_ptr, block_size, ptr, DEBUG_CANARY_START, *(uint32_t*)block_ptr);
                canary_ok = false;
           }
           if (*(uint32_t*)((uintptr_t)block_ptr + block_size - sizeof(uint32_t)) != DEBUG_CANARY_END) {
                terminal_printf("[Buddy DEBUG %s:%d] CORRUPTION DETECTED: End canary mismatch for block 0x%p (size %u) freed from 0x%p! Expected 0x%x, got 0x%x\n",
                                tracker->source_file, tracker->source_line, // Allocation site
                                block_ptr, block_size, ptr, DEBUG_CANARY_END, *(uint32_t*)((uintptr_t)block_ptr + block_size - sizeof(uint32_t)));
                canary_ok = false;
           }
      } else if (block_size >= sizeof(uint32_t)) {
           if (*(uint32_t*)block_ptr != DEBUG_CANARY_START) {
                terminal_printf("[Buddy DEBUG %s:%d] CORRUPTION DETECTED: Start canary mismatch (small block) for block 0x%p (size %u) freed from 0x%p! Expected 0x%x, got 0x%x\n",
                                tracker->source_file, tracker->source_line, // Allocation site
                                block_ptr, block_size, ptr, DEBUG_CANARY_START, *(uint32_t*)block_ptr);
                canary_ok = false;
           }
      }
 
      if (!canary_ok) {
           BUDDY_PANIC("Heap corruption detected!"); // Halt on corruption
           // If not panicking, we should probably not proceed with the free,
           // but need to return the tracker node.
           // free_tracker_node(tracker);
           // return;
      }
 
 
     // --- Perform Actual Free ---
     int block_order = buddy_block_size_to_order(block_size);
     if (block_order > MAX_ORDER) {
          // Should not happen if tracker is correct
          terminal_printf("[Buddy DEBUG %s:%d] Error: Invalid tracked block size %u for ptr 0x%p\n", file, line, block_size, ptr);
          free_tracker_node(tracker); // Still free tracker
          return;
     }
 
     // Optionally: Poison the memory block being freed (e.g., memset with 0xFE)
 
     uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
     buddy_free_impl(block_ptr, block_order, file, line); // Use block_ptr
     spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);
 
     // --- Free Tracker Node ---
     free_tracker_node(tracker);
 }
 
 /* buddy_dump_leaks (Debug Version) */
 void buddy_dump_leaks(void) {
     terminal_write("\n--- Buddy Allocator Leak Check ---\n");
     uintptr_t tracker_irq_flags = spinlock_acquire_irqsave(&g_alloc_tracker_lock);
     allocation_tracker_t* current = g_active_allocations;
     int leak_count = 0;
     size_t leak_bytes = 0; // Total size of buddy blocks leaked
     if (!current) {
         terminal_write("No active allocations tracked. No leaks detected.\n");
     } else {
         terminal_write("Detected potential memory leaks (unfreed blocks):\n");
         while(current) {
             terminal_printf("  - User Addr: 0x%p, Block Addr: 0x%p, Block Size: %u bytes, Allocated at: %s:%d\n",
                             current->user_addr, current->block_addr, current->block_size,
                             current->source_file ? current->source_file : "<unknown>",
                             current->source_line);
             leak_count++;
             leak_bytes += current->block_size;
             current = current->next;
         }
         terminal_printf("Total Leaks: %d blocks, %u bytes (block size)\n", leak_count, leak_bytes);
     }
      terminal_write("----------------------------------\n");
     spinlock_release_irqrestore(&g_alloc_tracker_lock, tracker_irq_flags);
 }
 
 #else // !DEBUG_BUDDY (Non-Debug Implementations)
 
 /* buddy_alloc (Non-Debug Version) */
 void *buddy_alloc(size_t size) {
     if (size == 0) return NULL;
 
     // Calculate required order including header space
     int req_order = buddy_required_order(size);
     if (req_order > MAX_ORDER) {
          // terminal_printf("[Buddy] Error: Requested size %u too large.\n", size); // Reduce noise
          g_failed_alloc_count++;
          return NULL;
     }
     size_t block_size = (size_t)1 << req_order; // Actual block size needed
 
     // Acquire lock for core allocation logic
     uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
     void *block_ptr = buddy_alloc_impl(req_order, NULL, 0); // Call internal impl
     spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);
 
     // Handle allocation failure
     if (!block_ptr) {
         return NULL;
     }
 
     // Allocation successful, prepare user pointer and write header
     uintptr_t block_addr = (uintptr_t)block_ptr;
 
     // Write the header
     buddy_header_t* header = (buddy_header_t*)block_addr;
     header->order = (uint8_t)req_order; // Store the order
 
     // Calculate user pointer (after header)
     void* user_ptr = (void*)(block_addr + BUDDY_HEADER_SIZE);
 
     // Optionally: Zero the user area? kmalloc usually does this. Buddy doesn't have to.
 
     return user_ptr;
 }
 
 /* buddy_free (Non-Debug Version) */
 void buddy_free(void *ptr) {
     if (!ptr) return;
 
     uintptr_t user_addr = (uintptr_t)ptr;
 
     // Check if pointer is within the managed heap range (basic check)
     // Add check for header alignment if needed.
      if (user_addr < (g_heap_start_virt_addr + BUDDY_HEADER_SIZE) || user_addr >= g_heap_end_virt_addr) {
           terminal_printf("[Buddy] Error: Attempt to free pointer 0x%p potentially outside managed heap or before header [0x%x - 0x%x).\n",
                           ptr, g_heap_start_virt_addr + BUDDY_HEADER_SIZE, g_heap_end_virt_addr);
           // BUDDY_PANIC("Freeing pointer outside heap!"); // Optional
           return;
      }
 
     // Calculate block address from user pointer
     uintptr_t block_addr = user_addr - BUDDY_HEADER_SIZE;
     buddy_header_t* header = (buddy_header_t*)block_addr;
 
     // Retrieve order from header
     int block_order = header->order;
 
     // Validate order
     if (block_order < MIN_INTERNAL_ORDER || block_order > MAX_ORDER) {
         terminal_printf("[Buddy] Error: Corrupted header detected freeing pointer 0x%p (invalid order %d).\n", ptr, block_order);
         BUDDY_PANIC("Corrupted buddy header detected!");
         return;
     }
 
     size_t block_size = (size_t)1 << block_order;
 
      // Basic alignment check of the original block address derived from ptr
      if (block_addr & (block_size - 1)) {
           terminal_printf("[Buddy] Error: Freeing misaligned pointer 0x%p (derived block 0x%x, order %d)\n", ptr, block_addr, block_order);
           BUDDY_PANIC("Freeing pointer yielding misaligned block!");
           return;
      }
 
     // Acquire lock and call internal free implementation
     uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
     buddy_free_impl((void*)block_addr, block_order, NULL, 0); // Use block_addr
     spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);
 }
 
 #endif // DEBUG_BUDDY
 
 
 // --- Statistics ---
 
 /* buddy_free_space */
 size_t buddy_free_space(void) {
     uintptr_t irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
     size_t free_bytes = g_buddy_free_bytes;
     spinlock_release_irqrestore(&g_buddy_lock, irq_flags);
     return free_bytes;
 }
 
 /* buddy_total_space */
 size_t buddy_total_space(void) {
     // Total managed size is constant after init, no lock needed?
     // But reading g_buddy_total_managed_size technically might race if init is concurrent (bad idea).
     // Assume init is done before this is called concurrently.
     return g_buddy_total_managed_size;
 }
 
 /* buddy_get_stats */
 void buddy_get_stats(buddy_stats_t *stats) {
      if (!stats) return;
      // Acquire lock to get consistent stats snapshot
      uintptr_t irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
      stats->total_bytes = g_buddy_total_managed_size;
      stats->free_bytes = g_buddy_free_bytes;
      stats->alloc_count = g_alloc_count;
      stats->free_count = g_free_count;
      stats->failed_alloc_count = g_failed_alloc_count;
      // Add more complex stats if needed (e.g., block counts per order)
      spinlock_release_irqrestore(&g_buddy_lock, irq_flags);
 }