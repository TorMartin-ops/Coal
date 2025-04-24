/**
 * @file uaccess.c
 * @brief User Space Memory Access C Implementation
 */

 #include "uaccess.h"
 #include "paging.h"     // For KERNEL_SPACE_VIRT_START definition
 #include "process.h"    // For get_current_process(), pcb_t
 #include "mm.h"         // For mm_struct_t, vma_struct_t, find_vma, VM_READ, VM_WRITE
 #include "assert.h"     // For KERNEL_ASSERT
 #include "fs_errno.h"   // For error codes like EFAULT
 #include "debug.h"      // For DEBUG_PRINTK
 #include "serial.h"     // For more primitive serial logging if needed

 // --- Debug Configuration ---
 // Enable detailed VMA checking logs
 #define DEBUG_UACCESS 1 // Set to 1 to enable uaccess debug messages

 #if DEBUG_UACCESS
 #define UACCESS_DEBUG_PRINTK(fmt, ...) DEBUG_PRINTK("[Uaccess] " fmt "\n", ##__VA_ARGS__)
 #else
 #define UACCESS_DEBUG_PRINTK(fmt, ...) ((void)0)
 #endif

 /**
  * @brief Checks if a userspace memory range is potentially accessible based on VMAs.
  * RESTORED AND ENHANCED VERSION.
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

     UACCESS_DEBUG_PRINTK("access_ok(type=%d, uaddr=%p, size=%zu) called", type, uaddr_void, size);

     // --- 1. Basic Pointer and Range Checks ---
     if (size == 0) {
         UACCESS_DEBUG_PRINTK(" -> OK (size is 0)");
         return true; // Zero size is always OK.
     }
     if (!uaddr_void) {
         UACCESS_DEBUG_PRINTK(" -> Denied: NULL pointer with size > 0");
         return false;
     }
     if (uaddr >= KERNEL_SPACE_VIRT_START) {
         UACCESS_DEBUG_PRINTK(" -> Denied: Address %p is in kernel space (>= %p)", uaddr_void, (void*)KERNEL_SPACE_VIRT_START);
         return false;
     }
     if (__builtin_add_overflow(uaddr, size, &end_addr)) {
         UACCESS_DEBUG_PRINTK(" -> Denied: Address range %p + %zu overflows", uaddr_void, size);
         return false;
     }
     // end_addr points *one byte past* the desired range. Check if it crossed into kernel space or wrapped.
     if (end_addr > KERNEL_SPACE_VIRT_START || end_addr <= uaddr) { // Use end_addr <= uaddr to detect wrap-around for size > 0
         UACCESS_DEBUG_PRINTK(" -> Denied: Address range [%p - %p) crosses kernel boundary or wraps", uaddr_void, (void*)end_addr);
         return false;
     }
     UACCESS_DEBUG_PRINTK(" Basic checks passed. Range: [%p - %p)", (void*)uaddr, (void*)end_addr);


     // --- 2. VMA Checks ---
     pcb_t *current_proc = get_current_process();
     if (!current_proc || !current_proc->mm) {
         UACCESS_DEBUG_PRINTK(" -> Denied: No current process or mm_struct for VMA check.");
         // Consider panic? Access check without process context is dangerous.
         return false;
     }
     mm_struct_t *mm = current_proc->mm;
     KERNEL_ASSERT(mm != NULL, "Process has NULL mm_struct");

     UACCESS_DEBUG_PRINTK(" Checking VMAs for PID %lu, mm=%p", current_proc->pid, mm);

     // Iterate through the required range [uaddr, end_addr), checking VMA coverage.
     uintptr_t current_check_addr = uaddr;
     while (current_check_addr < end_addr) {
         UACCESS_DEBUG_PRINTK("  Checking VMA coverage for address %p...", (void*)current_check_addr);
         vma_struct_t *vma = find_vma(mm, current_check_addr); // find_vma should find the VMA containing or immediately after current_check_addr

         // Check if address falls within a valid VMA
         if (!vma || current_check_addr < vma->vm_start) {
             // Address is not covered by any VMA or falls in a gap before the found VMA.
             UACCESS_DEBUG_PRINTK("  -> Denied: Address %p not covered by any VMA.", (void*)current_check_addr);
             return false;
         }

         UACCESS_DEBUG_PRINTK("  Found VMA: [%p - %p) Flags: 0x%x", (void*)vma->vm_start, (void*)vma->vm_end, vma->vm_flags);

         // Check VMA permissions against required type (VERIFY_READ or VERIFY_WRITE)
         bool read_needed = (type == VERIFY_READ); // Simplified check (assuming type is only READ or WRITE)
         bool write_needed = (type == VERIFY_WRITE);

         if (read_needed && !(vma->vm_flags & VM_READ)) {
              UACCESS_DEBUG_PRINTK("  -> Denied: VMA lacks READ permission (VM_READ flag missing).");
             return false;
         }
         if (write_needed && !(vma->vm_flags & VM_WRITE)) {
             UACCESS_DEBUG_PRINTK("  -> Denied: VMA lacks WRITE permission (VM_WRITE flag missing).");
             return false;
         }

         UACCESS_DEBUG_PRINTK("  Permissions OK for VMA.");

         // Advance check address to the end of the current VMA, or the end of the
         // requested range, whichever comes first.
         uintptr_t next_check_addr = vma->vm_end;
         if (next_check_addr > end_addr) {
             next_check_addr = end_addr;
         }

         // Basic sanity check to prevent infinite loops if VMA logic is flawed
         if (next_check_addr <= current_check_addr) {
             UACCESS_DEBUG_PRINTK("  -> INTERNAL ERROR: VMA check loop condition error (%p <= %p). Aborting check.", (void*)next_check_addr, (void*)current_check_addr);
             KERNEL_PANIC_HALT("VMA check loop error in access_ok"); // Consider panic or error return
             return false;
         }

         current_check_addr = next_check_addr; // Move to the next address to check
     }

     // If the loop completes, the entire range [uaddr, end_addr) is covered by VMAs
     // with the necessary permissions.
     UACCESS_DEBUG_PRINTK(" -> OK (VMA checks passed for entire range)");
     return true;
 }


 /**
  * @brief Copies a block of memory from userspace to kernelspace.
  * Performs access checks before attempting the raw copy.
  */
 size_t copy_from_user(void *k_dst, const void *u_src, size_t n) {
     UACCESS_DEBUG_PRINTK("copy_from_user(k_dst=%p, u_src=%p, n=%zu)", k_dst, u_src, n);
     // Basic sanity checks
     if (n == 0) return 0;
     KERNEL_ASSERT(k_dst != NULL, "copy_from_user called with NULL kernel destination");
     KERNEL_ASSERT((uintptr_t)k_dst >= KERNEL_SPACE_VIRT_START || n == 0, "Kernel destination is in user space!");

     // Permission Check (using restored access_ok)
     UACCESS_DEBUG_PRINTK(" Calling access_ok(READ, %p, %zu)...", u_src, n);
     if (!access_ok(VERIFY_READ, u_src, n)) {
         UACCESS_DEBUG_PRINTK(" -> access_ok failed, returning n=%zu", n);
         return n; // Indicate all 'n' bytes failed (permission denied)
     }
     UACCESS_DEBUG_PRINTK(" access_ok passed.");

     // Perform Raw Copy (Assembly handles faults)
     UACCESS_DEBUG_PRINTK(" Calling _raw_copy_from_user...");
     size_t not_copied = _raw_copy_from_user(k_dst, u_src, n);
     UACCESS_DEBUG_PRINTK(" _raw_copy_from_user returned %zu (not copied)", not_copied);

     // Result Handling
     if (not_copied > 0 && not_copied <= n) {
         UACCESS_DEBUG_PRINTK(" -> Faulted after copying %zu bytes from %p. Returning %zu.", n - not_copied, u_src, not_copied);
     } else if (not_copied > n) {
         UACCESS_DEBUG_PRINTK(" -> FATAL ERROR: _raw_copy_from_user returned %zu (> requested %zu)!", not_copied, n);
         KERNEL_PANIC_HALT("Inconsistent return value from _raw_copy_from_user");
         return n; // Fallback
     } else {
         UACCESS_DEBUG_PRINTK(" -> Success (copied %zu bytes). Returning 0.", n);
     }

     return not_copied; // 0 on success, >0 on partial copy due to fault
 }

 /**
  * @brief Copies a block of memory from kernelspace to userspace.
  * Performs access checks before attempting the raw copy.
  */
 size_t copy_to_user(void *u_dst, const void *k_src, size_t n) {
     UACCESS_DEBUG_PRINTK("copy_to_user(u_dst=%p, k_src=%p, n=%zu)", u_dst, k_src, n);
     // Basic sanity checks
     if (n == 0) return 0;
     KERNEL_ASSERT(k_src != NULL, "copy_to_user called with NULL kernel source");
     KERNEL_ASSERT((uintptr_t)k_src >= KERNEL_SPACE_VIRT_START || n == 0, "Kernel source is in user space!");

     // Permission Check (using restored access_ok)
     UACCESS_DEBUG_PRINTK(" Calling access_ok(WRITE, %p, %zu)...", u_dst, n);
     if (!access_ok(VERIFY_WRITE, u_dst, n)) {
         UACCESS_DEBUG_PRINTK(" -> access_ok failed, returning n=%zu", n);
         return n; // Indicate all 'n' bytes failed (permission denied)
     }
     UACCESS_DEBUG_PRINTK(" access_ok passed.");

     // Perform Raw Copy (Assembly handles faults)
     UACCESS_DEBUG_PRINTK(" Calling _raw_copy_to_user...");
     size_t not_copied = _raw_copy_to_user(u_dst, k_src, n);
     UACCESS_DEBUG_PRINTK(" _raw_copy_to_user returned %zu (not copied)", not_copied);

     // Result Handling
     if (not_copied > 0 && not_copied <= n) {
         UACCESS_DEBUG_PRINTK(" -> Faulted after copying %zu bytes to %p. Returning %zu.", n - not_copied, u_dst, not_copied);
     } else if (not_copied > n) {
         UACCESS_DEBUG_PRINTK(" -> FATAL ERROR: _raw_copy_to_user returned %zu (> requested %zu)!", not_copied, n);
         KERNEL_PANIC_HALT("Inconsistent return value from _raw_copy_to_user");
         return n; // Fallback
     } else {
         UACCESS_DEBUG_PRINTK(" -> Success (copied %zu bytes). Returning 0.", n);
     }

     return not_copied; // 0 on success, >0 on partial copy due to fault
 }