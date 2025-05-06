/**
 * @file syscall.c
 * @brief System Call Dispatcher and Implementations (v4.9.2 - Syntax and Return Fixes)
 * @version 4.9.2
 *
 * Implements the system call C-level dispatcher and the backend functions
 * for various system calls like open, read, write, close, exit, etc.
 * Handles user<->kernel memory copying and validation.
 * CORRECTED: syscall_dispatcher now correctly returns int32_t.
 * Ensured all static function declarations and definitions are consistent.
 */

// --- Includes ---
#include "syscall.h"       // Includes isr_frame.h implicitly now
#include "terminal.h"
#include "process.h"
#include "scheduler.h"
#include "sys_file.h"      // Includes sys_open, sys_read etc. declarations
#include "kmalloc.h"
#include "string.h"
#include "uaccess.h"       // Essential for pointer validation
#include "fs_errno.h"      // Error codes (EINVAL, EFAULT, ENOSYS, EBADF etc.)
#include "fs_limits.h"
#include "vfs.h"
#include "assert.h"
#include "serial.h"        // For serial_write, serial_print_hex
#include "paging.h"        // Include for KERNEL_SPACE_VIRT_START
#include <libc/limits.h>    // INT32_MIN, INT32_MAX (assuming int is 32-bit)
#include <libc/stdbool.h>   // For bool type
#include <libc/stddef.h>    // For size_t
#include "debug.h"         // For DEBUG_PRINTK

// --- Constants ---
#define MAX_PUTS_LEN 256
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define MAX_SYSCALL_STR_LEN MAX_PATH_LEN // Max length for paths copied from user
#define MAX_RW_CHUNK_SIZE   PAGE_SIZE    // Max bytes to copy in one chunk

// --- Utility Macros ---
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

// --- Static Data ---
static syscall_fn_t syscall_table[MAX_SYSCALLS];

// --- Forward Declarations of Syscall Implementations ---
// Ensure these match the definitions below
static int32_t sys_exit_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);
static int32_t sys_read_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);
static int32_t sys_write_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);
static int32_t sys_open_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);
static int32_t sys_close_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);
static int32_t sys_lseek_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);
static int32_t sys_getpid_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);
static int32_t sys_puts_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs);
static int32_t sys_not_implemented(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs); // Definition provided below
static int strncpy_from_user_safe(const char *u_src, char *k_dst, size_t maxlen);


// --- Logging Helper Functions ---
static void serial_print_sdec(int n) {
    char buf[12]; // Sufficient for -2147483648
    char *ptr = buf + sizeof(buf) - 1; // Start from the end
    *ptr = '\0'; // Null terminate
    uint32_t un;
    bool neg = false;

    if (n == 0) {
        if (ptr > buf) *--ptr = '0';
    } else {
        if (n < 0) {
            neg = true;
            if (n == INT32_MIN) { // Handle INT_MIN specifically
                un = 2147483648U;
            } else {
                un = (uint32_t)(-n);
            }
        } else {
            un = (uint32_t)n;
        }
        while (un > 0 && ptr > buf) { // Ensure not to underflow buffer
            *--ptr = '0' + (un % 10);
            un /= 10;
        }
        if (neg && ptr > buf) { // Ensure space for '-'
            *--ptr = '-';
        }
    }
    serial_write(ptr);
}

static inline void serial_log_entry(const char* func_name) {
    serial_write(" FNC_ENTER: "); serial_write(func_name); serial_write("\n");
}
static inline void serial_log_exit(const char* func_name, int32_t ret_val) {
    serial_write(" FNC_EXIT: "); serial_write(func_name);
    serial_write(" ret="); serial_print_sdec(ret_val); serial_write("\n");
}
static inline void serial_log_step(const char* step_msg) {
    serial_write("  STEP: "); serial_write(step_msg); serial_write("\n");
}
static inline void serial_log_error(const char* error_msg) {
    serial_write("  ERROR: "); serial_write(error_msg); serial_write("\n");
}
static inline void serial_log_debug(const char* debug_msg) {
    serial_write("  DBG: "); serial_write(debug_msg); serial_write("\n");
}

