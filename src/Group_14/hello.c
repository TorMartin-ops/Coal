// hello.c - Simplest exit test

#define SYS_EXIT  1

// Simple inline assembly wrapper for syscalls
static inline int syscall(int num, int arg1, int arg2, int arg3) {
    int ret;
    asm volatile (
        "int $0x80"
        : "=a" (ret)        // Output: return value in EAX -> ret
        // Ensure EAX is explicitly listed as input operand for syscall number
        : "a" (num), "b" (arg1), "c" (arg2), "d" (arg3)
        // IMPORTANT: Add input registers to clobber list IF they might be modified
        // by the syscall *and* the compiler needs to know. Usually "memory" is enough.
        : "memory", "cc" // "eax", "ebx", "ecx", "edx" might be needed if ABI is complex
    );
    return ret;
}

int main() {
    // First and ONLY action: Exit immediately.
    syscall(SYS_EXIT, 55, 0, 0); // Use a distinct exit code like 55

    // This should never be reached if exit works
    while(1) {} // Loop forever if exit fails

    return 99;
}