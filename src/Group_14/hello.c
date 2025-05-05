/**
 * @file hello.c
 * @brief Enhanced User Space Test Program for UiAOS
 *
 * Demonstrates basic syscall usage including:
 * - SYS_PUTS for printing strings
 * - SYS_GETPID for getting process ID
 * - SYS_OPEN, SYS_WRITE, SYS_READ, SYS_CLOSE for file I/O
 * - SYS_EXIT for terminating the process
 */

// ==========================================================================
// Includes & Type Definitions (Minimal Userspace Simulation)
// ==========================================================================
// These types should ideally come from a userspace libc header provided
// by the OS build process, matching kernel definitions.

// Standard Integer Types
typedef signed char         int8_t;
typedef unsigned char       uint8_t;
typedef signed short        int16_t;
typedef unsigned short      uint16_t;
typedef signed int          int32_t;
typedef unsigned int        uint32_t;
typedef signed long long    int64_t;
typedef unsigned long long  uint64_t;

// Size and Pointer Types
typedef uint32_t            size_t;     // Common for 32-bit systems
typedef int32_t             ssize_t;    // Signed size type
typedef uint32_t            uintptr_t;  // Unsigned integer type capable of holding a pointer

// Other Common Types
typedef int32_t             pid_t;      // Process ID type
typedef int64_t             off_t;      // File offset type (use 64-bit for large files)
// Note: If your kernel uses 32-bit off_t, change this definition accordingly.

// Boolean Type
typedef _Bool               bool;       // Requires C99 or later, or stdbool.h
#define true 1
#define false 0

// NULL Definition
#define NULL ((void*)0)

// ==========================================================================
// System Call Definitions (Must match kernel's syscall.h exactly)
// ==========================================================================
#define SYS_EXIT     1
#define SYS_READ     3
#define SYS_WRITE    4
#define SYS_OPEN     5
#define SYS_CLOSE    6
#define SYS_PUTS     7  // Non-standard, kernel-specific syscall for string output
#define SYS_LSEEK   19 // Standard POSIX syscall number
#define SYS_GETPID  20 // Standard POSIX syscall number

// ==========================================================================
// File Operation Flags & Constants (Must match kernel's definitions)
// ==========================================================================
// Open flags (combine with bitwise OR |)
#define O_RDONLY    0x0001  // Open for reading only
#define O_WRONLY    0x0002  // Open for writing only
#define O_RDWR      0x0003  // Open for reading and writing (usually O_RDONLY|O_WRONLY)
#define O_ACCMODE   0x0003  // Mask for access modes (O_RDONLY, O_WRONLY, O_RDWR)

#define O_CREAT     0x0100  // Create file if it does not exist
#define O_TRUNC     0x0200  // Truncate file to zero length if it exists
#define O_APPEND    0x0400  // Append data to the end of the file

// Standard file descriptors (POSIX standard)
#define STDIN_FILENO  0     // Standard input
#define STDOUT_FILENO 1     // Standard output
#define STDERR_FILENO 2     // Standard error

// Seek whence values (for lseek, POSIX standard)
#define SEEK_SET    0       // Seek from beginning of file.
#define SEEK_CUR    1       // Seek from current position.
#define SEEK_END    2       // Seek from end of file.

// ==========================================================================
// Syscall Wrapper Function
// ==========================================================================
/**
 * @brief Performs a system call using the int 0x80 interrupt.
 * @param num The system call number (placed in EAX).
 * @param arg1 The first argument (placed in EBX).
 * @param arg2 The second argument (placed in ECX).
 * @param arg3 The third argument (placed in EDX).
 * @return The value returned by the kernel in EAX (often status or result).
 * Negative values typically indicate errors in this kernel.
 */