//-----------------------------------------------------------------------------
// Syscall Initialization
//-----------------------------------------------------------------------------
void syscall_init(void) {
    serial_log_entry("syscall_init");
    serial_log_step("Looping to init table");
    for (int i = 0; i < MAX_SYSCALLS; i++) {
        syscall_table[i] = sys_not_implemented;
    }
    serial_log_step("Registering handlers");
    syscall_table[SYS_EXIT]   = sys_exit_impl;
    syscall_table[SYS_READ]   = sys_read_impl;
    syscall_table[SYS_WRITE]  = sys_write_impl;
    syscall_table[SYS_OPEN]   = sys_open_impl;
    syscall_table[SYS_CLOSE]  = sys_close_impl;
    syscall_table[SYS_LSEEK]  = sys_lseek_impl;
    syscall_table[SYS_GETPID] = sys_getpid_impl;
    syscall_table[SYS_PUTS]   = sys_puts_impl;

    serial_log_step("Verifying SYS_EXIT assignment...");
    KERNEL_ASSERT(syscall_table[SYS_EXIT] == sys_exit_impl, "SYS_EXIT assignment failed!");
    serial_log_step("SYS_EXIT assignment OK.");
    serial_log_exit("syscall_init", 0);
}

//-----------------------------------------------------------------------------
// Static Helper: Safe String Copy from User Space
//-----------------------------------------------------------------------------
static int strncpy_from_user_safe(const char *u_src, char *k_dst, size_t maxlen) {
    serial_log_entry("strncpy_from_user_safe");
    serial_write("   u_src="); serial_print_hex((uintptr_t)u_src);
    serial_write(" k_dst="); serial_print_hex((uintptr_t)k_dst);
    serial_write(" maxlen="); serial_print_hex((uint32_t)maxlen); serial_write("\n");

    KERNEL_ASSERT(k_dst != NULL, "Kernel destination buffer cannot be NULL");
    if (maxlen == 0) { serial_log_exit("strncpy_from_user_safe", -EINVAL); return -EINVAL; }
    
    k_dst[0] = '\0'; 

    serial_log_step("Basic u_src check");
    serial_write("   DBG: Checking u_src: "); serial_print_hex((uint32_t)u_src);
    serial_write(" against KERNEL_SPACE_VIRT_START: "); serial_print_hex(KERNEL_SPACE_VIRT_START); serial_write("\n");
    if (!u_src || (uintptr_t)u_src >= KERNEL_SPACE_VIRT_START) {
        serial_log_error("EFAULT (bad u_src basic check)");
        serial_log_exit("strncpy_from_user_safe", -EFAULT); 
        return -EFAULT;
    }

    serial_log_step("Calling access_ok");
    if (!access_ok(VERIFY_READ, u_src, 1)) { 
        serial_log_error("EFAULT (access_ok failed for first byte)");
        serial_log_exit("strncpy_from_user_safe", -EFAULT);
        return -EFAULT;
    }

    serial_log_step("Entering copy loop");
    size_t len = 0;
    while (len < maxlen) { 
        char current_char;
        if (!access_ok(VERIFY_READ, u_src + len, 1)) {
            serial_log_error("EFAULT (access_ok failed during loop)");
            k_dst[len] = '\0'; 
            serial_log_exit("strncpy_from_user_safe", -EFAULT);
            return -EFAULT;
        }
        size_t not_copied = copy_from_user(&current_char, u_src + len, 1);
        if (not_copied > 0) {
            serial_log_error("EFAULT (fault during copy_from_user)");
            k_dst[len] = '\0'; 
            serial_log_exit("strncpy_from_user_safe", -EFAULT);
            return -EFAULT;
        }
        k_dst[len] = current_char;
        if (current_char == '\0') {
            serial_write("   RET: 0 (Success, null terminator found)\n");
            serial_log_exit("strncpy_from_user_safe", 0);
            return 0;
        }
        len++;
    }

    serial_log_step("Loop finished (maxlen reached without null terminator)");
    k_dst[maxlen - 1] = '\0'; 
    serial_log_error("ENAMETOOLONG (or buffer full before null)");
    serial_log_exit("strncpy_from_user_safe", -ENAMETOOLONG); 
    return -ENAMETOOLONG;
}

