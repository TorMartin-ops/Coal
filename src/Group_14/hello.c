/**
 * @file hello.c
 * @brief Robust User Space Test Program for UiAOS (v1.5.4 - Syscall Fix 2)
 *
 * Demonstrates robust syscall usage including error checking and
 * proper resource management (file descriptors). Addresses potential
 * compiler scope issues.
 *
 * v1.5.4 Changes:
 * - CORRECTED: Removed "ebx", "ecx", "edx" from the clobber list in the
 * inline assembly syscall wrapper, resolving the "impossible constraints"
 * build error. Input constraints are sufficient to reserve these registers.
 * - Retained logic from 1.5.3 ensuring correct FD usage after open.
 */

// ==========================================================================
// Includes & Type Definitions (Minimal Userspace Simulation)
// ==========================================================================
// Standard integer types (assuming custom libc headers)
typedef signed char         int8_t;
typedef unsigned char       uint8_t;
typedef signed short        int16_t;
typedef unsigned short      uint16_t;
typedef signed int          int32_t;
typedef unsigned int        uint32_t;
typedef signed long long    int64_t;
typedef unsigned long long  uint64_t;

// Standard size/pointer types (assuming custom libc headers)
typedef uint32_t            size_t;
typedef int32_t             ssize_t; // Signed size for read/write returns
typedef uint32_t            uintptr_t;

// Process/File related types
typedef int32_t             pid_t;   // Process ID
typedef int64_t             off_t;   // File offset (64-bit recommended)
typedef unsigned int        mode_t;  // File mode

// Boolean type (assuming custom libc header)
typedef _Bool               bool;
#define true 1
#define false 0

// NULL definition (assuming custom libc header)
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
#define SYS_PUTS     7 // Non-standard, assumes kernel provides it
#define SYS_LSEEK   19
#define SYS_GETPID  20

// ==========================================================================
// File Operation Flags & Constants (Match kernel's sys_file.h)
// ==========================================================================
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_NOCTTY    0x0100
#define O_TRUNC     0x0200
#define O_APPEND    0x0400

#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IROTH 0004
#define S_IWOTH 0002
#define DEFAULT_FILE_MODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) // 0666

// ==========================================================================
// Buffer Sizes
// ==========================================================================
#define WRITE_BUFFER_SIZE   100
#define READ_BUFFER_SIZE    100
#define INT_STR_BUFFER_SIZE 12 // Sufficient for 32-bit signed int + sign + null

// ==========================================================================
// Syscall Wrapper Function - CORRECTED (v1.5.4)
// ==========================================================================
static inline int syscall(int num, int arg1, int arg2, int arg3) {
    int ret;
    // Standard syscall invocation via INT 0x80
    // EAX = syscall number
    // EBX, ECX, EDX = arguments 1, 2, 3
    // Return value in EAX
    asm volatile (
        "int $0x80"
        : "=a" (ret)  // Output: EAX ('a') -> ret
        : "a" (num), "b" (arg1), "c" (arg2), "d" (arg3) // Inputs: EAX, EBX, ECX, EDX
        // === FIX v1.5.4: REMOVED "ebx", "ecx", "edx" FROM CLOBBER LIST ===
        // Input constraints suffice. memory/cc are still needed.
        : "memory", "cc"
    );
    return ret;
}

// ==========================================================================
// Helper Functions for User Space (Unchanged)
// ==========================================================================

// Calculates string length
size_t strlen(const char *s) {
    size_t i = 0; if (!s) return 0; while (s[i] != '\0') { i++; } return i;
}

// Prints a null-terminated string using SYS_PUTS
void print_string(const char *s) {
    if (!s) return;
    syscall(SYS_PUTS, (int)s, 0, 0); // Use the kernel's string printing syscall
}

// Simple unsigned int to ASCII (decimal), returns pointer *within* buf
// Writes backwards from the end of the buffer.
char* utoa_simple(uint32_t un, char* buf, size_t buf_size) {
    if (buf_size < 2) return NULL; // Need space for at least '0' and '\0'
    char* ptr = buf + buf_size - 1; // Start from the end
    *ptr = '\0'; // Null-terminate

    if (un == 0) {
        if (ptr == buf) return NULL; // Buffer too small even for "0"
        *--ptr = '0';
        return ptr;
    }

    while (un > 0) {
        if (ptr == buf) return NULL; // Buffer too small
        *--ptr = '0' + (un % 10);
        un /= 10;
    }
    return ptr; // Return pointer to the start of the digits
}

