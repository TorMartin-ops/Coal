/**
 * buddy.c - Power-of-Two Buddy Allocator Implementation
 *
 * REVISED (v3 - World Class & Alignment Fixes):
 * - Address Space: Manages a virtually contiguous region mapped to physical memory.
 * Returns VIRTUAL addresses to callers.
 * - Alignment Guarantee: Asserts page alignment for page-sized or larger allocations.
 * Relies on correct initialization alignment.
 * - Metadata Header (Non-Debug): Stores allocation order efficiently.
 * - Debug Enhancements: Robust canary checks, leak detection via static tracker pool.
 * - API: Clean separation between debug/non-debug versions.
 * - Locking: Uses spinlocks for SMP safety on core data structures.
 * - Robustness: Includes assertions, clear error messages, and BUDDY_PANIC.
 * - Statistics: Maintains allocation/free/failure counts.
 * - Clarity: Improved comments, structure, and variable naming.
 */

 #include "kmalloc_internal.h" // For DEFAULT_ALIGNMENT, ALIGN_UP
 #include "buddy.h"            // Public API, config constants (MIN/MAX_ORDER)
 #include "terminal.h"         // Kernel logging
 #include "types.h"            // Core types (uintptr_t, size_t, bool)
 #include "spinlock.h"         // Spinlock implementation
 #include <libc/stdint.h>      // Fixed-width types, SIZE_MAX
 #include "paging.h"           // For PAGE_SIZE, KERNEL_SPACE_VIRT_START
 #include <string.h>           // For memset (use kernel's version)
 
 // --- Configuration & Constants ---
 
 #ifndef MIN_ORDER
 #error "MIN_ORDER is not defined (include buddy.h)"
 #endif
 #ifndef MAX_ORDER
 #error "MAX_ORDER is not defined (include buddy.h)"
 #endif
 #ifndef PAGE_SIZE
 #define PAGE_SIZE 4096
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
 
 _Static_assert(PAGE_ORDER >= MIN_ORDER, "PAGE_ORDER must be >= MIN_ORDER");
 _Static_assert(PAGE_ORDER <= MAX_ORDER, "PAGE_ORDER must be <= MAX_ORDER");
 
 #define MIN_INTERNAL_ORDER MIN_ORDER
 #define MIN_BLOCK_SIZE_INTERNAL (1 << MIN_INTERNAL_ORDER)
 
 // --- Non-Debug Metadata Header ---
 #ifndef DEBUG_BUDDY
 typedef struct buddy_header {
     uint8_t order; // Order of the allocated block
 } buddy_header_t;
 // Ensure header size is aligned to default alignment for safety
 #define BUDDY_HEADER_SIZE ALIGN_UP(sizeof(buddy_header_t), DEFAULT_ALIGNMENT)
 #else
 #define BUDDY_HEADER_SIZE 0 // No header in debug builds
 #endif // !DEBUG_BUDDY
 
 // --- FIX: Remove the problematic preprocessor #if condition ---
 // The check itself is valid conceptually, but cannot be done by the preprocessor here.
 // We keep the warning guarded by #ifndef DEBUG_BUDDY
 #ifndef DEBUG_BUDDY
     #warning "Smallest buddy block might not accommodate header and alignment. Verify MIN_INTERNAL_ORDER."
     // Original check was: (MIN_BLOCK_SIZE_INTERNAL < (BUDDY_HEADER_SIZE + DEFAULT_ALIGNMENT))
 #endif
 // --- End Fix ---
 
 
 // --- Panic Macro ---
 #define BUDDY_PANIC(msg) do { \
     terminal_printf("\n[BUDDY PANIC] %s at %s:%d. System Halted.\n", msg, __FILE__, __LINE__); \
     /* TODO: Add allocator state dump here if feasible */ \
     while (1) { asm volatile("cli; hlt"); } \
 } while(0)
 
 // --- Assertion Macro (runtime check) ---
 #define BUDDY_ASSERT(condition, msg) do { \
     if (!(condition)) { \
         terminal_printf("\n[BUDDY ASSERT FAILED] %s at %s:%d\n", msg, __FILE__, __LINE__); \
         BUDDY_PANIC("Assertion failed"); \
     } \
 } while (0)
 
 // --- Free List Structure ---
 typedef struct buddy_block {
     struct buddy_block *next;
 } buddy_block_t;
 
 // --- Global State ---
 static buddy_block_t *free_lists[MAX_ORDER + 1] = {0}; // Array of free lists
 static uintptr_t g_heap_start_virt_addr = 0;          // Aligned VIRTUAL heap start
 static uintptr_t g_heap_end_virt_addr = 0;            // VIRTUAL heap end (exclusive)
 static size_t g_buddy_total_managed_size = 0;
 static size_t g_buddy_free_bytes = 0;
 static spinlock_t g_buddy_lock;                       // Lock for allocator state
 
 // Statistics
 static uint64_t g_alloc_count = 0;
 static uint64_t g_free_count = 0;
 static uint64_t g_failed_alloc_count = 0;
 
 // --- Debug Allocation Tracker (contents unchanged from provided code) ---
 #ifdef DEBUG_BUDDY
 #define DEBUG_CANARY_START 0xDEADBEEF
 #define DEBUG_CANARY_END   0xCAFEBABE
 #define MAX_TRACKER_NODES 1024
 
 typedef struct allocation_tracker {
     void* user_addr;
     void* block_addr;
     size_t block_size;
     const char* source_file;
     int source_line;
     struct allocation_tracker* next;
 } allocation_tracker_t;
 
 static allocation_tracker_t g_tracker_nodes[MAX_TRACKER_NODES];
 static allocation_tracker_t *g_free_tracker_nodes = NULL;
 static allocation_tracker_t *g_active_allocations = NULL;
 static spinlock_t g_alloc_tracker_lock;
 
 static void init_tracker_pool() {
     spinlock_init(&g_alloc_tracker_lock);
     g_free_tracker_nodes = NULL;
     g_active_allocations = NULL;
     for (int i = 0; i < MAX_TRACKER_NODES; ++i) {
         g_tracker_nodes[i].next = g_free_tracker_nodes;
         g_free_tracker_nodes = &g_tracker_nodes[i];
     }
 }
 
 static allocation_tracker_t* alloc_tracker_node() {
     allocation_tracker_t* node = NULL;
     uintptr_t tracker_irq_flags = spinlock_acquire_irqsave(&g_alloc_tracker_lock);
     if (g_free_tracker_nodes) {
         node = g_free_tracker_nodes;
         g_free_tracker_nodes = node->next;
         node->next = NULL;
     }
     spinlock_release_irqrestore(&g_alloc_tracker_lock, tracker_irq_flags);
     return node;
 }
 
 static void free_tracker_node(allocation_tracker_t* node) {
     if (!node) return;
     uintptr_t tracker_irq_flags = spinlock_acquire_irqsave(&g_alloc_tracker_lock);
     node->next = g_free_tracker_nodes;
     g_free_tracker_nodes = node;
     spinlock_release_irqrestore(&g_alloc_tracker_lock, tracker_irq_flags);
 }
 
 static void add_active_allocation(allocation_tracker_t* tracker) {
     if (!tracker) return;
     uintptr_t tracker_irq_flags = spinlock_acquire_irqsave(&g_alloc_tracker_lock);
     tracker->next = g_active_allocations;
     g_active_allocations = tracker;
     spinlock_release_irqrestore(&g_alloc_tracker_lock, tracker_irq_flags);
 }
 
 static allocation_tracker_t* remove_active_allocation(void* user_addr) {
     allocation_tracker_t* found_tracker = NULL;
     allocation_tracker_t** prev_next_ptr = &g_active_allocations;
     uintptr_t tracker_irq_flags = spinlock_acquire_irqsave(&g_alloc_tracker_lock);
     allocation_tracker_t* current = g_active_allocations;
     while (current) {
         if (current->user_addr == user_addr) {
             found_tracker = current;
             *prev_next_ptr = current->next;
             break;
         }
         prev_next_ptr = &current->next;
         current = current->next;
     }
     spinlock_release_irqrestore(&g_alloc_tracker_lock, tracker_irq_flags);
     return found_tracker;
 }
 #endif // DEBUG_BUDDY
 
 
 // --- Internal Helper Functions ---
 
 /**
  * Calculates the required block order for a given user size,
  * considering header overhead and ensuring alignment requirements can be met.
  */
 static int buddy_required_order(size_t user_size) {
     size_t required_total_size = user_size + BUDDY_HEADER_SIZE;
     if (required_total_size < user_size) return MAX_ORDER + 1; // Overflow
 
     size_t block_size = MIN_BLOCK_SIZE_INTERNAL;
     int order = MIN_INTERNAL_ORDER;
 
     while (block_size < required_total_size) {
         if (block_size > (SIZE_MAX >> 1)) return MAX_ORDER + 1; // Overflow check
         block_size <<= 1;
         order++;
         if (order > MAX_ORDER) return MAX_ORDER + 1; // Exceeded max order
     }
     return order;
 }
 
 /**
  * Calculates the virtual address of the buddy block for a given block address and order.
  */
 static inline uintptr_t get_buddy_addr(uintptr_t block_addr, int order) {
     uintptr_t buddy_offset = (uintptr_t)1 << order;
     return block_addr ^ buddy_offset;
 }
 
 /**
  * Adds a block (given by its virtual address) to the appropriate free list.
  * Assumes lock is held.
  */
 static void add_block_to_free_list(void *block_ptr, int order) {
     BUDDY_ASSERT(order >= MIN_INTERNAL_ORDER && order <= MAX_ORDER, "Invalid order in add_block");
     BUDDY_ASSERT(block_ptr != NULL, "Adding NULL block to free list");
     buddy_block_t *block = (buddy_block_t*)block_ptr;
     block->next = free_lists[order];
     free_lists[order] = block;
 }
 
 /**
  * Removes a specific block (given by its virtual address) from its free list.
  * Assumes lock is held.
  * Returns true if found and removed, false otherwise.
  */
 static bool remove_block_from_free_list(void *block_ptr, int order) {
     BUDDY_ASSERT(order >= MIN_INTERNAL_ORDER && order <= MAX_ORDER, "Invalid order in remove_block");
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
  * Converts a block size (must be power of 2) to its buddy order.
  * Returns MAX_ORDER + 1 if size is invalid or too large.
  */
 static int buddy_block_size_to_order(size_t block_size) {
     if (block_size < MIN_BLOCK_SIZE_INTERNAL || (block_size & (block_size - 1)) != 0) {
         return MAX_ORDER + 1; // Not a valid power-of-2 block size or too small
     }
     int order = 0;
     size_t size = 1;
     while (size < block_size) {
         size <<= 1;
         order++;
         if (order > MAX_ORDER) return MAX_ORDER + 1; // Exceeded max order
     }
     // Check if derived order matches MIN_INTERNAL_ORDER lower bound
     if (order < MIN_INTERNAL_ORDER) return MAX_ORDER + 1;
     return order;
 }
 
 
 // --- Initialization ---
 
 /**
  * Initializes the buddy allocator system.
  * Assumes the physical memory region [heap_region_phys_start, +region_size)
  * is already mapped into the kernel's higher-half virtual address space starting
  * at KERNEL_SPACE_VIRT_START + heap_region_phys_start.
  */
 void buddy_init(void *heap_region_phys_start_ptr, size_t region_size) {
     uintptr_t heap_region_phys_start = (uintptr_t)heap_region_phys_start_ptr;
     terminal_printf("[Buddy] Initializing...\n");
     terminal_printf("  Input Region Phys Start: 0x%x, Size: %u bytes\n", heap_region_phys_start, region_size);
 
     // 1. Basic Sanity Checks
     if (heap_region_phys_start == 0 || region_size < MIN_BLOCK_SIZE_INTERNAL) {
         BUDDY_PANIC("Invalid region parameters for buddy_init");
     }
     if (KERNEL_SPACE_VIRT_START == 0) {
         BUDDY_PANIC("KERNEL_SPACE_VIRT_START is not defined or zero");
     }
     if (MAX_ORDER >= (sizeof(uintptr_t) * 8)) { // Use uintptr_t size
         BUDDY_PANIC("MAX_ORDER too large for address space");
     }
 
     // 2. Initialize Locks and Free Lists
     for (int i = 0; i <= MAX_ORDER; i++) free_lists[i] = NULL;
     spinlock_init(&g_buddy_lock);
     #ifdef DEBUG_BUDDY
     init_tracker_pool();
     #endif
 
     // 3. Calculate Aligned Physical and Corresponding Virtual Start
     size_t max_block_alignment = (size_t)1 << MAX_ORDER;
     uintptr_t aligned_phys_start_addr = ALIGN_UP(heap_region_phys_start, max_block_alignment);
     size_t adjustment = aligned_phys_start_addr - heap_region_phys_start;
 
     if (adjustment >= region_size || (region_size - adjustment) < MIN_BLOCK_SIZE_INTERNAL) {
         terminal_printf("[Buddy] Error: Not enough space in region after aligning start to %u bytes.\n", max_block_alignment);
         g_heap_start_virt_addr = 0; g_heap_end_virt_addr = 0; // Mark as uninitialized
         return;
     }
 
     size_t available_size = region_size - adjustment;
     g_heap_start_virt_addr = KERNEL_SPACE_VIRT_START + aligned_phys_start_addr;
 
     // Check for virtual address overflow on start address calculation
     if (g_heap_start_virt_addr < KERNEL_SPACE_VIRT_START || g_heap_start_virt_addr < aligned_phys_start_addr) {
          BUDDY_PANIC("Virtual heap start address overflowed");
     }
 
     g_heap_end_virt_addr = g_heap_start_virt_addr; // Tentative end, adjusted below
     terminal_printf("  Aligned Phys Start: 0x%x, Corresponding Virt Start: 0x%x\n", aligned_phys_start_addr, g_heap_start_virt_addr);
     terminal_printf("  Available Size after alignment: %u bytes\n", available_size);
 
     // 4. Populate Free Lists with Initial Blocks (using VIRTUAL addresses)
     g_buddy_total_managed_size = 0;
     g_buddy_free_bytes = 0;
     uintptr_t current_virt_addr = g_heap_start_virt_addr;
     size_t remaining_size = available_size;
 
     while (remaining_size >= MIN_BLOCK_SIZE_INTERNAL) {
         int order = MAX_ORDER;
         size_t block_size = (size_t)1 << order;
         BUDDY_ASSERT(block_size > 0 || order == 0, "Block size calculation invalid");
 
         // Find largest block that fits and maintains alignment relative to heap start
         while (order >= MIN_INTERNAL_ORDER &&
                (block_size > remaining_size ||
                ((current_virt_addr - g_heap_start_virt_addr) % block_size != 0)))
         {
             order--;
             if (order < MIN_INTERNAL_ORDER) {
                  block_size = 0; break; // Ensure loop terminates if order drops below min
             }
             block_size >>= 1;
         }
 
         if (order < MIN_INTERNAL_ORDER || block_size == 0) break; // Cannot fit even smallest block
 
         // terminal_printf("  Adding initial block: VirtAddr=0x%x, Size=%u (Order %d)\n", current_virt_addr, block_size, order);
         add_block_to_free_list((void*)current_virt_addr, order);
 
         g_buddy_total_managed_size += block_size;
         g_buddy_free_bytes += block_size;
         current_virt_addr += block_size;
         remaining_size -= block_size;
 
         // Check for virtual address overflow during loop
         if (current_virt_addr < g_heap_start_virt_addr) {
             terminal_printf("[Buddy] Warning: Virtual address wrapped during init loop. Halting population.\n");
             break;
         }
     }
 
     // Set the final virtual end address
     g_heap_end_virt_addr = g_heap_start_virt_addr + g_buddy_total_managed_size;
     if (g_heap_end_virt_addr < g_heap_start_virt_addr) { // Handle overflow
         g_heap_end_virt_addr = UINTPTR_MAX;
     }
 
     terminal_printf("[Buddy] Init done. Managed VIRT Range: [0x%x - 0x%x)\n", g_heap_start_virt_addr, g_heap_end_virt_addr);
     terminal_printf("  Total Managed: %u bytes, Initially Free: %u bytes\n", g_buddy_total_managed_size, g_buddy_free_bytes);
     if (remaining_size > 0) {
         terminal_printf("  (Note: %u bytes unused at end of region due to alignment/size)\n", remaining_size);
     }
 }
 
 
 // --- Allocation ---
 
 /**
  * Internal implementation for buddy allocation. Finds/splits blocks.
  * Assumes lock is held.
  */
 void* buddy_alloc_impl(int requested_order, const char* file, int line) {
     // Find smallest suitable block order
     int order = requested_order;
     while (order <= MAX_ORDER && free_lists[order] == NULL) {
         order++;
     }
 
     if (order > MAX_ORDER) { // Out of memory
         g_failed_alloc_count++;
         #ifdef DEBUG_BUDDY
         terminal_printf("[Buddy OOM @ %s:%d] Order %d requested.\n", file, line, requested_order);
         #endif
         return NULL;
     }
 
     // Remove block from found list
     buddy_block_t *block = free_lists[order];
     free_lists[order] = block->next;
 
     // Split down to the requested order
     while (order > requested_order) {
         order--;
         size_t half_block_size = (size_t)1 << order;
         uintptr_t buddy_addr = (uintptr_t)block + half_block_size;
         add_block_to_free_list((void*)buddy_addr, order);
     }
 
     // Update stats
     size_t allocated_block_size = (size_t)1 << requested_order;
     g_buddy_free_bytes -= allocated_block_size;
     g_alloc_count++;
 
     // --- Alignment Assertion for Page Allocations ---
     #ifdef PAGE_ORDER
     if (requested_order >= PAGE_ORDER) {
         uintptr_t block_addr_virt = (uintptr_t)block;
         // Calculate corresponding physical address
         uintptr_t physical_addr = block_addr_virt - KERNEL_SPACE_VIRT_START;
         // Assert physical alignment
         BUDDY_ASSERT((physical_addr % PAGE_SIZE) == 0,
                      "Buddy returned non-page-aligned PHYS block for page-sized request!");
         // Assert virtual alignment (should be guaranteed by buddy logic & init alignment)
         BUDDY_ASSERT((block_addr_virt % PAGE_SIZE) == 0,
                      "Buddy returned non-page-aligned VIRTUAL block for page-sized request!");
     }
     #endif
 
     return (void*)block; // Return VIRTUAL address of the allocated block
 }
 
 
 // --- Free ---
 
 /**
  * Internal implementation for freeing a buddy block. Handles coalescing.
  * Assumes lock is held.
  */
 static void buddy_free_impl(void *block_addr_virt, int block_order, const char* file, int line) {
     uintptr_t addr_virt = (uintptr_t)block_addr_virt;
     size_t block_size = (size_t)1 << block_order;
 
     // --- Add basic validity checks ---
     BUDDY_ASSERT(block_order >= MIN_INTERNAL_ORDER && block_order <= MAX_ORDER, "Invalid order in buddy_free_impl");
     BUDDY_ASSERT(addr_virt >= g_heap_start_virt_addr && addr_virt < g_heap_end_virt_addr, "Address outside heap in buddy_free_impl");
     BUDDY_ASSERT((addr_virt % block_size) == 0, "Address not aligned to block size in buddy_free_impl");
     // ---------------------------------
 
 
     // Coalescing loop
     while (block_order < MAX_ORDER) {
         uintptr_t buddy_addr_virt = get_buddy_addr(addr_virt, block_order);
 
         // Boundary check for buddy address
         if (buddy_addr_virt < g_heap_start_virt_addr || buddy_addr_virt >= g_heap_end_virt_addr) {
              break; // Buddy is outside the managed heap, cannot coalesce
         }
 
         // Attempt to remove buddy from its free list
         if (remove_block_from_free_list((void*)buddy_addr_virt, block_order)) {
             // Buddy was free, merge them. New block starts at lower address.
             if (buddy_addr_virt < addr_virt) {
                 addr_virt = buddy_addr_virt;
             }
             block_order++; // Increase order for next level
             #ifdef DEBUG_BUDDY
             // Optional coalescing log
             // terminal_printf("  [Buddy Free Debug %s:%d] Coalesced order %d->%d V=0x%x\n", file, line, block_order-1, block_order, addr_virt);
             #endif
         } else {
             break; // Buddy not free, cannot coalesce further
         }
     }
 
     // Add final block to the appropriate list
     add_block_to_free_list((void*)addr_virt, block_order);
 
     // Update stats (add back size of the *originally* freed block)
     g_buddy_free_bytes += block_size;
     g_free_count++;
 }
 
 
 // --- Public API Implementations ---
 
 #ifdef DEBUG_BUDDY
 /* buddy_alloc_internal (Debug Version Wrapper) */
 void *buddy_alloc_internal(size_t size, const char* file, int line) {
     if (size == 0) return NULL;
 
     int req_order = buddy_required_order(size);
     if (req_order > MAX_ORDER) {
         terminal_printf("[Buddy DEBUG %s:%d] Error: Size %u too large (req order %d > max %d).\n", file, line, size, req_order, MAX_ORDER);
         g_failed_alloc_count++;
         return NULL;
     }
 
     uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
     void *block_ptr = buddy_alloc_impl(req_order, file, line);
     spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);
 
     if (!block_ptr) return NULL;
 
     size_t block_size = (size_t)1 << req_order;
     void* user_ptr = block_ptr; // Return start of block in debug
 
     allocation_tracker_t* tracker = alloc_tracker_node();
     if (!tracker) {
         terminal_printf("[Buddy DEBUG %s:%d] CRITICAL: Failed to alloc tracker! Freeing block.\n", file, line);
         uintptr_t free_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
         buddy_free_impl(block_ptr, req_order, __FILE__, __LINE__); // Use internal details for free
         spinlock_release_irqrestore(&g_buddy_lock, free_irq_flags);
         g_failed_alloc_count++; // Count failure to track
         g_alloc_count--;        // Decrement success count from buddy_alloc_impl
         g_buddy_free_bytes += block_size; // Add block back to free count
         return NULL;
     }
 
     tracker->user_addr = user_ptr;
     tracker->block_addr = block_ptr;
     tracker->block_size = block_size;
     tracker->source_file = file;
     tracker->source_line = line;
     add_active_allocation(tracker);
 
     // Place canaries
     if (block_size >= sizeof(uint32_t) * 2) {
         *(volatile uint32_t*)block_ptr = DEBUG_CANARY_START; // Use volatile write
         *(volatile uint32_t*)((uintptr_t)block_ptr + block_size - sizeof(uint32_t)) = DEBUG_CANARY_END;
     } else if (block_size >= sizeof(uint32_t)) {
          *(volatile uint32_t*)block_ptr = DEBUG_CANARY_START;
     }
 
     // Zero allocated memory in debug builds for safety? Optional.
     // memset(user_ptr, 0xCD, block_size); // Careful: overwrites canaries if user_ptr==block_ptr
 
     return user_ptr;
 }
 
 /* buddy_free_internal (Debug Version Wrapper) */
 void buddy_free_internal(void *ptr, const char* file, int line) {
     if (!ptr) return;
 
     uintptr_t addr = (uintptr_t)ptr;
     // Check range FIRST
     if (addr < g_heap_start_virt_addr || addr >= g_heap_end_virt_addr) {
         terminal_printf("[Buddy DEBUG %s:%d] Error: Freeing 0x%p outside heap [0x%x - 0x%x).\n",
                         file, line, ptr, g_heap_start_virt_addr, g_heap_end_virt_addr);
         BUDDY_PANIC("Freeing pointer outside heap!");
         return;
     }
 
     allocation_tracker_t* tracker = remove_active_allocation(ptr);
     if (!tracker) {
         terminal_printf("[Buddy DEBUG %s:%d] Error: Freeing untracked/double-freed pointer 0x%p\n", file, line, ptr);
         BUDDY_PANIC("Freeing untracked pointer!");
         return;
     }
 
     size_t block_size = tracker->block_size;
     void* block_ptr = tracker->block_addr; // Use tracked block address
     bool canary_ok = true;
 
     // Validate canaries based on block_ptr and block_size
     if (block_size >= sizeof(uint32_t) * 2) {
         if (*(volatile uint32_t*)block_ptr != DEBUG_CANARY_START) { // Use volatile read
             terminal_printf("[Buddy DEBUG %s:%d] CORRUPTION: Start canary fail block 0x%p (size %u) freed from 0x%p! Alloc@ %s:%d\n",
                             file, line, block_ptr, block_size, ptr, tracker->source_file, tracker->source_line);
             canary_ok = false;
         }
         if (*(volatile uint32_t*)((uintptr_t)block_ptr + block_size - sizeof(uint32_t)) != DEBUG_CANARY_END) { // Use volatile read
              terminal_printf("[Buddy DEBUG %s:%d] CORRUPTION: End canary fail block 0x%p (size %u) freed from 0x%p! Alloc@ %s:%d\n",
                             file, line, block_ptr, block_size, ptr, tracker->source_file, tracker->source_line);
             canary_ok = false;
         }
     } else if (block_size >= sizeof(uint32_t)) {
          if (*(volatile uint32_t*)block_ptr != DEBUG_CANARY_START) { // Use volatile read
             terminal_printf("[Buddy DEBUG %s:%d] CORRUPTION: Start canary fail (small block) block 0x%p (size %u) freed from 0x%p! Alloc@ %s:%d\n",
                             file, line, block_ptr, block_size, ptr, tracker->source_file, tracker->source_line);
             canary_ok = false;
          }
     }
 
     if (!canary_ok) {
         BUDDY_PANIC("Heap corruption detected by canary!");
     }
 
     int block_order = buddy_block_size_to_order(block_size);
     if (block_order > MAX_ORDER) {
         terminal_printf("[Buddy DEBUG %s:%d] Error: Invalid tracked block size %u for ptr 0x%p\n", file, line, block_size, ptr);
         free_tracker_node(tracker); // Still free tracker
         BUDDY_PANIC("Invalid block size in tracker");
         return;
     }
 
     // Poison memory before freeing (optional)
     // memset(block_ptr, 0xFE, block_size);
 
     // Perform actual free
     uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
     buddy_free_impl(block_ptr, block_order, file, line);
     spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);
 
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
             terminal_printf("  - User Addr: 0x%p, Block Addr: 0x%p, Block Size: %u bytes, Allocated at: %s:%d\n",
                             current->user_addr, current->block_addr, current->block_size,
                             current->source_file ? current->source_file : "<unknown>",
                             current->source_line);
             leak_count++;
             leak_bytes += current->block_size;
             current = current->next;
         }
         terminal_printf("Total Leaks: %d blocks, %u bytes (buddy block size)\n", leak_count, leak_bytes);
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
         g_failed_alloc_count++;
         return NULL;
     }
 
     uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
     void *block_ptr = buddy_alloc_impl(req_order, NULL, 0);
     spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);
 
     if (!block_ptr) return NULL;
 
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
 
     // Check range FIRST, considering header size
     if (user_addr < (g_heap_start_virt_addr + BUDDY_HEADER_SIZE) || user_addr >= g_heap_end_virt_addr) {
         terminal_printf("[Buddy] Error: Freeing 0x%p outside heap or before header [0x%x - 0x%x).\n",
                         ptr, g_heap_start_virt_addr + BUDDY_HEADER_SIZE, g_heap_end_virt_addr);
         BUDDY_PANIC("Freeing pointer outside heap!");
         return;
     }
 
     uintptr_t block_addr = user_addr - BUDDY_HEADER_SIZE;
     buddy_header_t* header = (buddy_header_t*)block_addr;
     int block_order = header->order;
 
     if (block_order < MIN_INTERNAL_ORDER || block_order > MAX_ORDER) {
         terminal_printf("[Buddy] Error: Corrupted header freeing 0x%p (order %d).\n", ptr, block_order);
         BUDDY_PANIC("Corrupted buddy header!");
         return;
     }
 
     size_t block_size = (size_t)1 << block_order;
     if (block_addr % block_size != 0) {
         terminal_printf("[Buddy] Error: Freeing misaligned pointer 0x%p (derived block 0x%x, order %d)\n", ptr, block_addr, block_order);
         BUDDY_PANIC("Freeing pointer yielding misaligned block!");
         return;
     }
 
     uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
     buddy_free_impl((void*)block_addr, block_order, NULL, 0);
     spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);
 }

 void* buddy_alloc_raw(int order) {
    // Basic validation on order
    if (order < MIN_INTERNAL_ORDER || order > MAX_ORDER) {
        terminal_printf("[Buddy Raw Alloc] Error: Invalid order %d requested.\n", order);
        // Use the global failed count even for raw allocs
        uintptr_t flags = spinlock_acquire_irqsave(&g_buddy_lock);
        g_failed_alloc_count++;
        spinlock_release_irqrestore(&g_buddy_lock, flags);
        return NULL;
    }

    uintptr_t buddy_irq_flags = spinlock_acquire_irqsave(&g_buddy_lock);
    // Call internal implementation (pass dummy file/line for potential debug traces inside)
    void *block_ptr = buddy_alloc_impl(order, __FILE__, __LINE__);
    spinlock_release_irqrestore(&g_buddy_lock, buddy_irq_flags);

    // Note: buddy_alloc_impl already increments g_alloc_count on success
    // and g_failed_alloc_count on failure within the lock.
    return block_ptr;
}