//-----------------------------------------------------------------------------
// Syscall Implementations
//-----------------------------------------------------------------------------
static int32_t sys_not_implemented(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)arg1_ebx; (void)arg2_ecx; (void)arg3_edx; // Suppress unused parameter warnings
    serial_log_entry("sys_not_implemented");
    serial_write("  Syscall Num: "); serial_print_hex(regs->eax); serial_write("\n");
    pcb_t* proc = get_current_process();
    serial_write("  PID: "); serial_print_hex(proc ? proc->pid : 0xFFFFFFFF); serial_write("\n");
    serial_log_exit("sys_not_implemented", -ENOSYS);
    return -ENOSYS; // Return negative error code
}

static int32_t sys_exit_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)arg2_ecx; (void)arg3_edx; (void)regs;
    serial_log_entry("sys_exit_impl");
    int exit_code = (int)arg1_ebx;
    serial_write("  DBG: ExitCode="); serial_print_hex((uint32_t)exit_code); serial_write("\n");
    serial_log_step("Calling remove_current_task_with_code...");
    remove_current_task_with_code(exit_code);
    KERNEL_PANIC_HALT("sys_exit_impl returned!"); 
    return 0; 
}

static int32_t sys_read_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)regs;
    serial_log_entry("sys_read_impl");
    int fd                = (int)arg1_ebx;
    void *user_buf        = (void*)arg2_ecx;
    size_t count          = (size_t)arg3_edx;
    int32_t ret_val       = 0;
    ssize_t total_read    = 0;
    char* kbuf            = NULL;

    serial_write("  Args: fd="); serial_print_sdec(fd);
    serial_write(" user_buf="); serial_print_hex((uintptr_t)user_buf);
    serial_write(" count="); serial_print_hex((uint32_t)count); serial_write("\n");

    if ((ssize_t)count < 0) { ret_val = -EINVAL; goto read_exit; }
    if (count == 0) { ret_val = 0; goto read_exit; }

    serial_log_step("Pre-access_ok check for WRITE to user_buf...");
    if (!access_ok(VERIFY_WRITE, user_buf, count)) {
        serial_log_error("EFAULT (access_ok failed for user_buf)");
        ret_val = -EFAULT; goto read_exit;
    }
    serial_log_step("access_ok passed for user_buf.");

    size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
    kbuf = kmalloc(chunk_alloc_size);
    if (!kbuf) {
        serial_log_error("ENOMEM (kmalloc for kbuf failed)");
        ret_val = -ENOMEM; goto read_exit;
    }

    serial_log_step("Entering read loop");
    while (total_read < (ssize_t)count) {
        size_t current_chunk_size = MIN(chunk_alloc_size, count - (size_t)total_read);
        KERNEL_ASSERT(current_chunk_size > 0, "Zero chunk size in read loop");

        serial_log_debug("Loop: Calling backend sys_read...");
        ssize_t bytes_read_this_chunk = sys_read(fd, kbuf, current_chunk_size);
        serial_write("   sys_read (backend) returned "); serial_print_sdec(bytes_read_this_chunk); serial_write("\n");

        if (bytes_read_this_chunk < 0) { 
            serial_log_error("Error from backend sys_read"); 
            ret_val = bytes_read_this_chunk; 
            goto read_cleanup; 
        }
        if (bytes_read_this_chunk == 0) { serial_log_debug("EOF reached by backend sys_read"); break; }

        serial_log_debug("Loop: Copying to user...");
        size_t not_copied = copy_to_user((char*)user_buf + total_read, kbuf, (size_t)bytes_read_this_chunk);
        if (not_copied > 0) {
            serial_log_error("EFAULT (copy_to_user failed)");
            if (total_read > 0) { ret_val = total_read; } else { ret_val = -EFAULT; }
            goto read_cleanup;
        }
        total_read += bytes_read_this_chunk;
        if ((size_t)bytes_read_this_chunk < current_chunk_size) { 
            serial_log_debug("Short read from backend, breaking loop"); 
            break; 
        }
    }
    ret_val = total_read;

read_cleanup:
    if (kbuf) kfree(kbuf);
read_exit:
    serial_log_exit("sys_read_impl", ret_val);
    return ret_val;
}

