// hello.c - Simple user-space program for UiAOS

// Syscall numbers (must match your kernel's syscall.h)
#define SYS_WRITE 1
#define SYS_EXIT  2

// Crude strlen implementation (no libc)
int my_strlen(const char* s) {
    int len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

// Crude syscall wrappers using inline assembly
int syscall_write(const char *message) {
    int len = my_strlen(message);
    int result;
    // EAX = syscall number (SYS_WRITE = 1)
    // EBX = buffer pointer
    // ECX = buffer length
    asm volatile (
        "int $0x80"         // Invoke syscall interrupt
        : "=a" (result)     // Output: result in EAX
        : "a" (SYS_WRITE), "b" (message), "c" (len) // Inputs
        : "memory", "cc"    // Clobbers: memory, condition codes
    );
    return result;
}

void syscall_exit(int code) {
    // EAX = syscall number (SYS_EXIT = 2)
    // EBX = exit code
    asm volatile (
        "int $0x80"
        : // No output
        : "a" (SYS_EXIT), "b" (code)
        : "memory"
    );
    // This syscall should not return
    while(1); // Loop forever if it somehow returns
}


// Main function for the user program
int main() {
    syscall_write("Hello from User Space!\n");
    return 0; // Exit code 0
}