/*
 * System call layer: the Linux i386 ABI (see <libc/syscall_numbers.h>).
 *
 * One table indexed by the Linux number. Arguments arrive in EBX, ECX, EDX,
 * ESI, EDI, EBP; the result (or -errno) goes back in EAX.
 */
#ifndef KERNEL_CPU_SYSCALL_H
#define KERNEL_CPU_SYSCALL_H

#include <libc/syscall_numbers.h>
#include <kernel/cpu/isr_frame.h>
#include <kernel/cpu/linux_abi.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/process/process.h>

#define NR_SYSCALLS 384

void syscall_init(void);

/** Entry point called by syscall.asm with the saved register frame. */
void syscall_dispatch(isr_frame_t *regs);

/*
 * Services the system calls need from other subsystems. Each has a weak
 * fallback in syscall_deps.c that fails with -ENOSYS, so the owning
 * subsystem only has to provide a strong definition.
 */

/** Duplicate the calling process; returns the child's pid (child sees 0 via regs). */
int process_fork(isr_frame_t *regs);

/** Replace the image of the calling process. argv/envp are NULL-terminated kernel arrays. */
int process_execve(const char *path, char *const *argv, char *const *envp, isr_frame_t *regs);

/** Change / read the current working directory of the calling process. */
int process_chdir(const char *path);
int process_getcwd(char *buf, size_t size);   /* length including the NUL, or -errno */

/** Look up a process by pid (NULL if unknown). */
pcb_t *process_get_by_pid(uint32_t pid);

/** Stat by path / by open file, filling the Linux layout. Return FS_ERR_* on failure. */
int vfs_stat64(const char *path, struct linux_stat64 *st);
int vfs_fstat64(file_t *file, struct linux_stat64 *st);

/** Read directory entries as linux_dirent64 records; bytes written, 0 at the end. */
int vfs_getdents64(file_t *dir, void *buf, size_t len);

/** Create an anonymous pipe. */
int vfs_pipe(file_t **read_end, file_t **write_end);

/** Unix time at boot (from the RTC); 0 when unknown. */
uint32_t rtc_boot_time(void);

#endif
