#include "uaccess.h"
#include "paging.h"     // For KERNEL_SPACE_VIRT_START
#include "process.h"    // For get_current_process()
#include "mm.h"         // For find_vma, VM_READ, VM_WRITE
#include "terminal.h"   // For debugging output
#include "assert.h"     // For KERNEL_ASSERT
#include "string.h"     // For memcpy (in the unsafe stub)

/**
 * @brief Checks if a userspace memory range is accessible for read/write.
 * (Implementation of the function declared in uaccess.h)
 */
bool access_ok(int type, const void *uaddr_void, size_t size) {
    uintptr_t uaddr = (uintptr_t)uaddr_void;
    uintptr_t end_addr;

    // Basic checks: NULL pointer, kernel address space, size overflow
    if (!uaddr_void) {
        // terminal_write("[access_ok] Error: NULL pointer provided.\n");
        return false;
    }
    if (uaddr >= KERNEL_SPACE_VIRT_START) {
        // terminal_printf("[access_ok] Error: Start address 0x%lx is not in user space.\n", (unsigned long)uaddr);
        return false;
    }
    if (size == 0) {
        return true; // Zero size range is trivially OK
    }
    // Check for overflow when calculating end address
    if (__builtin_add_overflow(uaddr, size, &end_addr)) {
        // terminal_printf("[access_ok] Error: Overflow calculating end address for 0x%lx + %lu\n", (unsigned long)uaddr, (unsigned long)size);
        return false;
    }
    // Check if end address (exclusive) crosses into kernel space or wraps around
    if (end_addr > KERNEL_SPACE_VIRT_START || end_addr <= uaddr) {
         // terminal_printf("[access_ok] Error: Range [0x%lx - 0x%lx) crosses boundary or wraps.\n", (unsigned long)uaddr, (unsigned long)end_addr);
         return false;
    }

    // Get current process and its memory map
    pcb_t *current_proc = get_current_process();
    if (!current_proc || !current_proc->mm) {
        // terminal_printf("[access_ok] Error: No current process or mm_struct for validation.\n");
        return false; // Cannot verify without process context
    }
    mm_struct_t *mm = current_proc->mm;

    // Check VMA coverage and permissions for the entire range
    uintptr_t current_check_addr = uaddr;
    while (current_check_addr < end_addr) {
        vma_struct_t *vma = find_vma(mm, current_check_addr);

        // Check if address is covered by *any* VMA
        if (!vma || current_check_addr < vma->vm_start) {
            // terminal_printf("[access_ok] Error: Address 0x%lx not covered by any VMA.\n", (unsigned long)current_check_addr);
            return false; // Address is outside any defined user memory region
        }

        // Check permissions required by 'type' against VMA flags
        uint32_t vma_flags = vma->vm_flags;
        // Verify read permission if VERIFY_READ is requested
        bool read_ok = !(type & VERIFY_READ) || (vma_flags & VM_READ);
        // Verify write permission if VERIFY_WRITE is requested
        bool write_ok = !(type & VERIFY_WRITE) || (vma_flags & VM_WRITE);


        if (!read_ok || !write_ok) {
            // terminal_printf("[access_ok] Error: VMA [%#lx-%#lx) lacks required permission(s) (R:%d, W:%d requested type %d) for address 0x%lx.\n",
            //                (unsigned long)vma->vm_start, (unsigned long)vma->vm_end,
            //                (vma_flags & VM_READ) ? 1 : 0, (vma_flags & VM_WRITE) ? 1: 0, type,
            //                (unsigned long)current_check_addr);
            return false;
        }

        // Advance check address to the end of this VMA, capped by the requested end_addr
        uintptr_t next_check_boundary = vma->vm_end;
        if (next_check_boundary > end_addr) {
            next_check_boundary = end_addr;
        }
        // Basic check to prevent infinite loops if vma->vm_end isn't advancing
        if (next_check_boundary <= current_check_addr) {
             // terminal_printf("[access_ok] Error: VMA end address logic error.\n");
             // This likely indicates a corrupted VMA list or logic error in find_vma/insert_vma
             return false;
        }
        current_check_addr = next_check_boundary;
    }

    // If loop completed, the entire range is covered by VMAs with sufficient permissions
    return true;
}


// ========================================================================
// WARNING: UNSAFE STUB IMPLEMENTATION of copy_from_user
// ========================================================================
// A production kernel MUST replace this with an architecture-specific,
// fault-handling routine (e.g., using assembly and exception tables).
// This version will cause a KERNEL PAGE FAULT if u_src points to an
// unmapped page within an otherwise valid VMA range.
// ========================================================================
size_t copy_from_user(void *k_dst, const void *u_src, size_t n) {
    // Basic checks
    if (!k_dst || !u_src) return n;
    if (n == 0) return 0;

    // Use access_ok for validation (caller should ideally do this too)
    // This check is crucial but does NOT guarantee pages are mapped/readable.
    if (!access_ok(VERIFY_READ, u_src, n)) {
         return n; // Indicate all bytes failed
    }

    // --- THE UNSAFE PART ---
    // This memcpy will PANIC THE KERNEL if a page fault occurs reading u_src.
    memcpy(k_dst, u_src, n);
    // --- END UNSAFE PART ---

    // If memcpy didn't fault, this stub returns 0 (success).
    // A real implementation returns the number of bytes *not* copied if a fault occurred.
    return 0;
}

// ========================================================================
// WARNING: UNSAFE STUB IMPLEMENTATION of copy_to_user
// ========================================================================
// A production kernel MUST replace this with an architecture-specific,
// fault-handling routine (e.g., using assembly and exception tables).
// This version will cause a KERNEL PAGE FAULT if u_dst points to an
// unmapped or read-only page within an otherwise valid VMA range.
// ========================================================================
size_t copy_to_user(void *u_dst, const void *k_src, size_t n) {
    // Basic checks
    if (!k_src || !u_dst) return n;
     if (n == 0) return 0;

    // Use access_ok for validation (caller should ideally do this too)
    // This check is crucial but does NOT guarantee pages are mapped/writable.
    if (!access_ok(VERIFY_WRITE, u_dst, n)) {
        return n; // Indicate all bytes failed
    }

    // --- THE UNSAFE PART ---
    // This memcpy will PANIC THE KERNEL if a page fault occurs writing to u_dst
    // (e.g., page not present, write protection violation).
    memcpy(u_dst, k_src, n);
    // --- END UNSAFE PART ---

    // If memcpy didn't fault, this stub returns 0 (success).
    // A real implementation returns the number of bytes *not* copied if a fault occurred.
    return 0;
}