// Simple signed int to ASCII (decimal), handles negatives and INT_MIN edge case
// Uses a static buffer to prevent reuse issues between consecutive calls.
void print_integer(int n) {
    // NOTE: Using a static buffer makes this function non-reentrant.
    static char buf[INT_STR_BUFFER_SIZE];
    char *s_ptr;
    bool is_negative = (n < 0);
    uint32_t un;

    // Handle sign and potential INT_MIN overflow
    if (is_negative) {
        un = (n == INT32_MIN) ? 2147483648U : (uint32_t)(-n);
    } else {
        un = (uint32_t)n;
    }

    // Convert the unsigned number part
    s_ptr = utoa_simple(un, buf, sizeof(buf));

    if (!s_ptr) { // Check if conversion failed (buffer too small)
        print_string("<INT_ERR>");
        return;
    }

    // Prepend '-' if negative and space allows
    if (is_negative) {
        if (s_ptr > buf) {
            *--s_ptr = '-';
        } else {
            // Error: buffer too small for sign (should not happen with size 12)
            print_string("-<ERR>");
            return;
        }
    }
    print_string(s_ptr); // Print the resulting string
}

// Prints error message using print_string/print_integer and exits via syscall
void exit_on_error(const char *context_msg, int syscall_ret_val, int exit_code) {
    print_string("FATAL ERROR: ");
    print_string(context_msg);
    print_string(" (Syscall returned: ");
    print_integer(syscall_ret_val); // Print the actual (likely negative) error code
    print_string(")\n");
    syscall(SYS_EXIT, exit_code, 0, 0);
    // Halt loop in case exit syscall fails (should not happen)
    for(;;);
}

