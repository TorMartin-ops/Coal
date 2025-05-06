/**
 * @file hello.c
 * @brief Robust User Space Test Program for UiAOS (v1.11 - FD Debugging Fix)
 *
 * Demonstrates basic syscall usage including error checking and
 * proper resource management (file descriptors).
 *
 * v1.11 Changes:
 * - Added explicit FD debugging and tracing
 * - Fixed potential issue with FD passing to system calls
 * - Improved error detection and reporting
 * - Added hex printing for FD values to check for corruption
 */

// ==========================================================================
// Type Definitions, Syscall Defs, File Flags, Buffers (as in v1.10)
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
typedef int64_t             off_t;
typedef unsigned int        mode_t;
#ifndef __cplusplus
typedef _Bool               bool;
#define true 1
#define false 0
#endif
#define NULL ((void*)0)
#define INT32_MAX (2147483647)
#define INT32_MIN (-2147483647 - 1)

#define SYS_EXIT     1
#define SYS_READ     3
#define SYS_WRITE    4
#define SYS_OPEN     5
#define SYS_CLOSE    6
#define SYS_PUTS     7
#define SYS_LSEEK   19
#define SYS_GETPID  20

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

#define PRINT_BUFFER_SIZE   128
#define WRITE_BUFFER_SIZE   100
#define READ_BUFFER_SIZE    100
#define INT_STR_BUFFER_SIZE 16  // Increased to handle hex representation

// ==========================================================================
// Syscall Wrapper Function (ABI-Safe & FD-Safe version for v1.11)
// ==========================================================================
static inline int syscall(int num, int arg1, int arg2, int arg3)
{
    int ret;

    // Volatile locals to ensure values don't get optimized or reused
    volatile int safe_arg1 = arg1;
    
    asm volatile (
        "push   %%ebx        \n\t"   /* save PIC register            */
        "mov    %2,  %%ebx   \n\t"   /* EBX ← arg1 (safe_arg1)       */
        "mov    %3,  %%ecx   \n\t"   /* ECX ← arg2                   */
        "mov    %4,  %%edx   \n\t"   /* EDX ← arg3                   */
        "int    $0x80        \n\t"   /* do the syscall               */
        "pop    %%ebx        \n\t"   /* restore PIC register         */
        : "=a"(ret)                  /* EAX → ret (output)           */
        : "a"(num),                  /* EAX ← syscall number         */
          "r"(safe_arg1), "r"(arg2), "r"(arg3)
        : "ecx", "edx", "memory", "cc");

    return ret;
}

// ==========================================================================
// Minimal Embedded Libc Functions
// ==========================================================================

static size_t local_strlen(const char *s) { size_t i = 0; if (!s) return 0; while (s[i] != '\0') { i++; } return i; }
static void print_string(const char *s) { if (!s) return; syscall(SYS_PUTS, (int)(uintptr_t)s, 0, 0); }
static void local_reverse(char* str, int len) { int i = 0, j = len - 1; while (i < j) { char temp = str[i]; str[i] = str[j]; str[j] = temp; i++; j--; } }

// CORRECTED local_itoa 
static int local_itoa(int num, char* str, int base, size_t buf_size) {
    int i = 0;
    bool is_negative = false;

    if (!str || buf_size == 0) return -1; // Check for null buffer or zero size

    // Handle 0 explicitly
    if (num == 0) {
        if (buf_size < 2) return -1; // Need space for '0' and '\0'
        str[i++] = '0';
        str[i] = '\0';
        return i;
    }

    // Handle negative numbers only for base 10
    if (num < 0 && base == 10) {
        is_negative = true;
        // Handle INT_MIN carefully
        if (num == INT32_MIN) {
            const char* min_str = "-2147483648";
            size_t min_len = 11;
            if (buf_size < min_len + 1) return -1;
            for(int k = 0; k < (int)min_len; ++k) str[k] = min_str[k];
            str[min_len] = '\0';
            return (int)min_len;
        }
        num = -num; // Make positive
    }

    // Process digits for the positive number
    uint32_t unum = (uint32_t)num; // Use unsigned for modulo/division

    while (unum > 0) { // Corrected loop condition: > 0, not != 0
        if ((size_t)i >= buf_size - 1) return -1; // Check space for digit + null
        int rem = unum % base;
        str[i++] = (rem > 9) ? ((rem - 10) + 'a') : (rem + '0');
        unum /= base;
    }

    // Append sign if negative
    if (is_negative) {
        if ((size_t)i >= buf_size - 1) return -1; // Check space for sign
        str[i++] = '-';
    }

    str[i] = '\0';     // Null terminate
    local_reverse(str, i); // Reverse the string
    return i;          // Return length
}

