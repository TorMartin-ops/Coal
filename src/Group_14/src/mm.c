// src/mm.c

#include "mm.h"
#include "kmalloc.h"    // For allocating mm_struct and vma_struct
#include "terminal.h"   // For logging
#include "buddy.h"      // Underlying physical allocator (called by frame allocator)
#include "frame.h"      // Frame allocator header
#include "paging.h"     // For mapping pages, flags, TEMP addresses, g_kernel_page_directory_phys
#include "vfs.h"        // For vfs_read, vfs_lseek, file_t
#include "fs_errno.h"   // For VFS error codes
#include "rbtree.h"     // RB Tree header
#include "process.h"    // For pcb_t, get_current_process
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

// Kernel's page directory physical address (declared extern in paging.h)
// extern uint32_t g_kernel_page_directory_phys; // Included via paging.h

// --- VMA Struct Allocation Helpers ---
static vma_struct_t* alloc_vma_struct() {
    vma_struct_t* vma = (vma_struct_t*)kmalloc(sizeof(vma_struct_t));
    if (vma) { memset(vma, 0, sizeof(vma_struct_t)); }
    return vma;
}
// Frees the VMA structure and associated resources (like file handle ref count)
static void free_vma_resources(vma_struct_t* vma) {
    if (!vma) return;
    // TODO: If file-backed, decrement file reference count (vfs_file_put?)
    // if (vma->vm_file) { vfs_file_put(vma->vm_file); } // Assuming vfs_file_put exists
    kfree(vma); // Free the vma_struct itself
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
    rb_tree_init(&mm->vma_tree); // Initialize RB Tree
    mm->map_count = 0;
    spinlock_init(&mm->lock);
    // Initialize other mm fields if needed
    return mm;
}

// Helper callback for destroy_mm traversal
static void destroy_vma_node_callback(vma_struct_t *vma_node, void *data) {
    mm_struct_t *mm = (mm_struct_t*)data; // Get mm context
    if (vma_node) {
        // --- Unmap pages covered by 'vma_node' ---
        if (mm && mm->pgd_phys) {
            // terminal_printf("[MM Destroy] Unmapping VMA [0x%x-0x%x)\n",
            //                vma_node->vm_start, vma_node->vm_end);
            int ret = paging_unmap_range(mm->pgd_phys, vma_node->vm_start, vma_node->vm_end - vma_node->vm_start);
            if (ret != 0) {
                 terminal_printf("  Warning: paging_unmap_range failed during VMA destroy (code %d).\n", ret);
            }
        } else {
            terminal_write("  Warning: Cannot unmap VMA during destroy, mm or pgd_phys is NULL.\n");
        }
        free_vma_resources(vma_node); // Free the VMA struct itself
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
        // terminal_write("[MM] Destroying VMAs...\n");
        // Perform post-order traversal of RB Tree to unmap and free nodes
        rbtree_postorder_traverse(root, destroy_vma_node_callback, mm); // Pass mm for context
        // terminal_write("[MM] VMA destruction traversal complete.\n");
    }

    // Free the mm_struct itself
    kfree(mm);
}

// --- VMA Find/Insert Operations (Using RB Tree) ---

/**
 * Finds the VMA containing addr using RB Tree. Assumes lock held.
 */
static vma_struct_t* find_vma_locked(mm_struct_t *mm, uintptr_t addr) {
    // Call RB Tree find function
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
    // Check for overlap using RB Tree
    if (rbtree_find_overlap(mm->vma_tree.root, new_vma->vm_start, new_vma->vm_end)) {
         terminal_printf("[MM] Error: VMA overlap detected for range [0x%lx-0x%lx)\n",
                        (unsigned long)new_vma->vm_start, (unsigned long)new_vma->vm_end);
         return NULL;
    }

    // Find insertion point (needed for rb_tree_insert_at)
    struct rb_node **link = &mm->vma_tree.root;
    struct rb_node *parent = NULL;
    bool insert_left = true;

    while (*link) {
        parent = *link;
        vma_struct_t *current_vma = rb_entry(parent, vma_struct_t, rb_node);
        // Compare based on start address for RB tree ordering
        if (new_vma->vm_start < current_vma->vm_start) {
            link = &parent->left;
            insert_left = true;
        } else {
            // Since overlap is checked, new_vma->vm_start must be >= current_vma->vm_end
            // KERNEL_ASSERT(new_vma->vm_start >= current_vma->vm_end, "VMA insertion logic error - overlap missed?");
            link = &parent->right;
            insert_left = false;
        }
    }

    // Insert node using rb_tree_insert_at
    rb_tree_insert_at(&mm->vma_tree, parent, &new_vma->rb_node, insert_left);

    new_vma->vm_mm = mm;
    mm->map_count++;

    // TODO: Optional: Check for adjacent VMAs with compatible flags/file and merge them

    return new_vma;
}