static int32_t sys_write_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)regs;
    serial_log_entry("sys_write_impl");
    int fd                = (int)arg1_ebx;
    const void *user_buf  = (const void*)arg2_ecx;
    size_t count          = (size_t)arg3_edx;
    int32_t ret_val       = 0;
    ssize_t total_written = 0;
    char* kbuf            = NULL;

    serial_write("  Args: fd="); serial_print_sdec(fd);
    serial_write(" user_buf="); serial_print_hex((uintptr_t)user_buf);
    serial_write(" count="); serial_print_hex((uint32_t)count); serial_write("\n");

    if ((ssize_t)count < 0) { ret_val = -EINVAL; goto write_exit; }
    if (count == 0) { ret_val = 0; goto write_exit; }

    serial_log_step("Pre-access_ok check for READ from user_buf...");
    if (!access_ok(VERIFY_READ, user_buf, count)) {
        serial_log_error("EFAULT (access_ok failed for user_buf)");
        ret_val = -EFAULT; goto write_exit;
    }
    serial_log_step("access_ok passed for user_buf.");

    size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
    kbuf = kmalloc(chunk_alloc_size);
    if (!kbuf) {
        serial_log_error("ENOMEM (kmalloc for kbuf failed)");
        ret_val = -ENOMEM; goto write_exit;
    }

    serial_log_step("Entering write loop");
    while (total_written < (ssize_t)count) {
        size_t current_chunk_size = MIN(chunk_alloc_size, count - (size_t)total_written);
        KERNEL_ASSERT(current_chunk_size > 0, "Zero chunk size in write loop");

        serial_log_debug("Loop: Copying from user...");
        size_t not_copied = copy_from_user(kbuf, (char*)user_buf + total_written, current_chunk_size);
        size_t copied_this_chunk = current_chunk_size - not_copied;

        if (copied_this_chunk > 0) {
            serial_log_debug("Loop: Calling backend sys_write or terminal_write...");
            ssize_t bytes_written_this_chunk;
            if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
                terminal_write_bytes(kbuf, copied_this_chunk); 
                bytes_written_this_chunk = copied_this_chunk;
            } else {
                bytes_written_this_chunk = sys_write(fd, kbuf, copied_this_chunk); 
            }
            serial_write("   sys_write (backend) / terminal_write returned "); serial_print_sdec(bytes_written_this_chunk); serial_write("\n");

            if (bytes_written_this_chunk < 0) {
                serial_log_error("Error during backend write operation");
                ret_val = (total_written > 0) ? total_written : bytes_written_this_chunk;
                goto write_cleanup;
            }
            total_written += bytes_written_this_chunk;
            if ((size_t)bytes_written_this_chunk < copied_this_chunk) { 
                serial_log_debug("Short write by backend, breaking loop"); 
                break; 
            }
        }

        if (not_copied > 0) { 
            serial_log_error("EFAULT (copy_from_user failed mid-loop)");
            ret_val = (total_written > 0) ? total_written : -EFAULT;
            goto write_cleanup;
        }
         if (copied_this_chunk == 0 && not_copied == 0 && current_chunk_size > 0) {
            serial_log_error("Stalled in write loop (0 bytes copied, 0 not_copied)");
            ret_val = total_written > 0 ? total_written : -EFAULT; 
            goto write_cleanup;
        }
    }
    ret_val = total_written;

write_cleanup:
    if (kbuf) kfree(kbuf);
write_exit:
    serial_log_exit("sys_write_impl", ret_val);
    return ret_val;
}

static int32_t sys_open_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)regs;
    serial_log_entry("sys_open_impl");
    const char *user_pathname = (const char*)arg1_ebx;
    int flags                 = (int)arg2_ecx;
    int mode                  = (int)arg3_edx; 
    int32_t ret_val; 

    serial_write("  Args: user_path="); serial_print_hex((uintptr_t)user_pathname);
    serial_write(" flags=0x"); serial_print_hex((uint32_t)flags);
    serial_write(" mode=0"); serial_print_hex((uint32_t)mode); serial_write("\n");

    char k_pathname[MAX_SYSCALL_STR_LEN];

    serial_log_step("Calling strncpy_from_user_safe...");
    int copy_err = strncpy_from_user_safe(user_pathname, k_pathname, sizeof(k_pathname));
    if (copy_err != 0) {
        serial_write("  ERROR: Path copy failed (err="); serial_print_sdec(copy_err); serial_write(")\n");
        ret_val = copy_err; goto open_exit;
    }
    serial_write("  STEP: Path copied successfully: \""); serial_write(k_pathname); serial_write("\"\n");

    serial_log_step("Calling sys_open (backend)...");
    ret_val = sys_open(k_pathname, flags, mode); 

