// hello.c - Simple user-space program for UiAOS

// Syscall numbers (must match your kernel's syscall.h)
#define SYS_WRITE     4  // Updated to match syscall.h
#define SYS_EXIT      1  // Updated to match syscall.h
#define STDOUT_FILENO 1  // Standard file descriptor for stdout

// Crude strlen implementation (no libc)
int my_strlen(const char* s) {
    int len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

// Crude syscall wrapper for write using inline assembly
int syscall_write(int fd, const char *message, int len) {
    int result;
    // EAX = syscall number (SYS_WRITE = 4)
    // EBX = file descriptor
    // ECX = buffer pointer
    // EDX = buffer length
    asm volatile (
        "int $0x80"          // Invoke syscall interrupt
        : "=a" (result)      // Output: result in EAX
        : "a" (SYS_WRITE), "b" (fd), "c" (message), "d" (len) // Inputs
        : "memory", "cc"     // Clobbers: memory, condition codes
    );
    return result;
}

// NOTE: syscall_exit function removed as it's handled by entry.asm

// Main function for the user program
int main() {
    const char *msg = "Hello from User Space!\n";
    syscall_write(STDOUT_FILENO, msg, my_strlen(msg));
    return 0; // Exit code 0 (will be passed to SYS_EXIT by _start)
}