#ifndef FRAME_H
#define FRAME_H

#include <kernel/core/types.h>      // For uintptr_t, size_t, bool
#include <kernel/arch/multiboot2.h>

// Define flags for frame status (optional, can use ref_count ranges)
#define FRAME_AVAILABLE 0x00 // Implied if ref_count is 0 and allocatable
#define FRAME_RESERVED  0x01 // Kernel, hardware, unusable memory
#define FRAME_ALLOCATED 0x02 // In use (ref_count > 0)

// Structure to hold metadata for each physical frame (optional, could just be array)
// Using just an array of counts is simpler for now.
// typedef struct {
//     volatile uint32_t ref_count;
//     // uint8_t flags; // Optional flags like FRAME_RESERVED
//     // Could add pointers for page cache management later
// } frame_info_t;


/**
 * @brief Initializes the physical frame allocator and reference counting system.
 *
 * Needs the memory map provided by the bootloader to determine total memory
 * and usable ranges. Allocates the frame metadata array itself.
 *
 * @param mmap_tag Pointer to the Multiboot 2 memory map tag.
 * @param kernel_phys_start Physical start address of the kernel image.
 * @param kernel_phys_end Physical end address of the kernel image.
 * @param buddy_heap_phys_start Physical start address of the main buddy heap.
 * @param buddy_heap_phys_end Physical end address of the main buddy heap.
 * @return 0 on success, negative error code on failure.
 */
int frame_init(struct multiboot_tag_mmap *mmap_tag,
               uintptr_t kernel_phys_start, uintptr_t kernel_phys_end,
               uintptr_t buddy_heap_phys_start, uintptr_t buddy_heap_phys_end);

/**
 * @brief Allocates a single physical page frame (e.g., 4KB).
 * Calls the underlying page allocator (buddy) and sets the reference count to 1.
 *
 * @return Physical address of the allocated frame, or 0 (NULL) if OOM.
 */
uintptr_t frame_alloc(void);

/**
 * @brief Increments the reference count for a given physical frame.
 * Use this when a new PTE starts pointing to this frame (sharing).
 *
 * @param phys_addr Physical address of the frame.
 */
void get_frame(uintptr_t phys_addr);

/**
 * @brief Decrements the reference count for a given physical frame.
 * If the reference count drops to 0, the frame is freed back to the
 * underlying page allocator (buddy). Use this when a PTE stops pointing
 * to this frame (unmapping, process exit).
 *
 * @param phys_addr Physical address of the frame.
 */
void put_frame(uintptr_t phys_addr);

/**
 * @brief Gets the current reference count for a physical frame.
 *
 * @param phys_addr Physical address of the frame.
 * @return The reference count, or -1 if the address is invalid.
 */
int get_frame_refcount(uintptr_t phys_addr);

// <<< ADDED PROTOTYPE >>>
/**
 * @brief Frees a physical frame directly (equivalent to put_frame(phys_addr)).
 * This function exists primarily to resolve the missing declaration in elf_loader.c.
 * It simply calls put_frame.
 *
 * @param phys_addr Physical address of the frame to free.
 */
static inline void frame_free(uintptr_t phys_addr) {
    put_frame(phys_addr);
}

void frame_incref(uintptr_t phys_addr); // +++ ADD THIS LINE +++
// <<< END ADDED >>>


#endif // FRAME_H