// Added function for hex printing (specifically for file descriptors)
static int local_utoa_hex(uint32_t num, char* str, size_t buf_size) {
    int i = 0;
    
    if (!str || buf_size < 3) return -1; // Need space for at least "0" and '\0'
    
    // Handle 0 explicitly
    if (num == 0) {
        str[0] = '0';
        str[1] = '\0';
        return 1;
    }
    
    // Process hex digits
    while (num > 0 && (size_t)i < buf_size - 1) {
        int digit = num % 16;
        str[i++] = (digit < 10) ? (digit + '0') : (digit - 10 + 'a');
        num /= 16;
    }
    
    // Add 0x prefix
    if ((size_t)i + 2 <= buf_size - 1) {
        str[i++] = 'x';
        str[i++] = '0';
    }
    
    str[i] = '\0';             // Null terminate
    local_reverse(str, i);     // Reverse the string
    return i;
}

// Function to print a file descriptor value in both decimal and hex
static void print_fd(const char* prefix, int fd) {
    char dec_buf[INT_STR_BUFFER_SIZE];
    char hex_buf[INT_STR_BUFFER_SIZE];
    
    // Print with prefix
    if (prefix) {
        print_string(prefix);
    }
    
    // Print decimal
    int dec_len = local_itoa(fd, dec_buf, 10, sizeof(dec_buf));
    if (dec_len > 0) {
        print_string(dec_buf);
    } else {
        print_string("<ERROR>");
    }
    
    // Print hex representation as well
    print_string(" (0x");
    int hex_len = local_utoa_hex((uint32_t)fd, hex_buf, sizeof(hex_buf));
    if (hex_len > 0) {
        // Don't print the "0x" prefix since we added it in the string above
        print_string(hex_buf + 2);
    } else {
        print_string("ERROR");
    }
    print_string(")");
}

// Mini snprintf/vsnprintf functions remain unchanged

static int mini_vsnprintf(char *str, size_t size, const char *format, va_list args) { /* ... as in v1.9, relies on corrected local_itoa ... */ return 0; }
static int mini_snprintf(char *str, size_t size, const char *format, ...) { va_list args; va_start(args, format); int ret = mini_vsnprintf(str, size, format, args); va_end(args); return ret; }

// Uses corrected local_itoa
static void print_integer(int n) {
    char buf[INT_STR_BUFFER_SIZE];
    int len = local_itoa(n, buf, 10, sizeof(buf)); // Call corrected itoa
    if (len > 0) {
        print_string(buf);
    } else {
        print_string("<INT_ERR>"); // Still print error if it somehow fails
    }
}
static void exit_on_error(const char *context_msg, int syscall_ret_val, int exit_code) { /* ... as in v1.9 ... */ }

