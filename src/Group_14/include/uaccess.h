/**
 * @file uaccess.h
 * @brief User Space Memory Access Functions
 *
 * Provides functions for safely accessing memory in user space from the kernel,
 * handling potential page faults. Also includes helpers for checking access rights.
 */

 #ifndef UACCESS_H
 #define UACCESS_H
 
 #include <libc/stddef.h> // For size_t
 #include <libc/stdbool.h> // For bool
 #include <libc/stdint.h> // For uintptr_t, uint32_t
 
 // --- Verification Flags for access_ok ---
 
 /** @brief Flag indicating read access needs to be checked. */
 #define VERIFY_READ  1
 /** @brief Flag indicating write access needs to be checked. */
 #define VERIFY_WRITE 2
 
 // --- Core Access Functions ---
 
 /**
  * @brief Checks if a userspace memory range is potentially accessible.
  *
  * This function performs preliminary checks based on virtual memory areas (VMAs)
  * defined for the current process. It verifies:
  * 1. The address range does not fall within kernel space.
  * 2. The range does not wrap around the address space.
  * 3. The entire range is covered by one or more VMAs belonging to the current process.
  * 4. The VMAs covering the range have the required read/write permissions (`type`).
  *
  * @warning This function only checks VMA permissions. It does *not* guarantee
  * that the underlying physical pages are actually mapped or present.
  * Actual access must be performed using `copy_from_user`/`copy_to_user`,
  * which rely on the page fault handler and exception table to manage faults.
  *
  * @param type Verification type: `VERIFY_READ`, `VERIFY_WRITE`, or (`VERIFY_READ` | `VERIFY_WRITE`).
  * @param uaddr The starting user virtual address.
  * @param size The size of the memory range in bytes.
  * @return `true` if the range seems valid based on VMA checks, `false` otherwise.
  * Returns `false` if called without a valid process context (`get_current_process()` fails).
  */
 bool access_ok(int type, const void *uaddr, size_t size);
 
 /**
  * @brief Copies a block of memory from userspace to kernelspace.
  *
  * Safely copies `n` bytes from the user virtual address `u_src` to the kernel
  * virtual address `k_dst`. It first checks VMA permissions using `access_ok`.
  * The actual copy is performed by an optimized assembly routine (`_raw_copy_from_user`)
  * that leverages the kernel's exception table to handle page faults that might occur
  * while reading from `u_src`.
  *
  * @param k_dst Kernel destination buffer pointer. Must point to valid kernel memory.
  * @param u_src User source buffer virtual address.
  * @param n Number of bytes to copy.
  * @return 0 on complete success.
  * @return The number of bytes *that could not be copied* (i.e., `n` - bytes_copied)
  * if a fault occurred during the copy.
  * @return `n` if the initial `access_ok` check fails or if `k_dst` is NULL.
  */
 size_t copy_from_user(void *k_dst, const void *u_src, size_t n);
 
 /**
  * @brief Copies a block of memory from kernelspace to userspace.
  *
  * Safely copies `n` bytes from the kernel virtual address `k_src` to the user
  * virtual address `u_dst`. It first checks VMA permissions using `access_ok`.
  * The actual copy is performed by an optimized assembly routine (`_raw_copy_to_user`)
  * that leverages the kernel's exception table to handle page faults that might occur
  * while writing to `u_dst`.
  *
  * @param u_dst User destination buffer virtual address.
  * @param k_src Kernel source buffer pointer. Must point to valid kernel memory.
  * @param n Number of bytes to copy.
  * @return 0 on complete success.
  * @return The number of bytes *that could not be copied* (i.e., `n` - bytes_copied)
  * if a fault occurred during the copy.
  * @return `n` if the initial `access_ok` check fails or if `k_src` is NULL.
  */
 size_t copy_to_user(void *u_dst, const void *k_src, size_t n);
 
 
 // --- Assembly Helper Prototypes (Internal Use) ---
 // These perform the raw copy and rely on the exception table for fault handling.
 // The page fault handler must cooperate by checking the exception table
 // and modifying the EIP on the stack frame to jump to the fixup handler on fault.
 
 /**
  * @brief Raw assembly routine to copy from user to kernel.
  * @internal Used by `copy_from_user`. Should not be called directly.
  * @param k_dst Kernel destination.
  * @param u_src User source.
  * @param n Number of bytes.
  * @return Number of bytes *not* copied (0 on success, >0 on fault).
  */
 extern size_t _raw_copy_from_user(void *k_dst, const void *u_src, size_t n);
 
 /**
  * @brief Raw assembly routine to copy from kernel to user.
  * @internal Used by `copy_to_user`. Should not be called directly.
  * @param u_dst User destination.
  * @param k_src Kernel source.
  * @param n Number of bytes.
  * @return Number of bytes *not* copied (0 on success, >0 on fault).
  */
 extern size_t _raw_copy_to_user(void *u_dst, const void *k_src, size_t n);
 
 #endif // UACCESS_H