/**
 * Public version of insert_vma (allocates VMA, acquires lock, calls locked insert).
 */
vma_struct_t* insert_vma(mm_struct_t *mm, uintptr_t start, uintptr_t end,
                         uint32_t vm_flags, uint32_t page_prot,
                         file_t* file, size_t offset)
{
    if (!mm || start > end || (start % PAGE_SIZE) != 0 || (end % PAGE_SIZE) != 0) {
        terminal_write("[MM] insert_vma: Invalid parameters.\n");
        return NULL;
    }

    vma_struct_t* vma = alloc_vma_struct();
    if (!vma) { return NULL; }

    // Initialize VMA fields
    vma->vm_start = start;
    vma->vm_end = end;
    vma->vm_flags = vm_flags;
    vma->page_prot = page_prot;
    vma->vm_file = file;
    vma->vm_offset = offset;
    vma->vm_mm = mm;
    // RB node fields initialized by rb_tree_insert_at

    uintptr_t irq_flags = spinlock_acquire_irqsave(&mm->lock);
    vma_struct_t* result = insert_vma_locked(mm, vma);
    spinlock_release_irqrestore(&mm->lock, irq_flags);

    if (!result) {
        free_vma_resources(vma); // Free struct if insertion failed
        return NULL;
    }
    // TODO: If file-backed, increment file reference count (e.g., vfs_file_dup(file))
    return result;
}


// --- Page Fault Handling ---