// ==========================================================================
// Main Application Logic (Unchanged from v1.5.3)
// ==========================================================================
int main() {
    int exit_code = 0; // Final exit code for the process
    pid_t my_pid = -1;   // Process ID
    int fd_write = -1; // File descriptor for writing, initialized to invalid
    int fd_read = -1;  // File descriptor for reading, initialized to invalid
    ssize_t bytes_written = 0;
    ssize_t bytes_read = 0;
    const char *filename = "/testfile.txt";
    char write_buf[WRITE_BUFFER_SIZE];
    char read_buf[READ_BUFFER_SIZE];
    int syscall_ret_val; // Temporary variable to hold syscall return value


    print_string("--- User Program Started v1.5.4 (Syscall Fix 2) ---\n");

    // --- Get Process ID ---
    syscall_ret_val = syscall(SYS_GETPID, 0, 0, 0);
    my_pid = syscall_ret_val; // Store return value
    if (my_pid < 0) {
        print_string("Warning: Failed to get PID (Error: "); print_integer(my_pid); print_string(")\n");
        my_pid = 0;
    } else {
        print_string("My PID is: "); print_integer(my_pid); print_string("\n");
    }

    // --- File I/O Test ---
    print_string("Attempting file I/O with '"); print_string(filename); print_string("'...\n");

    // 1. Create/Truncate and Open for Writing
    print_string("Opening for writing (O_CREAT | O_WRONLY | O_TRUNC)...\n");
    syscall_ret_val = syscall(SYS_OPEN, (int)filename, O_CREAT | O_WRONLY | O_TRUNC, DEFAULT_FILE_MODE);
    fd_write = syscall_ret_val; // Store return value
    print_string("  -> syscall(SYS_OPEN) returned: "); print_integer(fd_write); print_string("\n"); // Print stored value
    if (fd_write < 0) {
        exit_on_error("Failed to open/create file for writing", fd_write, 1);
    }
    print_string("File opened successfully for writing (fd="); print_integer(fd_write); print_string(").\n");

    // 2. Prepare Write Buffer (Unchanged logic)
    const char *base_msg = "Hello from user program! PID: "; size_t base_len = strlen(base_msg);
    char pid_str_buf[INT_STR_BUFFER_SIZE]; char* pid_str = utoa_simple((uint32_t)my_pid, pid_str_buf, sizeof(pid_str_buf));
    if (!pid_str) { exit_on_error("Failed to convert PID to string", -1, 98); }
    size_t pid_len = strlen(pid_str);
    size_t total_write_len = base_len + pid_len + 1; // Include newline

    if (total_write_len + 1 > sizeof(write_buf)) { exit_on_error("Write buffer too small", -1, 97); }
    size_t pos = 0;
    for(size_t i = 0; i < base_len; ++i) write_buf[pos++] = base_msg[i];
    for(size_t i = 0; i < pid_len; ++i) write_buf[pos++] = pid_str[i];
    write_buf[pos++] = '\n';
    write_buf[pos++] = '\0';

    // 3. Write Data to File
    print_string("Writing data: \""); print_string(write_buf); print_string("\" (Length: "); print_integer((int)total_write_len); print_string(")\n");
    print_string("  -> Using fd: "); print_integer(fd_write); print_string(" for write\n"); // Correctly print fd_write
    syscall_ret_val = syscall(SYS_WRITE, fd_write, (int)write_buf, (int)total_write_len); // Pass fd_write
    bytes_written = syscall_ret_val; // Store return value
    print_string("  -> syscall(SYS_WRITE) returned: "); print_integer(bytes_written); print_string("\n"); // Print stored value
    if (bytes_written < 0) {
        exit_code = 2;
        print_string("ERROR: Failed to write data (Syscall returned: "); print_integer(bytes_written); print_string(")\n");
        goto cleanup;
    }
    if ((size_t)bytes_written != total_write_len) {
        print_string("Warning: Partial write occurred! Wrote "); print_integer(bytes_written); print_string(" of "); print_integer((int)total_write_len); print_string(" bytes.\n");
    } else {
         print_string("Data successfully written to file.\n");
    }

    // 4. Close Write File Descriptor
    print_string("Closing write fd (fd="); print_integer(fd_write); print_string(")...\n"); // Correctly print fd_write
    syscall_ret_val = syscall(SYS_CLOSE, fd_write, 0, 0); // Pass fd_write
    if (syscall_ret_val < 0) { print_string("Warning: Failed to close write fd ("); print_integer(fd_write); print_string("). Error: "); print_integer(syscall_ret_val); print_string("\n"); }
    fd_write = -1; // Mark as closed

    // 5. Re-open for Reading
    print_string("Re-opening file for reading (O_RDONLY)...\n");
    syscall_ret_val = syscall(SYS_OPEN, (int)filename, O_RDONLY, 0); // Mode not needed for RDONLY
    fd_read = syscall_ret_val; // Store return value
    print_string("  -> syscall(SYS_OPEN) returned: "); print_integer(fd_read); print_string("\n"); // Print stored value
    if (fd_read < 0) {
        exit_on_error("Failed to open file for reading", fd_read, 4);
    }
    print_string("File opened successfully for reading (fd="); print_integer(fd_read); print_string(").\n");

    // 6. Read Data Back
    print_string("Reading data from file...\n");
    for(size_t i=0; i<sizeof(read_buf); ++i) { read_buf[i] = 0; } // Clear buffer
    print_string("  -> Using fd: "); print_integer(fd_read); print_string(" for read\n"); // Correctly print fd_read
    syscall_ret_val = syscall(SYS_READ, fd_read, (int)read_buf, sizeof(read_buf) - 1); // Pass fd_read
    bytes_read = syscall_ret_val; // Store return value
    print_string("  -> syscall(SYS_READ) returned: "); print_integer(bytes_read); print_string("\n"); // Print stored value
    if (bytes_read < 0) {
        exit_code = 5;
        print_string("ERROR: Failed to read data (Syscall returned: "); print_integer(bytes_read); print_string(")\n");
        goto cleanup;
    }
    // Null-terminate the buffer correctly
    if (bytes_read >= 0 && (size_t)bytes_read < sizeof(read_buf)) {
        read_buf[bytes_read] = '\0';
    } else if ((size_t)bytes_read >= sizeof(read_buf)) {
        read_buf[sizeof(read_buf) - 1] = '\0';
        print_string("Warning: Read filled buffer, potential truncation.\n");
    }

    print_string("Data read from file: \""); print_string(read_buf); print_string("\"\n");

    // 7. Verify read content matches written content (Unchanged logic)
    if (bytes_read != (ssize_t)total_write_len) {
        print_string("ERROR: Read length ("); print_integer(bytes_read); print_string(") does not match written length ("); print_integer((int)total_write_len); print_string(").\n");
        exit_code = 6;
    } else {
        bool content_match = true;
        for(size_t i=0; i<total_write_len; ++i) { if(read_buf[i] != write_buf[i]) { content_match = false; break; } }
        if (!content_match) { print_string("ERROR: Read content does not match written content!\n"); exit_code = 7; }
        else { print_string("Read content matches written content.\n"); }
    }

cleanup:
    // 8. Cleanup: Ensure FDs are closed if they are still valid (>= 0)
    print_string("--- Entering Cleanup ---\n");
    if (fd_write >= 0) {
        print_string("Closing write fd (fd="); print_integer(fd_write); print_string(") during cleanup.\n"); // Correctly print fd_write
        syscall_ret_val = syscall(SYS_CLOSE, fd_write, 0, 0); // Pass fd_write
        if (syscall_ret_val < 0) { print_string("  Warning: Close failed (Error: "); print_integer(syscall_ret_val); print_string(")\n"); }
    }
    if (fd_read >= 0) {
        print_string("Closing read fd (fd="); print_integer(fd_read); print_string(") during cleanup.\n"); // Correctly print fd_read
        syscall_ret_val = syscall(SYS_CLOSE, fd_read, 0, 0); // Pass fd_read
        if (syscall_ret_val < 0) { print_string("  Warning: Close failed (Error: "); print_integer(syscall_ret_val); print_string(")\n"); }
    }

    // 9. Exit
    if (exit_code == 0) {
        print_string("--- User Program Exiting Successfully ---\n");
    } else {
        print_string("--- User Program Exiting with Error Code: "); print_integer(exit_code); print_string(" ---\n");
    }
    syscall(SYS_EXIT, exit_code, 0, 0);

    // Should not be reached
    print_string("--- ERROR: Execution continued after SYS_EXIT! ---\n");
    for(;;);
    return exit_code;
}