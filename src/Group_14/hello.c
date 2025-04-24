// File: hello.c - Enhanced User Space Test Program

// ==========================================================================
// Includes (Simulated - Add actual libc headers if available/desired)
// ==========================================================================
// Basic types often come from a minimal stdint.h or types.h
typedef unsigned int   uint32_t;
typedef          int   int32_t;
typedef unsigned int   size_t;
typedef          int   ssize_t;
typedef          int   pid_t;
typedef long           off_t; // Assuming off_t is long for lseek

// Function for basic string length (implement or link if available)
size_t strlen(const char *s) {
    size_t i = 0;
    while (s[i]) i++;
    return i;
}

// ==========================================================================
// System Call Definitions (Match your kernel's syscall.h)
// ==========================================================================
#define SYS_EXIT     1
#define SYS_READ     3
#define SYS_WRITE    4
#define SYS_OPEN     5
#define SYS_CLOSE    6
#define SYS_PUTS     7
#define SYS_LSEEK   19 // Add if you plan to use it
#define SYS_GETPID  20

// ==========================================================================
// File Flags / Constants (Simulated - Match your fs_limits.h/sys_file.h)
// ==========================================================================
// Open flags (combine with |)
#define O_RDONLY    0x0001  // Read only
#define O_WRONLY    0x0002  // Write only
#define O_RDWR      0x0003  // Read/write
#define O_CREAT     0x0100  // Create if doesn't exist
#define O_TRUNC     0x0200  // Truncate size to 0
#define O_APPEND    0x0400  // Append mode

// Standard file descriptors
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// Seek whence values (for lseek)
#define SEEK_SET    0 /* Seek from beginning of file. */
#define SEEK_CUR    1 /* Seek from current position. */
#define SEEK_END    2 /* Seek from end of file. */

// ==========================================================================
// Syscall Wrapper (Consider placing in a separate userspace header)
// ==========================================================================
static inline int syscall(int num, int arg1, int arg2, int arg3) {
    int ret;
    // Use volatile to prevent compiler optimizations removing the asm
    asm volatile (
        "int $0x80"         // Trigger syscall interrupt
        : "=a" (ret)        // Output: return value in EAX ('ret')
        : "a" (num),        // Input: syscall number in EAX ('num')
          "b" (arg1),       // Input: first argument in EBX ('arg1')
          "c" (arg2),       // Input: second argument in ECX ('arg2')
          "d" (arg3)        // Input: third argument in EDX ('arg3')
        : "memory", "cc"    // Clobbers: informs compiler memory and flags might change
    );
    // Negative return values typically indicate errors
    return ret;
}

// ==========================================================================
// Helper Functions for User Space
// ==========================================================================

// Simple string printing using SYS_PUTS
void print_string(const char *s) {
    syscall(SYS_PUTS, (int)s, 0, 0);
}

// Print an integer (requires manual conversion or a printf implementation)
// Basic implementation (only handles non-negative for simplicity here)
void print_integer(int n) {
    char buf[12]; // Enough for 32-bit int + null
    char *ptr = buf + sizeof(buf) - 1;
    *ptr = '\0';

    if (n == 0) {
        *--ptr = '0';
    } else {
        // Handle potential negative numbers if needed
        // uint32_t un = (n < 0) ? -n : n; // Need signed handling
        uint32_t un = n; // Assume non-negative for this simple example
        while (un > 0) {
            *--ptr = '0' + (un % 10);
            un /= 10;
        }
        // if (n < 0) *--ptr = '-'; // Add sign if handling negatives
    }
    print_string(ptr);
}

// Helper to print messages and exit on failure
void exit_on_error(const char *msg, int exit_code) {
    print_string("ERROR: ");
    print_string(msg);
    print_string("\n");
    syscall(SYS_EXIT, exit_code, 0, 0);
}


