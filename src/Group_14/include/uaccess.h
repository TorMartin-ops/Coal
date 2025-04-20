#ifndef UACCESS_H
#define UACCESS_H

#include "types.h"
#include "mm.h" // <<< Corrected include: Use mm.h directly

// Access verification flags (permissions requested)
#define VERIFY_READ  0x01
#define VERIFY_WRITE 0x02

// Standard Linux-like error codes (define these centrally if possible)
#ifndef EFAULT
#define EFAULT 14  // Bad address
#endif
#ifndef EINVAL
#define EINVAL 22  // Invalid argument
#endif
#ifndef ENOMEM
#define ENOMEM 12 // Out of memory
#endif
#ifndef EACCES
#define EACCES 13 // Permission denied
#endif
#ifndef ENOSYS
#define ENOSYS 38 // Function not implemented
#endif
#ifndef EBADF
#define EBADF 9 // Bad file descriptor
#endif


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Checks if a userspace memory range is accessible for read/write.
 * Verifies that the entire range falls within valid VMAs of the current process
 * and has the appropriate permissions (VM_READ or VM_WRITE). Checks against
 * kernel space boundaries and potential overflows.
 *
 * @param type Bitmask of VERIFY_READ and/or VERIFY_WRITE.
 * @param uaddr Start address in userspace.
 * @param size Size of the memory range.
 * @return true if the entire range is accessible with the specified type, false otherwise.
 */
bool access_ok(int type, const void *uaddr, size_t size);

/**
 * @brief Copies a block of memory from userspace to kernelspace.
 * Includes address validation via access_ok.
 *
 * WARNING: THIS IS CURRENTLY AN UNSAFE STUB IMPLEMENTATION.
 * A production kernel requires architecture-specific assembly or page fault
 * handling mechanisms (e.g., exception tables) to safely handle faults
 * during the copy process itself. This stub uses memcpy and will likely
 * cause a kernel panic if 'u_src' points to an unmapped page within an
 * otherwise valid VMA range.
 *
 * @param k_dst Destination buffer in kernelspace. Must be non-NULL and large enough.
 * @param u_src Source buffer in userspace. Must be validated by caller or internally.
 * @param n Number of bytes to copy.
 * @return Number of bytes NOT copied (0 on success, >0 on failure).
 * On failure, the contents of k_dst beyond successfully copied bytes are undefined.
 */
size_t copy_from_user(void *k_dst, const void *u_src, size_t n);

/**
 * @brief Copies a block of memory from kernelspace to userspace.
 * Includes address validation via access_ok.
 *
 * WARNING: THIS IS CURRENTLY AN UNSAFE STUB IMPLEMENTATION. See copy_from_user.
 * A production kernel requires fault handling. This stub uses memcpy and
 * will likely cause a kernel panic if 'u_dst' points to an unmapped or
 * read-only page within an otherwise valid VMA range.
 *
 * @param u_dst Destination buffer in userspace. Must be validated by caller or internally.
 * @param k_src Source buffer in kernelspace. Must be non-NULL.
 * @param n Number of bytes to copy.
 * @return Number of bytes NOT copied (0 on success, >0 on failure).
 */
size_t copy_to_user(void *u_dst, const void *k_src, size_t n);


#ifdef __cplusplus
}
#endif

#endif // UACCESS_H