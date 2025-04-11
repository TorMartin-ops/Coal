#include "syscall.h"
#include "terminal.h"
#include "scheduler.h"
#include "process.h"
#include "mm.h"         // Added include for find_vma_locked, free_vma_resources, vma_struct_t, mm_struct_t
#include "paging.h"     // Added include for PAGE_SIZE, ALIGN_UP, PTE flags, mapping functions
#include "kmalloc.h"
#include "string.h"
#include "types.h"
#include "fs_errno.h"
#include "sys_file.h"   // For fd_table access (if validating fd in mmap)

// Max syscall number supported
#define MAX_SYSCALL 32

// User space memory limit
#define USER_SPACE_LIMIT  0xC0000000

// Extern kernel PD physical address (needed for validation mapping)
extern uint32_t g_kernel_page_directory_phys;
// TEMP addresses are now in paging.h

// --- Forward Declarations for Syscall Handlers ---
static int sys_write(syscall_context_t *ctx);
static int sys_exit(syscall_context_t *ctx);
static int sys_mmap(syscall_context_t *ctx);
static int sys_munmap(syscall_context_t *ctx);
static int sys_brk(syscall_context_t *ctx);
static int sys_unknown(syscall_context_t *ctx);

// --- Forward Declarations for Helpers ---
static bool validate_user_access(const void *ptr, size_t len, bool require_write);
static bool copy_from_user(void *kernel_dest, const void *user_src, size_t len);
static bool copy_to_user(void *user_dest, const void *kernel_src, size_t len);
// Forward declare find_vma_locked if it's static in mm.c (or include appropriate header)
// Assuming find_vma (public) is sufficient for now in sys_brk, or we add a static decl if needed.
// Assuming free_vma_resources is static in mm.c and not directly needed here.

// --- Syscall Table ---
// Initializer elements should be constant (function pointers are constants)
static int (*syscall_table[MAX_SYSCALL])(syscall_context_t *ctx) = {
    [0] = sys_unknown,
    [1] = sys_write,
    [2] = sys_exit,
    [3] = sys_mmap,
    [4] = sys_munmap,
    [5] = sys_brk,
    // Use designated initializers for the rest
    [6 ... MAX_SYSCALL-1] = sys_unknown,
};

// --- Syscall Dispatcher ---
// This function is called by the assembly stub
int syscall_handler(syscall_context_t *ctx) {
    uint32_t num = ctx->eax;
    int ret = -FS_ERR_NOT_SUPPORTED; // Default error

    if (num < MAX_SYSCALL && syscall_table[num]) {
        ret = syscall_table[num](ctx);
    } else {
        ret = sys_unknown(ctx);
    }
    ctx->eax = ret; // Put result in EAX for user space
    return ret; // Return value for kernel use (optional)
}


// --- Robust User Pointer Validation ---
static bool validate_user_access(const void *ptr, size_t len, bool require_write) {
    if (len == 0) return true;
    uintptr_t start_addr = (uintptr_t)ptr;
    uintptr_t end_addr = start_addr + len;

    // Basic Range Check
    if (start_addr >= USER_SPACE_LIMIT || end_addr > USER_SPACE_LIMIT || end_addr < start_addr) {
        return false;
    }

    pcb_t* current_process = get_current_process();
    if (!current_process || !current_process->page_directory_phys) { return false; }
    uint32_t* target_pd_phys = current_process->page_directory_phys;
    uint32_t* target_pd_virt = NULL;
    uint32_t* pt_virt = NULL;
    bool valid = true;

    // Map target PD temporarily (read-only is sufficient for checking)
     if (paging_map_single((uint32_t*)g_kernel_page_directory_phys, TEMP_PD_MAP_ADDR, (uint32_t)target_pd_phys, PTE_KERNEL_READONLY) != 0) {
         return false; // Cannot map target PD
    }
    target_pd_virt = (uint32_t*)TEMP_PD_MAP_ADDR;

    uintptr_t current_vpage = PAGE_ALIGN_DOWN(start_addr);
    while (current_vpage < end_addr) {
        uint32_t pde_idx = PDE_INDEX(current_vpage);
        uint32_t pde = target_pd_virt[pde_idx];

        // Check PDE validity
        if (!(pde & PAGE_PRESENT) || !(pde & PAGE_USER)) { valid = false; break; }
        if (require_write && !(pde & PAGE_RW)) { valid = false; break; }

        if (pde & PAGE_SIZE_4MB) { // Handle 4MB page
            if (require_write && !(pde & PAGE_RW)) { valid = false; break; }
            uintptr_t next_page = PAGE_LARGE_ALIGN_UP(current_vpage + PAGE_SIZE);
            current_vpage = (next_page > current_vpage) ? next_page : end_addr; // Advance or finish
            continue;
        }

        // Handle 4KB page (Check PTE)
        uint32_t* pt_phys = (uint32_t*)(pde & ~0xFFF);
        pt_virt = NULL;
         if (paging_map_single((uint32_t*)g_kernel_page_directory_phys, TEMP_PT_MAP_ADDR, (uint32_t)pt_phys, PTE_KERNEL_READONLY) != 0) {
             valid = false; break; // Failed to map PT
         }
         pt_virt = (uint32_t*)TEMP_PT_MAP_ADDR;
         uint32_t pte_idx = PTE_INDEX(current_vpage);
         uint32_t pte = pt_virt[pte_idx];

         // Check PTE validity
         if (!(pte & PAGE_PRESENT) || !(pte & PAGE_USER)) { valid = false; }
         if (require_write && !(pte & PAGE_RW)) { valid = false; }

         // Unmap temp PT
         paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_PT_MAP_ADDR, PAGE_SIZE);
         paging_invalidate_page((void*)TEMP_PT_MAP_ADDR); // Invalidate its temp mapping
         pt_virt = NULL;

         if (!valid) break; // Exit loop if invalid PTE found
        current_vpage += PAGE_SIZE;
    }

    // Unmap temp PD
    paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_PD_MAP_ADDR, PAGE_SIZE);
    paging_invalidate_page((void*)TEMP_PD_MAP_ADDR);

    // if (!valid) { terminal_printf("[Validate] Failed: range [0x%x-0x%x), Write=%d\n", start_addr, end_addr, require_write); }
    return valid;
}

