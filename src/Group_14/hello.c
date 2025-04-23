// hello.c - Improved exit test

// Include necessary definitions (assuming a minimal userspace header exists or is added)
// If not, defining SYS_EXIT directly is fine.
// #include "usersyscalls.h" // Example if you create this header
#define SYS_EXIT  1

// Syscall wrapper (remains the same, as it's essential for this environment)
static inline int syscall(int num, int arg1, int arg2, int arg3) {
    int ret;
    // The "=a"(ret) constraint tells GCC that EAX will contain the return value.
    // The "a"(num), "b"(arg1), "c"(arg2), "d"(arg3) constraints map the C variables
    // to the correct registers for the syscall convention.
    asm volatile (
        "int $0x80"
        : "=a" (ret)
        : "a" (num), "b" (arg1), "c" (arg2), "d" (arg3)
        // Clobbers tell GCC which registers might be changed by the inline assembly,
        // beyond the output operand. "memory" indicates memory might be changed,
        // and "cc" indicates the condition codes (flags register) might change.
        : "memory", "cc"
    );
    // Note: This wrapper assumes the kernel doesn't preserve EBX, ECX, EDX across
    // the syscall. If it did, they wouldn't need to be listed as clobbers
    // unless explicitly modified by the asm block itself.
    return ret;
}

// Standard C entry point
int main() {
    // Option 1: Exit immediately from main (simple, but less standard C)
    // syscall(SYS_EXIT, 55, 0, 0);
    // // If syscall returns unexpectedly (it shouldn't for exit),
    // // fall through to return a different code.
    // return 1; // Indicate potential failure if syscall returned

    // Option 2: Behave like a standard C main and return the exit code
    // This lets _start in entry.asm handle the actual exit syscall.
    int exit_code = 55;
    // Add other test code here if needed...
    // E.g., printf("Hello from user space!\n"); (if you implement printf syscall)

    return exit_code; // Return the desired exit code
}