/**
 * @file syscall_dispatcher.c
 * @brief System Call Dispatcher and Registration
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Core system call dispatcher that manages the syscall table,
 * handles Linux compatibility mode, and routes system calls to appropriate
 * handler modules. Follows Facade pattern for coordinating syscall modules.
 */

//============================================================================
// Includes
//============================================================================
#include "syscall_dispatcher.h"
#include "syscall_fileio.h"
#include "syscall_utils.h"
#include "syscall_process.h"
#include <kernel/cpu/syscall.h>
#include <kernel/cpu/syscall_linux.h>
#include <kernel/process/process.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/fs/vfs/fs_errno.h>

//============================================================================
// Dispatcher Configuration
//============================================================================
static syscall_fn_t syscall_table[MAX_SYSCALLS];
static bool linux_compat_mode = true; // Enable Linux compatibility by default

//============================================================================
// Forward Declarations for Module Functions
//============================================================================
// Process management
extern int32_t sys_exit_impl(uint32_t code, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
extern int32_t sys_fork_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
extern int32_t sys_execve_impl(uint32_t user_pathname_ptr, uint32_t user_argv_ptr, uint32_t user_envp_ptr, isr_frame_t *regs);
extern int32_t sys_waitpid_impl(uint32_t pid, uint32_t user_status_ptr, uint32_t options, isr_frame_t *regs);
extern int32_t sys_getpid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
extern int32_t sys_getppid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);