static inline int syscall(int num, int arg1, int arg2, int arg3) {
    int ret;
    // Use volatile to prevent compiler optimizations removing the asm block.
    // Input constraints specify registers for syscall number and arguments.
    // Output constraint specifies EAX for the return value.
    // Clobbers list informs the compiler that memory and condition codes might change.
    asm volatile (
        "int $0x80"         // Trigger the system call interrupt vector.
        : "=a" (ret)        // Output: return value from kernel is in EAX ('ret').
        : "a" (num),        // Input: syscall number 'num' goes into EAX.
          "b" (arg1),       // Input: first argument 'arg1' goes into EBX.
          "c" (arg2),       // Input: second argument 'arg2' goes into ECX.
          "d" (arg3)        // Input: third argument 'arg3' goes into EDX.
        : "memory", "cc"    // Clobbers: memory and condition code flags might be modified by the kernel.
    );
    return ret; // Return the value placed in EAX by the kernel.
}

// ==========================================================================
// Helper Functions for User Space (Minimal Standard Library Simulation)
// ==========================================================================

/**
 * @brief Calculates the length of a null-terminated string.
 * @param s Pointer to the string.
 * @return The number of characters before the null terminator.
 */
size_t strlen(const char *s) {
    size_t i = 0;
    if (!s) return 0; // Handle NULL pointer gracefully.
    while (s[i] != '\0') { // Iterate until the null terminator is found.
        i++;
    }
    return i; // Return the count.
}

/**
 * @brief Prints a null-terminated string to the kernel console using SYS_PUTS.
 * @param s The string to print.
 */
void print_string(const char *s) {
    if (!s) return; // Safety check for NULL pointer.
    // Call the kernel's SYS_PUTS syscall. The kernel handles printing.
    // Cast the pointer to int as expected by the syscall wrapper.
    syscall(SYS_PUTS, (int)s, 0, 0);
}

/**
 * @brief Prints an integer (positive or negative) to the kernel console.
 * Converts the integer to a string representation before printing.
 * @param n The integer to print.
 */
void print_integer(int n) {
    char buf[12]; // Buffer: 10 digits for 32-bit int + sign + null terminator.
    char *ptr = buf + sizeof(buf) - 1; // Start from the end of the buffer.
    *ptr = '\0'; // Null-terminate the buffer.
    bool is_negative = false;
    uint32_t un; // Use unsigned integer for calculations to handle INT_MIN correctly.

    if (n == 0) {
        *--ptr = '0'; // Handle the special case of zero.
    } else {
        // Check if the number is negative.
        if (n < 0) {
            is_negative = true;
            // Handle potential INT_MIN overflow when negating.
            // Cast to a wider signed type (int64_t) before negation, then to uint32_t.
            // This ensures -(-2147483648) becomes +2147483648 correctly.
            // If int64_t is unavailable, a specific check for INT_MIN is needed.
            #ifdef __GNUC__ // Check if using GCC or Clang which support 64-bit integers
                un = (uint32_t)(-(int64_t)n);
            #else
                // Fallback if 64-bit integers are not easily available
                if (n == -2147483648) { // Specific check for INT_MIN
                     un = 2147483648U; // The positive equivalent as an unsigned int
                } else {
                     un = (uint32_t)(-n); // Safe for other negative numbers
                }
            #endif
        } else {
            un = (uint32_t)n; // Number is positive, cast directly.
        }

        // Convert digits to characters from right to left.
        while (un > 0) {
            *--ptr = '0' + (un % 10); // Get the last digit and convert to char.
            un /= 10; // Remove the last digit.
        }
        // Add the negative sign if necessary.
        if (is_negative) {
            *--ptr = '-';
        }
    }
    // Print the resulting string using the print_string helper.
    print_string(ptr);
}

/**
 * @brief Prints an error message and terminates the program using SYS_EXIT.
 * @param msg The error message string.
 * @param exit_code The exit code to pass to the kernel.
 */
void exit_on_error(const char *msg, int exit_code) {
    print_string("ERROR: ");
    print_string(msg);
    print_string("\n");
    syscall(SYS_EXIT, exit_code, 0, 0); // Call the kernel exit syscall.
    // This function should not return. Add infinite loop as fallback.
    for(;;);
}


