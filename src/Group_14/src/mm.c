#include "mm.h"
#include "kmalloc.h"    // For allocating mm_struct and vma_struct
#include "terminal.h"   // For logging
#include "buddy.h"      // Underlying physical allocator (called by frame allocator)
#include "frame.h"      // Frame allocator header
#include "paging.h"     // For mapping pages, flags, TEMP addresses
#include "vfs.h"        // For vfs_read, vfs_lseek, file_t
#include "fs_errno.h"   // For VFS error codes
#include "rbtree.h"     // RB Tree header
#include "process.h"    // For pcb_t, get_current_process
#include <string.h>     // For memset, memcpy
#include "paging.h"

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
            terminal_printf("[MM Destroy] Unmapping VMA [0x%x-0x%x)\n",
                            vma_node->vm_start, vma_node->vm_end);
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
        terminal_write("[MM] Destroying VMAs...\n");
        // Perform post-order traversal of RB Tree to unmap and free nodes
        rbtree_postorder_traverse(root, destroy_vma_node_callback, mm); // Pass mm for context
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
static uint32_t* get_pte_ptr(mm_struct_t *mm, uintptr_t vaddr, bool allocate_pt) {
     if (!mm || !mm->pgd_phys) return NULL;
     uint32_t pd_idx = PDE_INDEX(vaddr); uint32_t pt_idx = PTE_INDEX(vaddr);
     uint32_t* proc_pd_virt = NULL; uint32_t* pt_virt = NULL; uint32_t* pte_ptr = NULL;

     // Map Process PD
     if (paging_map_single(kernel_page_directory, TEMP_MAP_ADDR_PD, (uint32_t)mm->pgd_phys, PTE_KERNEL_DATA) != 0) { return NULL; }
     proc_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;
     uint32_t pde = proc_pd_virt[pd_idx];

     // Allocate PT if needed
     uint32_t* pt_phys = NULL; // Store physical address
     if (!(pde & PAGE_PRESENT)) {
         if (!allocate_pt) { goto cleanup_pd_map_pte_gpp; }
         pt_phys = allocate_page_table_phys(); // Gets phys addr
         if (!pt_phys) { goto cleanup_pd_map_pte_gpp; }
         uint32_t pde_flags = PAGE_PRESENT | PAGE_RW | PAGE_USER; // Assume USER/RW needed for PT
         proc_pd_virt[pd_idx] = (uint32_t)pt_phys | pde_flags;
         // Invalidate TLB entry for the PDE itself? Or rely on later page invalidation?
         // For safety, invalidate something related to the PD change
         // paging_invalidate_page((void*)PAGE_ALIGN_DOWN(vaddr)); // Invalidate any cached entry for the vaddr range
     } else {
         pt_phys = (uint32_t*)(pde & ~0xFFF); // Get phys addr from existing PDE
     }

     // Map PT
     if (paging_map_single(kernel_page_directory, TEMP_MAP_ADDR_PT, (uint32_t)pt_phys, PTE_KERNEL_DATA) != 0) { goto cleanup_pd_map_pte_gpp; }
     pt_virt = (uint32_t*)TEMP_MAP_ADDR_PT;
     pte_ptr = &pt_virt[pt_idx]; // Calculate pointer to PTE within the mapped PT

 cleanup_pd_map_pte_gpp:
     // Unmap Process PD (we are done with it)
     paging_unmap_range(kernel_page_directory, TEMP_MAP_ADDR_PD, PAGE_SIZE);
     paging_invalidate_page((void*)TEMP_MAP_ADDR_PD);

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
    bool is_write = (error_code & PAGE_RW) != 0;
    bool is_user = (error_code & PAGE_USER) != 0;
    bool present = (error_code & PAGE_PRESENT) != 0;

    if (!is_user) {
        terminal_printf("[PF] Fault: Kernel access in user VMA? Addr 0x%x\n", fault_address);
        // Kernel should generally not fault on user pages unless accessing via temp map.
        // Handle appropriately - likely a kernel bug.
        return -1; // Indicate error
    }

    if (is_write && !(vma->vm_flags & VM_WRITE)) {
        terminal_printf("[PF] Fault: Write permission denied VMA [0x%x-0x%x) Addr 0x%x\n",
                       vma->vm_start, vma->vm_end, fault_address);
        return -FS_ERR_PERMISSION_DENIED;
    }
    if (!is_write && !(vma->vm_flags & VM_READ)) {
         // This check might be less critical as read faults are less common if mapping exists
         terminal_printf("[PF] Fault: Read permission denied VMA [0x%x-0x%x) Addr 0x%x\n",
                        vma->vm_start, vma->vm_end, fault_address);
        return -FS_ERR_PERMISSION_DENIED;
    }


    // --- Handle Present Page Fault (Protection Violation -> COW) ---
    if (present) {
        // If page is present, the fault must be due to protection violation (e.g., write to read-only page).
        // We only handle this if it's a private, writable VMA (COW case).
        if (is_write && (vma->vm_flags & VM_WRITE) && !(vma->vm_flags & VM_SHARED)) {
            terminal_printf("[PF] COW Triggered VMA [0x%x-0x%x) Addr 0x%x\n", vma->vm_start, vma->vm_end, fault_address);

            // 1. Get pointer to PTE (PT must exist if page is present)
            uint32_t* pte_ptr = get_pte_ptr(mm, page_addr, false); // Don't allocate PT
            if (!pte_ptr) { return -10; } // Should not happen if page is present

            uint32_t pte = *pte_ptr; // Read PTE value

            // Check if PTE is *actually* present and read-only (consistency check)
             if (!(pte & PAGE_PRESENT)) {
                terminal_write("  [PF COW Error] Page marked present by CPU, but PTE not present!\n");
                ret = -11; goto unmap_cow_pt_hvf2;
            }
            if (pte & PAGE_RW) {
                terminal_write("  [PF COW Warning] Write fault on already writable page?!\n");
                ret = 0; goto unmap_cow_pt_hvf2; // Already writable, maybe TLB issue?
            }

            uintptr_t src_phys_page = pte & ~0xFFF;

            // 2. Check reference count of the physical frame
            int ref_count = get_frame_refcount(src_phys_page);
            if (ref_count < 0) {
                terminal_write("  [PF COW Error] Invalid frame address for ref count check.\n");
                ret = -12; goto unmap_cow_pt_hvf2;
            }

            if (ref_count == 1) {
                // --- Frame Not Shared: Make PTE Writable ---
                terminal_write("  COW: Frame not shared, making writable.\n");
                *pte_ptr = (pte | PAGE_RW); // Add RW flag
                paging_invalidate_page((void*)page_addr); // Invalidate TLB
                ret = 0;
            } else {
                // --- Frame Shared: Perform Copy ---
                 terminal_printf("  COW: Frame shared (refcnt=%d), performing copy.\n", ref_count);
                uintptr_t dst_phys_page = 0;
                void* temp_src_map = (void*)TEMP_MAP_ADDR_COW_SRC;
                void* temp_dst_map = (void*)TEMP_MAP_ADDR_COW_DST;

                // a. Allocate destination frame (refcnt=1)
                dst_phys_page = frame_alloc();
                if (!dst_phys_page) {
                    terminal_write("  [PF COW Error] Out of memory for copy.\n");
                    ret = -FS_ERR_OUT_OF_MEMORY; goto unmap_cow_pt_hvf2;
                }

                // b/c/d. Map, Copy, Unmap
                ret = -FS_ERR_IO; // Default error for mapping/copying
                if (paging_map_single(kernel_page_directory, (uint32_t)temp_src_map, src_phys_page, PTE_KERNEL_READONLY) == 0) {
                    if (paging_map_single(kernel_page_directory, (uint32_t)temp_dst_map, dst_phys_page, PTE_KERNEL_DATA) == 0) {
                        memcpy(temp_dst_map, temp_src_map, PAGE_SIZE);
                        paging_unmap_range(kernel_page_directory, (uint32_t)temp_dst_map, PAGE_SIZE);
                        paging_invalidate_page(temp_dst_map);
                        ret = 0; // Copy successful
                    } else { terminal_write("  [PF COW Error] Failed map dst page.\n"); }
                    paging_unmap_range(kernel_page_directory, (uint32_t)temp_src_map, PAGE_SIZE);
                    paging_invalidate_page(temp_src_map);
                } else { terminal_write("  [PF COW Error] Failed map src page.\n"); }

                if (ret != 0) goto cleanup_cow_alloc_hvf2; // Cleanup if mapping/copy failed

                // e. Update PTE to new page (use VMA prot flags + RW)
                *pte_ptr = (dst_phys_page & ~0xFFF) | (vma->page_prot | PAGE_RW);

                // f. Decrement ref count of original page
                put_frame(src_phys_page);
                paging_invalidate_page((void*)page_addr); // Invalidate TLB for the changed PTE
                ret = 0; // Success

cleanup_cow_alloc_hvf2:
                if (ret != 0 && dst_phys_page != 0) {
                    put_frame(dst_phys_page); // Free destination frame if COW failed after alloc
                }
            } // End if(ref_count > 1)

unmap_cow_pt_hvf2:
            // Unmap the temporarily mapped Page Table
            paging_unmap_range(kernel_page_directory, TEMP_MAP_ADDR_PT, PAGE_SIZE);
            paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
            return ret; // Return result of COW operation
        } else {
            // Present fault, but not a COW situation (e.g., write to RO shared VMA, or read fault on present page?)
             terminal_printf("[PF] Fault: Unexpected present fault. ErrorCode=0x%x, VMAFlags=0x%x, Addr=0x%x\n",
                            error_code, vma->vm_flags, fault_address);
             return -FS_ERR_PERMISSION_DENIED; // Or other error
        }
    } // End if(present)


    // --- Handle Non-Present Page Fault (Allocate and Map) ---
    terminal_printf("[PF] NP Fault VMA [0x%x-0x%x) Addr 0x%x\n", vma->vm_start, vma->vm_end, fault_address);
    uintptr_t phys_page = 0;
    void* temp_vaddr_kernel = (void*)TEMP_MAP_ADDR_PF;

    // 1. Allocate frame (refcnt=1)
    phys_page = frame_alloc();
    if (!phys_page) {
        terminal_write("  [PF NP Error] Out of physical memory.\n");
        return -FS_ERR_OUT_OF_MEMORY;
    }
    terminal_printf("  Allocated phys frame: 0x%x\n", phys_page);

    // 2. Map frame temporarily into kernel to populate
    if (paging_map_single(kernel_page_directory, (uint32_t)temp_vaddr_kernel, phys_page, PTE_KERNEL_DATA) != 0) {
        terminal_write("  [PF NP Error] Failed to map frame into kernel.\n");
        put_frame(phys_page); return -FS_ERR_IO;
    }

    // 3. Populate frame (Zero or File Read)
    if (vma->vm_flags & VM_FILEBACKED) {
        terminal_write("  Populating from file (TODO: Implement file read)\n");
        // TODO: Implement file reading logic similar to previous version
         if (!vma->vm_file) { ret = -FS_ERR_INVALID_PARAM; goto cleanup_pf_kernel_map_hvf2; }
         off_t file_offset = (off_t)vma->vm_offset + (off_t)(page_addr - vma->vm_start);
         // Seek and Read from vma->vm_file into temp_vaddr_kernel
         // Need VFS functions integrated here. For now, just zero it.
         memset(temp_vaddr_kernel, 0, PAGE_SIZE); // Placeholder
         // Example VFS calls (replace placeholder above):
         /*
         if (vfs_lseek(vma->vm_file, file_offset, SEEK_SET) != file_offset) { ret = -FS_ERR_IO; goto cleanup_pf_kernel_map_hvf2; }
         int bytes_read = vfs_read(vma->vm_file, temp_vaddr_kernel, PAGE_SIZE);
         if (bytes_read < 0) { ret = bytes_read; goto cleanup_pf_kernel_map_hvf2; }
         if (bytes_read < PAGE_SIZE) { memset((uint8_t*)temp_vaddr_kernel + bytes_read, 0, PAGE_SIZE - bytes_read); }
         */
    } else { // Anonymous VMA
        terminal_write("  Zeroing anonymous page.\n");
        memset(temp_vaddr_kernel, 0, PAGE_SIZE);
    }

cleanup_pf_kernel_map_hvf2:
    paging_unmap_range(kernel_page_directory, (uint32_t)temp_vaddr_kernel, PAGE_SIZE);
    paging_invalidate_page(temp_vaddr_kernel);
    if (ret != 0) {
        terminal_printf("  [PF NP Error] Failed to populate frame (code %d).\n", ret);
        put_frame(phys_page); return ret;
    }

    // 4. Map frame into process space using VMA's protection flags
    //    For COW, map initially read-only even if VMA allows write.
    uint32_t map_flags = vma->page_prot;
    if ((vma->vm_flags & VM_WRITE) && !(vma->vm_flags & VM_SHARED)) {
        map_flags &= ~PAGE_RW; // COW
    }
    uint32_t* pte_ptr = get_pte_ptr(mm, page_addr, true); // Allocate PT if needed
    if (!pte_ptr) {
        terminal_write("  [PF NP Error] Failed to get PTE pointer.\n");
        put_frame(phys_page); return -FS_ERR_IO;
    }
    *pte_ptr = (phys_page & ~0xFFF) | map_flags | PAGE_PRESENT; // Set PTE
    // Unmap the temporarily mapped Page Table from get_pte_ptr
    paging_unmap_range(kernel_page_directory, TEMP_MAP_ADDR_PT, PAGE_SIZE);
    paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);

    // 5. Invalidate TLB for the specific page
    paging_invalidate_page((void*)page_addr);

    // Frame ref count is already 1 from frame_alloc

    terminal_write("  NP Fault handled successfully.\n");
    return 0; // Success
}