// get_pte_ptr helper function (maps PD/PT temporarily)
// Ensures TEMP_MAP_ADDR_PT is unmapped before returning (unless error)
static uint32_t* get_pte_ptr(mm_struct_t *mm, uintptr_t vaddr, bool allocate_pt) {
    // terminal_printf("[get_pte_ptr] Enter: V=%p, alloc=%d\n", (void*)vaddr, allocate_pt);
    if (!mm || !mm->pgd_phys) {
        terminal_printf("[get_pte_ptr] Error: Invalid mm or pgd_phys.\n");
        return NULL;
    }
    // Paging must be active to use temporary mappings based on kernel PD
    if (!g_kernel_page_directory_virt) {
        terminal_printf("[get_pte_ptr] Error: Kernel paging not fully active.\n");
        return NULL;
    }

    uint32_t pd_idx = PDE_INDEX(vaddr);
    uint32_t pt_idx = PTE_INDEX(vaddr);
    uint32_t* proc_pd_virt = NULL; // Temp VAddr for target PD
    uint32_t* pt_virt = NULL;      // Temp VAddr for target PT
    uint32_t* pte_ptr = NULL;      // Result pointer (inside pt_virt mapping)
    uintptr_t pt_phys_addr_val = 0;
    bool allocated_pt_frame = false;

    // 1. Map Target PD temporarily to TEMP_MAP_ADDR_PD
    proc_pd_virt = paging_temp_map((uintptr_t)mm->pgd_phys, PTE_KERNEL_DATA_FLAGS);
    if (!proc_pd_virt) {
        terminal_printf("[get_pte_ptr] Error: Failed to temp map target PD %#lx.\n", (unsigned long)mm->pgd_phys);
        return NULL; // Cannot proceed
    }

    // 2. Read the PDE from the temporarily mapped PD
    uint32_t pde = proc_pd_virt[pd_idx];

    // 3. Check PDE and Handle PT Allocation if needed
    if (!(pde & PAGE_PRESENT)) {
        // PDE Not Present: Allocate and setup new PT
        if (!allocate_pt) {
            // terminal_printf("[get_pte_ptr] Info: PDE[%lu] not present and allocation not requested.\n", pd_idx);
            goto fail_gpp; // PT doesn't exist, fail
        }

        // Allocate a new frame for the Page Table
        pt_phys_addr_val = frame_alloc();
        if (pt_phys_addr_val == 0) {
            terminal_printf("[get_pte_ptr] Error: Failed to allocate frame for new PT (PDE[%lu]).\n", (unsigned long)pd_idx);
            goto fail_gpp;
        }
        allocated_pt_frame = true;
        // terminal_printf("[get_pte_ptr] Allocated new PT frame %#lx for PDE[%lu]\n", pt_phys_addr_val, pd_idx);

        // Map the NEW PT frame temporarily to zero it out
        pt_virt = paging_temp_map(pt_phys_addr_val, PTE_KERNEL_DATA_FLAGS);
        if (!pt_virt) {
            terminal_printf("[get_pte_ptr] Error: Failed to temp map new PT frame %#lx for zeroing.\n", (unsigned long)pt_phys_addr_val);
            goto fail_gpp; // Free frame in cleanup
        }
        memset(pt_virt, 0, PAGE_SIZE);
        paging_temp_unmap(pt_virt); // Unmap after zeroing
        pt_virt = NULL; // Reset pointer

        // Set the PDE in the temporarily mapped target PD
        // Use USER flags if the eventual PTE will need them (most flexible)
        uint32_t pde_flags = PAGE_PRESENT | PAGE_RW | PAGE_USER; // Common flags for a PT PDE
        proc_pd_virt[pd_idx] = (pt_phys_addr_val & PAGING_ADDR_MASK) | pde_flags;
        // Invalidate TLB for the VAddr range affected by the PDE update
        // This needs to happen in the *actual* context using this PD later,
        // but flushing the current CPU for the PD's temp map address might be needed if reusing immediately.
        // For simplicity, we assume TLB invalidation is handled when switching CR3 or explicitly later.
        // paging_invalidate_page((void*)PAGE_ALIGN_DOWN(vaddr)); // Maybe needed if target PD active on another core?

    } else if (pde & PAGE_SIZE_4MB) {
        // Cannot get a PTE pointer if the PDE maps a 4MB page
        terminal_printf("[get_pte_ptr] Error: Cannot get PTE for V=%p; PDE[%lu] is a 4MB page.\n", (void*)vaddr, (unsigned long)pd_idx);
        goto fail_gpp;
    } else {
        // PDE is present and points to a 4KB Page Table
        pt_phys_addr_val = (uintptr_t)(pde & PAGING_ADDR_MASK);
    }

    // 4. Map the target Page Table (either existing or newly created) to TEMP_MAP_ADDR_PT
    pt_virt = paging_temp_map(pt_phys_addr_val, PTE_KERNEL_DATA_FLAGS);
    if (!pt_virt) {
        terminal_printf("[get_pte_ptr] Error: Failed to temp map target PT frame %#lx.\n", (unsigned long)pt_phys_addr_val);
        goto fail_gpp;
    }

    // 5. Calculate pointer to the specific PTE within the temporary PT mapping
    pte_ptr = &pt_virt[pt_idx];
    // terminal_printf("[get_pte_ptr] Success: Returning PTE pointer %p (inside temp map %p for PT Phys %#lx)\n",
    //                 pte_ptr, pt_virt, pt_phys_addr_val);

    // Cleanup the PD mapping, leave PT mapped for caller
    paging_temp_unmap(proc_pd_virt);
    return pte_ptr; // Success! Caller must unmap TEMP_MAP_ADDR_PT

fail_gpp:
    // Cleanup allocated frame if allocation failed mid-way
    if (allocated_pt_frame && pt_phys_addr_val != 0) {
         put_frame(pt_phys_addr_val);
    }
    // Cleanup temporary PD mapping
    if (proc_pd_virt) {
        paging_temp_unmap(proc_pd_virt);
    }
    // Ensure temporary PT mapping is also cleaned up on failure
    if (pt_virt) { // Check if PT was mapped before failure
        paging_temp_unmap(pt_virt);
    }
    return NULL; // Indicate failure
}

