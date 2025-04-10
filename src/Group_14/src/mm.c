#include "mm.h"
#include "kmalloc.h"    // For allocating mm_struct and vma_struct
#include "terminal.h"   // For logging
#include "buddy.h"      // Underlying physical allocator (called by frame allocator)
#include "frame.h"      // *** Include frame allocator header ***
#include "paging.h"     // For mapping pages, flags, TEMP addresses
#include "vfs.h"        // For vfs_read, vfs_lseek, file_t
#include "fs_errno.h"   // For VFS error codes
#include "rbtree.h"     // *** Include RB Tree header ***
#include <string.h>     // For memset, memcpy

// --- Temporary Mapping Addresses ---
#ifndef TEMP_MAP_ADDR_PD // Guard against redefinition
#define TEMP_MAP_ADDR_PD (KERNEL_SPACE_VIRT_START - 1 * PAGE_SIZE)
#endif
#ifndef TEMP_MAP_ADDR_PT
#define TEMP_MAP_ADDR_PT (KERNEL_SPACE_VIRT_START - 2 * PAGE_SIZE)
#endif
#ifndef TEMP_MAP_ADDR_PF
#define TEMP_MAP_ADDR_PF (KERNEL_SPACE_VIRT_START - 3 * PAGE_SIZE)
#endif
#ifndef TEMP_MAP_ADDR_COW_SRC
#define TEMP_MAP_ADDR_COW_SRC (KERNEL_SPACE_VIRT_START - 4 * PAGE_SIZE)
#endif
#ifndef TEMP_MAP_ADDR_COW_DST
#define TEMP_MAP_ADDR_COW_DST (KERNEL_SPACE_VIRT_START - 5 * PAGE_SIZE)
#endif

extern uint32_t* kernel_page_directory; // Kernel's virtual PGD address

// --- VMA Struct Allocation Helpers ---
static vma_struct_t* alloc_vma_struct() {
    vma_struct_t* vma = (vma_struct_t*)kmalloc(sizeof(vma_struct_t));
    if (vma) { memset(vma, 0, sizeof(vma_struct_t)); }
    return vma;
 }
static void free_vma_struct(vma_struct_t* vma) {
    kfree(vma); // Assuming kfree handles NULL
 }

// --- MM Struct Management ---

/**
 * Creates a new memory descriptor (mm_struct). Initializes RB Tree.
 */
mm_struct_t *create_mm(uint32_t* pgd_phys) {
    mm_struct_t *mm = (mm_struct_t *)kmalloc(sizeof(mm_struct_t));
    if (!mm) {
        terminal_write("[MM] Failed to allocate mm_struct.\n");
        return NULL;
    }
    memset(mm, 0, sizeof(mm_struct_t));
    mm->pgd_phys = pgd_phys;
    rb_tree_init(&mm->vma_tree); // *** Initialize RB Tree ***
    mm->map_count = 0;
    spinlock_init(&mm->lock);
    // Initialize other mm fields if needed
    return mm;
}

// Helper callback for destroy_mm traversal
static void destroy_vma_node_callback(vma_struct_t *vma_node, void *data) {
    (void)data; // Unused data pointer
    if (vma_node) {
        // --- Unmap pages covered by 'vma_node' ---
        // This should call put_frame for each physical page,
        // and potentially free page tables via paging_unmap_range.
        // paging_unmap_range needs to be robust for this to work fully.
        if (vma_node->vm_mm && vma_node->vm_mm->pgd_phys) {
             terminal_printf("[MM] TODO: Implement proper unmapping in destroy_vma_node_callback for VMA [0x%x-0x%x)\n",
                             vma_node->vm_start, vma_node->vm_end);
             // Example call (needs target PD mapped virtually if not current):
             // paging_unmap_range(mapped_proc_pd_virt, vma_node->vm_start, vma_node->vm_end - vma_node->vm_start);
        }


        // If file-backed, potentially close the file handle if the VMA owned it
        // (Depends on how file handles are managed during mmap/process lifecycle)
        // if (vma_node->vm_flags & VM_FILEBACKED && vma_node->vm_file) {
        //     vfs_close(vma_node->vm_file);
        // }

        free_vma_struct(vma_node); // Free the VMA struct itself
    }
}

/**
 * Destroys a memory descriptor and its VMAs using RB Tree traversal.
 */
