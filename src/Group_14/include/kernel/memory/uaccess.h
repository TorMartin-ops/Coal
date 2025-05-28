#ifndef UACCESS_H
#define UACCESS_H

#include <kernel/core/types.h>        // For size_t, uintptr_t etc.
#include <libc/stddef.h>  // For NULL, size_t
#include <libc/stdbool.h> // For bool
#include <libc/stdint.h>  // For uintptr_t, uint32_t if not in types.h

// --- Opaque Pointer Typedefs for Type Safety ---
// Using distinct types helps the compiler catch argument swaps.
// The __user and __kernel are primarily for documentation and static analysis tools.
typedef void * /*__attribute__((address_space(1)))*/ userptr_t; // User space pointer
typedef const void * /*__attribute__((address_space(1)))*/ const_userptr_t; // Const user space pointer
typedef void * /*__attribute__((address_space(0)))*/ kernelptr_t; // Kernel space pointer
typedef const void * /*__attribute__((address_space(0)))*/ const_kernelptr_t; // Const kernel space pointer


// Verification flags for access_ok
#define VERIFY_READ  1 // Check for read permission
#define VERIFY_WRITE 2 // Check for write permission

/**
 * @brief Checks if a userspace memory range is potentially accessible.
 * @param type Verification type: VERIFY_READ, VERIFY_WRITE.
 * @param uaddr The starting user virtual address.
 * @param size The size of the memory range in bytes.
 * @return `true` if the range seems valid based on VMA checks, `false` otherwise.
 */
bool access_ok(int type, const_userptr_t uaddr, size_t size);

/**
 * @brief Copies a block of memory from userspace to kernelspace.
 * @param k_dst Kernel destination buffer pointer.
 * @param u_src User source buffer virtual address.
 * @param n Number of bytes to copy.
 * @return Number of bytes that could NOT be copied (0 on success).
 */
size_t copy_from_user(kernelptr_t k_dst, const_userptr_t u_src, size_t n) __attribute__((nonnull (1, 2)));


/**
 * @brief Copies a block of memory from kernelspace to userspace.
 * @param u_dst User destination buffer virtual address.
 * @param k_src Kernel source buffer pointer.
 * @param n Number of bytes to copy.
 * @return Number of bytes that could NOT be copied (0 on success).
 */
size_t copy_to_user(userptr_t u_dst, const_kernelptr_t k_src, size_t n) __attribute__((nonnull (1, 2)));


// --- Assembly Helper Prototypes (Internal Use) ---
extern size_t _raw_copy_from_user(void *k_dst, const void *u_src, size_t n);
extern size_t _raw_copy_to_user(void *u_dst, const void *k_src, size_t n);

#endif // UACCESS_H