/**
 * @file uaccess.c
 * @brief User Space Memory Access C Implementation
 *
 * Provides C wrapper functions for checking user memory accessibility (`access_ok`)
 * and initiating safe copies between kernel and user space (`copy_from_user`,
 * `copy_to_user`). Relies on assembly helpers and the exception table mechanism
 * to handle page faults during access.
 */

 #include "uaccess.h"
 #include "paging.h"     // For KERNEL_SPACE_VIRT_START definition
 #include "process.h"    // For get_current_process(), pcb_t
 #include "mm.h"         // For mm_struct_t, vma_struct_t, find_vma, VM_READ, VM_WRITE
 #include "assert.h"     // For KERNEL_ASSERT
 #include "fs_errno.h"   // For error codes like EFAULT (though not returned directly here)
 #include "debug.h"      // For DEBUG_PRINTK
 
 // --- Debug Configuration ---
 #define DEBUG_UACCESS 0 // Set to 1 to enable uaccess debug messages
 
 #if DEBUG_UACCESS
 #define UACCESS_DEBUG_PRINTK(fmt, ...) DEBUG_PRINTK("[Uaccess] " fmt, ##__VA_ARGS__)
 #else
 #define UACCESS_DEBUG_PRINTK(fmt, ...) ((void)0)
 #endif
 
 /**
  * @brief Checks if a userspace memory range is potentially accessible based on VMAs.
  *
  * Verifies that the given user virtual address range [uaddr, uaddr+size) falls
  * entirely within valid userspace addresses and is covered by VMAs associated
  * with the current process, possessing the required read/write permissions.
  *
  * @param type Verification type: VERIFY_READ, VERIFY_WRITE, or (VERIFY_READ | VERIFY_WRITE).
  * @param uaddr_void The starting user virtual address.
  * @param size The size of the memory range in bytes.
  * @return true if the range seems valid based on VMA checks, false otherwise.
  * @see uaccess.h for detailed explanation and warnings.
  */
 bool access_ok(int type, const void *uaddr_void, size_t size) {
     // Cast to 32-bit address for calculation. Using uintptr_t might be slightly cleaner conceptually.
     uintptr_t uaddr = (uintptr_t)uaddr_void;
     uintptr_t end_addr;
 
     UACCESS_DEBUG_PRINTK("access_ok(type=%d, uaddr=%p, size=%u) called\n", type, uaddr_void, size);
 
     // --- 1. Basic Pointer and Range Checks ---
     if (size == 0) {
         return true; // Zero size is always OK, even with NULL pointer.
     }
     // Check for NULL pointer only if size > 0.
     if (!uaddr_void) {
         UACCESS_DEBUG_PRINTK(" -> Denied: NULL pointer with size > 0\n");
         return false;
     }
     // Check if start address is in kernel space.
     #ifndef KERNEL_SPACE_VIRT_START
     #error "KERNEL_SPACE_VIRT_START is not defined! Check paging.h or architecture constants."
     #define KERNEL_SPACE_VIRT_START 0xC0000000 // Define a default if necessary, but should be elsewhere
     #endif
     if (uaddr >= KERNEL_SPACE_VIRT_START) {
          UACCESS_DEBUG_PRINTK(" -> Denied: Address %p is in kernel space (>= %p)\n", uaddr_void, (void*)KERNEL_SPACE_VIRT_START);
          return false;
     }
     // Check for arithmetic overflow when calculating end address.
     if (__builtin_add_overflow(uaddr, size, &end_addr)) {
         UACCESS_DEBUG_PRINTK(" -> Denied: Address range %p + %u overflows\n", uaddr_void, size);
         return false;
     }
     // Check if the range ends in kernel space or wraps around.
     // end_addr points *after* the range, so check > KERNEL_SPACE_VIRT_START.
     if (end_addr > KERNEL_SPACE_VIRT_START || end_addr <= uaddr) {
          UACCESS_DEBUG_PRINTK(" -> Denied: Address range [%p - %p) crosses kernel boundary or wraps\n", uaddr_void, (void*)end_addr);
          return false;
     }
 
     // --- 2. VMA Checks ---
     pcb_t *current_proc = get_current_process();
     if (!current_proc || !current_proc->mm) {
         // Cannot verify VMAs without process context. Safer to deny access.
         UACCESS_DEBUG_PRINTK(" -> Denied: No current process or mm_struct for VMA check.\n");
         return false;
     }
     mm_struct_t *mm = current_proc->mm;
     KERNEL_ASSERT(mm != NULL, "Process has NULL mm_struct"); // Should be caught above, but assert anyway
 
     // Iterate through the required range, checking VMA coverage and permissions for each part.
     uintptr_t current_check_addr = uaddr;
     while (current_check_addr < end_addr) {
         vma_struct_t *vma = find_vma(mm, current_check_addr);
 
         // Check if address falls within a VMA
         if (!vma || current_check_addr < vma->vm_start) {
             // Address is not covered by any VMA or falls in a gap before a VMA.
             UACCESS_DEBUG_PRINTK(" -> Denied: Address 0x%lx not covered by a VMA.\n", (unsigned long)current_check_addr);
             return false;
         }
         // Note: find_vma should return the VMA containing the address, so current_check_addr should be >= vma->vm_start.
         // KERNEL_ASSERT(current_check_addr >= vma->vm_start, "find_vma returned inconsistent VMA");
 
         // Check VMA permissions against required type
         bool read_needed = (type & VERIFY_READ);
         bool write_needed = (type & VERIFY_WRITE);
 
         if (read_needed && !(vma->vm_flags & VM_READ)) {
              UACCESS_DEBUG_PRINTK(" -> Denied: VMA [%p-%p) lacks READ permission for address 0x%lx.\n",
                                (void*)vma->vm_start, (void*)vma->vm_end, (unsigned long)current_check_addr);
             return false;
         }
         if (write_needed && !(vma->vm_flags & VM_WRITE)) {
             UACCESS_DEBUG_PRINTK(" -> Denied: VMA [%p-%p) lacks WRITE permission for address 0x%lx.\n",
                                (void*)vma->vm_start, (void*)vma->vm_end, (unsigned long)current_check_addr);
             return false;
         }
 
         // Advance check address to the start of the next VMA or the end of the requested range.
         uintptr_t next_check_boundary = vma->vm_end; // vm_end is the first address *outside* the VMA.
 
         // If the requested range ends within this VMA, only check up to the requested end.
         if (next_check_boundary > end_addr) {
             next_check_boundary = end_addr;
         }
 
         // Prevent infinite loop if VMA logic is flawed (e.g., vm_end <= vm_start or find_vma loops)
         KERNEL_ASSERT(next_check_boundary > current_check_addr, "VMA check loop condition error");
 
         current_check_addr = next_check_boundary;
     }
 
     // If the loop completes, the entire range [uaddr, end_addr) is covered by VMAs
     // with the necessary permissions.
     UACCESS_DEBUG_PRINTK(" -> OK\n");
     return true;
 }
 
 
 /**
  * @brief Copies a block of memory from userspace to kernelspace.
  * @see uaccess.h for details.
  */
 size_t copy_from_user(void *k_dst, const void *u_src, size_t n) {
     UACCESS_DEBUG_PRINTK("copy_from_user(k_dst=%p, u_src=%p, n=%u)\n", k_dst, u_src, n);
     // --- Basic sanity checks ---
     if (n == 0) return 0;
     KERNEL_ASSERT(k_dst != NULL, "copy_from_user called with NULL kernel destination");
     // Ensure kernel destination doesn't accidentally point to user space
     KERNEL_ASSERT((uintptr_t)k_dst >= KERNEL_SPACE_VIRT_START || n == 0, "Kernel destination is in user space!");
 
     // --- Permission Check ---
     // Check VMA read permissions *before* attempting the potentially faulting copy.
     if (!access_ok(VERIFY_READ, u_src, n)) {
         UACCESS_DEBUG_PRINTK(" -> access_ok failed, returning n=%u\n", n);
         return n; // Indicate all 'n' bytes failed due to bad VMA/args.
     }
 
     // --- Perform Raw Copy ---
     // Call the assembly routine which handles potential faults via exception table.
     // Returns the number of bytes *not* copied.
     size_t not_copied = _raw_copy_from_user(k_dst, u_src, n);
 
     // --- Result Handling ---
     if (not_copied > 0 && not_copied <= n) {
          UACCESS_DEBUG_PRINTK(" -> Faulted after copying %u bytes from %p (requested %u). Returning %u.\n",
                               n - not_copied, u_src, n, not_copied);
     } else if (not_copied > n) {
         // This indicates an internal error in the assembly routine or fault handler logic.
         UACCESS_DEBUG_PRINTK(" -> FATAL ERROR: _raw_copy_from_user returned %u (more than requested %u)!\n", not_copied, n);
         KERNEL_PANIC_HALT("Inconsistent return value from _raw_copy_from_user");
         return n; // Return n as a safe fallback, though panic is likely better.
     } else {
          UACCESS_DEBUG_PRINTK(" -> Success (copied %u bytes). Returning 0.\n", n);
     }
 
     return not_copied; // 0 on success, >0 on partial copy due to fault
 }
 
 /**
  * @brief Copies a block of memory from kernelspace to userspace.
  * @see uaccess.h for details.
  */
 size_t copy_to_user(void *u_dst, const void *k_src, size_t n) {
      UACCESS_DEBUG_PRINTK("copy_to_user(u_dst=%p, k_src=%p, n=%u)\n", u_dst, k_src, n);
     // --- Basic sanity checks ---
     if (n == 0) return 0;
     KERNEL_ASSERT(k_src != NULL, "copy_to_user called with NULL kernel source");
     // Ensure kernel source doesn't accidentally point to user space
     KERNEL_ASSERT((uintptr_t)k_src >= KERNEL_SPACE_VIRT_START || n == 0, "Kernel source is in user space!");
 
 
     // --- Permission Check ---
     // Check VMA write permissions *before* attempting the potentially faulting copy.
     if (!access_ok(VERIFY_WRITE, u_dst, n)) {
         UACCESS_DEBUG_PRINTK(" -> access_ok failed, returning n=%u\n", n);
         return n; // Indicate all 'n' bytes failed due to bad VMA/args.
     }
 
     // --- Perform Raw Copy ---
     // Call the assembly routine which handles potential faults via exception table.
     size_t not_copied = _raw_copy_to_user(u_dst, k_src, n);
 
     // --- Result Handling ---
     if (not_copied > 0 && not_copied <= n) {
          UACCESS_DEBUG_PRINTK(" -> Faulted after copying %u bytes to %p (requested %u). Returning %u.\n",
                               n - not_copied, u_dst, n, not_copied);
     } else if (not_copied > n) {
         // Internal error check
         UACCESS_DEBUG_PRINTK(" -> FATAL ERROR: _raw_copy_to_user returned %u (more than requested %u)!\n", not_copied, n);
         KERNEL_PANIC_HALT("Inconsistent return value from _raw_copy_to_user");
         return n;
     } else {
         UACCESS_DEBUG_PRINTK(" -> Success (copied %u bytes). Returning 0.\n", n);
     }
 
     return not_copied; // 0 on success, >0 on partial copy due to fault
 }