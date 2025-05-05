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
 * - Provides mode argument for open() with O_CREAT.
 * - Uses valid stack buffers for read() and write() syscalls.
 * - Checks return values for all syscalls that can fail.
 * - Robust integer to string conversion for PID.
 * - Uses goto for cleaner error handling and resource cleanup.
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
typedef unsigned int        mode_t; // <<< ADDED: Type for file mode
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
// File Operation Flags & Constants (Must match kernel's definitions)
// ==========================================================================
#define O_RDONLY    0x0001
#define O_WRONLY    0x0002
#define O_RDWR      0x0003
#define O_ACCMODE   0x0003
#define O_CREAT     0x0100
#define O_TRUNC     0x0200
#define O_APPEND    0x0400

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

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
        : "=a" (ret)
        : "a" (num), "b" (arg1), "c" (arg2), "d" (arg3)
        : "memory", "cc"
    );
    return ret;
}

// ==========================================================================
// Helper Functions for User Space (Minimal Standard Library Simulation)
// ==========================================================================

size_t strlen(const char *s) {
    size_t i = 0;
    if (!s) return 0;
    while (s[i] != '\0') { i++; }
    return i;
}

void print_string(const char *s) {
    if (!s) return;
    syscall(SYS_PUTS, (int)s, 0, 0);
}

// Convert positive integer to string in buffer, returns pointer to start of string
char* utoa_simple(uint32_t un, char* buf, size_t buf_size) {
    if (buf_size < 2) return NULL; // Need space for at least one digit and null
    char* ptr = buf + buf_size - 1;
    *ptr = '\0';
    if (un == 0) {
        *--ptr = '0';
        return ptr;
    }
    while (un > 0 && ptr > buf) {
        *--ptr = '0' + (un % 10);
        un /= 10;
    }
    if (un > 0) { // Buffer too small
        return NULL;
    }
    return ptr;
}

void print_integer(int n) {
    char buf[12];
    char *s_ptr;
    bool is_negative = false;
    uint32_t un;

    if (n < 0) {
        is_negative = true;
        // Handle INT_MIN carefully
        if (n == INT32_MIN) {
            un = 2147483648U;
        } else {
            un = (uint32_t)(-n);
        }
    } else {
        un = (uint32_t)n;
    }

    s_ptr = utoa_simple(un, buf, sizeof(buf));

    if (s_ptr) { // Check if conversion was successful
        if (is_negative) {
            // Prepend '-' sign (make sure there's space)
            if (s_ptr > buf) {
                *--s_ptr = '-';
                 print_string(s_ptr);
            } else {
                // Buffer too small for negative sign + number (shouldn't happen with buf[12])
                print_string("-<ERR>");
            }
        } else {
            print_string(s_ptr);
        }
    } else {
        print_string("<ERR>"); // Indicate conversion error
    }
}