void destroy_mm(mm_struct_t *mm) {
    if (!mm) return;

    // Acquire lock to safely get root, then clear it
    uintptr_t irq_flags = spinlock_acquire_irqsave(&mm->lock);
    struct rb_node *root = mm->vma_tree.root;
    mm->vma_tree.root = NULL; // Clear root immediately
    mm->map_count = 0;
    spinlock_release_irqrestore(&mm->lock, irq_flags); // Release lock before traversal

    if (root) {
        terminal_write("[MM] Destroying VMAs...\n");
        // *** Perform post-order traversal of RB Tree to free nodes ***
        rbtree_postorder_traverse(root, destroy_vma_node_callback, mm); // Pass mm for unmapping context?
        terminal_write("[MM] VMA destruction traversal complete.\n");
    }

    // Free the mm_struct itself
    kfree(mm);
}

// --- VMA Find/Insert Operations (Using RB Tree) ---

/**
 * Finds the VMA containing addr using RB Tree. Assumes lock held.
 */
static vma_struct_t* find_vma_locked(mm_struct_t *mm, uintptr_t addr) {
    // *** Call RB Tree find function ***
    return rbtree_find_vma(mm->vma_tree.root, addr);
}

/**
 * Public version of find_vma (acquires lock).
 */
vma_struct_t *find_vma(mm_struct_t *mm, uintptr_t addr) {
    if (!mm) return NULL;
    uintptr_t irq_flags = spinlock_acquire_irqsave(&mm->lock);
    vma_struct_t *vma = find_vma_locked(mm, addr);
    spinlock_release_irqrestore(&mm->lock, irq_flags);
    return vma;
}

/**
 * Inserts a VMA into RB Tree, checking overlaps. Assumes lock held.
 */
static vma_struct_t* insert_vma_locked(mm_struct_t *mm, vma_struct_t* new_vma) {
    // *** Check for overlap using RB Tree ***
    if (rbtree_find_overlap(mm->vma_tree.root, new_vma->vm_start, new_vma->vm_end)) {
         terminal_printf("[MM] Error: VMA overlap detected for range [0x%x-0x%x)\n",
                        new_vma->vm_start, new_vma->vm_end);
         return NULL;
    }

    // --- Find insertion point (needed for rb_tree_insert_at) ---
    struct rb_node **link = &mm->vma_tree.root;
    struct rb_node *parent = NULL;
    bool insert_left = true;

    while (*link) {
        parent = *link;
        vma_struct_t *current_vma = rb_entry(parent, vma_struct_t, rb_node);
        if (new_vma->vm_start < current_vma->vm_start) {
            link = &parent->left;
            insert_left = true;
        } else {
            KERNEL_ASSERT(new_vma->vm_start >= current_vma->vm_end, "VMA insertion logic error");
            link = &parent->right;
            insert_left = false;
        }
    }

    // *** Insert node using rb_tree_insert_at ***
    // The library initializes the node's parent_color, left, right.
    rb_tree_insert_at(&mm->vma_tree, parent, &new_vma->rb_node, insert_left);

    new_vma->vm_mm = mm;
    mm->map_count++;

    // TODO: Add merging logic with adjacent VMAs if flags/file match

    return new_vma;
}


/**
 * Public version of insert_vma (allocates VMA, acquires lock, calls locked insert).
 */
vma_struct_t* insert_vma(mm_struct_t *mm, uintptr_t start, uintptr_t end,
                         uint32_t vm_flags, uint32_t page_prot,
                         file_t* file, size_t offset)
{
    if (!mm || start >= end || (start & (PAGE_SIZE-1)) || (end & (PAGE_SIZE-1))) { return NULL; }

    vma_struct_t* vma = alloc_vma_struct();
    if (!vma) { return NULL; }

    // Initialize VMA fields
    vma->vm_start = start; vma->vm_end = end; vma->vm_flags = vm_flags;
    vma->page_prot = page_prot; vma->vm_file = file; vma->vm_offset = offset;
    vma->vm_mm = mm;
    // RB node fields initialized by rb_tree_insert_at

    uintptr_t irq_flags = spinlock_acquire_irqsave(&mm->lock);
    vma_struct_t* result = insert_vma_locked(mm, vma);
    spinlock_release_irqrestore(&mm->lock, irq_flags);

    if (!result) {
        free_vma_struct(vma); // Free struct if insertion failed
        return NULL;
    }
    // TODO: If file-backed, increment file reference count (e.g., vfs_file_dup(file))
    return result;
}


// --- Page Fault Handling ---

