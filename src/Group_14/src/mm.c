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
         terminal_printf("[MM] Error: VMA overlap detected for range [0x%x-0x%x)\n",
                        new_vma->vm_start, new_vma->vm_end);
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
    if (!mm || start >= end || (start % PAGE_SIZE) != 0 || (end % PAGE_SIZE) != 0) {
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
    if (!mm || !mm->pgd_phys) return NULL;
    uint32_t pd_idx = PDE_INDEX(vaddr);
    uint32_t pt_idx = PTE_INDEX(vaddr);
    uint32_t* proc_pd_virt = NULL;
    uint32_t* pt_virt = NULL;
    uint32_t* pte_ptr_in_temp = NULL; // Pointer to PTE within the temporary mapping
    int ret_status = -1; // Track success/failure

    // Map Process PD using KERNEL's page directory
    // Use g_kernel_page_directory_phys (declared extern in paging.h)
    if (paging_map_single((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PD, (uint32_t)mm->pgd_phys, PTE_KERNEL_DATA) != 0) {
        terminal_write("[MM get_pte_ptr] Failed to temp map process PD.\n");
        return NULL;
    }
    proc_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;
    uint32_t pde = proc_pd_virt[pd_idx];

    // Allocate PT if needed
    uintptr_t pt_phys_addr_val = 0; // Store physical address value
    if (!(pde & PAGE_PRESENT)) {
        if (!allocate_pt) { goto cleanup_pte_gpp; } // PT doesn't exist, don't allocate -> fail

        // *** Use frame_alloc() instead of allocate_page_table_phys ***
        pt_phys_addr_val = frame_alloc();
        if (pt_phys_addr_val == 0) {
            terminal_write("[MM get_pte_ptr] Failed to allocate frame for Page Table.\n");
            goto cleanup_pte_gpp;
        }

        // Temporarily map the new PT frame to zero it
        if (paging_map_single((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, pt_phys_addr_val, PTE_KERNEL_DATA) != 0) {
             terminal_printf("[MM get_pte_ptr] Failed to temp map new PT frame 0x%x for zeroing.\n", pt_phys_addr_val);
             put_frame(pt_phys_addr_val); // Free the allocated frame
             goto cleanup_pte_gpp;
        }
        memset((void*)TEMP_MAP_ADDR_PT, 0, PAGE_SIZE);
        // Unmap after zeroing
        paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, PAGE_SIZE);
        paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
        // *** End zeroing ***

        uint32_t pde_flags = PAGE_PRESENT | PAGE_RW | PAGE_USER; // Assume USER/RW needed for PT
        proc_pd_virt[pd_idx] = (pt_phys_addr_val & ~0xFFF) | pde_flags;
        // Invalidate TLB for the address range affected by the PDE update
        paging_invalidate_page((void*)PAGE_ALIGN_DOWN(vaddr));

    } else {
        pt_phys_addr_val = (uintptr_t)(pde & ~0xFFF); // Get phys addr from existing PDE
    }

    // Map the potentially existing or newly created PT using its physical address
    if (paging_map_single((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, pt_phys_addr_val, PTE_KERNEL_DATA) != 0) {
        terminal_printf("[MM get_pte_ptr] Failed to temp map existing PT frame 0x%x.\n", pt_phys_addr_val);
        // If we allocated the PT earlier in this call, free it
        if (!(pde & PAGE_PRESENT)) { // Check if PT was newly allocated
             put_frame(pt_phys_addr_val);
             proc_pd_virt[pd_idx] = 0; // Clear the PDE we just set
             paging_invalidate_page((void*)PAGE_ALIGN_DOWN(vaddr));
        }
        goto cleanup_pte_gpp;
    }
    pt_virt = (uint32_t*)TEMP_MAP_ADDR_PT;
    pte_ptr_in_temp = &pt_virt[pt_idx]; // Calculate pointer to PTE within the temp mapping
    ret_status = 0; // Success

cleanup_pte_gpp:
    // Unmap Process PD (we are done with it now)
    paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PD, PAGE_SIZE);
    paging_invalidate_page((void*)TEMP_MAP_ADDR_PD);

    // CRITICAL: The caller (handle_vma_fault) now needs to unmap TEMP_MAP_ADDR_PT
    // after using the returned pte_ptr_in_temp.
    // If ret_status is not 0, it means an error occurred, and TEMP_MAP_ADDR_PT
    // might not be mapped or might point to invalid memory, so return NULL.
    return (ret_status == 0) ? pte_ptr_in_temp : NULL;
}

/**
 * Handles a page fault for a given VMA. Includes COW using reference counting.
 */
int handle_vma_fault(mm_struct_t *mm, vma_struct_t *vma, uintptr_t fault_address, uint32_t error_code) {
    uintptr_t page_addr = PAGE_ALIGN_DOWN(fault_address);
    int ret = 0;
    uint32_t* pte_ptr = NULL; // Pointer to PTE within temporary mapping

    // --- Check Permissions ---
    bool is_write = (error_code & PAGE_RW) != 0;
    bool is_user = (error_code & PAGE_USER) != 0;
    bool present = (error_code & PAGE_PRESENT) != 0;

    if (!is_user) {
        terminal_printf("[PF] Fault: Kernel access in user VMA? Addr 0x%x\n", fault_address);
        return -1; // Indicate kernel error
    }
    if (is_write && !(vma->vm_flags & VM_WRITE)) {
        terminal_printf("[PF] Fault: Write permission denied VMA [0x%x-0x%x) Addr 0x%x\n",
                       vma->vm_start, vma->vm_end, fault_address);
        return -FS_ERR_PERMISSION_DENIED;
    }
    if (!is_write && !(vma->vm_flags & VM_READ)) {
        terminal_printf("[PF] Fault: Read permission denied VMA [0x%x-0x%x) Addr 0x%x\n",
                       vma->vm_start, vma->vm_end, fault_address);
        return -FS_ERR_PERMISSION_DENIED;
    }

    // --- Handle Present Page Fault (Protection Violation -> COW) ---
    if (present) {
        if (is_write && (vma->vm_flags & VM_WRITE) && !(vma->vm_flags & VM_SHARED)) {
            // terminal_printf("[PF] COW Triggered VMA [0x%x-0x%x) Addr 0x%x\n", vma->vm_start, vma->vm_end, fault_address);

            pte_ptr = get_pte_ptr(mm, page_addr, false); // Get PTE pointer (PT must exist)
            if (!pte_ptr) {
                terminal_write("  [PF COW Error] Failed to get PTE pointer for present page.\n");
                return -10; // Internal error
            }
            uint32_t pte = *pte_ptr;

            if (!(pte & PAGE_PRESENT)) { // Consistency check
                terminal_write("  [PF COW Error] Page marked present by CPU, but PTE not present!\n"); ret = -11; goto unmap_temp_pt_cow;
            }
            if (pte & PAGE_RW) { // Already writable?
                terminal_write("  [PF COW Warning] Write fault on already writable page?!\n"); ret = 0; goto unmap_temp_pt_cow;
            }

            uintptr_t src_phys_page = pte & ~0xFFF;
            int ref_count = get_frame_refcount(src_phys_page);
            if (ref_count < 0) { ret = -12; goto unmap_temp_pt_cow; }

            if (ref_count == 1) { // Frame Not Shared
                // terminal_write("  COW: Frame not shared, making writable.\n");
                *pte_ptr = (pte | PAGE_RW);
                paging_invalidate_page((void*)page_addr);
                ret = 0;
            } else { // Frame Shared: Perform Copy
                // terminal_printf("  COW: Frame shared (refcnt=%d), performing copy.\n", ref_count);
                uintptr_t dst_phys_page = 0;
                void* temp_src_map = (void*)TEMP_MAP_ADDR_COW_SRC;
                void* temp_dst_map = (void*)TEMP_MAP_ADDR_COW_DST;

                dst_phys_page = frame_alloc();
                if (!dst_phys_page) { ret = -FS_ERR_OUT_OF_MEMORY; goto cleanup_cow_alloc; }

                ret = -FS_ERR_IO; // Default error
                // Use g_kernel_page_directory_phys for temporary kernel mappings
                if (paging_map_single((uint32_t*)g_kernel_page_directory_phys, (uint32_t)temp_src_map, src_phys_page, PTE_KERNEL_READONLY) == 0) {
                    if (paging_map_single((uint32_t*)g_kernel_page_directory_phys, (uint32_t)temp_dst_map, dst_phys_page, PTE_KERNEL_DATA) == 0) {
                        memcpy(temp_dst_map, temp_src_map, PAGE_SIZE);
                        paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, (uint32_t)temp_dst_map, PAGE_SIZE);
                        paging_invalidate_page(temp_dst_map);
                        ret = 0; // Copy success
                    } else { terminal_write("  [PF COW Error] Failed map dst page.\n"); }
                    paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, (uint32_t)temp_src_map, PAGE_SIZE);
                    paging_invalidate_page(temp_src_map);
                } else { terminal_write("  [PF COW Error] Failed map src page.\n"); }

            cleanup_cow_alloc:
                if (ret != 0 && dst_phys_page != 0) { put_frame(dst_phys_page); }
                if (ret == 0) {
                    *pte_ptr = (dst_phys_page & ~0xFFF) | (vma->page_prot | PAGE_RW);
                    put_frame(src_phys_page);
                    paging_invalidate_page((void*)page_addr);
                }
            } // End Frame Shared case
        unmap_temp_pt_cow:
            if (pte_ptr) { // Unmap the PT that get_pte_ptr mapped
                paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, PAGE_SIZE);
                paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
            }
            return ret;
        } else { // Present fault, but not COW
             terminal_printf("[PF] Fault: Unexpected present fault. ErrorCode=0x%x, VMAFlags=0x%x, Addr=0x%x\n", error_code, vma->vm_flags, fault_address);
             return -FS_ERR_PERMISSION_DENIED;
        }
    } // End if(present)

    // --- Handle Non-Present Page Fault (Allocate and Map) ---
    // terminal_printf("[PF] NP Fault VMA [0x%x-0x%x) Addr 0x%x\n", vma->vm_start, vma->vm_end, fault_address);
    uintptr_t phys_page = 0;
    void* temp_vaddr_kernel = (void*)TEMP_MAP_ADDR_PF;

    phys_page = frame_alloc(); // 1. Allocate frame
    if (!phys_page) { return -FS_ERR_OUT_OF_MEMORY; }
    // terminal_printf("  Allocated phys frame: 0x%x\n", phys_page);

    // 2. Map frame temporarily into kernel to populate
    if (paging_map_single((uint32_t*)g_kernel_page_directory_phys, (uint32_t)temp_vaddr_kernel, phys_page, PTE_KERNEL_DATA) != 0) {
        put_frame(phys_page); return -FS_ERR_IO;
    }

    // 3. Populate frame
    if (vma->vm_flags & VM_FILEBACKED) {
        terminal_write("  Populating from file (TODO)\n"); // TODO: File read logic
        if (!vma->vm_file) { ret = -FS_ERR_INVALID_PARAM; goto cleanup_np_fault; }
        memset(temp_vaddr_kernel, 0, PAGE_SIZE); // Placeholder zeroing
        // Add file read logic here...
    } else { // Anonymous VMA
        // terminal_write("  Zeroing anonymous page.\n");
        memset(temp_vaddr_kernel, 0, PAGE_SIZE);
    }

cleanup_np_fault:
    // Unmap temporary kernel mapping
    paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, (uint32_t)temp_vaddr_kernel, PAGE_SIZE);
    paging_invalidate_page(temp_vaddr_kernel);
    if (ret != 0) { put_frame(phys_page); return ret; }

    // 4. Map frame into process space
    uint32_t map_flags = vma->page_prot;
    if ((vma->vm_flags & VM_WRITE) && !(vma->vm_flags & VM_SHARED)) { map_flags &= ~PAGE_RW; } // COW
    pte_ptr = get_pte_ptr(mm, page_addr, true); // Allocate PT if needed
    if (!pte_ptr) { put_frame(phys_page); return -FS_ERR_IO; }
    *pte_ptr = (phys_page & ~0xFFF) | map_flags | PAGE_PRESENT;

    // Unmap the PT mapping created by get_pte_ptr
    paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, PAGE_SIZE);
    paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);

    // 5. Invalidate TLB for the specific user page
    paging_invalidate_page((void*)page_addr);

    // terminal_write("  NP Fault handled successfully.\n");
    return 0; // Success
}

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