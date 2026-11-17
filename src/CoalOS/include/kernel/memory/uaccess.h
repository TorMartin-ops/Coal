/**
 * @file uaccess.h
 * @brief Safe access to user memory from the kernel.
 *
 * Pointers handed in by user programs are validated against the current
 * process's VMAs; pages that are not mapped yet are faulted in first. A copy
 * can still fail part-way (the exception table turns a fault inside the
 * copy loop into a short count), and the functions then return the number of
 * bytes NOT copied.
 */
#ifndef UACCESS_H
#define UACCESS_H

#include <kernel/core/types.h>

typedef void *userptr_t;
typedef const void *const_userptr_t;
typedef void *kernelptr_t;
typedef const void *const_kernelptr_t;

#define VERIFY_READ  1
#define VERIFY_WRITE 2

/** True if [uaddr, uaddr+size) lies in VMAs of the current process that permit the access. */
bool access_ok(int type, const_userptr_t uaddr, size_t size);

/** @return number of bytes not copied (0 on success). */
size_t copy_from_user(kernelptr_t k_dst, const_userptr_t u_src, size_t n);
size_t copy_to_user(userptr_t u_dst, const_kernelptr_t k_src, size_t n);

/* Raw copy loops (uaccess.asm); the caller has validated the range. */
size_t _raw_copy_from_user(void *k_dst, const void *u_src, size_t n);
size_t _raw_copy_to_user(void *u_dst, const void *k_src, size_t n);

#endif /* UACCESS_H */
