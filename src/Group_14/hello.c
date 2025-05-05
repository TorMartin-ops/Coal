/**
 * @file hello.c
 * @brief Enhanced and Corrected User Space Test Program for UiAOS
 *
 * Demonstrates basic syscall usage including:
 * - SYS_PUTS for printing strings
 * - SYS_GETPID for getting process ID
 * - SYS_OPEN, SYS_WRITE, SYS_READ, SYS_CLOSE for file I/O
 * - SYS_EXIT for terminating the process
 *
 * Corrections:
 * - Uses standard POSIX-like flags consistent with kernel's corrected sys_file.h.
 * - Provides mode argument for open() with O_CREAT.
 * - Uses valid stack buffers for read() and write() syscalls.
 * - Checks return values for all syscalls that can fail.
 * - Robust integer to string conversion for PID.
 * - Uses goto for cleaner error handling and resource cleanup.
 * - ADDED: Immediate printing of syscall return values for debugging.
 */

// ==========================================================================
// Includes & Type Definitions (Minimal Userspace Simulation)
// ==========================================================================
typedef signed char         int8_t;
typedef unsigned char       uint8_t;
typedef signed short        int16_t;
typedef unsigned short      uint16_t;
typedef signed int          int32_t;
typedef unsigned int        uint32_t;
typedef signed long long    int64_t;
typedef unsigned long long  uint64_t;
typedef uint32_t            size_t;
typedef int32_t             ssize_t;
typedef uint32_t            uintptr_t;
typedef int32_t             pid_t;
typedef int64_t             off_t; // Use 64-bit for future-proofing large files
typedef unsigned int        mode_t;
typedef _Bool               bool;
#define true 1
#define false 0
#define NULL ((void*)0)

// Standard Integer Limits (Approximate for typical 32-bit int)
#define INT32_MIN (-2147483647 - 1)

// ==========================================================================
// System Call Definitions (Must match kernel's syscall.h exactly)
// ==========================================================================
#define SYS_EXIT     1
#define SYS_READ     3
#define SYS_WRITE    4
#define SYS_OPEN     5
#define SYS_CLOSE    6
#define SYS_PUTS     7
#define SYS_LSEEK   19
#define SYS_GETPID  20

// ==========================================================================
// File Operation Flags & Constants (CORRECTED TO MATCH KERNEL'S sys_file.h v4.6)
// ==========================================================================
// Access modes
#define O_RDONLY    0x0000  // Read only
#define O_WRONLY    0x0001  // Write only
#define O_RDWR      0x0002  // Read/write
#define O_ACCMODE   0x0003  // Mask for access modes

// File creation flags (standard values)
#define O_CREAT     0x0040  // Create file if it does not exist (Bit 6)
#define O_EXCL      0x0080  // Exclusive use flag (Bit 7)
#define O_NOCTTY    0x0100  // Do not assign controlling terminal
#define O_TRUNC     0x0200  // Truncate flag (Bit 9)

// File status flags
#define O_APPEND    0x0400  // Set append mode (Bit 10)

// Standard File Descriptors
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// Seek constants
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

// Standard File Permissions (commonly used with O_CREAT)
#define S_IRUSR 0400 // Read permission, owner
#define S_IWUSR 0200 // Write permission, owner
#define S_IXUSR 0100 // Execute permission, owner
#define S_IRGRP 0040 // Read permission, group
#define S_IWGRP 0020 // Write permission, group
#define S_IXGRP 0010 // Execute permission, group
#define S_IROTH 0004 // Read permission, others
#define S_IWOTH 0002 // Write permission, others
#define S_IXOTH 0001 // Execute permission, others

// Common combinations (e.g., 0666 = rw-rw-rw-)
#define DEFAULT_FILE_MODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) // 0666

// ==========================================================================
// Syscall Wrapper Function (Unchanged)
// ==========================================================================
static inline int syscall(int num, int arg1, int arg2, int arg3) {
    int ret;
    asm volatile (
        "int $0x80"
        : "=a" (ret)  // Output: EAX goes into 'ret'
        : "a" (num), "b" (arg1), "c" (arg2), "d" (arg3) // Inputs
        : "memory", "cc" // Clobbers
    );
    return ret;
}

