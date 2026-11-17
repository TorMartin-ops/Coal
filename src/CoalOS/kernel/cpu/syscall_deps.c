/**
 * @file syscall_deps.c
 * @brief Weak fallbacks for services owned by other subsystems.
 *
 * Until the owning subsystem defines the strong symbol, the call fails with
 * -ENOSYS (or reports "unknown"). See the declarations in <kernel/cpu/syscall.h>.
 */

#include "syscall_internal.h"
#include <kernel/lib/string.h>

#define WEAK __attribute__((weak))

WEAK int process_fork(isr_frame_t *regs) { (void)regs; return -ENOSYS; }

WEAK int process_execve(const char *path, char *const *argv, char *const *envp, isr_frame_t *regs) {
    (void)path; (void)argv; (void)envp; (void)regs;
    return -ENOSYS;
}

WEAK int process_chdir(const char *path) { (void)path; return -ENOSYS; }

/* Until processes track a directory, every process is in the root. */
WEAK int process_getcwd(char *buf, size_t size) {
    if (size < 2) return -ERANGE;
    strcpy(buf, "/");
    return 2;
}

WEAK pcb_t *process_get_by_pid(uint32_t pid) {
    pcb_t *cur = get_current_process();
    return cur && cur->pid == pid ? cur : NULL;
}

WEAK int vfs_stat64(const char *path, struct linux_stat64 *st) { (void)path; (void)st; return -FS_ERR_NOT_SUPPORTED; }
WEAK int vfs_fstat64(file_t *file, struct linux_stat64 *st) { (void)file; (void)st; return -FS_ERR_NOT_SUPPORTED; }
WEAK int vfs_getdents64(file_t *dir, void *buf, size_t len) { (void)dir; (void)buf; (void)len; return -FS_ERR_NOT_SUPPORTED; }
WEAK int vfs_pipe(file_t **rd, file_t **wr) { (void)rd; (void)wr; return -FS_ERR_NOT_SUPPORTED; }

WEAK uint32_t rtc_boot_time(void) { return 0; }
