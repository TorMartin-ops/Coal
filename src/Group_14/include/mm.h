#ifndef MM_H
#define MM_H

#include "types.h"
#include "paging.h"
#include "spinlock.h"
#include "vfs.h"
#include "rbtree.h"

#ifndef TEMP_MAP_ADDR_PF
#define TEMP_MAP_ADDR_PF (KERNEL_SPACE_VIRT_START - 3 * PAGE_SIZE)
#endif


// --- Virtual Memory Area (VMA) Flags ---
#define VM_READ         0x0001
#define VM_WRITE        0x0002
#define VM_EXEC         0x0004
#define VM_SHARED       0x0008
#define VM_PRIVATE      0x0000 // Default, implies COW on write if VM_WRITE
#define VM_GROWS_DOWN   0x0010
#define VM_ANONYMOUS    0x0020
#define VM_FILEBACKED   0x0040
// Add more flags as needed

typedef struct vma_struct {
    uintptr_t vm_start;
    uintptr_t vm_end;
    uint32_t vm_flags;
    uint32_t page_prot;
    file_t* vm_file;
    size_t vm_offset;
    struct rb_node rb_node;
    struct mm_struct *vm_mm;
} vma_struct_t;

typedef struct mm_struct {
    struct rb_tree vma_tree;
    uint32_t *pgd_phys;
    spinlock_t lock;
    int map_count;
    uintptr_t start_code, end_code;
    uintptr_t start_data, end_data;
    uintptr_t start_brk, end_brk;
    uintptr_t start_stack;
} mm_struct_t;

// --- Function Signatures ---

mm_struct_t *create_mm(uint32_t* pgd_phys);
void destroy_mm(mm_struct_t *mm);
vma_struct_t *find_vma(mm_struct_t *mm, uintptr_t addr);
vma_struct_t* insert_vma(mm_struct_t *mm, uintptr_t start, uintptr_t end,
                         uint32_t vm_flags, uint32_t page_prot,
                         file_t* file, size_t offset);

/**
 * @brief Removes/modifies VMAs overlapping a given range.
 * Unmaps pages and frees corresponding physical frames.
 * Requires complex RB Tree manipulation (splitting/removing nodes).
 *
 * @param mm Pointer to the process's mm_struct_t.
 * @param start Start virtual address (page aligned).
 * @param length Length of the range (must be > 0).
 * @return 0 on success, negative error code on failure.
 */
int remove_vma_range(mm_struct_t *mm, uintptr_t start, size_t length); // <-- Added for munmap

int handle_vma_fault(mm_struct_t *mm, vma_struct_t *vma, uintptr_t address, uint32_t error_code);


#endif // MM_H