// ==========================================================================
// Main Application Logic
// ==========================================================================
int main() {
    int exit_code = 0; // Default to success
    pid_t my_pid;
    int fd = -1; // File descriptor
    ssize_t bytes_written, bytes_read;
    char read_buffer[100];
    const char *filename = "/testfile.txt"; // File to create/use
    const char *write_data = "Hello from PID ";

    // 1. Print initial message using SYS_PUTS
    print_string("--- User Program Started ---\n");

    // 2. Get and print Process ID using SYS_GETPID
    my_pid = syscall(SYS_GETPID, 0, 0, 0);
    if (my_pid < 0) {
        print_string("Failed to get PID.\n");
        // Continue anyway for demonstration
    } else {
        print_string("My PID is: ");
        print_integer(my_pid);
        print_string("\n");
    }

    // 3. File I/O Demonstration
    print_string("Attempting file I/O with '");
    print_string(filename);
    print_string("'...\n");

    // 3a. Create and open the file for writing
    // Flags: O_CREAT (create if not exists) | O_WRONLY (write-only) | O_TRUNC (clear if exists)
    // Mode argument (like 0644) is often ignored if not fully implemented, pass 0.
    fd = syscall(SYS_OPEN, (int)filename, O_CREAT | O_WRONLY | O_TRUNC, 0);
    if (fd < 0) {
        print_string("Failed to open/create file for writing. Error code: ");
        print_integer(fd); // Print negative error code
        print_string("\n");
        exit_code = 1;
        goto cleanup; // Skip rest of file I/O
    }
    print_string("File opened for writing (fd=");
    print_integer(fd);
    print_string(").\n");

    // 3b. Write some data using SYS_WRITE
    bytes_written = syscall(SYS_WRITE, fd, (int)write_data, strlen(write_data));
    if (bytes_written < 0) {
        print_string("Failed to write initial data. Error code: ");
        print_integer(bytes_written);
        print_string("\n");
        exit_code = 2;
        goto cleanup;
    }
    // Try writing the PID as well (convert PID to string first - complex without sprintf)
    // For simplicity, we'll just write a fixed string now. Add PID writing later if needed.
    const char *pid_msg = "(PID writing not implemented)\n";
    bytes_written = syscall(SYS_WRITE, fd, (int)pid_msg, strlen(pid_msg));
     if (bytes_written < 0) {
        print_string("Failed to write PID message. Error code: ");
        print_integer(bytes_written);
        print_string("\n");
        exit_code = 3;
        goto cleanup;
    }

    print_string("Data written to file.\n");

    // 3c. Close the file
    syscall(SYS_CLOSE, fd, 0, 0); // Ignore close errors for simplicity here
    fd = -1; // Mark fd as closed
    print_string("File closed.\n");

    // 3d. Re-open the file for reading
    fd = syscall(SYS_OPEN, (int)filename, O_RDONLY, 0);
     if (fd < 0) {
        print_string("Failed to open file for reading. Error code: ");
        print_integer(fd);
        print_string("\n");
        exit_code = 4;
        goto cleanup; // Skip reading
    }
     print_string("File opened for reading (fd=");
     print_integer(fd);
     print_string(").\n");

    // 3e. Read the data back using SYS_READ
    // Zero the buffer first
    for(size_t i=0; i<sizeof(read_buffer); ++i) read_buffer[i]=0;

    bytes_read = syscall(SYS_READ, fd, (int)read_buffer, sizeof(read_buffer) - 1); // Leave space for null
    if (bytes_read < 0) {
         print_string("Failed to read data. Error code: ");
         print_integer(bytes_read);
         print_string("\n");
         exit_code = 5;
         goto cleanup;
    }
    // Null-terminate the buffer based on bytes read
    read_buffer[bytes_read] = '\0';

    print_string("Read from file: \"");
    print_string(read_buffer); // Print the content using SYS_PUTS
    print_string("\"\n");


cleanup:
    // 4. Clean up - Close file if still open
    if (fd >= 0) {
        print_string("Closing file (fd=");
        print_integer(fd);
        print_string(") before exit.\n");
        syscall(SYS_CLOSE, fd, 0, 0);
    }

    // 5. Exit using the standard return mechanism
    print_string("--- User Program Exiting (Code: ");
    print_integer(exit_code);
    print_string(") ---\n");
    return exit_code;
}