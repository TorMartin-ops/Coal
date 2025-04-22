// hello.c - Simple user-space program for UiAOS - Prints Hello World

// Define syscall numbers directly if no user-space header exists
#define SYS_EXIT  1
#define SYS_WRITE 4

// Define standard file descriptors
#define STDOUT_FILENO 1

// Simple inline assembly wrapper for syscalls
// Returns the value from EAX (syscall return value)
static inline int syscall(int num, int arg1, int arg2, int arg3) {
    int ret;
    asm volatile (
        "int $0x80"         // Invoke syscall interrupt
        : "=a" (ret)        // Output: return value in EAX -> ret
        : "a" (num),        // Input: syscall number in EAX
          "b" (arg1),       // Input: argument 1 in EBX
          "c" (arg2),       // Input: argument 2 in ECX
          "d" (arg3)        // Input: argument 3 in EDX
        : "memory", "cc"    // Clobbers memory and condition codes
    );
    return ret;
}

// Simple strlen implementation (no standard library)
static unsigned int my_strlen(const char *str) {
    unsigned int len = 0;
    while (str[len]) {
        len++;
    }
    return len;
}

// Main entry point
int main() {
    const char *message = "Hello, World from User Space!\n";
    unsigned int len = my_strlen(message);

    // Call SYS_WRITE(STDOUT_FILENO, message, len)
    syscall(SYS_WRITE, STDOUT_FILENO, (int)message, (int)len);

    // Returning 0 implicitly calls SYS_EXIT(0) via entry.asm
    return 0;
}