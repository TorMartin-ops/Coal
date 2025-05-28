// Include Guard
#ifndef MM_H
#define MM_H

#include <kernel/core/types.h>
#include <kernel/memory/paging.h>     // Include for PAGE_SIZE, KERNEL_SPACE_VIRT_START etc.
#include <kernel/sync/spinlock.h>
#include <kernel/fs/vfs/vfs.h>        // Include for file_t definition used in vma_struct
#include <kernel/lib/rbtree.h>     // Include for rb_node and rb_tree definitions

// Define temporary mapping address if not already defined elsewhere (e.g., paging.h)
#ifndef TEMP_MAP_ADDR_PF
#define TEMP_MAP_ADDR_PF (KERNEL_SPACE_VIRT_START - 5u * PAGE_SIZE) // Example address
#endif


// --- Virtual Memory Area (VMA) Flags ---
// Permissions
#define VM_READ         0x00000001  // VMA is readable
#define VM_WRITE        0x00000002  // VMA is writable
#define VM_EXEC         0x00000004  // VMA is executable
// Sharing/Type
#define VM_SHARED       0x00000008  // VMA is shared (changes visible to others mapping same object)
#define VM_PRIVATE      0x00000000  // VMA is private (default, implies Copy-on-Write if writable)
// Behavior
#define VM_GROWS_DOWN   0x00000010  // VMA is a stack that grows downwards
#define VM_ANONYMOUS    0x00000020  // VMA is not backed by a file (e.g., heap, stack, anonymous mmap)
#define VM_FILEBACKED   0x00000040  // VMA is backed by a file
#define VM_USER         0x00000080  // VMA is accessible by user mode (redundant with PAGE_USER?)

// --- ADDED VMA Type Flags (Example bits) ---
#define VM_HEAP         0x00000100  // VMA represents the process heap (managed by brk/sbrk)
#define VM_STACK        0x00000200  // VMA represents a stack region

// Add more flags as needed (e.g., VM_LOCKED, VM_IO, VM_GUARD)


/**
 * @brief Virtual Memory Area (VMA) structure.
 * Represents a contiguous range of virtual addresses with specific properties.
 */
typedef struct vma_struct {
    uintptr_t vm_start;         // Start virtual address (inclusive, page-aligned)
    uintptr_t vm_end;           // End virtual address (exclusive, page-aligned)
    uint32_t vm_flags;          // Flags describing the VMA (VM_READ, VM_WRITE, etc.)
    uint32_t page_prot;         // Page protection flags (PTE flags: PAGE_PRESENT, PAGE_RW, PAGE_USER, PAGE_NX_BIT etc.)
    file_t* vm_file;            // File backing the VMA (NULL for anonymous)
    size_t vm_offset;           // Offset within the backing file (in bytes)
    struct rb_node rb_node;     // Node for Red-Black tree linkage
    struct mm_struct *vm_mm;    // Pointer back to the owning mm_struct
} vma_struct_t;

/**
 * @brief Memory Management structure for a process.
 * Contains the VMA tree, page directory pointer, and other memory-related info.
 */
typedef struct mm_struct {
    struct rb_tree vma_tree;    // Red-Black tree organizing VMAs for efficient lookup
    uint32_t *pgd_phys;         // Physical address of the process's page directory
    spinlock_t lock;            // Lock protecting this mm_struct (especially the VMA tree)
    int map_count;              // Number of VMAs in the tree

    // Optional fields for tracking specific memory regions
    uintptr_t start_code, end_code; // Virtual address range of executable code
    uintptr_t start_data, end_data; // Virtual address range of initialized data
    uintptr_t start_brk, end_brk;   // Current program break (heap boundary)
    uintptr_t start_stack;          // Base address of the main user stack VMA
    // Add other fields as needed (e.g., arg_start, env_start)
} mm_struct_t;

// --- Function Signatures ---

/**
 * @brief Creates and initializes a new mm_struct.
 * @param pgd_phys Physical address of the process's page directory.
 * @return Pointer to the new mm_struct, or NULL on allocation failure.
 */
mm_struct_t *create_mm(uint32_t* pgd_phys);

/**
 * @brief Destroys an mm_struct and frees associated resources.
 * This includes freeing all VMAs, unmapping user pages, freeing associated
 * physical frames and page tables (unless shared).
 * @param mm Pointer to the mm_struct to destroy.
 */
void destroy_mm(mm_struct_t *mm);

/**
 * @brief Finds the VMA that contains a given virtual address.
 * @param mm Pointer to the process's mm_struct.
 * @param addr The virtual address to look up.
 * @return Pointer to the vma_struct containing the address, or NULL if not found.
 */
vma_struct_t *find_vma(mm_struct_t *mm, uintptr_t addr);

/**
 * @brief Inserts a new VMA into the process's address space.
 * Handles merging with adjacent compatible VMAs if possible.
 * @param mm Pointer to the process's mm_struct.
 * @param start Start virtual address (page-aligned).
 * @param end End virtual address (page-aligned, exclusive).
 * @param vm_flags Flags for the new VMA.
 * @param page_prot Page protection flags for the underlying pages.
 * @param file File backing the VMA (NULL for anonymous).
 * @param offset Offset within the file (if file-backed).
 * @return Pointer to the newly created or merged vma_struct on success, NULL on failure (e.g., overlap, allocation failure).
 */
vma_struct_t* insert_vma(mm_struct_t *mm, uintptr_t start, uintptr_t end,
                         uint32_t vm_flags, uint32_t page_prot,
                         file_t* file, size_t offset);

/**
 * @brief Removes/modifies VMAs overlapping a given range.
 * Unmaps pages and frees corresponding physical frames (if not shared).
 * Requires complex RB Tree manipulation (splitting/removing nodes).
 * Used for operations like munmap.
 *
 * @param mm Pointer to the process's mm_struct_t.
 * @param start Start virtual address (page aligned).
 * @param length Length of the range (must be > 0).
 * @return 0 on success, negative error code on failure.
 */
int remove_vma_range(mm_struct_t *mm, uintptr_t start, size_t length); // <-- Added for munmap

/**
 * @brief Handles a page fault within the context of a VMA.
 * Implements demand paging (allocating/mapping frames for anonymous or file-backed pages)
 * and Copy-on-Write (COW) for private writable mappings.
 *
 * @param mm The process's memory structure.
 * @param vma The VMA covering the faulting address.
 * @param address The exact virtual address that caused the fault.
 * @param error_code The page fault error code provided by the CPU.
 * @return 0 if the fault was handled successfully, negative error code otherwise.
 */
int handle_vma_fault(mm_struct_t *mm, vma_struct_t *vma, uintptr_t address, uint32_t error_code);


#endif // MM_H