/**
 * Handles a page fault for a given VMA. Includes COW using reference counting.
 */
 int handle_vma_fault(mm_struct_t *mm, vma_struct_t *vma, uintptr_t fault_address, uint32_t error_code) {
    uintptr_t page_addr = PAGE_ALIGN_DOWN(fault_address);
    int ret = -FS_ERR_INTERNAL; // Default to internal error
    uint32_t* pte_ptr = NULL;   // Pointer to PTE within TEMP_MAP_ADDR_PT mapping
    bool is_write = (error_code & PAGE_RW) != 0;
    bool present = (error_code & PAGE_PRESENT) != 0;
    uintptr_t phys_page = 0;    // For allocating new frames
    void* temp_addr_for_copy = NULL; // For mapping frames for memcpy/memset

    // --- Permission Checks (Simplified, assumes VMA lookup already done) ---
    if (is_write && !(vma->vm_flags & VM_WRITE)) return -FS_ERR_PERMISSION_DENIED;
    if (!is_write && !(vma->vm_flags & VM_READ)) return -FS_ERR_PERMISSION_DENIED; // Read or Execute needs VM_READ


    // --- Handle Present Page Fault (Protection Violation -> COW) ---
    if (present) {
        // terminal_printf("[PF Handle] Present Fault: V=%p, Write=%d\n", (void*)fault_address, is_write);
        if (is_write && (vma->vm_flags & VM_WRITE) && !(vma->vm_flags & VM_SHARED)) {
            // --- COW Logic ---
            pte_ptr = get_pte_ptr(mm, page_addr, false); // PT must exist if page is present
            if (!pte_ptr) {
                terminal_printf("[PF COW] Error: Failed get PTE for present page V=%p\n", (void*)page_addr);
                return -FS_ERR_INTERNAL;
            }

            uint32_t pte = *pte_ptr; // Read PTE value via temporary mapping
            if (!(pte & PAGE_PRESENT)) {
                terminal_printf("[PF COW] Error: Page present but PTE not!? PTE=%#lx V=%p\n", (unsigned long)pte, (void*)page_addr);
                ret = -FS_ERR_INTERNAL; goto cleanup_cow;
            }
            if (pte & PAGE_RW) {
                terminal_printf("[PF COW] Warning: Write fault on page already marked RW? PTE=%#lx V=%p\n", (unsigned long)pte, (void*)page_addr);
                ret = 0; goto cleanup_cow; // Already writable, maybe TLB issue? Return success.
            }

            // Page is Present but Read-Only, proceed with COW
            uintptr_t src_phys_page = pte & PAGING_ADDR_MASK;
            int ref_count = get_frame_refcount(src_phys_page);
            if (ref_count < 0) { terminal_printf("[PF COW] Error: Failed get refcount P=%#lx\n", (unsigned long)src_phys_page); ret = -FS_ERR_INTERNAL; goto cleanup_cow; }

            if (ref_count == 1) { // Frame Not Shared
                // terminal_printf("[PF COW] Frame P=%#lx not shared (ref=%d), making writable for V=%p\n", src_phys_page, ref_count, (void*)page_addr);
                *pte_ptr = (pte | PAGE_RW); // Set RW bit via temporary mapping
                ret = 0; // Success
            } else { // Frame Shared: Perform Copy
                // terminal_printf("[PF COW] Frame P=%#lx shared (ref=%d), copying for V=%p\n", src_phys_page, ref_count, (void*)page_addr);
                phys_page = frame_alloc(); // Allocate destination frame
                if (!phys_page) { ret = -FS_ERR_OUT_OF_MEMORY; goto cleanup_cow; }

                // Map source and destination frames temporarily for copy
                void* temp_src = paging_temp_map(src_phys_page, PTE_KERNEL_READONLY_FLAGS);
                void* temp_dst = paging_temp_map(phys_page, PTE_KERNEL_DATA_FLAGS);

                if (!temp_src || !temp_dst) {
                    terminal_printf("[PF COW] Error: Failed to temp map frames for copy (src=%p, dst=%p)\n", temp_src, temp_dst);
                    if (temp_src) paging_temp_unmap(temp_src);
                    if (temp_dst) paging_temp_unmap(temp_dst);
                    put_frame(phys_page); // Free the allocated frame
                    ret = -FS_ERR_INTERNAL; goto cleanup_cow;
                }

                memcpy(temp_dst, temp_src, PAGE_SIZE); // Copy data

                paging_temp_unmap(temp_dst); // Unmap temporary pages
                paging_temp_unmap(temp_src);

                // Update the PTE to point to the new frame with RW permission
                *pte_ptr = (phys_page & PAGING_ADDR_MASK) | (pte & PAGING_FLAG_MASK) | PAGE_RW | PAGE_PRESENT;
                put_frame(src_phys_page); // Decrement ref count of original frame
                ret = 0; // Success
            }
        cleanup_cow:
            if (pte_ptr) { // Unmap the PT that get_pte_ptr mapped
                paging_temp_unmap((void*)TEMP_MAP_ADDR_PT); // Use the specific PT temp address
            }
            if (ret == 0) { paging_invalidate_page((void*)page_addr); } // Invalidate TLB on success
            return ret;
        } else { // Present fault, but not a COW situation
            terminal_printf("[PF Handle] Error: Unexpected present fault. ErrCode=%#x VMAFlags=%#x Addr=%p\n",
                            (unsigned int)error_code, (unsigned int)vma->vm_flags, (void*)fault_address);
            return -FS_ERR_PERMISSION_DENIED;
        }
    } // End if(present)

    // --- Handle Non-Present Page Fault (Allocate and Map) ---
    // terminal_printf("[PF Handle] NP Fault: V=%p\n", (void*)fault_address);
    phys_page = frame_alloc(); // 1. Allocate frame
    if (!phys_page) { return -FS_ERR_OUT_OF_MEMORY; }
    // terminal_printf("  Allocated phys frame: %#lx\n", phys_page);

    // 2. Map frame temporarily into kernel to populate using TEMP_MAP_ADDR_PF
    temp_addr_for_copy = paging_temp_map(phys_page, PTE_KERNEL_DATA_FLAGS);
    if (!temp_addr_for_copy) {
        put_frame(phys_page); return -FS_ERR_IO;
    }

    // 3. Populate frame
    if (vma->vm_flags & VM_FILEBACKED) {
        terminal_printf("  Populating from file (TODO) V=%p P=%#lx\n", (void*)page_addr, (unsigned long)phys_page);
        // TODO: Implement file read logic here
        // Need vma->vm_file, vma->vm_offset, page_addr - vma->vm_start
        memset(temp_addr_for_copy, 0, PAGE_SIZE); // Placeholder
    } else { // Anonymous VMA
        // terminal_printf("  Zeroing anonymous page V=%p P=%#lx\n", (void*)page_addr, phys_page);
        memset(temp_addr_for_copy, 0, PAGE_SIZE);
    }

    // 4. Unmap temporary kernel mapping
    paging_temp_unmap(temp_addr_for_copy);

    // 5. Map frame into process space via PTE
    pte_ptr = get_pte_ptr(mm, page_addr, true); // Allocate PT if needed
    if (!pte_ptr) {
        put_frame(phys_page); return -FS_ERR_IO; // Failed to get PTE access
    }

    // Determine final flags (Apply COW by mapping RO initially if needed)
    uint32_t map_flags = vma->page_prot; // Start with VMA's base page permissions
    if ((vma->vm_flags & VM_WRITE) && !(vma->vm_flags & VM_SHARED)) {
        map_flags &= ~PAGE_RW; // Clear RW for Copy-on-Write
    }

    // Write the PTE using the pointer from get_pte_ptr (which is inside TEMP_MAP_ADDR_PT)
    *pte_ptr = (phys_page & PAGING_ADDR_MASK) | map_flags | PAGE_PRESENT;
    // terminal_printf("  Set PTE at %p = %#lx\n", pte_ptr, *pte_ptr);


    // 6. Unmap the temporary PT mapping created by get_pte_ptr
    paging_temp_unmap((void*)TEMP_MAP_ADDR_PT);

    // 7. Invalidate TLB for the specific user page
    paging_invalidate_page((void*)page_addr);

    // terminal_printf("  NP Fault handled successfully for V=%p -> P=%#lx\n", (void*)page_addr, phys_page);
    return 0; // Success
}
// --- END UPDATED handle_vma_fault ---