// --- Copy Helpers ---
static bool copy_from_user(void *kernel_dest, const void *user_src, size_t len) {
    if (!validate_user_access(user_src, len, false)) { // Validate READ access
        return false;
    }
    memcpy(kernel_dest, user_src, len);
    return true;
}

static bool copy_to_user(void *user_dest, const void *kernel_src, size_t len) {
    if (!validate_user_access(user_dest, len, true)) { // Validate WRITE access
        return false;
    }
    memcpy(user_dest, kernel_src, len);
    return true;
}

// --- Syscall Handler Implementations ---
// *** Make static ***
static int sys_write(syscall_context_t *ctx) {
    char *usr_str = (char *)ctx->ebx;
    uint32_t usr_len= ctx->ecx;
    if (!usr_str) return -FS_ERR_INVALID_PARAM;
    // Avoid excessively large copies
    if (usr_len > 4095) usr_len = 4095; // Limit to slightly less than a page

    // Use a kernel buffer (stack or kmalloc for larger sizes)
    // Stack buffer is faster for small, common sizes
    char local_buf[256];
    char *kbuf = local_buf;
    bool dynamic_alloc = false;
    if (usr_len >= sizeof(local_buf)) {
        // If needed size > local buffer, try dynamic allocation
        kbuf = kmalloc(usr_len + 1); // +1 for null terminator
        if (!kbuf) return -FS_ERR_OUT_OF_MEMORY;
        dynamic_alloc = true;
    }

    // *** Use validated copy_from_user ***
    if (!copy_from_user(kbuf, usr_str, usr_len)) {
        if (dynamic_alloc) kfree(kbuf);
        return -FS_ERR_INVALID_PARAM; // Bad pointer or permissions
    }
    kbuf[usr_len] = '\0'; // Null terminate
    terminal_write(kbuf); // Write to kernel console

    if (dynamic_alloc) kfree(kbuf); // Free if dynamically allocated
    return (int)usr_len;
}

static int sys_exit(syscall_context_t *ctx) {
    uint32_t code = ctx->ebx;
    terminal_printf("[syscall] sys_exit code=%d\n", code);
    remove_current_task_with_code(code);
    return 0; // Not reached
}