// --- VMA Range Removal ---

/**
 * remove_vma_range_locked
 *
 * Removes/modifies VMAs overlapping a given range [start, start + length).
 * Unmaps pages and frees corresponding physical frames.
 * Assumes mm->lock is held.
 */
static int remove_vma_range_locked(mm_struct_t *mm, uintptr_t start, size_t length) {
    uintptr_t end = start + length;
    if (start >= end) return -FS_ERR_INVALID_PARAM; // Length must be > 0

    terminal_printf("[MM] remove_vma_range_locked: Request [0x%x - 0x%x)\n", start, end);

    int result = 0;
    struct rb_node *node = NULL;
    struct rb_node *next_node = NULL;

    // We need a way to iterate safely while modifying the tree.
    // Option 1: Find the first potentially overlapping node, then use rb_node_next.
    // Option 2: Re-find the next candidate after each modification (safer but slower).
    // Let's try Option 1.

    // Find the first VMA whose end is >= start
    // (We could potentially start the search more efficiently, but this is simpler)
    node = rb_tree_first(&mm->vma_tree);

    while (node) {
        vma_struct_t *vma = rb_entry(node, vma_struct_t, rb_node);
        next_node = rb_node_next(node); // Get next pointer BEFORE potentially deleting 'node'

        // Check for overlap: max(vma->start, start) < min(vma->end, end)
        uintptr_t overlap_start = (vma->vm_start > start) ? vma->vm_start : start;
        uintptr_t overlap_end = (vma->vm_end < end) ? vma->vm_end : end;

        // If the VMA starts after our range ends, we are done iterating.
        if (vma->vm_start >= end) {
            break;
        }

        // If there is an overlap region
        if (overlap_start < overlap_end) {
            terminal_printf("  Overlap found: VMA [0x%x-0x%x) overlaps request [0x%x-0x%x) => unmap [0x%x-0x%x)\n",
                           vma->vm_start, vma->vm_end, start, end, overlap_start, overlap_end);

            // --- 1. Unmap the overlapping pages ---
            result = paging_unmap_range(mm->pgd_phys, overlap_start, overlap_end - overlap_start);
            if (result != 0) {
                terminal_printf("    Error: paging_unmap_range failed (code %d) for [0x%x-0x%x)\n", result, overlap_start, overlap_end);
                // Decide how to handle partial failure. Stop for now.
                return result;
            }

            // --- 2. Adjust or Remove VMA ---
            bool remove_original = false;
            vma_struct_t* created_second_part = NULL;

            // Case 1: VMA is fully contained within the unmap range
            if (vma->vm_start >= start && vma->vm_end <= end) {
                 terminal_write("    VMA fully contained. Removing.\n");
                remove_original = true;
            }
            // Case 2: Unmap range splits the VMA
            else if (vma->vm_start < start && vma->vm_end > end) {
                terminal_write("    VMA split needed.\n");
                // Need to create a new VMA for the second part [end, vma->vm_end)
                created_second_part = alloc_vma_struct();
                if (!created_second_part) return -FS_ERR_OUT_OF_MEMORY;

                memcpy(created_second_part, vma, sizeof(vma_struct_t)); // Copy props
                created_second_part->vm_start = end; // Adjust start for second part
                 // Adjust file offset if needed
                 if (created_second_part->vm_flags & VM_FILEBACKED) {
                     created_second_part->vm_offset += (end - vma->vm_start);
                     // TODO: Increment file ref count for the new VMA
                 }

                // Shrink the original VMA for the first part
                vma->vm_end = start;
                 terminal_printf("    Original shrunk to [0x%x-0x%x), New VMA [0x%x-0x%x)\n",
                                vma->vm_start, vma->vm_end, created_second_part->vm_start, created_second_part->vm_end);
                // Original VMA (node) remains in tree, shrunk.
                // created_second_part needs insertion below.
            }
            // Case 3: Overlap is at the end of the VMA
            else if (vma->vm_start < start /* implies vma->vm_end <= end */) {
                 terminal_write("    Shrinking VMA end.\n");
                vma->vm_end = start; // Shrink original VMA
            }
            // Case 4: Overlap is at the beginning of the VMA
            else /* implies vma->vm_start >= start && vma->vm_end > end */ {
                 terminal_write("    Shrinking VMA start.\n");
                 // Adjust file offset first
                  if(vma->vm_flags & VM_FILEBACKED) {
                     vma->vm_offset += (end - vma->vm_start);
                 }
                vma->vm_start = end; // Shrink original VMA

                // *** IMPORTANT RB TREE NOTE ***: Changing vm_start changes the node's key!
                // The RB Tree property might be violated. A simple adjustment might work
                // if the relative order doesn't change drastically, but a remove/re-insert
                // is safer but more complex to do within this loop.
                // For now, we just modify vm_start and hope for the best (simplification).
                terminal_write("    Warning: Modifying vm_start might affect RB Tree validity without re-insertion.\n");
            }

            // --- 3. Perform Tree Modifications ---
            if (remove_original) {
                rb_tree_remove(&mm->vma_tree, node); // Remove original VMA node
                mm->map_count--;
                free_vma_resources(vma); // Free the structure
            }
            if (created_second_part) {
                 // Insert the newly created second part (after split)
                 // We need to find the insertion spot again, relative to the potentially modified tree
                 struct rb_node **link = &mm->vma_tree.root;
                 struct rb_node *parent = NULL;
                 bool insert_left = true;
                 while (*link) {
                     parent = *link;
                     vma_struct_t* current = rb_entry(parent, vma_struct_t, rb_node);
                     if (created_second_part->vm_start < current->vm_start) {
                         link = &parent->left; insert_left = true;
                     } else {
                         link = &parent->right; insert_left = false;
                     }
                 }
                rb_tree_insert_at(&mm->vma_tree, parent, &created_second_part->rb_node, insert_left);
                mm->map_count++; // Increment count for the new split part
            }

        } // End if overlap

        // Move to the next node in the original sequence
        node = next_node;
    } // End while loop

    return result; // Return the last result code (0 if all okay)
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