// --- VMA Range Removal ---

/**
 * remove_vma_range_locked
 */
static int remove_vma_range_locked(mm_struct_t *mm, uintptr_t start, size_t length) {
    uintptr_t end = start + length;
    if (start >= end) return -FS_ERR_INVALID_PARAM;

    // terminal_printf("[MM] remove_vma_range_locked: Request [0x%x - 0x%x)\n", start, end);
    int result = 0;
    struct rb_node *node = NULL;
    struct rb_node *next_node = NULL;
    node = rb_tree_first(&mm->vma_tree);

    while (node) {
        vma_struct_t *vma = rb_entry(node, vma_struct_t, rb_node);
        next_node = rb_node_next(node);
        uintptr_t overlap_start = (vma->vm_start > start) ? vma->vm_start : start;
        uintptr_t overlap_end = (vma->vm_end < end) ? vma->vm_end : end;

        if (vma->vm_start >= end) break;

        if (overlap_start < overlap_end) {
            // terminal_printf("  Overlap found: VMA [0x%x-0x%x) overlaps request [0x%x-0x%x) => unmap [0x%x-0x%x)\n", vma->vm_start, vma->vm_end, start, end, overlap_start, overlap_end);

            result = paging_unmap_range(mm->pgd_phys, overlap_start, overlap_end - overlap_start);
            if (result != 0) { return result; } // Stop on error

            bool remove_original = false;
            vma_struct_t* created_second_part = NULL;

            if (vma->vm_start >= start && vma->vm_end <= end) { // Case 1: Fully contained
                // terminal_write("    VMA fully contained. Removing.\n");
                remove_original = true;
            } else if (vma->vm_start < start && vma->vm_end > end) { // Case 2: Split
                // terminal_write("    VMA split needed.\n");
                created_second_part = alloc_vma_struct();
                if (!created_second_part) return -FS_ERR_OUT_OF_MEMORY;
                memcpy(created_second_part, vma, sizeof(vma_struct_t));
                created_second_part->vm_start = end;
                if (created_second_part->vm_flags & VM_FILEBACKED) { created_second_part->vm_offset += (end - vma->vm_start); }
                vma->vm_end = start;
                // terminal_printf("    Original shrunk to [0x%x-0x%x), New VMA [0x%x-0x%x)\n", vma->vm_start, vma->vm_end, created_second_part->vm_start, created_second_part->vm_end);
            } else if (vma->vm_start < start) { // Case 3: Overlap at end
                // terminal_write("    Shrinking VMA end.\n");
                vma->vm_end = start;
            } else { // Case 4: Overlap at beginning
                // terminal_write("    Shrinking VMA start.\n");
                 if(vma->vm_flags & VM_FILEBACKED) { vma->vm_offset += (end - vma->vm_start); }
                vma->vm_start = end;
                terminal_write("    Warning: Modifying vm_start might affect RB Tree validity without re-insertion.\n");
            }

            if (remove_original) {
                rb_tree_remove(&mm->vma_tree, node);
                mm->map_count--;
                free_vma_resources(vma);
            }
            if (created_second_part) {
                 struct rb_node **link = &mm->vma_tree.root;
                 struct rb_node *parent = NULL;
                 bool insert_left = true;
                 while (*link) {
                     parent = *link;
                     vma_struct_t* current = rb_entry(parent, vma_struct_t, rb_node);
                     if (created_second_part->vm_start < current->vm_start) { link = &parent->left; insert_left = true; }
                     else { link = &parent->right; insert_left = false; }
                 }
                rb_tree_insert_at(&mm->vma_tree, parent, &created_second_part->rb_node, insert_left);
                mm->map_count++;
            }
        } // End if overlap
        node = next_node;
    } // End while loop
    return result;
}


/**
 * Public wrapper for remove_vma_range. Acquires lock.
 */
int remove_vma_range(mm_struct_t *mm, uintptr_t start, size_t length) {
    if (!mm || length == 0) return -FS_ERR_INVALID_PARAM;
    uintptr_t irq_flags = spinlock_acquire_irqsave(&mm->lock);
    int result = remove_vma_range_locked(mm, start, length);
    spinlock_release_irqrestore(&mm->lock, irq_flags);
    return result;
}