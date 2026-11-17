/**
 * @file syscall_deps.c
 * @brief Weak fallbacks for services owned by other subsystems.
 *
 * Until the owning subsystem defines the strong symbol, the call fails with
 * -ENOSYS (or reports "nothing"). See the declarations in <kernel/cpu/syscall.h>.
 */

#include "syscall_internal.h"

#define WEAK __attribute__((weak))

WEAK int vfs_utime(const char *path, uint32_t atime, uint32_t mtime) {
    (void)path; (void)atime; (void)mtime;
    return -ENOSYS;
}

WEAK size_t klog_ring_size(void) { return 0; }
WEAK size_t klog_ring_copy(char *buf, size_t len) { (void)buf; (void)len; return 0; }
WEAK void   klog_ring_clear(void) {}
