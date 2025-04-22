#include "uaccess.h"
#include "paging.h"     // For KERNEL_SPACE_VIRT_START definition
#include "process.h"    // For get_current_process(), pcb_t
#include "mm.h"         // For mm_struct_t, vma_struct_t, find_vma, VM_READ, VM_WRITE
#include "terminal.h"   // For debugging output (optional)
#include "assert.h"     // For KERNEL_ASSERT (optional)
#include "fs_errno.h"   // For error codes like EFAULT
#include "debug.h"      // For DEBUG_PRINTK

// Note: _raw_copy_from_user and _raw_copy_to_user are now implemented
// in uaccess.asm using optimized techniques.


/**
 * @brief Checks if a userspace memory range is potentially accessible based on VMAs.
 * (Implementation remains largely the same as provided previously, ensure it's correct
 * for your specific VMA and MM structures).
 */
bool access_ok(int type, const void *uaddr_void, size_t size) {
    uint32_t uaddr = (uint32_t)(uintptr_t)uaddr_void; // Use 32-bit address
    uint32_t end_addr;

    // 1. Basic Pointer and Range Checks
    // Allow NULL pointer only if size is 0.
    if (!uaddr_void && size > 0) {
        DEBUG_PRINTK("[access_ok] Denied: NULL pointer with size > 0\n");
        return false;
    }
     if (size == 0) { return true; } // Zero size is always OK

    // Check if start address is in kernel space. Ensure KERNEL_SPACE_VIRT_START is defined correctly.
    #ifndef KERNEL_SPACE_VIRT_START
    #error "KERNEL_SPACE_VIRT_START is not defined! Check paging.h"
    #endif
    if (uaddr >= KERNEL_SPACE_VIRT_START) {
         DEBUG_PRINTK("[access_ok] Denied: Address %p is in kernel space (>= %p)\n", uaddr_void, (void*)KERNEL_SPACE_VIRT_START);
         return false;
    }

    // Check for arithmetic overflow (32-bit). end_addr is the address AFTER the last byte.
    if (__builtin_add_overflow(uaddr, size, &end_addr)) {
        DEBUG_PRINTK("[access_ok] Denied: Address range %p + %u overflows\n", uaddr_void, size);
        return false;
    }

    // Check if the range ends in kernel space or wraps around.
    // end_addr points *after* the range, so it can be == KERNEL_SPACE_VIRT_START
    // if the range ends exactly at the boundary.
    if (end_addr > KERNEL_SPACE_VIRT_START || end_addr <= uaddr) { // Check wrap-around too (end <= start)
         DEBUG_PRINTK("[access_ok] Denied: Address range %p-%p crosses kernel boundary or wraps\n", uaddr_void, (void*)end_addr);
         return false;
    }

    // 2. VMA Checks (Crucial part)
    pcb_t *current_proc = get_current_process();
    if (!current_proc || !current_proc->mm) {
        // Cannot verify VMAs without process context. Safer to deny access.
        DEBUG_PRINTK("[access_ok] Warning: No current process or mm_struct. Denying access for %p size %u.\n", uaddr_void, size);
        return false;
    }
    mm_struct_t *mm = current_proc->mm;

    // Iterate through the required range, checking VMA coverage and permissions.
    uint32_t current_check_addr = uaddr;
    while (current_check_addr < end_addr) {
        // Assuming find_vma takes mm_struct_t* and a 32-bit address
        vma_struct_t *vma = find_vma(mm, current_check_addr);

        if (!vma || current_check_addr < vma->vm_start) {
            DEBUG_PRINTK("[access_ok] Error: Address 0x%x not in any VMA.\n", current_check_addr);
            return false; // Gap in VMA coverage.
        }

        // Check VMA permissions. Assuming VM_READ/VM_WRITE flags exist.
        bool read_needed = (type & VERIFY_READ);
        bool write_needed = (type & VERIFY_WRITE);

        // Check if the *required* permissions are present in the VMA flags
        if (read_needed && !(vma->vm_flags & VM_READ)) {
             DEBUG_PRINTK("[access_ok] Error: VMA lacks READ permission for 0x%x (needed for %p, size %u).\n", current_check_addr, uaddr_void, size);
            return false;
        }
        if (write_needed && !(vma->vm_flags & VM_WRITE)) {
            DEBUG_PRINTK("[access_ok] Error: VMA lacks WRITE permission for 0x%x (needed for %p, size %u).\n", current_check_addr, uaddr_void, size);
            return false;
        }

        // Advance check address to the end of this VMA or the requested end, whichever comes first.
        uint32_t next_check_boundary = vma->vm_end; // vm_end is exclusive

        // Important: Handle the case where the requested range ends *within* this VMA
        if (next_check_boundary > end_addr) {
            next_check_boundary = end_addr;
        }

        // Sanity check to prevent infinite loop if vm_end <= current_check_addr
        if (next_check_boundary <= current_check_addr) {
            DEBUG_PRINTK("[access_ok] FATAL: VMA loop error near 0x%x (VMA end 0x%x).\n", current_check_addr, vma->vm_end);
            // This indicates a potential issue in find_vma or VMA structure management
            return false; // Prevent infinite loop
        }

        current_check_addr = next_check_boundary;
    }

    // If loop completes, the entire range [uaddr, end_addr) is covered by VMAs
    // with the necessary permissions.
    return true;
}