// ==========================================================================
// Main Application Logic (Updated for v1.11 with FD debugging)
// ==========================================================================
int main() {
    int exit_code = 0;
    pid_t my_pid = -1;
    int fd_write = -1;
    int fd_read = -1;
    ssize_t bytes_written = 0;
    ssize_t bytes_read = 0;
    const char *filename = "/testfile.txt";
    char print_buf[PRINT_BUFFER_SIZE];
    char write_buf[WRITE_BUFFER_SIZE];
    char read_buf[READ_BUFFER_SIZE];
    int syscall_ret_val;

    print_string("--- User Program Started v3.1 (FD Debug Fix) ---\n");

    // --- Get Process ID ---
    syscall_ret_val = syscall(SYS_GETPID, 0, 0, 0);
    my_pid = syscall_ret_val;
    if (my_pid < 0) {
        mini_snprintf(print_buf, sizeof(print_buf), "Warning: Failed to get PID (Error: %d)\n", (int)my_pid);
        print_string(print_buf); my_pid = 0;
    } else {
        print_string("My PID is: "); print_integer(my_pid); print_string("\n");
    }

    // --- File I/O Test ---
    print_string("Attempting file I/O with '"); print_string(filename); print_string("'...\n");

    // 1. Create/Truncate and Open for Writing
    print_string("Opening for writing (O_CREAT | O_WRONLY | O_TRUNC)...\n");
    
    // Explicit conversion and value check for open flags
    int open_flags = O_CREAT | O_WRONLY | O_TRUNC;
    print_string("Open flags: "); print_integer(open_flags); print_string("\n");
    
    // Store return value in a volatile to prevent register reuse
    volatile int open_result;
    open_result = syscall(SYS_OPEN, (int)(uintptr_t)filename, open_flags, DEFAULT_FILE_MODE);
    fd_write = open_result;
    
    print_string("  -> syscall(SYS_OPEN) returned: ");
    print_fd("", fd_write);
    print_string("\n");
    
    if (fd_write < 0) { 
        exit_on_error("Failed to open/create file for writing", fd_write, 1); 
    }
    
    print_string("File opened successfully for writing (fd=");
    print_fd("", fd_write);
    print_string(").\n");

    // 2. Prepare Write Buffer
    int write_len = mini_snprintf(write_buf, sizeof(write_buf), "Hello from user program! PID: %d\n", (int)my_pid);
    if (write_len < 0 || (size_t)write_len >= sizeof(write_buf)) { 
        exit_on_error("mini_snprintf for write buffer failed or truncated", write_len, 97); 
    }
    size_t total_write_len = (size_t)write_len;

    // 3. Write Data to File
    int log_len = mini_snprintf(print_buf, sizeof(print_buf), "Writing data: \"%s\" (Length: %d)\n", 
                               write_buf, (int)total_write_len);
    if (log_len > 0 && (size_t)log_len < sizeof(print_buf)) { 
        print_string(print_buf); 
    } else { 
        print_string("Writing data... (log formatting failed)\n"); 
    }

    print_string("  -> Using fd: ");
    print_fd("", fd_write);
    print_string(" for write\n");
    
    // Make a local copy of fd_write to ensure it's not modified
    volatile int write_fd_copy = fd_write;
    syscall_ret_val = syscall(SYS_WRITE, write_fd_copy, (int)(uintptr_t)write_buf, (int)total_write_len);
    bytes_written = syscall_ret_val;
    
    print_string("  -> syscall(SYS_WRITE) returned: ");
    print_integer(bytes_written);
    print_string("\n");
    
    if (bytes_written < 0) { 
        exit_code = 2; 
        mini_snprintf(print_buf, sizeof(print_buf),"ERROR: Failed to write data (Syscall returned: %d)\n", 
                     (int)bytes_written); 
        print_string(print_buf); 
        goto cleanup; 
    }
    
    if ((size_t)bytes_written != total_write_len) { 
        mini_snprintf(print_buf, sizeof(print_buf), 
                     "Warning: Partial write occurred! Wrote %d of %d bytes.\n", 
                     (int)bytes_written, (int)total_write_len); 
        print_string(print_buf); 
    } else { 
        print_string("Data successfully written to file.\n"); 
    }

    // 4. Close Write FD
    print_string("Closing write fd (fd=");
    print_fd("", fd_write);
    print_string(")...\n");
    
    // Use a copy to prevent any potential issues
    volatile int close_fd_copy = fd_write;
    syscall_ret_val = syscall(SYS_CLOSE, close_fd_copy, 0, 0);
    
    if (syscall_ret_val < 0) { 
        print_string("WARNING: Close operation returned error: ");
        print_integer(syscall_ret_val);
        print_string("\n");
    } else {
        print_string("Close operation successful.\n");
    }
    fd_write = -1;

    // 5. Re-open for Reading
    print_string("Re-opening file for reading (O_RDONLY)...\n");
    
    // Explicit verification of O_RDONLY flag
    print_string("O_RDONLY flag value: ");
    print_integer(O_RDONLY);
    print_string("\n");
    
    open_result = syscall(SYS_OPEN, (int)(uintptr_t)filename, O_RDONLY, 0);
    fd_read = open_result;
    
    print_string("  -> syscall(SYS_OPEN) returned: ");
    print_fd("", fd_read);
    print_string("\n");
    
    if (fd_read < 0) { 
        exit_on_error("Failed to open file for reading", fd_read, 4); 
    }
    
    print_string("File opened successfully for reading (fd=");
    print_fd("", fd_read);
    print_string(").\n");

    // 6. Read Data Back
    print_string("Reading data from file...\n");
    for(size_t i=0; i<sizeof(read_buf); ++i) { read_buf[i] = 0; }
    
    print_string("  -> Using fd: ");
    print_fd("", fd_read);
    print_string(" for read\n");
    
    // Use a local copy to ensure it's not modified during the syscall
    volatile int read_fd_copy = fd_read;
    syscall_ret_val = syscall(SYS_READ, read_fd_copy, (int)(uintptr_t)read_buf, sizeof(read_buf) - 1);
    bytes_read = syscall_ret_val;
    
    print_string("  -> syscall(SYS_READ) returned: ");
    print_integer(bytes_read);
    print_string("\n");
    
    if (bytes_read < 0) { 
        exit_code = 5; 
        mini_snprintf(print_buf, sizeof(print_buf), 
                     "ERROR: Failed to read data (Syscall returned: %d)\n", 
                     (int)bytes_read); 
        print_string(print_buf); 
        goto cleanup; 
    }
    
    read_buf[bytes_read >= 0 ? bytes_read : 0] = '\0';
    print_string("Data read from file: \"");
    print_string(read_buf);
    print_string("\"\n");

    // 7. Verify read content matches written content
    if (bytes_read != (ssize_t)total_write_len) {
        mini_snprintf(print_buf, sizeof(print_buf), 
                     "ERROR: Read length (%d) does not match written length (%d).\n", 
                     (int)bytes_read, (int)total_write_len);
        print_string(print_buf); 
        exit_code = 6;
    } else {
        bool content_match = true;
        size_t read_len_check = local_strlen(read_buf); 
        size_t write_len_check = local_strlen(write_buf);
        
        if (read_len_check != write_len_check) { 
            content_match = false; 
        } else { 
            for(size_t i=0; i<total_write_len; ++i) { 
                if(read_buf[i] != write_buf[i]) { 
                    content_match = false; 
                    break; 
                } 
            } 
        }
        
        if (!content_match) { 
            print_string("ERROR: Read content does not match written content!\n"); 
            exit_code = 7; 
        } else { 
            print_string("Read content matches written content.\n"); 
        }
    }

cleanup:
    // 8. Cleanup
    print_string("--- Entering Cleanup ---\n");
    if (fd_write >= 0) { 
        mini_snprintf(print_buf, sizeof(print_buf), "Closing write fd (fd=%d) during cleanup.\n", fd_write); 
        print_string(print_buf); 
        volatile int cleanup_fd = fd_write;
        syscall(SYS_CLOSE, cleanup_fd, 0, 0); 
    }
    
    if (fd_read >= 0) { 
        mini_snprintf(print_buf, sizeof(print_buf), "Closing read fd (fd=%d) during cleanup.\n", fd_read); 
        print_string(print_buf); 
        volatile int cleanup_fd = fd_read;
        syscall(SYS_CLOSE, cleanup_fd, 0, 0); 
    }

    // 9. Exit
    if (exit_code == 0) { 
        print_string("--- User Program Exiting Successfully ---\n"); 
    } else { 
        mini_snprintf(print_buf, sizeof(print_buf), 
                     "--- User Program Exiting with Error Code: %d ---\n", 
                     exit_code); 
        print_string(print_buf); 
    }
    
    syscall(SYS_EXIT, exit_code, 0, 0);

    for(;;); // Should not be reached
    return exit_code;
}