open_exit:
    serial_log_exit("sys_open_impl", ret_val);
    return ret_val;
}

static int32_t sys_close_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)arg2_ecx; (void)arg3_edx; (void)regs;
    serial_log_entry("sys_close_impl");
    int fd = (int)arg1_ebx;
    serial_write("  Args: fd="); serial_print_sdec(fd); serial_write("\n");

    serial_log_step("Calling sys_close (backend)");
    int32_t ret_val = sys_close(fd); 

    serial_log_exit("sys_close_impl", ret_val);
    return ret_val;
}

static int32_t sys_lseek_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)regs;
    serial_log_entry("sys_lseek_impl");
    int fd         = (int)arg1_ebx;
    off_t offset   = (off_t)arg2_ecx; 
    int whence     = (int)arg3_edx;
    off_t ret_val_off; 

    serial_write("  Args: fd="); serial_print_sdec(fd);
    serial_write(" offset="); serial_print_sdec((int32_t)offset); 
    serial_write(" whence="); serial_print_sdec(whence); serial_write("\n");

    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        serial_log_exit("sys_lseek_impl", -EINVAL);
        return -EINVAL;
    }

    serial_log_step("Calling sys_lseek (backend)");
    ret_val_off = sys_lseek(fd, offset, whence); 

    serial_log_exit("sys_lseek_impl", (int32_t)ret_val_off);
    return (int32_t)ret_val_off; 
}

static int32_t sys_getpid_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)arg1_ebx; (void)arg2_ecx; (void)arg3_edx; (void)regs;
    serial_log_entry("sys_getpid_impl");
    pcb_t* current_proc = get_current_process();
    KERNEL_ASSERT(current_proc != NULL, "sys_getpid: No process context");
    int32_t pid = (int32_t)current_proc->pid; 
    serial_log_exit("sys_getpid_impl", pid);
    return pid;
}

static int32_t sys_puts_impl(uint32_t arg1_ebx, uint32_t arg2_ecx, uint32_t arg3_edx, isr_frame_t *regs) {
    (void)arg2_ecx; (void)arg3_edx; (void)regs;
    serial_log_entry("sys_puts_impl");
    const char *user_str_ptr = (const char *)arg1_ebx;
    int32_t ret_val = 0; 
    serial_write("  Args: user_str="); serial_print_hex((uintptr_t)user_str_ptr); serial_write("\n");

    char kbuffer[MAX_PUTS_LEN]; 
    serial_log_step("Calling strncpy_from_user_safe");
    int copy_err = strncpy_from_user_safe(user_str_ptr, kbuffer, sizeof(kbuffer));

    if (copy_err != 0) {
        ret_val = copy_err; goto puts_exit;
    }

    serial_log_step("Calling terminal_write");
    terminal_write(kbuffer); 
    ret_val = 0; 

puts_exit:
    serial_log_exit("sys_puts_impl", ret_val);
    return ret_val;
}


