/**
 * @file uaccess.c
 * @brief User Space Memory Access C Implementation
 */

// --- Includes (Ensure these are present in your uaccess.c) ---
#include "uaccess.h"
#include "paging.h"     // For KERNEL_SPACE_VIRT_START definition
#include "process.h"    // For get_current_process(), pcb_t
#include "mm.h"         // For mm_struct_t, vma_struct_t, find_vma, VM_READ, VM_WRITE, VM_EXEC
#include "assert.h"     // For KERNEL_ASSERT
#include "fs_errno.h"   // For error codes like EFAULT
#include "debug.h"      // For DEBUG_PRINTK (if used)
#include "serial.h"     // Using serial logging for reliability during debugging
#include <libc/stdint.h> // For uintptr_t
#include <libc/stddef.h> // For size_t, NULL
#include <libc/stdbool.h> // For bool

// --- Debug Configuration ---
// Enable detailed VMA checking logs via serial port
#define DEBUG_UACCESS_SERIAL 0 // Set to 1 to enable detailed serial logs

#if DEBUG_UACCESS_SERIAL
// Helper for serial hex print if not already available
static inline void serial_print_hex_uaccess(uintptr_t n) {
    char buf[9];
    char hex_digits[] = "0123456789abcdef";
    buf[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        buf[i] = hex_digits[n & 0xF];
        n >>= 4;
    }
    serial_write(buf);
}
#define UACCESS_SERIAL_PRINT(str) serial_write("[Uaccess] " str)
#define UACCESS_SERIAL_PRINTF(fmt, ...) \
    do { \
        serial_write("[Uaccess] "); \
        /* Basic printf simulation for serial */ \
        /* Replace with your kernel's serial printf if available */ \
        /* This is a placeholder - needs real implementation */ \
        if (strcmp(fmt, "access_ok(type=%d, uaddr=%p, size=%zu) called") == 0) { \
            serial_write("access_ok(type="); serial_print_hex_uaccess(__VA_ARGS__); \
            serial_write(", uaddr="); serial_print_hex_uaccess(__VA_ARGS__); \
            serial_write(", size="); serial_print_hex_uaccess(__VA_ARGS__); \
            serial_write(") called\n"); \
        } else { \
             serial_write(fmt); serial_write("\n"); \
        } \
    } while (0)

#define UACCESS_SERIAL_LOG(msg) do { serial_write("[Uaccess] "); serial_write(msg); serial_write("\n"); } while(0)
#define UACCESS_SERIAL_LOG_ADDR(msg, addr) do { serial_write("[Uaccess] "); serial_write(msg); serial_write(" "); serial_print_hex_uaccess((uintptr_t)addr); serial_write("\n"); } while(0)
#define UACCESS_SERIAL_LOG_RANGE(msg, start, end) \
    do { \
        serial_write("[Uaccess] "); serial_write(msg); \
        serial_write(" ["); serial_print_hex_uaccess((uintptr_t)start); \
        serial_write(" - "); serial_print_hex_uaccess((uintptr_t)end); \
        serial_write(")\n"); \
    } while(0)
#define UACCESS_SERIAL_LOG_VMA(vma) \
    do { \
        serial_write("[Uaccess]   Found VMA: ["); serial_print_hex_uaccess(vma->vm_start); \
        serial_write("-"); serial_print_hex_uaccess(vma->vm_end); \
        serial_write(") Flags: 0x"); serial_print_hex_uaccess(vma->vm_flags); \
        serial_write("\n"); \
    } while(0)

#else
#define UACCESS_SERIAL_PRINTF(fmt, ...) ((void)0)
#define UACCESS_SERIAL_LOG(msg) ((void)0)
#define UACCESS_SERIAL_LOG_ADDR(msg, addr) ((void)0)
#define UACCESS_SERIAL_LOG_RANGE(msg, start, end) ((void)0)
#define UACCESS_SERIAL_LOG_VMA(vma) ((void)0)
#endif

/**
 * @brief Checks if a userspace memory range is potentially accessible based on VMAs.
 * Verifies that the given user virtual address range [uaddr, uaddr+size) falls
 * entirely within valid userspace addresses and is covered by VMAs associated
 * with the current process, possessing the required read/write permissions.
 *
 * @param type Verification type: VERIFY_READ or VERIFY_WRITE.
 * @param uaddr_void The starting user virtual address.
 * @param size The size of the memory range in bytes.
 * @return true if the range is valid and covered by VMAs with required permissions, false otherwise.
 */
