#ifndef MM_H
#define MM_H

#include "types.h"
#include "paging.h"     // For page flags (PTE_USER_DATA etc.) & registers_t
#include "spinlock.h"   // For protecting mm_struct
#include "vfs.h"        // For file_t definition
#include "rbtree.h"     // *** Include RB Tree header ***

// Define a temporary kernel virtual address for mapping pages during fault handling
// Ensure this doesn't clash with TEMP_MAP_ADDR_PT/PD used elsewhere
#ifndef TEMP_MAP_ADDR_PF // Guard against redefinition if moved to paging.h
#define TEMP_MAP_ADDR_PF (KERNEL_SPACE_VIRT_START - 3 * PAGE_SIZE)
#endif


// --- Virtual Memory Area (VMA) Flags ---
#define VM_READ         0x0001  // Pages can be read
#define VM_WRITE        0x0002  // Pages can be written
#define VM_EXEC         0x0004  // Pages can be executed (ignored if no NX support)
#define VM_SHARED       0x0008  // Shared mapping (vs. private/COW)
#define VM_PRIVATE      0x0000  // Private mapping (default, implies COW on write if VM_WRITE)
#define VM_GROWS_DOWN   0x0010  // Stack-like growth direction
#define VM_ANONYMOUS    0x0020  // Anonymous memory (no file backing)
#define VM_FILEBACKED   0x0040  // Backed by a file
// Add more flags as needed (VM_IO, VM_LOCKED, VM_GUARD, etc.)


/**
 * @brief Represents a contiguous Virtual Memory Area (VMA).
 */
typedef struct vma_struct {
    uintptr_t vm_start;     // Start virtual address (page aligned)
    uintptr_t vm_end;       // End virtual address (exclusive, page aligned)
    uint32_t vm_flags;      // Combination of VM_ flags
    uint32_t page_prot;     // Page protection flags (PTE flags)

    // For file-backed VMAs
    file_t* vm_file;        // Pointer to the VFS file handle (must be kept open)
    size_t vm_offset;       // Offset within the file (page aligned)

    // *** Embed the RB Tree node structure ***
    struct rb_node rb_node; // Use the name defined in rbtree.h

    struct mm_struct *vm_mm;    // Pointer back to the owning mm_struct

} vma_struct_t;

/**
 * @brief Represents the memory map of a process.
 */
typedef struct mm_struct {
    // *** Use the rb_tree structure for the root ***
    struct rb_tree vma_tree;    // Root and helper structure for the VMA RB Tree

    uint32_t *pgd_phys;     // Physical address of the page directory
    spinlock_t lock;        // Lock protecting the VMA tree
    int map_count;          // Number of VMAs

    // Optional tracking fields
    uintptr_t start_code, end_code;
    uintptr_t start_data, end_data;
    uintptr_t start_brk, end_brk;     // Program break (heap)
    uintptr_t start_stack;            // Base of the main stack VMA

} mm_struct_t;

// --- VMA Management Function Signatures ---

/**
 * @brief Creates a new memory descriptor (mm_struct) for a process.
 * Allocates and initializes an mm_struct, including the RB Tree.
 *
 * @param pgd_phys Physical address of the process's page directory.
 * @return Pointer to the newly allocated mm_struct_t, or NULL on failure.
 */
mm_struct_t *create_mm(uint32_t* pgd_phys);

/**
 * @brief Destroys a memory descriptor, freeing all associated VMAs via RB Tree traversal.
 * Unmaps pages associated with VMAs (implementation primarily in paging_unmap_range/put_frame).
 *
 * @param mm Pointer to the mm_struct_t to destroy.
 */
void destroy_mm(mm_struct_t *mm);

/**
 * @brief Finds the VMA that contains a given virtual address using RB Tree search.
 * Acquires and releases the mm->lock.
 *
 * @param mm Pointer to the process's mm_struct_t.
 * @param addr The virtual address to look up.
 * @return Pointer to the vma_struct_t containing the address, or NULL if not found.
 */
vma_struct_t *find_vma(mm_struct_t *mm, uintptr_t addr);

/**
 * @brief Creates and inserts a new VMA into the process's RB Tree.
 * Ensures the new VMA doesn't overlap with existing ones.
 * Acquires and releases the mm->lock.
 *
 * @param mm Pointer to the process's mm_struct_t.
 * @param start Start virtual address (page aligned).
 * @param end End virtual address (page aligned).
 * @param vm_flags VM_ flags (VM_READ, VM_WRITE, VM_ANONYMOUS, etc.).
 * @param page_prot PTE flags (PTE_USER_DATA, etc.).
 * @param file Optional backing file pointer (NULL for anonymous).
 * @param offset Offset within the backing file (0 for anonymous).
 * @return Pointer to the created VMA on success, NULL on failure (overlap, OOM).
 */
vma_struct_t* insert_vma(mm_struct_t *mm, uintptr_t start, uintptr_t end,
                         uint32_t vm_flags, uint32_t page_prot,
                         file_t* file, size_t offset);

// Potentially add functions like split_vma, merge_vma, remove_vma for mmap/munmap

/**
 * @brief Handles a page fault within the context of a specific VMA.
 * Called by the main page_fault_handler if a valid VMA is found.
 * Responsible for allocating/mapping pages (Anon, File, COW).
 * Assumes mm->lock is NOT held by caller (acquires locks as needed internally if modifying shared state, but VMA lookup is done before calling).
 *
 * @param mm The process memory map.
 * @param vma The VMA covering the faulting address.
 * @param address The faulting virtual address.
 * @param error_code The page fault error code from the CPU.
 * @return 0 on success, negative error code on failure.
 */
int handle_vma_fault(mm_struct_t *mm, vma_struct_t *vma, uintptr_t address, uint32_t error_code);


#endif // MM_H