// ==========================================================================
// Main Application Logic
// ==========================================================================
int main() {
    int exit_code = 0; // Default exit code to success (0).
    pid_t my_pid = -1; // Initialize PID to an invalid value.
    int fd_write = -1; // File descriptor for writing, initialize to invalid (-1).
    int fd_read = -1;  // File descriptor for reading, initialize to invalid (-1).
    ssize_t bytes_written, bytes_read; // Variables for tracking I/O results.
    char read_buffer[100]; // Buffer for reading file content.
    const char *filename = "/testfile.txt"; // Define the filename to use.
    const char *write_data = "Hello from user program! PID: "; // Data to write.

    // 1. Print initial startup message.
    print_string("--- User Program Started ---\n");

    // 2. Get and print the Process ID.
    my_pid = syscall(SYS_GETPID, 0, 0, 0); // Call SYS_GETPID syscall.
    if (my_pid < 0) { // Check if the syscall returned an error (negative value).
        print_string("Failed to get PID. Error code: ");
        print_integer(my_pid); // Print the negative error code correctly.
        print_string("\n");
        // Decide whether to exit or continue. Continuing for demonstration.
        // exit_on_error("Could not get PID", 1);
    } else {
        print_string("My PID is: ");
        print_integer(my_pid); // Print the obtained PID.
        print_string("\n");
    }

    // 3. File I/O Demonstration.
    print_string("Attempting file I/O with '");
    print_string(filename);
    print_string("'...\n");

    // --- 3a. Create and open the file for writing ---
    print_string("Opening for writing (O_CREAT | O_WRONLY | O_TRUNC)...\n");
    // Attempt to open the file. Create if it doesn't exist (O_CREAT),
    // open for writing only (O_WRONLY), and truncate to zero length if it exists (O_TRUNC).
    fd_write = syscall(SYS_OPEN, (int)filename, O_CREAT | O_WRONLY | O_TRUNC, 0);

    // --- Check the result of sys_open ---
    if (fd_write < 0) { // If fd is negative, an error occurred.
        print_string("Failed to open/create file for writing. Error code: ");
        print_integer(fd_write); // Print the negative error code.
        print_string("\n");
        exit_code = 1; // Set an error exit code.
        goto cleanup; // Jump to the cleanup section.
    } else {
        // Only print the success message if fd is non-negative.
         print_string("File opened successfully for writing (fd=");
         print_integer(fd_write);
         print_string(").\n");
    }

    // --- 3b. Write data to the opened file ---
    print_string("Writing data...\n");
    // Write the initial string data.
    bytes_written = syscall(SYS_WRITE, fd_write, (int)write_data, strlen(write_data));
    if (bytes_written < 0) { // Check for write errors.
        print_string("Failed to write initial data. Error code: ");
        print_integer(bytes_written);
        print_string("\n");
        exit_code = 2;
        goto cleanup;
    }

    // Convert the PID to a string to write it to the file.
    char pid_str_buf[12]; // Buffer for PID string conversion.
    char *pid_ptr = pid_str_buf + sizeof(pid_str_buf) - 1; // Start from end.
    *pid_ptr = '\0'; // Null-terminate.
    // Use the PID obtained earlier, or 0 if it failed.
    uint32_t upid = (my_pid >= 0) ? (uint32_t)my_pid : 0;
    if (upid == 0) {
        *--pid_ptr = '0';
    } else {
        while (upid > 0) { *--pid_ptr = '0' + (upid % 10); upid /= 10; }
    }

    // Write the PID string to the file.
    bytes_written = syscall(SYS_WRITE, fd_write, (int)pid_ptr, strlen(pid_ptr));
    if (bytes_written < 0) { // Check for errors.
       print_string("Failed to write PID to file. Error code: ");
       print_integer(bytes_written);
       print_string("\n");
       exit_code = 3;
       goto cleanup;
   }
   // Write a newline character for formatting in the file.
   bytes_written = syscall(SYS_WRITE, fd_write, (int)"\n", 1);
   if (bytes_written < 0) {
       print_string("Failed to write newline to file. Error code: ");
       print_integer(bytes_written);
       print_string("\n");
       // Optional: set exit_code and goto cleanup
   }

    print_string("Data successfully written to file.\n");

    // --- 3c. Close the file after writing ---
    print_string("Closing file after writing...\n");
    syscall(SYS_CLOSE, fd_write, 0, 0); // Close the file descriptor used for writing.
    fd_write = -1; // Mark the write file descriptor as invalid/closed.


    // --- 3d. Re-open the same file for reading ---
    print_string("Re-opening file for reading (O_RDONLY)...\n");
    fd_read = syscall(SYS_OPEN, (int)filename, O_RDONLY, 0); // Open for reading only.

    // --- Check the result of sys_open ---
     if (fd_read < 0) { // If fd is negative, an error occurred.
        print_string("Failed to open file for reading. Error code: ");
        print_integer(fd_read); // Print the negative error code.
        print_string("\n");
        exit_code = 4; // Set an error exit code.
        goto cleanup; // Jump to the cleanup section.
    } else {
         // Only print the success message if fd is non-negative.
         print_string("File opened successfully for reading (fd=");
         print_integer(fd_read);
         print_string(").\n");
    }

    // --- 3e. Read data back from the file ---
    print_string("Reading data from file...\n");
    // Clear the read buffer before reading into it.
    for(size_t i=0; i<sizeof(read_buffer); ++i) { read_buffer[i] = 0; }

    // Attempt to read data into the buffer. Read up to one less than buffer size
    // to ensure space for a null terminator.
    bytes_read = syscall(SYS_READ, fd_read, (int)read_buffer, sizeof(read_buffer) - 1);
    if (bytes_read < 0) { // Check for read errors.
         print_string("Failed to read data from file. Error code: ");
         print_integer(bytes_read);
         print_string("\n");
         exit_code = 5;
         goto cleanup;
    }

    // Null-terminate the string read from the file based on the number of bytes read.
    // Ensure bytes_read is not negative before using as index.
    if (bytes_read >= 0 && (size_t)bytes_read < sizeof(read_buffer)) {
        read_buffer[bytes_read] = '\0';
    } else if ((size_t)bytes_read >= sizeof(read_buffer)) {
        // If the buffer was filled completely, ensure the last character is null.
        read_buffer[sizeof(read_buffer) - 1] = '\0';
    }

    // Print the content read from the file.
    print_string("Data read from file: \"");
    print_string(read_buffer);
    print_string("\"\n");


cleanup:
    // 4. Cleanup Section: Ensure all potentially open file descriptors are closed.
    if (fd_write >= 0) { // Check if write fd is valid before closing
        print_string("Closing write fd (fd=");
        print_integer(fd_write);
        print_string(") during cleanup.\n");
        syscall(SYS_CLOSE, fd_write, 0, 0); // Close the file descriptor.
        fd_write = -1; // Mark as closed.
    }
     if (fd_read >= 0) { // Check if read fd is valid before closing
        print_string("Closing read fd (fd=");
        print_integer(fd_read);
        print_string(") during cleanup.\n");
        syscall(SYS_CLOSE, fd_read, 0, 0); // Close the file descriptor.
        fd_read = -1; // Mark as closed.
    }

    // 5. Exit the program using the SYS_EXIT syscall.
    print_string("--- User Program Exiting (Code: ");
    print_integer(exit_code); // Print the final exit code.
    print_string(") ---\n");
    syscall(SYS_EXIT, exit_code, 0, 0); // Terminate the process.

    // The program should not reach here because SYS_EXIT terminates it.
    // Return statement is for C standard compliance.
    return exit_code;
}