/**
 * @brief Copies 'n' bytes from userspace 'u_src' to kernelspace 'k_dst'.
 * Handles page faults using assembly helper and exception table.
 */
size_t copy_from_user(void *k_dst, const void *u_src, size_t n) {
    // Check basic conditions
    if (n == 0) return 0;
    if (!k_dst) return n; // Invalid kernel buffer

    // Check VMA permissions *before* attempting the potentially faulting copy.
    // This avoids calling the assembly if the VMA setup itself prohibits access.
    if (!access_ok(VERIFY_READ, u_src, n)) {
        DEBUG_PRINTK("[CopyFromUser] access_ok failed src=%p size=%u\n", u_src, n);
        return n; // Return 'n' indicating all 'n' bytes failed (due to bad VMA/args).
    }

    // Call the assembly routine which handles potential faults.
    // It returns the number of bytes *not* copied.
    size_t not_copied = _raw_copy_from_user(k_dst, u_src, n);

    if (not_copied > 0 && not_copied <= n) {
         DEBUG_PRINTK("[CopyFromUser] Faulted after copying %u bytes from %p (requested %u)\n", n - not_copied, u_src, n);
    } else if (not_copied > n) {
        // This indicates an internal error in the assembly routine or fault handler
        DEBUG_PRINTK("[CopyFromUser] FATAL ERROR: _raw_copy_from_user returned %u (more than requested %u)\n", not_copied, n);
        // KERNEL_PANIC?
        return n; // Return n as a safe fallback
    }

    return not_copied; // 0 on success, >0 on partial copy due to fault
}

/**
 * @brief Copies 'n' bytes from kernelspace 'k_src' to userspace 'u_dst'.
 * Handles page faults using assembly helper and exception table.
 */
size_t copy_to_user(void *u_dst, const void *k_src, size_t n) {
    // Check basic conditions
    if (n == 0) return 0;
    if (!k_src) return n; // Invalid kernel buffer (should not happen for kernel data)

    // Check VMA permissions *before* attempting the potentially faulting copy.
    if (!access_ok(VERIFY_WRITE, u_dst, n)) {
        DEBUG_PRINTK("[CopyToUser] access_ok failed dst=%p size=%u\n", u_dst, n);
        return n; // Return 'n' indicating all 'n' bytes failed (due to bad VMA/args).
    }

    // Call the assembly routine which handles potential faults.
    size_t not_copied = _raw_copy_to_user(u_dst, k_src, n);

     if (not_copied > 0 && not_copied <= n) {
         DEBUG_PRINTK("[CopyToUser] Faulted after copying %u bytes to %p (requested %u)\n", n - not_copied, u_dst, n);
    } else if (not_copied > n) {
        // Internal error check
        DEBUG_PRINTK("[CopyToUser] FATAL ERROR: _raw_copy_to_user returned %u (more than requested %u)\n", not_copied, n);
        // KERNEL_PANIC?
        return n;
    }

    return not_copied; // 0 on success, >0 on partial copy due to fault
}