// get_pte_ptr helper function (implementation as before)
static uint32_t* get_pte_ptr(mm_struct_t *mm, uintptr_t vaddr, bool allocate_pt) {
    // ... (implementation as provided previously, maps PD/PT temporarily) ...
     if (!mm || !mm->pgd_phys) return NULL;
     uint32_t pd_idx = PDE_INDEX(vaddr); uint32_t pt_idx = PTE_INDEX(vaddr);
     uint32_t* proc_pd_virt = NULL; uint32_t* pt_virt = NULL; uint32_t* pte_ptr = NULL;

     // Map Process PD
     if (paging_map_single(kernel_page_directory, TEMP_MAP_ADDR_PD, (uint32_t)mm->pgd_phys, PTE_KERNEL_DATA) != 0) { return NULL; }
     proc_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;
     uint32_t pde = proc_pd_virt[pd_idx];

     // Allocate PT if needed
     if (!(pde & PAGE_PRESENT)) {
         if (!allocate_pt) { goto cleanup_pd_map_pte_gpp; }
         uint32_t* pt_phys = allocate_page_table_phys(); if (!pt_phys) { goto cleanup_pd_map_pte_gpp; }
         uint32_t pde_flags = PAGE_PRESENT | PAGE_RW | PAGE_USER;
         proc_pd_virt[pd_idx] = (uint32_t)pt_phys | pde_flags; pde = proc_pd_virt[pd_idx];
     }

     // Map PT
     uint32_t* pt_phys = (uint32_t*)(pde & ~0xFFF);
     if (paging_map_single(kernel_page_directory, TEMP_MAP_ADDR_PT, (uint32_t)pt_phys, PTE_KERNEL_DATA) != 0) { goto cleanup_pd_map_pte_gpp; }
     pt_virt = (uint32_t*)TEMP_MAP_ADDR_PT;
     pte_ptr = &pt_virt[pt_idx];

 cleanup_pd_map_pte_gpp:
     paging_unmap_range(kernel_page_directory, TEMP_MAP_ADDR_PD, PAGE_SIZE); paging_invalidate_page((void*)TEMP_MAP_ADDR_PD);
     // NOTE: TEMP_MAP_ADDR_PT remains mapped! Caller must unmap it.
     return pte_ptr; // May be NULL if error occurred
}


/**
 * Handles a page fault for a given VMA. Includes COW using reference counting.
 */
