/**
 * @file uaccess.c
 * @brief Validation of user ranges and copying to/from user space.
 */

#include <kernel/memory/uaccess.h>
#include <kernel/memory/mm.h>
#include <kernel/process/process.h>

static mm_struct_t *current_mm(void) {
    pcb_t *p = get_current_process();
    return p ? p->mm : NULL;
}

static bool user_range(int type, uintptr_t addr, size_t n, bool populate) {
    mm_struct_t *mm = current_mm();
    return mm && addr && mm_access_ok(mm, addr, n, type == VERIFY_WRITE, populate);
}

bool access_ok(int type, const_userptr_t uaddr, size_t size) {
    return size == 0 || user_range(type, (uintptr_t)uaddr, size, false);
}

size_t copy_from_user(kernelptr_t k_dst, const_userptr_t u_src, size_t n) {
    if (n == 0)
        return 0;
    if (!user_range(VERIFY_READ, (uintptr_t)u_src, n, true))
        return n;
    return _raw_copy_from_user(k_dst, u_src, n);
}

size_t copy_to_user(userptr_t u_dst, const_kernelptr_t k_src, size_t n) {
    if (n == 0)
        return 0;
    if (!user_range(VERIFY_WRITE, (uintptr_t)u_dst, n, true))
        return n;
    return _raw_copy_to_user(u_dst, k_src, n);
}