//-----------------------------------------------------------------------------
// Main Syscall Dispatcher
//-----------------------------------------------------------------------------
int32_t syscall_dispatcher(isr_frame_t *regs) { // CORRECTED: Returns int32_t
    serial_write("SD: Enter (v4.9 Debug)\n");
    KERNEL_ASSERT(regs != NULL, "Syscall dispatcher received NULL registers!");
    serial_write("SD: regs* = "); serial_print_hex((uintptr_t)regs); serial_write("\n");

    serial_write("SD: Frame: EAX(syscall#)=0x"); serial_print_hex(regs->eax);
    serial_write(" EBX(arg1)=0x"); serial_print_hex(regs->ebx);
    serial_write(" ECX(arg2)=0x"); serial_print_hex(regs->ecx);
    serial_write(" EDX(arg3)=0x"); serial_print_hex(regs->edx); serial_write("\n");
    serial_write("SD: Frame: EDI=0x"); serial_print_hex(regs->edi);
    serial_write(" ESI=0x"); serial_print_hex(regs->esi);
    serial_write(" EBP=0x"); serial_print_hex(regs->ebp);
    serial_write(" ESP_dummy=0x"); serial_print_hex(regs->esp_dummy); serial_write("\n");
    serial_write("SD: Frame: GS=0x"); serial_print_hex(regs->gs);
    serial_write(" FS=0x"); serial_print_hex(regs->fs);
    serial_write(" ES=0x"); serial_print_hex(regs->es);
    serial_write(" DS=0x"); serial_print_hex(regs->ds); serial_write("\n");
    serial_write("SD: Frame: INT#=0x"); serial_print_hex(regs->int_no);
    serial_write(" ERR=0x"); serial_print_hex(regs->err_code);
    serial_write(" EIP=0x"); serial_print_hex(regs->eip);
    serial_write(" CS=0x"); serial_print_hex(regs->cs);
    serial_write(" EFLAGS=0x"); serial_print_hex(regs->eflags); serial_write("\n");
    if ((regs->cs & 3) != 0) { 
        serial_write("SD: Frame: U_ESP=0x"); serial_print_hex(regs->useresp);
        serial_write(" U_SS=0x"); serial_print_hex(regs->ss); serial_write("\n");
    }

    uint32_t syscall_num = regs->eax;
    uint32_t arg1_ebx    = regs->ebx;
    uint32_t arg2_ecx    = regs->ecx;
    uint32_t arg3_edx    = regs->edx;

    serial_write("SD: Syscall Num (from regs->eax): "); serial_print_hex(syscall_num); serial_write("\n");
    serial_write("SD: Arg 1 (from regs->ebx): "); serial_print_hex(arg1_ebx); serial_write("\n");
    serial_write("SD: Arg 2 (from regs->ecx): "); serial_print_hex(arg2_ecx); serial_write("\n");
    serial_write("SD: Arg 3 (from regs->edx): "); serial_print_hex(arg3_edx); serial_write("\n");

    pcb_t* current_proc = get_current_process();
    uint32_t pid = current_proc ? current_proc->pid : 0xFFFFFFFF;
    serial_write("SD: GetProc -> PID="); serial_print_hex(pid); serial_write("\n");
    if (!current_proc) KERNEL_PANIC_HALT("Syscall executed without process context!");

    int32_t ret_val; 
    syscall_fn_t handler = NULL;

    if (syscall_num < MAX_SYSCALLS) {
        handler = syscall_table[syscall_num];
        serial_write("SD: Handler Lookup: syscall_table["); serial_print_hex(syscall_num);
        serial_write("] = 0x"); serial_print_hex((uintptr_t)handler); serial_write("\n");

        if (handler) { // Removed check against sys_not_implemented to ensure it's called if assigned
            serial_write("SD: Calling Specific Handler for syscall "); serial_print_hex(syscall_num); serial_write("...\n");
            ret_val = handler(arg1_ebx, arg2_ecx, arg3_edx, regs); 
            serial_write("SD: Specific Handler RetVal = "); serial_print_sdec(ret_val); serial_write("\n");
        } else {
            // This case should ideally not be reached if table is initialized to sys_not_implemented
            serial_write("SD: Handler is NULL for syscall "); serial_print_hex(syscall_num); serial_write("\n");
            ret_val = -ENOSYS;
        }
    } else {
        serial_write("SD: ERR Syscall Num Out Of Bounds ("); serial_print_hex(syscall_num);
        serial_write(" >= "); serial_print_hex(MAX_SYSCALLS); serial_write(")\n");
        ret_val = -ENOSYS;
    }

    serial_write("SD: SetRet: regs->eax will be set to "); serial_print_sdec(ret_val); serial_write("\n");
    regs->eax = (uint32_t)ret_val; 
    serial_write("SD: Exit (Final RetVal="); serial_print_sdec(ret_val);
    serial_write(" stored in regs->eax=0x"); serial_print_hex(regs->eax); serial_write(")\n");
    
    return ret_val; // CRITICAL FIX: Return the value for EAX register
}

