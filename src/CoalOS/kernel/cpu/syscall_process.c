/**
 * @file syscall_process.c
 * @brief Process Management System Call Implementations
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Handles process-related system calls including fork, execve, exit,
 * waitpid, and process identification functions.
 */

//============================================================================
// Includes
//============================================================================
#include "syscall_process.h"
#include "syscall_utils.h"
#include "syscall_security.h"
#include <kernel/process/process.h>
#include <kernel/process/scheduler.h>
#include <kernel/process/elf_loader.h>
#include <kernel/memory/uaccess.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/paging_process.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/fs/vfs/fs_errno.h>

//============================================================================
// Process Lifecycle Implementation
//============================================================================

int32_t sys_exit_impl(uint32_t code, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg2; (void)arg3; (void)regs;
    process_exit(PROC_EXITCODE(code));
}

int32_t sys_fork_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg1; (void)arg2; (void)arg3;
    return process_fork(regs);
}

int32_t sys_execve_impl(uint32_t user_pathname_ptr, uint32_t user_argv_ptr, uint32_t user_envp_ptr, isr_frame_t *regs)
{
    char k_pathname[SYSCALL_MAX_PATH_LEN];
    char **argv = NULL, **envp = NULL;
    int argc = 0, envc = 0;
    int ret = syscall_copy_path_from_user((const_userptr_t)user_pathname_ptr, k_pathname, sizeof(k_pathname));

    if (ret != 0) return ret;
    ret = parse_argv(user_argv_ptr, &argv, &argc);
    if (ret == 0) ret = parse_argv(user_envp_ptr, &envp, &envc);
    if (ret == 0) ret = process_execve(k_pathname, (const char *const *)argv, (const char *const *)envp, regs);
    free_argv(argv, argc);
    free_argv(envp, envc);
    return ret;
}

int32_t sys_waitpid_impl(uint32_t pid, uint32_t user_status_ptr, uint32_t options, isr_frame_t *regs)
{
    int status = 0;
    int ret;

    (void)regs;
    if (user_status_ptr && !syscall_validate_buffer((userptr_t)user_status_ptr, sizeof(int), true))
        return -EFAULT;
    ret = process_waitpid((int)pid, &status, (int)options);
    if (ret > 0 && user_status_ptr &&
        copy_to_user((userptr_t)user_status_ptr, &status, sizeof(status)) != 0)
        return -EFAULT;
    return ret;
}

//============================================================================
// Process Identification Implementation
//============================================================================

int32_t sys_getpid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg1; (void)arg2; (void)arg3; (void)regs;
    
    pcb_t *current = get_current_process();
    if (!current) {
        return -ESRCH;
    }
    
    return (int32_t)current->pid;
}

int32_t sys_getppid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg1; (void)arg2; (void)arg3; (void)regs;
    
    pcb_t *current = get_current_process();
    if (!current) {
        return -ESRCH;
    }
    
    return (int32_t)current->ppid;
}