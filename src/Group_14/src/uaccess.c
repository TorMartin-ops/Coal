#include "uaccess.h"
#include "paging.h"     // For KERNEL_SPACE_VIRT_START definition
#include "process.h"    // For get_current_process(), pcb_t
#include "mm.h"         // For mm_struct_t, vma_struct_t, find_vma, VM_READ, VM_WRITE
#include "terminal.h"   // For debugging output (optional)
#include "assert.h"     // For KERNEL_ASSERT (optional)
#include "fs_errno.h"   // For error codes like EFAULT

// External assembly routine prototypes
extern size_t _raw_copy_from_user(void *k_dst, const void *u_src, size_t n);
extern size_t _raw_copy_to_user(void *u_dst, const void *k_src, size_t n);


/**
 * @brief Checks if a userspace memory range is potentially accessible based on VMAs.
 * (Implementation assumes find_vma and process structures exist as used below)
 */
bool access_ok(int type, const void *uaddr_void, size_t size) {
    uint32_t uaddr = (uint32_t)(uintptr_t)uaddr_void; // Use 32-bit address
    uint32_t end_addr;

    // 1. Basic Pointer and Range Checks
    if (!uaddr_void && size > 0) { return false; }
    if (size == 0) { return true; }

    // Check if start address is in kernel space. Ensure KERNEL_SPACE_VIRT_START is defined correctly.
    #ifndef KERNEL_SPACE_VIRT_START
    #error "KERNEL_SPACE_VIRT_START is not defined! Check paging.h"
    #endif
    if (uaddr >= KERNEL_SPACE_VIRT_START) { return false; }

    // Check for arithmetic overflow (32-bit).
    if (__builtin_add_overflow(uaddr, size, &end_addr)) { return false; }

    // Check if the range ends in kernel space or wraps around. End address is exclusive.
    if (end_addr > KERNEL_SPACE_VIRT_START || end_addr <= uaddr) { return false; }

    // 2. VMA Checks
    pcb_t *current_proc = get_current_process();
    if (!current_proc || !current_proc->mm) {
        // Cannot verify VMAs without process context. Safer to deny access.
        // terminal_printf("[access_ok] Warning: No current process or mm_struct.\n");
        return false;
    }
    mm_struct_t *mm = current_proc->mm;

    // Iterate through the required range, checking VMA coverage and permissions.
    uint32_t current_check_addr = uaddr;
    while (current_check_addr < end_addr) {
        // Assuming find_vma takes mm_struct_t* and a 32-bit address
        vma_struct_t *vma = find_vma(mm, current_check_addr);

        if (!vma || current_check_addr < vma->vm_start) {
            // terminal_printf("[access_ok] Error: Address 0x%x not in VMA.\n", current_check_addr);
            return false; // Gap in VMA coverage.
        }

        // Check VMA permissions. Assuming VM_READ/VM_WRITE flags exist.
        bool read_needed = (type & VERIFY_READ);
        bool write_needed = (type & VERIFY_WRITE);
        bool read_ok = !read_needed || (vma->vm_flags & VM_READ);
        bool write_ok = !write_needed || (vma->vm_flags & VM_WRITE);

        if (!read_ok || !write_ok) {
            // terminal_printf("[access_ok] Error: VMA lacks permissions for 0x%x.\n", current_check_addr);
            return false;
        }

        // Advance check address to the end of this VMA or the requested end, whichever comes first.
        uint32_t next_check_boundary = vma->vm_end;
        if (next_check_boundary > end_addr) {
            next_check_boundary = end_addr;
        }
        if (next_check_boundary <= current_check_addr) {
            // terminal_printf("[access_ok] FATAL: VMA loop error near 0x%x.\n", current_check_addr);
            return false; // Prevent infinite loop
        }
        current_check_addr = next_check_boundary;
    }
    return true; // Entire range covered by VMAs with correct permissions.
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
        // terminal_printf("[CopyFromUser] access_ok failed src=%p size=%u\n", u_src, n);
        return n; // Return 'n' indicating all 'n' bytes failed (due to bad VMA).
    }

    // Call the assembly routine which handles potential faults.
    // It returns the number of bytes *not* copied.
    size_t not_copied = _raw_copy_from_user(k_dst, u_src, n);

    // if (not_copied > 0) {
    //     terminal_printf("[CopyFromUser] Faulted after copying %u bytes from %p\n", n - not_copied, u_src);
    // }

    return not_copied; // 0 on success, >0 on partial copy due to fault
}

/**
 * @brief Copies 'n' bytes from kernelspace 'k_src' to userspace 'u_dst'.
 * Handles page faults using assembly helper and exception table.
 */
size_t copy_to_user(void *u_dst, const void *k_src, size_t n) {
    // Check basic conditions
    if (n == 0) return 0;
    if (!k_src) return n; // Invalid kernel buffer

    // Check VMA permissions *before* attempting the potentially faulting copy.
    if (!access_ok(VERIFY_WRITE, u_dst, n)) {
        // terminal_printf("[CopyToUser] access_ok failed dst=%p size=%u\n", u_dst, n);
        return n; // Return 'n' indicating all 'n' bytes failed (due to bad VMA).
    }

    // Call the assembly routine which handles potential faults.
    size_t not_copied = _raw_copy_to_user(u_dst, k_src, n);

    // if (not_copied > 0) {
    //     terminal_printf("[CopyToUser] Faulted after copying %u bytes to %p\n", n - not_copied, u_dst);
    // }

    return not_copied; // 0 on success, >0 on partial copy due to fault
}