bool access_ok(int type, const void *uaddr_void, size_t size) {
    // Cast to uintptr_t for calculations
    uintptr_t uaddr = (uintptr_t)uaddr_void;
    uintptr_t end_addr;

    // Log entry parameters
    UACCESS_SERIAL_LOG("Enter access_ok");
    UACCESS_SERIAL_LOG_ADDR("  Type:", type);
    UACCESS_SERIAL_LOG_ADDR("  Addr:", uaddr);
    UACCESS_SERIAL_LOG_ADDR("  Size:", size);

    // --- 1. Basic Pointer and Range Checks ---
    if (size == 0) {
        UACCESS_SERIAL_LOG("  -> OK (size is 0)");
        return true; // Zero size is always OK.
    }
    if (!uaddr_void) {
        UACCESS_SERIAL_LOG("  -> Denied: NULL pointer");
        return false;
    }
    if (uaddr >= KERNEL_SPACE_VIRT_START) {
        UACCESS_SERIAL_LOG_ADDR("  -> Denied: Address in kernel space", uaddr);
        return false;
    }
    // Use __builtin_add_overflow for safer addition
    if (__builtin_add_overflow(uaddr, size, &end_addr)) {
        UACCESS_SERIAL_LOG_ADDR("  -> Denied: Address range overflow", uaddr);
        return false;
    }
    // end_addr points *one byte past* the desired range. Check if it crossed into kernel space or wrapped.
    // Check end_addr > KERNEL_SPACE_VIRT_START OR end_addr <= uaddr (detects wrap-around for size > 0)
    if (end_addr > KERNEL_SPACE_VIRT_START || end_addr <= uaddr) {
        UACCESS_SERIAL_LOG_RANGE("  -> Denied: Address range crosses kernel boundary or wraps", uaddr, end_addr);
        return false;
    }
    UACCESS_SERIAL_LOG_RANGE("  Basic checks passed. Range:", uaddr, end_addr);


    // --- 2. VMA Checks ---
    pcb_t *current_proc = get_current_process();
    if (!current_proc || !current_proc->mm) {
        UACCESS_SERIAL_LOG("  -> Denied: No current process or mm_struct for VMA check.");
        return false;
    }
    mm_struct_t *mm = current_proc->mm;
    KERNEL_ASSERT(mm != NULL, "Process has NULL mm_struct"); // Should not happen if current_proc->mm check passed

    UACCESS_SERIAL_LOG_ADDR("  Checking VMAs for PID:", current_proc->pid);
    UACCESS_SERIAL_LOG_ADDR("  mm_struct at:", (uintptr_t)mm);

    // Iterate through the required range [uaddr, end_addr), checking VMA coverage page by page or VMA by VMA.
    // Checking VMA by VMA is more efficient.
    uintptr_t current_check_addr = uaddr;
    while (current_check_addr < end_addr) {
        UACCESS_SERIAL_LOG_ADDR("  Checking VMA coverage starting at:", current_check_addr);
        vma_struct_t *vma = find_vma(mm, current_check_addr); // find_vma should find the VMA containing or immediately after current_check_addr

        // Check if address falls within a valid VMA
        if (!vma || current_check_addr < vma->vm_start) {
            // Address is not covered by any VMA or falls in a gap before the found VMA.
            UACCESS_SERIAL_LOG_ADDR("  -> Denied: Address not covered by any VMA", current_check_addr);
            return false;
        }

        // Log the found VMA details
        UACCESS_SERIAL_LOG_VMA(vma);

        // Check VMA permissions against required type (VERIFY_READ or VERIFY_WRITE)
        bool read_needed = (type == VERIFY_READ);
        bool write_needed = (type == VERIFY_WRITE);

        UACCESS_SERIAL_LOG_ADDR("  Checking permissions: Read needed?", read_needed);
        UACCESS_SERIAL_LOG_ADDR("  Checking permissions: Write needed?", write_needed);

        if (read_needed && !(vma->vm_flags & VM_READ)) {
             UACCESS_SERIAL_LOG("  -> Denied: VMA lacks READ permission (VM_READ flag missing).");
            return false;
        }
        if (write_needed && !(vma->vm_flags & VM_WRITE)) {
            UACCESS_SERIAL_LOG("  -> Denied: VMA lacks WRITE permission (VM_WRITE flag missing).");
            return false;
        }

        UACCESS_SERIAL_LOG("  Permissions OK for this VMA.");

        // Advance check address to the end of the current VMA, or the end of the
        // requested range, whichever comes first. This correctly handles ranges
        // spanning multiple VMAs.
        uintptr_t next_check_addr = vma->vm_end;

        // Basic sanity check to prevent infinite loops if VMA logic is flawed (e.g., vm_end <= vm_start)
        if (next_check_addr <= current_check_addr) {
            UACCESS_SERIAL_LOG_ADDR("  -> INTERNAL ERROR: VMA end <= start. VMA:", (uintptr_t)vma);
            UACCESS_SERIAL_LOG_RANGE("  VMA Range:", vma->vm_start, vma->vm_end);
            UACCESS_SERIAL_LOG_ADDR("  Current Check Addr:", current_check_addr);
            KERNEL_PANIC_HALT("VMA check loop error in access_ok");
            return false; // Should not be reached
        }

        // Move to the start of the next VMA to check, or the end of the requested range
        current_check_addr = next_check_addr;
        UACCESS_SERIAL_LOG_ADDR("  Advanced check address to:", current_check_addr);

    } // End while loop

    // If the loop completes, the entire range [uaddr, end_addr) is covered by VMAs
    // with the necessary permissions.
    UACCESS_SERIAL_LOG("  -> OK (VMA checks passed for entire range)");
    return true;
}