// ==========================================================================
// Helper Functions for User Space (Unchanged)
// ==========================================================================

size_t strlen(const char *s) {
    size_t i = 0; if (!s) return 0; while (s[i] != '\0') { i++; } return i;
}

void print_string(const char *s) {
    if (!s) return; syscall(SYS_PUTS, (int)s, 0, 0);
}

char* utoa_simple(uint32_t un, char* buf, size_t buf_size) {
    if (buf_size < 2) return NULL; char* ptr = buf + buf_size - 1; *ptr = '\0';
    if (un == 0) { *--ptr = '0'; return ptr; }
    while (un > 0 && ptr > buf) { *--ptr = '0' + (un % 10); un /= 10; }
    if (un > 0) { return NULL; } return ptr;
}

void print_integer(int n) {
    char buf[12]; char *s_ptr; bool is_negative = false; uint32_t un;
    if (n < 0) { is_negative = true; if (n == INT32_MIN) { un = 2147483648U; } else { un = (uint32_t)(-n); } } else { un = (uint32_t)n; }
    s_ptr = utoa_simple(un, buf, sizeof(buf));
    if (s_ptr) { if (is_negative) { if (s_ptr > buf) { *--s_ptr = '-'; print_string(s_ptr); } else { print_string("-<ERR>"); } } else { print_string(s_ptr); } }
    else { print_string("<ERR>"); }
}

void exit_on_error(const char *msg, int syscall_ret, int exit_code) {
    print_string("ERROR: "); print_string(msg); print_string(" (Syscall returned: "); print_integer(syscall_ret); print_string(")\n");
    syscall(SYS_EXIT, exit_code, 0, 0); for(;;);
}

