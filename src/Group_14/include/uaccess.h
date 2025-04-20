#ifndef UACCESS_H
#define UACCESS_H

#include <libc/stddef.h> // For size_t
#include <libc/stdbool.h> // For bool
#include <libc/stdint.h> // For uintptr_t (if using C99 standard, otherwise define appropriately)

// Verification type flags for access_ok
#define VERIFY_READ  1
#define VERIFY_WRITE 2

/**
 * @brief Checks if a userspace memory range is potentially accessible based on VMAs.
 * WARNING: Does NOT guarantee pages are mapped or present.
 *
 * @param type VERIFY_READ, VERIFY_WRITE, or (VERIFY_READ | VERIFY_WRITE).
 * @param uaddr The starting user virtual address (32-bit).
 * @param size The size of the memory range in bytes.
 * @return true if the range is covered by valid VMAs with correct permissions, false otherwise.
 */
bool access_ok(int type, const void *uaddr, size_t size);

/**
 * @brief Copies 'n' bytes from userspace 'u_src' to kernelspace 'k_dst'.
 * Handles page faults during the copy using the exception table mechanism.
 *
 * @param k_dst Kernel destination buffer. Must be valid kernel memory.
 * @param u_src User source buffer. Access is checked.
 * @param n Number of bytes to copy.
 * @return 0 on success, or the number of bytes *that could not be copied* on failure (due to fault).
 */
size_t copy_from_user(void *k_dst, const void *u_src, size_t n);

/**
 * @brief Copies 'n' bytes from kernelspace 'k_src' to userspace 'u_dst'.
 * Handles page faults during the copy using the exception table mechanism.
 *
 * @param u_dst User destination buffer. Access is checked.
 * @param k_src Kernel source buffer. Must be valid kernel memory.
 * @param n Number of bytes to copy.
 * @return 0 on success, or the number of bytes *that could not be copied* on failure (due to fault).
 */
size_t copy_to_user(void *u_dst, const void *k_src, size_t n);


#endif // UACCESS_H