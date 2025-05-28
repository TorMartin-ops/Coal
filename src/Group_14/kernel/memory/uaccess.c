/**
 * @file uaccess.c
 * @brief User Space Memory Access C Implementation
 * @version 2.1 - Using opaque pointer types from uaccess.h in function signatures.
 */

// --- Includes ---
#include <kernel/memory/uaccess.h>
#include <kernel/memory/paging.h>
#include <kernel/process/process.h>
#include <kernel/memory/mm.h>
#include <kernel/lib/assert.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/core/debug.h>  // Assuming DEBUG_PRINTK might be here
#include <kernel/drivers/display/serial.h>
#include <libc/stdint.h>
#include <libc/stddef.h>
#include <libc/stdbool.h>
#include <kernel/lib/string.h> // For strcmp (if used in debug macros)

// ... (DEBUG_UACCESS_SERIAL macros can remain as they were) ...
#define DEBUG_UACCESS_SERIAL 0
#if DEBUG_UACCESS_SERIAL
static inline void serial_print_hex_uaccess(uintptr_t n) { /* ... */ }
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
#define UACCESS_SERIAL_LOG(msg) ((void)0)
#define UACCESS_SERIAL_LOG_ADDR(msg, addr) ((void)0)
#define UACCESS_SERIAL_LOG_RANGE(msg, start, end) ((void)0)
#define UACCESS_SERIAL_LOG_VMA(vma) ((void)0)
#endif


bool access_ok(int type, const_userptr_t uaddr_user, size_t size) {
    uintptr_t uaddr = (uintptr_t)uaddr_user;
    uintptr_t end_addr;

    UACCESS_SERIAL_LOG("Enter access_ok");
    UACCESS_SERIAL_LOG_ADDR("  Type:", type);
    UACCESS_SERIAL_LOG_ADDR("  Addr:", uaddr);
    UACCESS_SERIAL_LOG_ADDR("  Size:", size);


    if (size == 0) {
        UACCESS_SERIAL_LOG("  -> OK (size is 0)");
        return true;
    }
    if (!uaddr_user) { // Check the opaque pointer
        UACCESS_SERIAL_LOG("  -> Denied: NULL pointer");
        return false;
    }
    if (uaddr >= KERNEL_SPACE_VIRT_START) {
        UACCESS_SERIAL_LOG_ADDR("  -> Denied: Address in kernel space", uaddr);
        return false;
    }
    if (__builtin_add_overflow(uaddr, size, &end_addr)) {
        UACCESS_SERIAL_LOG_ADDR("  -> Denied: Address range overflow", uaddr);
        return false;
    }
    if (end_addr > KERNEL_SPACE_VIRT_START || end_addr <= uaddr) {
        UACCESS_SERIAL_LOG_RANGE("  -> Denied: Address range crosses kernel boundary or wraps", uaddr, end_addr);
        return false;
    }
    UACCESS_SERIAL_LOG_RANGE("  Basic checks passed. Range:", uaddr, end_addr);

    pcb_t *current_proc = get_current_process();
    if (!current_proc || !current_proc->mm) {
        UACCESS_SERIAL_LOG("  -> Denied: No current process or mm_struct for VMA check.");
        return false;
    }
    mm_struct_t *mm = current_proc->mm;
    // KERNEL_ASSERT(mm != NULL, "Process has NULL mm_struct"); // Already covered by !current_proc->mm

    UACCESS_SERIAL_LOG_ADDR("  Checking VMAs for PID:", current_proc->pid);

    uintptr_t current_check_addr = uaddr;
    while (current_check_addr < end_addr) {
        UACCESS_SERIAL_LOG_ADDR("  Checking VMA coverage starting at:", current_check_addr);
        vma_struct_t *vma = find_vma(mm, current_check_addr);

        if (!vma || current_check_addr < vma->vm_start) {
            UACCESS_SERIAL_LOG_ADDR("  -> Denied: Address not covered by any VMA", current_check_addr);
            return false;
        }
        UACCESS_SERIAL_LOG_VMA(vma);

        // BUG_ON(vma->vm_end <= vma->vm_start); // As per review suggestion
        KERNEL_ASSERT(vma->vm_end > vma->vm_start, "VMA has vm_end <= vm_start in access_ok");


        bool access_granted = true;
        if ((type & VERIFY_READ) && !(vma->vm_flags & VM_READ)) {
            access_granted = false;
            UACCESS_SERIAL_LOG("  -> Denied: VMA lacks READ permission.");
        }
        if ((type & VERIFY_WRITE) && !(vma->vm_flags & VM_WRITE)) {
            access_granted = false;
            UACCESS_SERIAL_LOG("  -> Denied: VMA lacks WRITE permission.");
        }
        if (!access_granted) return false;

        UACCESS_SERIAL_LOG("  Permissions OK for this VMA segment.");
        current_check_addr = vma->vm_end;
        UACCESS_SERIAL_LOG_ADDR("  Advanced check address to:", current_check_addr);
    }

    UACCESS_SERIAL_LOG("  -> OK (VMA checks passed for entire range)");
    return true;
}

size_t copy_from_user(kernelptr_t k_dst, const_userptr_t u_src, size_t n) {
    if (n == 0) return 0;
    KERNEL_ASSERT(k_dst != NULL, "copy_from_user: k_dst is NULL");
    KERNEL_ASSERT((uintptr_t)k_dst >= KERNEL_SPACE_VIRT_START, "Kernel destination for copy_from_user is in user space!");
    // u_src NULL check is implicitly handled by access_ok with the nonnull attribute or explicit check

    if (!access_ok(VERIFY_READ, u_src, n)) {
        return n;
    }
    // The assembly function _raw_copy_from_user still takes void* internally
    return _raw_copy_from_user((void*)k_dst, (const void*)u_src, n);
}

size_t copy_to_user(userptr_t u_dst, const_kernelptr_t k_src, size_t n) {
    if (n == 0) return 0;
    KERNEL_ASSERT(k_src != NULL, "copy_to_user: k_src is NULL");
    KERNEL_ASSERT((uintptr_t)k_src >= KERNEL_SPACE_VIRT_START, "Kernel source for copy_to_user is in user space!");
    // u_dst NULL check is implicitly handled by access_ok with the nonnull attribute or explicit check

    if (!access_ok(VERIFY_WRITE, u_dst, n)) {
        return n;
    }
    // The assembly function _raw_copy_to_user still takes void* internally
    return _raw_copy_to_user((void*)u_dst, (const void*)k_src, n);
}