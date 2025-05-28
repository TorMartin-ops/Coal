/**
 * @file mm.c
 * @brief Process Memory Management (VMAs, Page Fault Handling)
 *
 * Manages virtual memory areas (VMAs) for processes using an RB Tree.
 * Handles page faults, including demand paging and copy-on-write (COW).
 */

 #include <kernel/memory/mm.h>
 #include <kernel/memory/kmalloc.h>    // For allocating mm_struct and vma_struct
 #include <kernel/drivers/display/terminal.h>   // For logging (terminal_printf)
 #include <kernel/memory/buddy.h>      // Underlying physical allocator (called by frame allocator) - Needed indirectly
 #include <kernel/memory/frame.h>      // Frame allocator header (frame_alloc, put_frame, get_frame_refcount)
 #include <kernel/memory/paging.h>     // For mapping pages, flags, KERNEL_SPACE_VIRT_START, paging_temp_map/unmap, paging_invalidate_page, paging_unmap_range
 #include <kernel/fs/vfs/vfs.h>        // For file_t definition (though not used directly here, included for completeness)
 #include <kernel/fs/vfs/fs_errno.h>   // For error codes (EFAULT, ENOMEM, EPERM, etc.)
 #include <kernel/lib/rbtree.h>     // RB Tree header
 #include <kernel/process/process.h>    // For pcb_t, get_current_process
 #include <kernel/lib/string.h>     // For memset, memcpy
 #include <kernel/drivers/display/serial.h>     // For serial_write debug logging
 #include <kernel/sync/spinlock.h>   // For spinlock_t and functions
 #include <kernel/lib/assert.h>     // For KERNEL_ASSERT, KERNEL_PANIC_HALT
 #include <libc/stddef.h> // NULL, size_t
 #include <libc/stdbool.h> // bool
 
 // --- Temporary Mapping Addresses ---
 // Define placeholder addresses if not in paging.h
 // These should be distinct virtual addresses in kernel space reserved for temporary mappings.
 #ifndef TEMP_MAP_ADDR_PD // Guard against redefinition
 #define TEMP_MAP_ADDR_PD (KERNEL_SPACE_VIRT_START - 1 * PAGE_SIZE) // Placeholder
 #endif
 #ifndef TEMP_MAP_ADDR_PT
 #define TEMP_MAP_ADDR_PT (KERNEL_SPACE_VIRT_START - 2 * PAGE_SIZE) // Placeholder
 #endif
 #ifndef TEMP_MAP_ADDR_PF
 #define TEMP_MAP_ADDR_PF (KERNEL_SPACE_VIRT_START - 3 * PAGE_SIZE) // Placeholder
 #endif
 #ifndef TEMP_MAP_ADDR_COW_SRC
 #define TEMP_MAP_ADDR_COW_SRC (KERNEL_SPACE_VIRT_START - 4 * PAGE_SIZE) // Placeholder
 #endif
 #ifndef TEMP_MAP_ADDR_COW_DST
 #define TEMP_MAP_ADDR_COW_DST (KERNEL_SPACE_VIRT_START - 5 * PAGE_SIZE) // Placeholder
 #endif
 
 // Ensure kernel virtual directory pointer is available (defined in paging.c)
 extern uint32_t* g_kernel_page_directory_virt;
 
 
 // --- Forward Declarations ---
 static vma_struct_t* find_vma_locked(mm_struct_t *mm, uintptr_t addr);
 static vma_struct_t* insert_vma_locked(mm_struct_t *mm, vma_struct_t* new_vma);
 static int remove_vma_range_locked(mm_struct_t *mm, uintptr_t start, size_t length);
 static uint32_t* get_pte_ptr(mm_struct_t *mm, uintptr_t vaddr, bool allocate_pt);
 // <<<--- ADDED FORWARD DECLARATION --->>>
 static void destroy_vma_node_callback(vma_struct_t *vma_node, void *data);
 
 
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
     // Initialize other mm fields if needed (start_brk, end_brk etc. set during load)
     return mm;
 }
 
 /**
  * Destroys a memory descriptor and its VMAs using RB Tree traversal.
  */
 // --- THIS IS THE SINGLE, CORRECT DEFINITION ---
 // Forward declaration for idle task stack checking
 extern void check_idle_task_stack_integrity(const char *checkpoint);
 
 void destroy_mm(mm_struct_t *mm) {
     if (!mm) return;
 
     serial_write("[destroy_mm] Enter.\n"); // <-- Logging
     check_idle_task_stack_integrity("destroy_mm: Enter");
 
     // Acquire lock to safely get root, then clear it
     uintptr_t irq_flags = spinlock_acquire_irqsave(&mm->lock);
     struct rb_node *root = mm->vma_tree.root;
     // ---> Log root node and map count <---
     serial_printf("[destroy_mm] Root node = %p, map_count = %u\n", root, mm->map_count);
     // ---> END Log <---
     mm->vma_tree.root = NULL; // Clear root immediately
     mm->map_count = 0;
     spinlock_release_irqrestore(&mm->lock, irq_flags); // Release lock before traversal
 
     if (root) {
         serial_write("[destroy_mm] Traversing VMA tree...\n"); // <-- Logging
         check_idle_task_stack_integrity("destroy_mm: Before VMA traversal");
         // Perform post-order traversal of RB Tree to unmap and free nodes
         rbtree_postorder_traverse(root, destroy_vma_node_callback, mm); // Pass mm for context
         check_idle_task_stack_integrity("destroy_mm: After VMA traversal");
         serial_write("[destroy_mm] VMA traversal complete.\n"); // <-- Logging
     } else {
         serial_write("[destroy_mm] VMA tree is empty, skipping traversal.\n"); // <-- Logging
     }
 
     serial_write("[destroy_mm] Calling kfree(mm)...\n"); // <-- Logging
     check_idle_task_stack_integrity("destroy_mm: Before kfree(mm)");
     // Free the mm_struct itself
     kfree(mm);
     check_idle_task_stack_integrity("destroy_mm: After kfree(mm)");
     serial_write("[destroy_mm] Returned from kfree(mm). Exit.\n"); // <-- Logging
 }
 
 
 // Helper callback for destroy_mm traversal
 // (Includes logging added previously)
 static void destroy_vma_node_callback(vma_struct_t *vma_node, void *data) {
    mm_struct_t *mm = (mm_struct_t*)data;
    if (!vma_node) {
        serial_write("[destroy_vma_cb] Warning: Callback invoked with NULL vma_node.\n");
        return;
    }

    // Logging the VMA being processed
    serial_printf("[destroy_vma_cb] Processing VMA [%p-%p)\n", 
                  (void*)vma_node->vm_start, (void*)vma_node->vm_end);

    // Check if the memory manager context and its page directory are valid
    if (mm && mm->pgd_phys) {
        serial_write("  Calling paging_unmap_range...\n");

        // Calculate the size of the VMA, handling potential underflow
        size_t vma_size = 0;
        if (vma_node->vm_end >= vma_node->vm_start) {
             vma_size = vma_node->vm_end - vma_node->vm_start;
        } else {
             // Log a warning if end address is less than start address
             terminal_printf("   Warning: VMA end address 0x%lx < start 0x%lx during destroy. Treating size as 0.\n",
                             (unsigned long)vma_node->vm_end, (unsigned long)vma_node->vm_start);
        }

        // Call the paging function to unmap the range
        // Only proceed if size is greater than 0, as paging_unmap_range handles 0 size internally now.
        int ret = 0; // Assume success for zero size
        if (vma_size > 0) {
            check_idle_task_stack_integrity("destroy_vma_cb: Before paging_unmap_range");
            ret = paging_unmap_range(mm->pgd_phys, vma_node->vm_start, vma_size);
            check_idle_task_stack_integrity("destroy_vma_cb: After paging_unmap_range");
        } else {
            // If size is 0, no unmapping needed, but log it for clarity if desired
            serial_write("  Skipping paging_unmap_range for zero-sized VMA.\n");
        }
        serial_write("  Returned from paging_unmap_range.\n");

        // --- FIXED: Avoid warning for intentionally zero-sized VMAs ---
        // Only print a warning if unmapping failed (ret != 0) for a non-zero sized VMA.
        if (ret != 0 && vma_size > 0) {
             terminal_printf("   Warning: paging_unmap_range failed during VMA destroy (code %d) for VMA [%p-%p).\n",
                             ret, (void*)vma_node->vm_start, (void*)vma_node->vm_end);
        }
        // --- END FIX ---

    } else {
         // Log a warning if unmapping cannot be performed
         terminal_write("   Warning: Cannot unmap VMA during destroy, mm or pgd_phys is NULL.\n");
    }

    // Free the resources associated with the VMA structure itself
    serial_write("  Calling free_vma_resources (kfree)...\n");
    check_idle_task_stack_integrity("destroy_vma_cb: Before free_vma_resources");
    free_vma_resources(vma_node); // Assumes this function calls kfree(vma_node)
    check_idle_task_stack_integrity("destroy_vma_cb: After free_vma_resources");
    serial_write("  Returned from free_vma_resources.\n");
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
             KERNEL_ASSERT(new_vma->vm_start >= current_vma->vm_end, "VMA insertion logic error - overlap missed?");
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
     vma->vm_file = file; // Note: This doesn't take ownership or increment refcount here
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
     // TODO: If file-backed, increment file reference count NOW (e.g., vfs_file_dup(file))
     // if (vma->vm_file) { vfs_file_dup(vma->vm_file); } // Requires vfs_file_dup
     return result;
 }
 
 
 // --- Page Fault Handling ---
 
 // get_pte_ptr helper function (maps PD/PT temporarily)
 // Returns pointer to PTE within temporarily mapped PT
 // Caller MUST unmap the returned PT mapping address after use.
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
     uint32_t* pt_virt = NULL;      // Temp VAddr for target PT (mapped dynamically)
     uint32_t* pte_ptr = NULL;      // Result pointer (inside pt_virt mapping)
     uintptr_t pt_phys_addr_val = 0;
     bool allocated_pt_frame = false;
 
     // 1. Map Target PD temporarily using a dynamic temp VA slot
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
         void* temp_pt_zero_map = paging_temp_map(pt_phys_addr_val, PTE_KERNEL_DATA_FLAGS);
         if (!temp_pt_zero_map) {
             terminal_printf("[get_pte_ptr] Error: Failed to temp map new PT frame %#lx for zeroing.\n", (unsigned long)pt_phys_addr_val);
             goto fail_gpp; // Free frame in cleanup
         }
         memset(temp_pt_zero_map, 0, PAGE_SIZE);
         paging_temp_unmap(temp_pt_zero_map); // Unmap after zeroing
 
         // Set the PDE in the temporarily mapped target PD
         // Use USER flags if the eventual PTE will need them (most flexible)
         uint32_t pde_flags = PAGE_PRESENT | PAGE_RW | PAGE_USER; // Common flags for a PT PDE
         proc_pd_virt[pd_idx] = (pt_phys_addr_val & PAGING_ADDR_MASK) | pde_flags;
         // TLB invalidation is handled by caller or context switch
 
     } else if (pde & PAGE_SIZE_4MB) {
         // Cannot get a PTE pointer if the PDE maps a 4MB page
         terminal_printf("[get_pte_ptr] Error: Cannot get PTE for V=%p; PDE[%lu] is a 4MB page.\n", (void*)vaddr, (unsigned long)pd_idx);
         goto fail_gpp;
     } else {
         // PDE is present and points to a 4KB Page Table
         pt_phys_addr_val = (uintptr_t)(pde & PAGING_ADDR_MASK);
     }
 
     // 4. Map the target Page Table (either existing or newly created) using a dynamic temp map
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
     // NOTE: The caller is responsible for unmapping the dynamically mapped PT (pt_virt) later!
     return pte_ptr;
 
 fail_gpp:
     // Cleanup allocated frame if allocation failed mid-way
     if (allocated_pt_frame && pt_phys_addr_val != 0) {
          put_frame(pt_phys_addr_val);
     }
     // Cleanup temporary PD mapping
     if (proc_pd_virt) {
         paging_temp_unmap(proc_pd_virt);
     }
     // Ensure temporary PT mapping is also cleaned up on failure if it got mapped dynamically
     if (pt_virt) {
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
     uint32_t* pte_ptr = NULL;      // Pointer to PTE within temp map
     void* pt_temp_map_addr = NULL; // Keep track of the PT temp map address returned by get_pte_ptr
     bool is_write = (error_code & PAGE_FAULT_WRITE) != 0; // Assumes PAGE_FAULT_WRITE is defined (e.g., 0x2)
     bool present = (error_code & PAGE_FAULT_PRESENT) != 0; // Assumes PAGE_FAULT_PRESENT is defined (e.g., 0x1)
     uintptr_t phys_page = 0;       // For allocating new frames
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
                 return -FS_ERR_INTERNAL; // get_pte_ptr cleans up its maps
             }
             // Since get_pte_ptr returns a pointer inside a *dynamic* temporary map, remember its base
             pt_temp_map_addr = (void*)PAGE_ALIGN_DOWN((uintptr_t)pte_ptr);
 
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
             if (pt_temp_map_addr) { // Unmap the dynamically mapped PT
                 paging_temp_unmap(pt_temp_map_addr);
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
     // terminal_printf("   Allocated phys frame: %#lx\n", phys_page);
 
     // 2. Map frame temporarily into kernel to populate
     temp_addr_for_copy = paging_temp_map(phys_page, PTE_KERNEL_DATA_FLAGS);
     if (!temp_addr_for_copy) {
         put_frame(phys_page); return -FS_ERR_IO;
     }
 
     // 3. Populate frame
     if (vma->vm_flags & VM_FILEBACKED) {
         terminal_printf("   Populating from file (TODO) V=%p P=%#lx\n", (void*)page_addr, (unsigned long)phys_page);
         // TODO: Implement file read logic here
         // Need vma->vm_file, vma->vm_offset, page_addr - vma->vm_start
         memset(temp_addr_for_copy, 0, PAGE_SIZE); // Placeholder
     } else { // Anonymous VMA
         // terminal_printf("   Zeroing anonymous page V=%p P=%#lx\n", (void*)page_addr, phys_page);
         memset(temp_addr_for_copy, 0, PAGE_SIZE);
     }
 
     // 4. Unmap temporary kernel mapping
     paging_temp_unmap(temp_addr_for_copy);
     temp_addr_for_copy = NULL; // Mark as unmapped
 
     // 5. Map frame into process space via PTE
     pte_ptr = get_pte_ptr(mm, page_addr, true); // Allocate PT if needed
     if (!pte_ptr) {
         put_frame(phys_page); return -FS_ERR_IO; // Failed to get PTE access
     }
     pt_temp_map_addr = (void*)PAGE_ALIGN_DOWN((uintptr_t)pte_ptr); // Remember PT temp map addr
 
     // Determine final flags (Apply COW by mapping RO initially if needed)
     uint32_t map_flags = vma->page_prot; // Start with VMA's base page permissions
     if ((vma->vm_flags & VM_WRITE) && !(vma->vm_flags & VM_SHARED)) {
         map_flags &= ~PAGE_RW; // Clear RW for Copy-on-Write
     }
 
     // Write the PTE using the pointer from get_pte_ptr
     *pte_ptr = (phys_page & PAGING_ADDR_MASK) | map_flags | PAGE_PRESENT;
     // terminal_printf("   Set PTE at %p = %#lx\n", pite_ptr, *pte_ptr);
 
 
     // 6. Unmap the temporary PT mapping created by get_pte_ptr
     paging_temp_unmap(pt_temp_map_addr);
 
     // 7. Invalidate TLB for the specific user page
     paging_invalidate_page((void*)page_addr);
 
     // terminal_printf("   NP Fault handled successfully for V=%p -> P=%#lx\n", (void*)page_addr, phys_page);
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
         next_node = rb_node_next(node); // Get next before potentially removing current node
         uintptr_t overlap_start = (vma->vm_start > start) ? vma->vm_start : start;
         uintptr_t overlap_end = (vma->vm_end < end) ? vma->vm_end : end;
 
         // If the current VMA starts after the requested removal range ends, we're done.
         if (vma->vm_start >= end) break;
 
         // If the current VMA ends before the requested removal range starts, skip it.
         if (vma->vm_end <= start) {
              node = next_node;
              continue;
         }
 
 
         // Check for overlap (should always be true if neither of the above conditions were met)
         if (overlap_start < overlap_end) {
             // terminal_printf("   Overlap found: VMA [0x%x-0x%x) overlaps request [0x%x-0x%x) => unmap [0x%x-0x%x)\n", vma->vm_start, vma->vm_end, start, end, overlap_start, overlap_end);
 
             result = paging_unmap_range(mm->pgd_phys, overlap_start, overlap_end - overlap_start);
             if (result != 0) { return result; } // Stop on error
 
             bool remove_original = false;
             vma_struct_t* created_second_part = NULL;
 
             if (vma->vm_start >= start && vma->vm_end <= end) { // Case 1: Fully contained
                 // terminal_write("     VMA fully contained. Removing.\n");
                 remove_original = true;
             } else if (vma->vm_start < start && vma->vm_end > end) { // Case 2: Split
                 // terminal_write("     VMA split needed.\n");
                 created_second_part = alloc_vma_struct();
                 if (!created_second_part) return -FS_ERR_OUT_OF_MEMORY;
                 memcpy(created_second_part, vma, sizeof(vma_struct_t)); // Copy original VMA data
                 created_second_part->vm_start = end; // Set new start for second part
                 // Adjust file offset if file-backed
                 if (created_second_part->vm_flags & VM_FILEBACKED) {
                      // Ensure offset doesn't wrap around
                      size_t diff = end - vma->vm_start;
                      if (created_second_part->vm_offset > SIZE_MAX - diff) {
                           terminal_printf("Warning: VMA split offset overflow!\n"); // Handle error appropriately
                           free_vma_resources(created_second_part); return -FS_ERR_INVALID_PARAM;
                      }
                      created_second_part->vm_offset += diff;
                 }
                 // Shrink the original VMA
                 vma->vm_end = start;
                 // terminal_printf("     Original shrunk to [0x%x-0x%x), New VMA [0x%x-0x%x)\n", vma->vm_start, vma->vm_end, created_second_part->vm_start, created_second_part->vm_end);
             } else if (vma->vm_start < start) { // Case 3: Overlap at end - Shrink original
                 // terminal_write("     Shrinking VMA end.\n");
                 vma->vm_end = start;
             } else { // Case 4: Overlap at beginning - Shrink original
                 // terminal_write("     Shrinking VMA start.\n");
                 // Adjust file offset before changing vm_start
                 if (vma->vm_flags & VM_FILEBACKED) {
                      size_t diff = end - vma->vm_start;
                      if (vma->vm_offset > SIZE_MAX - diff) {
                          terminal_printf("Warning: VMA shrink offset overflow!\n"); return -FS_ERR_INVALID_PARAM;
                      }
                      vma->vm_offset += diff;
                 }
                 vma->vm_start = end;
                 // Modifying vm_start invalidates RB-tree ordering. Must remove and re-insert.
                 // This case is more complex. For simplicity, let's disallow partial removal from start
                 // or implement remove/re-insert logic carefully.
                 // *** For now: Simplification - Let's just remove fully contained and shrink-end cases ***
                 // *** And splitting. Removing from start needs more work. ***
                 terminal_write("    Warning: Removing VMA overlapping at start is complex due to RB tree key. Only handling full contain/end overlap/split.\n");
                 // Let's convert this case to full removal if start matches vma->vm_start
                  if (vma->vm_start == start) {
                      remove_original = true;
                      terminal_write("    Treating start overlap as full removal for simplicity.\n");
                  } else {
                      // If not starting exactly at VMA start, we cannot easily shrink start.
                      // Return error or handle more complex RB tree update.
                      terminal_printf("Error: Cannot currently handle removing VMA partial overlap at the beginning.\n");
                      return -FS_ERR_NOT_SUPPORTED;
                  }
             }
 
             if (remove_original) {
                 rb_tree_remove(&mm->vma_tree, node);
                 mm->map_count--;
                 free_vma_resources(vma); // Free the VMA struct
             }
             if (created_second_part) {
                  // Need to insert the split part back into the tree
                  // This requires finding the correct parent/link again, as the tree might have changed
                  struct rb_node **new_link = &mm->vma_tree.root;
                  struct rb_node *new_parent = NULL;
                  bool new_insert_left = true;
                  while (*new_link) {
                      new_parent = *new_link;
                      vma_struct_t* current = rb_entry(new_parent, vma_struct_t, rb_node);
                      if (created_second_part->vm_start < current->vm_start) { new_link = &new_parent->left; new_insert_left = true; }
                      else { new_link = &new_parent->right; new_insert_left = false; }
                  }
                 rb_tree_insert_at(&mm->vma_tree, new_parent, &created_second_part->rb_node, new_insert_left);
                 mm->map_count++;
             }
         } // End if overlap
         node = next_node; // Move to the next node saved earlier
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