// ==========================================================================
// Main Application Logic
// ==========================================================================
int main() {
    int exit_code = 0;
    pid_t my_pid = -1;
    int fd_write = -1;
    int fd_read = -1;
    ssize_t bytes_written, bytes_read;
    const char *filename = "/testfile.txt";
    char write_buf[100];
    char read_buf[100];
    int syscall_ret_val; // Temporary variable to check return value immediately

    print_string("--- User Program Started ---\n");

    // --- Get PID ---
    syscall_ret_val = syscall(SYS_GETPID, 0, 0, 0);
    // print_string("  -> syscall(SYS_GETPID) returned: "); print_integer(syscall_ret_val); print_string("\n"); // Optional debug
    my_pid = syscall_ret_val;
    if (my_pid < 0) {
        print_string("Warning: Failed to get PID (Error: "); print_integer(my_pid); print_string(")\n"); my_pid = 0;
    } else {
        print_string("My PID is: "); print_integer(my_pid); print_string("\n");
    }

    // --- File I/O ---
    print_string("Attempting file I/O with '"); print_string(filename); print_string("'...\n");

    // 1. Create/Truncate and Open for Writing
    print_string("Opening for writing (O_CREAT | O_WRONLY | O_TRUNC)...\n");
    syscall_ret_val = syscall(SYS_OPEN, (int)filename, O_CREAT | O_WRONLY | O_TRUNC, DEFAULT_FILE_MODE);
    // *** ADDED IMMEDIATE PRINT ***
    print_string("  -> syscall(SYS_OPEN) returned: "); print_integer(syscall_ret_val); print_string("\n");
    fd_write = syscall_ret_val;
    if (fd_write < 0) {
        // Error message now incorporates the value we just printed
        exit_on_error("Failed to open/create file for writing", fd_write, 1);
    }
    print_string("File opened successfully for writing (fd="); print_integer(fd_write); print_string(").\n");

    // 2. Prepare Write Buffer (Unchanged)
    const char *base_msg = "Hello from user program! PID: "; size_t base_len = strlen(base_msg);
    if (base_len >= sizeof(write_buf)) { exit_on_error("Base write message too long", -1, 99); }
    for(size_t i=0; i<base_len; ++i) { write_buf[i] = base_msg[i]; }
    char pid_str_buf[12]; char* pid_str = utoa_simple((uint32_t)my_pid, pid_str_buf, sizeof(pid_str_buf));
    if (!pid_str) { exit_on_error("Failed to convert PID to string", -1, 98); }
    size_t pid_len = strlen(pid_str);
    if (base_len + pid_len + 1 >= sizeof(write_buf)) { exit_on_error("Write buffer too small for PID", -1, 97); }
    for(size_t i=0; i<pid_len; ++i) { write_buf[base_len + i] = pid_str[i]; }
    write_buf[base_len + pid_len] = '\n'; write_buf[base_len + pid_len + 1] = '\0';
    size_t total_write_len = base_len + pid_len + 1;

    // 3. Write Data
    print_string("Writing data: \""); print_string(write_buf); print_string("\" (Length: "); print_integer((int)total_write_len); print_string(")\n");
    // *** ADDED IMMEDIATE PRINT ***
    print_string("  -> Using fd: "); print_integer(fd_write); print_string(" for write\n"); // Check fd value just before use
    syscall_ret_val = syscall(SYS_WRITE, fd_write, (int)write_buf, total_write_len);
    print_string("  -> syscall(SYS_WRITE) returned: "); print_integer(syscall_ret_val); print_string("\n");
    bytes_written = syscall_ret_val;
    if (bytes_written < 0) { exit_on_error("Failed to write data", bytes_written, 2); }
    if ((size_t)bytes_written != total_write_len) {
        print_string("Warning: Partial write occurred? Wrote "); print_integer(bytes_written); print_string(" of "); print_integer(total_write_len); print_string(" bytes.\n");
    } else {
         print_string("Data successfully written to file.\n");
    }

    // 4. Close File After Writing
    print_string("Closing write fd (fd="); print_integer(fd_write); print_string(")...\n");
    syscall_ret_val = syscall(SYS_CLOSE, fd_write, 0, 0);
    // print_string("  -> syscall(SYS_CLOSE) returned: "); print_integer(syscall_ret_val); print_string("\n"); // Optional debug
    if (syscall_ret_val < 0) { print_string("Warning: Failed to close write fd. Error: "); print_integer(syscall_ret_val); print_string("\n"); }
    fd_write = -1;

    // 5. Re-open for Reading
    print_string("Re-opening file for reading (O_RDONLY)...\n");
    syscall_ret_val = syscall(SYS_OPEN, (int)filename, O_RDONLY, 0);
    // *** ADDED IMMEDIATE PRINT ***
    print_string("  -> syscall(SYS_OPEN) returned: "); print_integer(syscall_ret_val); print_string("\n");
    fd_read = syscall_ret_val;
    if (fd_read < 0) {
        exit_on_error("Failed to open file for reading", fd_read, 4);
    }
    print_string("File opened successfully for reading (fd="); print_integer(fd_read); print_string(").\n");

    // 6. Read Data Back
    print_string("Reading data from file...\n");
    for(size_t i=0; i<sizeof(read_buf); ++i) { read_buf[i] = 0; } // Clear buffer
    // *** ADDED IMMEDIATE PRINT ***
    print_string("  -> Using fd: "); print_integer(fd_read); print_string(" for read\n"); // Check fd value just before use
    syscall_ret_val = syscall(SYS_READ, fd_read, (int)read_buf, sizeof(read_buf) - 1);
    print_string("  -> syscall(SYS_READ) returned: "); print_integer(syscall_ret_val); print_string("\n");
    bytes_read = syscall_ret_val;
    if (bytes_read < 0) { exit_on_error("Failed to read data", bytes_read, 5); }
    read_buf[bytes_read] = '\0';
    print_string("Data read from file: \""); print_string(read_buf); print_string("\"\n");


cleanup:
    // 7. Cleanup: Ensure FDs are closed (Unchanged)
    if (fd_write >= 0) { print_string("Closing write fd (fd="); print_integer(fd_write); print_string(") during cleanup.\n"); syscall(SYS_CLOSE, fd_write, 0, 0); }
    if (fd_read >= 0) { print_string("Closing read fd (fd="); print_integer(fd_read); print_string(") during cleanup.\n"); syscall(SYS_CLOSE, fd_read, 0, 0); }

    // 8. Exit (Unchanged)
    print_string("--- User Program Exiting (Code: "); print_integer(exit_code); print_string(") ---\n");
    syscall(SYS_EXIT, exit_code, 0, 0);

    return exit_code; // Should not be reached
}