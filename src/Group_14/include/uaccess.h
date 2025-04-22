#ifndef UACCESS_H
#define UACCESS_H

#include <libc/stddef.h> // For size_t
#include <libc/stdbool.h> // For bool
#include <libc/stdint.h> // For uintptr_t, uint32_t

// Verification type flags for access_ok
#define VERIFY_READ  1
#define VERIFY_WRITE 2

/**
 * @brief Checks if a userspace memory range is potentially accessible based on VMAs.
 * WARNING: Does NOT guarantee pages are mapped or present. This is a preliminary check.
 * Actual access is verified by copy_from/to_user which handles faults.
 *
 * @param type VERIFY_READ, VERIFY_WRITE, or (VERIFY_READ | VERIFY_WRITE).
 * @param uaddr The starting user virtual address (32-bit).
 * @param size The size of the memory range in bytes.
 * @return true if the range seems valid based on basic checks and VMA permissions, false otherwise.
 */
bool access_ok(int type, const void *uaddr, size_t size);

/**
 * @brief Copies 'n' bytes from userspace 'u_src' to kernelspace 'k_dst'.
 * Handles page faults during the copy using the exception table mechanism.
 * Uses optimized assembly routines.
 *
 * @param k_dst Kernel destination buffer. Must be valid kernel memory.
 * @param u_src User source buffer. Access is checked.
 * @param n Number of bytes to copy.
 * @return 0 on success, or the number of bytes *that could not be copied* on failure (due to fault).
 * Returns 'n' if access_ok fails initially or k_dst is NULL.
 */
size_t copy_from_user(void *k_dst, const void *u_src, size_t n);

/**
 * @brief Copies 'n' bytes from kernelspace 'k_src' to userspace 'u_dst'.
 * Handles page faults during the copy using the exception table mechanism.
 * Uses optimized assembly routines.
 *
 * @param u_dst User destination buffer. Access is checked.
 * @param k_src Kernel source buffer. Must be valid kernel memory.
 * @param n Number of bytes to copy.
 * @return 0 on success, or the number of bytes *that could not be copied* on failure (due to fault).
 * Returns 'n' if access_ok fails initially or k_src is NULL.
 */
size_t copy_to_user(void *u_dst, const void *k_src, size_t n);


// --- Assembly Helper Prototypes ---
// These perform the raw copy and rely on the exception table for fault handling.
// Return value is the number of bytes *NOT* copied (0 on success).

/**
 * @brief Raw assembly routine to copy from user to kernel. (Internal use by copy_from_user).
 * @param k_dst Kernel destination.
 * @param u_src User source.
 * @param n Number of bytes.
 * @return Number of bytes *not* copied.
 */
extern size_t _raw_copy_from_user(void *k_dst, const void *u_src, size_t n);

/**
 * @brief Raw assembly routine to copy from kernel to user. (Internal use by copy_to_user).
 * @param u_dst User destination.
 * @param k_src Kernel source.
 * @param n Number of bytes.
 * @return Number of bytes *not* copied.
 */
extern size_t _raw_copy_to_user(void *u_dst, const void *k_src, size_t n);


#endif // UACCESS_H