static int sys_mmap(syscall_context_t *ctx) {
    uintptr_t addr   = (uintptr_t)ctx->ebx;
    size_t    length = (size_t)ctx->ecx;
    int       prot   = (int)ctx->edx;
    int       flags  = (int)ctx->esi;
    int       fd     = (int)ctx->edi;
    off_t     offset = (off_t)ctx->ebp;

    // terminal_printf("[syscall] sys_mmap(addr=0x%x, len=%u, prot=0x%x, flags=0x%x, fd=%d, off=%ld)\n",
    //               addr, length, prot, flags, fd, offset);

    if (length == 0) return -FS_ERR_INVALID_PARAM;
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return -FS_ERR_INVALID_PARAM;
    if (!((flags & MAP_PRIVATE) || (flags & MAP_SHARED))) return -FS_ERR_INVALID_PARAM;
    if ((flags & MAP_PRIVATE) && (flags & MAP_SHARED)) return -FS_ERR_INVALID_PARAM;

    pcb_t* current_process = get_current_process();
    if (!current_process || !current_process->mm) return -FS_ERR_UNKNOWN;
    mm_struct_t *mm = current_process->mm;

    uint32_t vm_flags = 0;
    uint32_t page_prot = PAGE_PRESENT | PAGE_USER; // Base flags for VMA/PTE
    if (prot & PROT_READ)  vm_flags |= VM_READ;
    if (prot & PROT_WRITE) vm_flags |= VM_WRITE;
    if (prot & PROT_EXEC)  vm_flags |= VM_EXEC;

    if (flags & MAP_SHARED) {
        vm_flags |= VM_SHARED;
        if (vm_flags & VM_WRITE) page_prot |= PAGE_RW;
    } else { // MAP_PRIVATE
        vm_flags |= VM_PRIVATE;
        // For COW: Map RW pages initially RO in PTE if VMA is writable
        if (vm_flags & VM_WRITE) {
             page_prot |= PAGE_RW; // PTE needs RW eventually
             // page_prot &= ~PAGE_RW; // Uncomment this line for true COW trigger via PTE
        }
    }
    if (!(prot & PROT_WRITE)) {
        page_prot &= ~PAGE_RW; // Ensure RO if PROT_WRITE not set
    }


    file_t *file = NULL; size_t file_offset_aligned = 0;
    if (flags & MAP_ANONYMOUS) {
        vm_flags |= VM_ANONYMOUS;
        if (fd != -1) return -FS_ERR_INVALID_PARAM;
        offset = 0;
    } else {
        vm_flags |= VM_FILEBACKED;
        // TODO: Full file descriptor validation and lookup needed
        // file = get_file_from_fd(current_process, fd); if (!file) return -EBADF;
        // check file permissions vs prot; vfs_file_dup(file);
        if (fd < 0) return -FS_ERR_INVALID_PARAM; // Basic fd check
        if (offset < 0 || (offset % PAGE_SIZE) != 0) return -FS_ERR_INVALID_PARAM;
        file_offset_aligned = (size_t)offset;
        terminal_write("  Warning: File-backed mmap FD validation/lookup needed.\n");
    }

    // *** Use ALIGN_UP from paging.h ***
    size_t length_aligned = ALIGN_UP(length, PAGE_SIZE);
    uintptr_t map_addr = 0;

    if (flags & MAP_FIXED) {
        if (addr == 0 || (addr % PAGE_SIZE) != 0 || addr >= USER_SPACE_LIMIT) return -FS_ERR_INVALID_PARAM;
        map_addr = addr;
        // TODO: Implement remove_vma_range call here before inserting
        terminal_write("  Warning: MAP_FIXED unmapping not implemented.\n");
        // remove_vma_range(mm, map_addr, length_aligned);
    } else {
        // TODO: Implement find_free_vma_range(mm, length_aligned)
        map_addr = 0x40000000; // Placeholder address
        terminal_printf("  Warning: Using placeholder address 0x%x for mmap.\n", map_addr);
    }

    vma_struct_t* vma = insert_vma(mm, map_addr, map_addr + length_aligned,
                                   vm_flags, page_prot, file, file_offset_aligned);
    if (!vma) {
         // TODO: vfs_file_put(file); if file was dup'd
        return -1; // Map to ENOMEM or other error
    }
    return (int)vma->vm_start;
}

static int sys_munmap(syscall_context_t *ctx) {
    uintptr_t addr   = (uintptr_t)ctx->ebx;
    size_t    length = (size_t)ctx->ecx;

    if (length == 0 || (addr % PAGE_SIZE) != 0) return -FS_ERR_INVALID_PARAM;
    if (addr >= USER_SPACE_LIMIT || (addr + length) > USER_SPACE_LIMIT || (addr + length) < addr) return -FS_ERR_INVALID_PARAM;

    pcb_t* current_process = get_current_process();
    if (!current_process || !current_process->mm) return -FS_ERR_UNKNOWN;
    mm_struct_t *mm = current_process->mm;

    int result = remove_vma_range(mm, addr, length);
    return (result == 0) ? 0 : -1; // Map internal error
}