// --- copy_from_user and copy_to_user remain unchanged ---
// (They already call the enhanced access_ok)

/**
 * @brief Copies a block of memory from userspace to kernelspace.
 * Performs access checks before attempting the raw copy.
 */
size_t copy_from_user(void *k_dst, const void *u_src, size_t n) {
    // Basic sanity checks
    if (n == 0) return 0;
    KERNEL_ASSERT(k_dst != NULL, "copy_from_user called with NULL kernel destination");
    KERNEL_ASSERT((uintptr_t)k_dst >= KERNEL_SPACE_VIRT_START || n == 0, "Kernel destination is in user space!");

    // Permission Check (using enhanced access_ok)
    if (!access_ok(VERIFY_READ, u_src, n)) {
        return n; // Indicate all 'n' bytes failed (permission denied)
    }

    // Perform Raw Copy (Assembly handles faults)
    size_t not_copied = _raw_copy_from_user(k_dst, u_src, n);

    // Result Handling (optional logging)
    #if DEBUG_UACCESS_SERIAL
    if (not_copied > 0 && not_copied <= n) {
        UACCESS_SERIAL_LOG_ADDR("copy_from_user: Faulted after copying bytes:", n - not_copied);
        UACCESS_SERIAL_LOG_ADDR("  Source:", (uintptr_t)u_src);
        UACCESS_SERIAL_LOG_ADDR("  Returning (not copied):", not_copied);
    } else if (not_copied == 0) {
        UACCESS_SERIAL_LOG_ADDR("copy_from_user: Success (copied bytes):", n);
    }
    #endif

    return not_copied; // 0 on success, >0 on partial copy due to fault
}

/**
 * @brief Copies a block of memory from kernelspace to userspace.
 * Performs access checks before attempting the raw copy.
 */
size_t copy_to_user(void *u_dst, const void *k_src, size_t n) {
    // Basic sanity checks
    if (n == 0) return 0;
    KERNEL_ASSERT(k_src != NULL, "copy_to_user called with NULL kernel source");
    KERNEL_ASSERT((uintptr_t)k_src >= KERNEL_SPACE_VIRT_START || n == 0, "Kernel source is in user space!");

    // Permission Check (using enhanced access_ok)
    if (!access_ok(VERIFY_WRITE, u_dst, n)) {
        return n; // Indicate all 'n' bytes failed (permission denied)
    }

    // Perform Raw Copy (Assembly handles faults)
    size_t not_copied = _raw_copy_to_user(u_dst, k_src, n);

    // Result Handling (optional logging)
    #if DEBUG_UACCESS_SERIAL
    if (not_copied > 0 && not_copied <= n) {
        UACCESS_SERIAL_LOG_ADDR("copy_to_user: Faulted after copying bytes:", n - not_copied);
        UACCESS_SERIAL_LOG_ADDR("  Destination:", (uintptr_t)u_dst);
        UACCESS_SERIAL_LOG_ADDR("  Returning (not copied):", not_copied);
    } else if (not_copied == 0) {
        UACCESS_SERIAL_LOG_ADDR("copy_to_user: Success (copied bytes):", n);
    }
    #endif

    return not_copied; // 0 on success, >0 on partial copy due to fault
}