// Terminal operations  
extern int32_t sys_puts_impl(uint32_t user_str_ptr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
extern int32_t sys_read_terminal_line_impl(uint32_t user_buf_ptr, uint32_t count, uint32_t arg3, isr_frame_t *regs);

// Pipe operations
extern int32_t sys_pipe_impl(uint32_t user_pipefd_ptr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);

// Signal handling
extern int32_t sys_signal_impl(uint32_t signum, uint32_t user_handler_ptr, uint32_t arg3, isr_frame_t *regs);
extern int32_t sys_kill_impl(uint32_t pid, uint32_t sig, uint32_t arg3, isr_frame_t *regs);

// File system operations
extern int32_t sys_chdir_impl(uint32_t user_path_ptr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
extern int32_t sys_getcwd_impl(uint32_t user_buf_ptr, uint32_t size, uint32_t arg3, isr_frame_t *regs);
extern int32_t sys_stat_impl(uint32_t user_path_ptr, uint32_t user_stat_ptr, uint32_t arg3, isr_frame_t *regs);
extern int32_t sys_readdir_impl(uint32_t fd, uint32_t user_buf_ptr, uint32_t count, isr_frame_t *regs);
extern int32_t sys_getdents_impl(uint32_t fd, uint32_t user_dirp, uint32_t count, isr_frame_t *regs);
extern int32_t sys_mkdir_impl(uint32_t user_pathname_ptr, uint32_t mode, uint32_t arg3, isr_frame_t *regs);
extern int32_t sys_rmdir_impl(uint32_t user_pathname_ptr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
extern int32_t sys_unlink_impl(uint32_t user_pathname_ptr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);

// Process groups and sessions
extern int32_t sys_setsid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
extern int32_t sys_getsid_impl(uint32_t pid, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
extern int32_t sys_setpgid_impl(uint32_t pid, uint32_t pgid, uint32_t arg3, isr_frame_t *regs);
extern int32_t sys_getpgid_impl(uint32_t pid, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
extern int32_t sys_getpgrp_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
extern int32_t sys_tcsetpgrp_impl(uint32_t fd, uint32_t pgid, uint32_t arg3, isr_frame_t *regs);
extern int32_t sys_tcgetpgrp_impl(uint32_t fd, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);

// Memory management
extern int32_t sys_brk_impl(uint32_t brk_addr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);
extern int32_t sys_mmap_impl(uint32_t addr, uint32_t length, uint32_t prot, isr_frame_t *regs);

// Linux compatibility
extern void init_linux_syscall_table(void);
extern int linux_syscall_dispatcher(uint32_t syscall_num, uint32_t arg1, uint32_t arg2, 
                                    uint32_t arg3, uint32_t arg4, uint32_t arg5);

//============================================================================
// Default Handler Implementation
//============================================================================

int32_t sys_not_implemented(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg1; (void)arg2; (void)arg3;
    serial_write("[Syscall] WARNING: Unimplemented syscall #");
    serial_print_hex(regs->eax);
    serial_write("\n");
    return -ENOSYS;
}

//============================================================================
// Dispatcher Core Implementation
//============================================================================

void syscall_init(void)
{
    serial_write("[Syscall] Initializing modular dispatcher...\n");
    
    // Initialize all syscalls to unimplemented
    for (int i = 0; i < MAX_SYSCALLS; i++) {
        syscall_table[i] = sys_not_implemented;
    }
    
    // Register file I/O syscalls (from syscall_fileio module)
    syscall_table[SYS_READ]   = sys_read_impl;
    syscall_table[SYS_WRITE]  = sys_write_impl;
    syscall_table[SYS_OPEN]   = sys_open_impl;
    syscall_table[SYS_CLOSE]  = sys_close_impl;
    syscall_table[SYS_LSEEK]  = sys_lseek_impl;
    syscall_table[SYS_DUP2]   = sys_dup2_impl;
    
    // Register process management syscalls (will be in separate modules)
    syscall_table[SYS_EXIT]   = sys_exit_impl;
    syscall_table[SYS_FORK]   = sys_fork_impl;
    syscall_table[SYS_EXECVE] = sys_execve_impl;
    syscall_table[SYS_WAITPID] = sys_waitpid_impl;
    syscall_table[SYS_GETPID] = sys_getpid_impl;
    syscall_table[SYS_GETPPID] = sys_getppid_impl;
    
    // Register terminal syscalls (will be in separate module)
    syscall_table[SYS_PUTS]   = sys_puts_impl;
    syscall_table[SYS_READ_TERMINAL_LINE] = sys_read_terminal_line_impl;
    
    // Register pipe syscalls (will be in separate module)
    syscall_table[SYS_PIPE]   = sys_pipe_impl;
    
    // Register signal syscalls (will be in separate module)
    syscall_table[SYS_SIGNAL] = sys_signal_impl;
    syscall_table[SYS_KILL]   = sys_kill_impl;
    
    // Register file system syscalls (will be in separate module)
    syscall_table[SYS_CHDIR]  = sys_chdir_impl;
    syscall_table[SYS_GETCWD] = sys_getcwd_impl;
    syscall_table[SYS_STAT]   = sys_stat_impl;
    syscall_table[SYS_GETDENTS] = sys_getdents_impl;
    syscall_table[SYS_UNLINK] = sys_unlink_impl;
    syscall_table[SYS_MKDIR] = sys_mkdir_impl;
    syscall_table[SYS_RMDIR] = sys_rmdir_impl;
    
    // Register memory management syscalls (will be in separate module)
    syscall_table[SYS_BRK] = sys_brk_impl;
    syscall_table[SYS_MMAP] = sys_mmap_impl;
    
    // Register process groups and sessions syscalls (will be in separate module)
    syscall_table[SYS_SETSID] = sys_setsid_impl;
    syscall_table[SYS_GETSID] = sys_getsid_impl;
    syscall_table[SYS_SETPGID] = sys_setpgid_impl;
    syscall_table[SYS_GETPGID] = sys_getpgid_impl;
    syscall_table[SYS_GETPGRP] = sys_getpgrp_impl;
    syscall_table[SYS_TCSETPGRP] = sys_tcsetpgrp_impl;
    syscall_table[SYS_TCGETPGRP] = sys_tcgetpgrp_impl;

    KERNEL_ASSERT(syscall_table[SYS_EXIT] == sys_exit_impl, "SYS_EXIT assignment sanity check failed!");
    serial_write("[Syscall] Core table initialized.\n");
    
    // Initialize Linux-compatible syscall table
    init_linux_syscall_table();
    serial_write("[Syscall] Linux compatibility mode enabled.\n");
}

int32_t syscall_dispatcher(isr_frame_t *regs)
{
    // Critical safety check - never use assertions in system call dispatcher
    if (!regs) {
        serial_printf("[Syscall CRITICAL] NULL registers in system call dispatcher!\n");
        return -EFAULT;
    }

    uint32_t syscall_num = regs->eax;
    uint32_t arg1_ebx    = regs->ebx;
    uint32_t arg2_ecx    = regs->ecx;
    uint32_t arg3_edx    = regs->edx;
    uint32_t arg4_esi    = regs->esi;
    uint32_t arg5_edi    = regs->edi;
    int32_t ret_val;

    pcb_t* current_proc = get_current_process();
    
    // Validate current process context for user-space syscalls
    if (!current_proc) {
        serial_printf("[Syscall] No current process context for syscall %u\n", syscall_num);
        return -ESRCH;
    }
    
    // Debug output for first few syscalls
    static int syscall_count = 0;
    if (syscall_count < 10) {
        serial_printf("[SYSCALL DEBUG] Syscall #%d: num=%u, arg1=%x, arg2=%x, arg3=%x\n", 
                      syscall_count++, syscall_num, arg1_ebx, arg2_ecx, arg3_edx);
    }
    
    if (!current_proc && syscall_num != SYS_EXIT && syscall_num != __NR_exit) {
        KERNEL_PANIC_HALT("Syscall (not SYS_EXIT) executed without process context!");
        regs->eax = (uint32_t)-EFAULT;
        return -EFAULT;
    }

    // Check if we're in Linux compatibility mode
    if (linux_compat_mode && syscall_num < __NR_syscalls && syscall_num >= 100) {
        // Use Linux syscall dispatcher for syscalls >= 100
        ret_val = linux_syscall_dispatcher(syscall_num, arg1_ebx, arg2_ecx, 
                                         arg3_edx, arg4_esi, arg5_edi);
    } else if (syscall_num < MAX_SYSCALLS && syscall_table[syscall_num] != NULL) {
        // Use CoalOS native syscalls (including low-numbered ones)
        ret_val = syscall_table[syscall_num](arg1_ebx, arg2_ecx, arg3_edx, regs);
    } else {
        ret_val = -ENOSYS;
    }

    regs->eax = (uint32_t)ret_val;
    return ret_val;
}

//============================================================================
// Registration and Configuration Implementation
//============================================================================

void syscall_set_linux_compat(bool enable)
{
    linux_compat_mode = enable;
    serial_printf("[Syscall] Linux compatibility mode %s\n", 
                  enable ? "enabled" : "disabled");
}

bool syscall_get_linux_compat(void)
{
    return linux_compat_mode;
}

int syscall_register(uint32_t syscall_num, syscall_fn_t handler)
{
    if (syscall_num >= MAX_SYSCALLS) {
        return -EINVAL;
    }
    
    if (!handler) {
        return -EINVAL;
    }
    
    syscall_table[syscall_num] = handler;
    return 0;
}

void syscall_unregister(uint32_t syscall_num)
{
    if (syscall_num < MAX_SYSCALLS) {
        syscall_table[syscall_num] = sys_not_implemented;
    }
}

syscall_fn_t syscall_get_handler(uint32_t syscall_num)
{
    if (syscall_num >= MAX_SYSCALLS) {
        return NULL;
    }
    
    return syscall_table[syscall_num];
}