void exit_on_error(const char *msg, int syscall_ret, int exit_code) {
    print_string("ERROR: ");
    print_string(msg);
    print_string(" (Syscall returned: ");
    print_integer(syscall_ret);
    print_string(")\n");
    syscall(SYS_EXIT, exit_code, 0, 0);
    for(;;); // Should not return
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
    char write_buf[100]; // Buffer for constructing write data
    char read_buf[100];  // Buffer for reading data back

    print_string("--- User Program Started ---\n");

    // --- Get PID ---
    my_pid = syscall(SYS_GETPID, 0, 0, 0);
    if (my_pid < 0) {
        // Warning, but continue - use PID=0 in file write
        print_string("Warning: Failed to get PID (Error: ");
        print_integer(my_pid);
        print_string(")\n");
        my_pid = 0; // Use 0 as a placeholder PID
    } else {
        print_string("My PID is: ");
        print_integer(my_pid);
        print_string("\n");
    }

    // --- File I/O ---
    print_string("Attempting file I/O with '");
    print_string(filename);
    print_string("'...\n");

    // 1. Create/Truncate and Open for Writing
    print_string("Opening for writing (O_CREAT | O_WRONLY | O_TRUNC)...\n");
    // *** FIXED: Added DEFAULT_FILE_MODE (0666) for O_CREAT ***
    fd_write = syscall(SYS_OPEN, (int)filename, O_CREAT | O_WRONLY | O_TRUNC, DEFAULT_FILE_MODE);
    if (fd_write < 0) {
        exit_on_error("Failed to open file for writing", fd_write, 1);
    }
    print_string("File opened successfully for writing (fd=");
    print_integer(fd_write);
    print_string(").\n");

    // 2. Prepare Write Buffer
    // A simple approach using manual string concatenation into the buffer
    // (More robust would be a mini-sprintf if available)
    const char *base_msg = "Hello from user program! PID: ";
    size_t base_len = strlen(base_msg);
    if (base_len >= sizeof(write_buf)) { exit_on_error("Base write message too long", -1, 99); } // Sanity check
    for(size_t i=0; i<base_len; ++i) { write_buf[i] = base_msg[i]; } // Manual copy

    // Convert PID to string and append
    char pid_str_buf[12];
    char* pid_str = utoa_simple((uint32_t)my_pid, pid_str_buf, sizeof(pid_str_buf));
    if (!pid_str) { exit_on_error("Failed to convert PID to string", -1, 98); }
    size_t pid_len = strlen(pid_str);
    if (base_len + pid_len + 1 >= sizeof(write_buf)) { exit_on_error("Write buffer too small for PID", -1, 97); } // +1 for newline
    for(size_t i=0; i<pid_len; ++i) { write_buf[base_len + i] = pid_str[i]; } // Manual copy

    // Append newline and null terminator
    write_buf[base_len + pid_len] = '\n';
    write_buf[base_len + pid_len + 1] = '\0';
    size_t total_write_len = base_len + pid_len + 1;

    // 3. Write Data
    print_string("Writing data: \"");
    print_string(write_buf); // Print what we intend to write (might not include newline visually)
    print_string("\" (Length: ");
    print_integer((int)total_write_len);
    print_string(")\n");
    // *** FIXED: Pass write_buf address and correct length ***
    bytes_written = syscall(SYS_WRITE, fd_write, (int)write_buf, total_write_len);
    if (bytes_written < 0) {
        exit_on_error("Failed to write data", bytes_written, 2);
    }
    if ((size_t)bytes_written != total_write_len) {
        print_string("Warning: Partial write occurred? Wrote ");
        print_integer(bytes_written);
        print_string(" of ");
        print_integer(total_write_len);
        print_string(" bytes.\n");
        // Continue anyway for test
    } else {
         print_string("Data successfully written to file.\n");
    }

    // 4. Close File After Writing
    print_string("Closing write fd (fd="); print_integer(fd_write); print_string(")...\n");
    int close_res = syscall(SYS_CLOSE, fd_write, 0, 0);
    if (close_res < 0) {
        // Non-fatal warning, cleanup will try again if needed
        print_string("Warning: Failed to close write fd. Error: "); print_integer(close_res); print_string("\n");
    }
    fd_write = -1; // Mark as closed

    // 5. Re-open for Reading
    print_string("Re-opening file for reading (O_RDONLY)...\n");
    fd_read = syscall(SYS_OPEN, (int)filename, O_RDONLY, 0);
    if (fd_read < 0) {
        exit_on_error("Failed to open file for reading", fd_read, 4);
    }
    print_string("File opened successfully for reading (fd=");
    print_integer(fd_read);
    print_string(").\n");

    // 6. Read Data Back
    print_string("Reading data from file...\n");
    // *** FIXED: Use read_buf and pass its address ***
    for(size_t i=0; i<sizeof(read_buf); ++i) { read_buf[i] = 0; } // Clear buffer
    bytes_read = syscall(SYS_READ, fd_read, (int)read_buf, sizeof(read_buf) - 1);
    if (bytes_read < 0) {
        exit_on_error("Failed to read data", bytes_read, 5);
    }
    // Null terminate based on actual bytes read
    // (No need to check >= size, as we read size-1 max)
    read_buf[bytes_read] = '\0';

    print_string("Data read from file: \"");
    print_string(read_buf);
    print_string("\"\n");


cleanup:
    // 7. Cleanup: Ensure FDs are closed
    if (fd_write >= 0) {
        print_string("Closing write fd (fd="); print_integer(fd_write); print_string(") during cleanup.\n");
        syscall(SYS_CLOSE, fd_write, 0, 0);
    }
     if (fd_read >= 0) {
        print_string("Closing read fd (fd="); print_integer(fd_read); print_string(") during cleanup.\n");
        syscall(SYS_CLOSE, fd_read, 0, 0);
    }

    // 8. Exit
    print_string("--- User Program Exiting (Code: ");
    print_integer(exit_code);
    print_string(") ---\n");
    syscall(SYS_EXIT, exit_code, 0, 0);

    // Should not be reached
    return exit_code;
}