static int sys_brk(syscall_context_t *ctx) {
    uintptr_t new_brk = (uintptr_t)ctx->ebx;

    pcb_t* current_process = get_current_process();
    if (!current_process || !current_process->mm) return 0; // Return 0 on error?
    mm_struct_t *mm = current_process->mm;

    uintptr_t irq_flags = spinlock_acquire_irqsave(&mm->lock);
    uintptr_t current_brk = mm->end_brk;

    if (new_brk == 0) {
        spinlock_release_irqrestore(&mm->lock, irq_flags);
        return (int)current_brk;
    }
    if (new_brk < mm->start_brk) {
         spinlock_release_irqrestore(&mm->lock, irq_flags);
         return (int)current_brk; // Return old break on failure
    }

    // *** Use ALIGN_UP from paging.h ***
    uintptr_t aligned_new_brk = ALIGN_UP(new_brk, PAGE_SIZE);
    uintptr_t aligned_current_brk = ALIGN_UP(current_brk, PAGE_SIZE);

    // terminal_printf("[syscall] sys_brk: Request=0x%x, Current=0x%x (Al=0x%x), NewAl=0x%x\n",
    //               new_brk, current_brk, aligned_current_brk, aligned_new_brk);

    if (aligned_new_brk == aligned_current_brk) {
        mm->end_brk = new_brk; // Just update logical break
    } else if (aligned_new_brk > aligned_current_brk) {
        // Expanding
        vma_struct_t *heap_vma = find_vma(mm, mm->start_brk); // Use public find_vma
        if (!heap_vma || heap_vma->vm_end != aligned_current_brk) {
            // Try finding based on end address (simpler search than previous attempt)
            heap_vma = find_vma(mm, aligned_current_brk - 1); // Find VMA covering end
             if (!heap_vma || heap_vma->vm_end != aligned_current_brk || heap_vma->vm_start < mm->start_brk) { // Ensure it's the right VMA
                terminal_printf("sys_brk: Heap VMA ending at 0x%x not found/valid for expansion.\n", aligned_current_brk);
                spinlock_release_irqrestore(&mm->lock, irq_flags); return (int)current_brk;
             }
        }

        // Check overlap with next VMA
        struct rb_node *next_node = rb_node_next(&heap_vma->rb_node);
        if (next_node) {
            vma_struct_t *next_vma = rb_entry(next_node, vma_struct_t, rb_node);
            if (aligned_new_brk > next_vma->vm_start) { /* Overlap error */ spinlock_release_irqrestore(&mm->lock, irq_flags); return (int)current_brk; }
        }
        if (aligned_new_brk > USER_SPACE_LIMIT) { /* Limit error */ spinlock_release_irqrestore(&mm->lock, irq_flags); return (int)current_brk; }

        heap_vma->vm_end = aligned_new_brk; // Expand VMA
        mm->end_brk = new_brk;
    } else { // Shrinking
        // Find heap VMA (similar to above)
         vma_struct_t *heap_vma = find_vma(mm, aligned_new_brk); // Find VMA covering new end
         if (!heap_vma || heap_vma->vm_end != aligned_current_brk || heap_vma->vm_start > aligned_new_brk) { // Ensure it's the right one
              terminal_printf("sys_brk: Heap VMA ending at 0x%x not found/valid for shrinking.\n", aligned_current_brk);
             spinlock_release_irqrestore(&mm->lock, irq_flags); return (int)current_brk;
         }

        if (aligned_new_brk < heap_vma->vm_start) { /* Shrink below start error */ spinlock_release_irqrestore(&mm->lock, irq_flags); return (int)current_brk; }

        // Unmap pages
        if (aligned_new_brk < aligned_current_brk) {
            if (paging_unmap_range(mm->pgd_phys, aligned_new_brk, aligned_current_brk - aligned_new_brk) != 0) {
                 /* Unmap error */ spinlock_release_irqrestore(&mm->lock, irq_flags); return (int)current_brk;
            }
        }

        // Adjust/Remove VMA
        if (aligned_new_brk <= heap_vma->vm_start) { // Remove if zero size or less
             rb_tree_remove(&mm->vma_tree, &heap_vma->rb_node);
             mm->map_count--;
             // Use free_vma_resources from mm.c (make it non-static or call via helper)
             // Assuming free_vma_resources is accessible or logic moved here/called differently
             // free_vma_resources(heap_vma); // Needs access or alternative
             kfree(heap_vma); // Simple free if helper not available
        } else {
            heap_vma->vm_end = aligned_new_brk; // Shrink
        }
        mm->end_brk = new_brk;
    }

    // Success path
    spinlock_release_irqrestore(&mm->lock, irq_flags);
    return (int)new_brk; // Return NEW break on success
}

static int sys_unknown(syscall_context_t *ctx) {
    terminal_printf("[syscall] Unknown syscall number: %d\n", ctx->eax);
    return -FS_ERR_NOT_SUPPORTED;
}