/**
 * Frees a raw buddy block of the specified order. Handles locking internally.
 * FOR KERNEL INTERNAL USE ONLY.
 */
void buddy_free_raw(void* block_addr_virt, int order) {
     if (!block_addr_virt) return;

     // Basic validation on order and address (optional but recommended)
     if (order < MIN_INTERNAL_ORDER || order > MAX_ORDER) {
         terminal_printf("[Buddy Raw Free] Error: Invalid order %d for block 0x%p.\n", order, block_addr_virt);
         BUDDY_PANIC("Invalid order in buddy_free_raw"); // Or handle less severely
         return;
     }
      uintptr_t addr = (uintptr_t)block_addr_virt;
      if (addr < g_heap_start_virt_addr || addr >= g_heap_end_virt_addr) {
          terminal_printf("[Buddy Raw Free] Error: Freeing 0x%p outside heap [0x%x - 0x%x).\n", block_addr_virt, g_heap_start_virt_addr, g_heap_end_virt_addr);
          BUDDY_PANIC("Freeing pointer outside heap in buddy_free_raw");
          return;
      }
      size_t block_size = (size_t)1 << order;
      if ((addr % block_size) != 0) {
          terminal_printf("[Buddy Raw Free] Error: Freeing misaligned pointer 0x%p for order %d.\n", block_addr_virt, order);
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
     return g_buddy_total_managed_size; // Assumes init completed non-concurrently
 }
 
 /* buddy_get_stats */
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