// hello.c - Extremely simple user-space program for UiAOS

// Syscall numbers (must match your kernel's syscall.h or definitions)
#define SYS_EXIT      1   // The syscall number for exiting the process

/**
 * main - Entry point for the user program.
 *
 * This simplified version does absolutely nothing except return 0.
 * The C runtime startup code (in entry.asm) will take the return value (0)
 * and use it as the argument for the SYS_EXIT system call.
 */
int main() {
    // No operations needed here.
    return 0; // Exit code 0
}

// Removed my_strlen, syscall_write, and STDOUT_FILENO as they are not used.