int handle_vma_fault(mm_struct_t *mm, vma_struct_t *vma, uintptr_t fault_address, uint32_t error_code) {
    uintptr_t page_addr = PAGE_ALIGN_DOWN(fault_address);
    int ret = 0;

    // --- Check Permissions ---
    bool is_write = error_code & PAGE_RW;
    if ((is_write && !(vma->vm_flags & VM_WRITE)) || (!is_write && !(vma->vm_flags & VM_READ))) {
        terminal_printf("[PF] Fault: Permission denied VMA [0x%x-0x%x) Addr 0x%x W=%d\n",
                       vma->vm_start, vma->vm_end, fault_address, is_write);
        return -1;
    }

    // --- Handle Present Page Fault (Protection Violation or COW) ---
    if (error_code & PAGE_PRESENT) {
        if (is_write && (vma->vm_flags & VM_WRITE) && !(vma->vm_flags & VM_SHARED)) {
            // --- Copy-on-Write ---
            terminal_printf("[PF] COW Triggered VMA [0x%x-0x%x) Addr 0x%x\n", vma->vm_start, vma->vm_end, fault_address);

            // 1. Get pointer to PTE
             uint32_t* pte_ptr = get_pte_ptr(mm, page_addr, false); // Don't allocate PT if missing
             if (!pte_ptr) { return -10; }
             uint32_t pte = *pte_ptr; // Read PTE value

             if (!(pte & PAGE_PRESENT)) { ret = -11; goto unmap_cow_pt_hvf2; } // Consistency check
             if (pte & PAGE_RW) { ret = 0; goto unmap_cow_pt_hvf2; } // Already writable

            uintptr_t src_phys_page = pte & ~0xFFF;

            // 2. *** Check reference count ***
            int ref_count = get_frame_refcount(src_phys_page);
            if (ref_count < 0) { ret = -12; goto unmap_cow_pt_hvf2; } // Error

            if (ref_count == 1) {
                // --- Not Shared: Make PTE Writable ---
                *pte_ptr = (pte | PAGE_RW); // Add RW flag
                ret = 0;
            } else {
                // --- Shared: Perform Copy ---
                uintptr_t dst_phys_page = 0;
                void* temp_src_map = (void*)TEMP_MAP_ADDR_COW_SRC;
                void* temp_dst_map = (void*)TEMP_MAP_ADDR_COW_DST;

                // a. Allocate destination frame (refcnt=1)
                dst_phys_page = frame_alloc();
                if (!dst_phys_page) { ret = -4; goto unmap_cow_pt_hvf2; } // OOM

                // b/c/d. Map, Copy, Unmap
                if (paging_map_single(kernel_page_directory, (uint32_t)temp_src_map, src_phys_page, PTE_KERNEL_READONLY)!=0) { ret = -5; goto cleanup_cow_alloc_hvf2; }
                if (paging_map_single(kernel_page_directory, (uint32_t)temp_dst_map, dst_phys_page, PTE_KERNEL_DATA)!=0) { ret = -5; goto cleanup_cow_src_map_hvf2; }
                memcpy(temp_dst_map, temp_src_map, PAGE_SIZE);
                paging_unmap_range(kernel_page_directory, (uint32_t)temp_dst_map, PAGE_SIZE); paging_invalidate_page(temp_dst_map);
cleanup_cow_src_map_hvf2:
                paging_unmap_range(kernel_page_directory, (uint32_t)temp_src_map, PAGE_SIZE); paging_invalidate_page(temp_src_map);
                if (ret != 0) goto cleanup_cow_alloc_hvf2;

                // e. Update PTE to new page (writable)
                *pte_ptr = (dst_phys_page & ~0xFFF) | (vma->page_prot | PAGE_RW);

                // f. Decrement ref count of original page
                put_frame(src_phys_page);
                ret = 0; // Success

cleanup_cow_alloc_hvf2:
                if (ret != 0 && dst_phys_page) put_frame(dst_phys_page); // Free dst if failed
            }

            paging_invalidate_page((void*)page_addr); // Invalidate TLB

unmap_cow_pt_hvf2:
            paging_unmap_range(kernel_page_directory, TEMP_MAP_ADDR_PT, PAGE_SIZE); // Unmap temp PT
            paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
            return ret;
        } else { /* ... Unexpected present fault ... */ return -3; }
    } // End if(present)


    // --- Handle Non-Present Page Fault ---
    uintptr_t phys_page = 0;
    void* temp_vaddr_kernel = (void*)TEMP_MAP_ADDR_PF;

    // 1. Allocate frame (refcnt=1)
    phys_page = frame_alloc();
    if (!phys_page) { return -4; }

    // 2. Map frame temporarily into kernel
    if (paging_map_single(kernel_page_directory, (uint32_t)temp_vaddr_kernel, phys_page, PTE_KERNEL_DATA) != 0) {
        put_frame(phys_page); return -5;
    }

    // 3. Populate frame (Zero or File Read)
    if (vma->vm_flags & VM_FILEBACKED) { /* ... File-backed logic as before ... */
         if (!vma->vm_file) { ret = -6; goto cleanup_pf_kernel_map_hvf2; }
         off_t file_offset = (off_t)vma->vm_offset + (off_t)(page_addr - vma->vm_start);
         if (vfs_lseek(vma->vm_file, file_offset, SEEK_SET) != file_offset) { ret = -7; goto cleanup_pf_kernel_map_hvf2; }
         int bytes_read = vfs_read(vma->vm_file, temp_vaddr_kernel, PAGE_SIZE);
         if (bytes_read < 0) { ret = -8; goto cleanup_pf_kernel_map_hvf2; }
         if (bytes_read < PAGE_SIZE) { memset((uint8_t*)temp_vaddr_kernel + bytes_read, 0, PAGE_SIZE - bytes_read); }
    } else {
        memset(temp_vaddr_kernel, 0, PAGE_SIZE); // Anonymous: Zero fill
    }

cleanup_pf_kernel_map_hvf2:
    paging_unmap_range(kernel_page_directory, (uint32_t)temp_vaddr_kernel, PAGE_SIZE);
    paging_invalidate_page(temp_vaddr_kernel);
    if (ret != 0) { put_frame(phys_page); return ret; }

    // 4. Map frame into process space
    if (!mm->pgd_phys) { put_frame(phys_page); return -9; }
    if (paging_map_single(kernel_page_directory, TEMP_MAP_ADDR_PD, (uint32_t)mm->pgd_phys, PTE_KERNEL_DATA) != 0) {
        put_frame(phys_page); return -10;
    }
    uint32_t* proc_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;
    ret = paging_map_single(proc_pd_virt, page_addr, phys_page, vma->page_prot); // Use VMA prot
    paging_unmap_range(kernel_page_directory, TEMP_MAP_ADDR_PD, PAGE_SIZE);
    paging_invalidate_page((void*)TEMP_MAP_ADDR_PD);
    if (ret != 0) { put_frame(phys_page); return -11; }

    // 5. Invalidate TLB
    paging_invalidate_page((void*)page_addr);

    // Ref count is 1 from frame_alloc

    return 0; // Success
}