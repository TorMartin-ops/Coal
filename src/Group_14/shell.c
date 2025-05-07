// shell.c
// A very simple, self-contained shell for UiAOS

// --- BEGIN In-file Type Definitions and Constants ---
// From a conceptual stdint.h
typedef signed   char      int8_t;
typedef unsigned char      uint8_t;
typedef signed   short     int16_t;
typedef unsigned short     uint16_t;
typedef signed   int       int32_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t; // If needed, though not by current shell logic
typedef uint32_t           uintptr_t; // For i386 user space

#ifndef INT32_MIN
#define INT32_MIN (-2147483647 - 1)
#endif

// From a conceptual stddef.h
#ifndef _SIZE_T_DEFINED_SHELL_C // Unique guard for this file
#define _SIZE_T_DEFINED_SHELL_C
typedef uint32_t           size_t;
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

// Other common types for this shell
typedef int32_t            ssize_t;
typedef int32_t            pid_t;
// bool is not standard C99; often implemented as a #define or typedef.
// If your kernel's syscalls or logic expect a specific 'bool' type,
// use that. Otherwise, 'int' or 'char' can work. For simplicity:
typedef char bool; // Or use 'int'
#define true  1
#define false 0
// --- END In-file Type Definitions and Constants ---


// --- Syscall Numbers (MUST MATCH YOUR KERNEL'S syscall.h) ---
#define SYS_EXIT    1
#define SYS_READ    3
#define SYS_WRITE   4
// #define SYS_OPEN    5 // Not used by this simple shell directly
// #define SYS_CLOSE   6 // Not used by this simple shell directly
#define SYS_PUTS    7

#define STDIN_FILENO  0
#define STDOUT_FILENO 1

// --- Syscall Wrapper ---
static inline int32_t syscall(int32_t syscall_number,
                              int32_t arg1_val,
                              int32_t arg2_val,
                              int32_t arg3_val) {
    int32_t return_value;
    __asm__ volatile (
        "pushl %%ebx          \n\t"
        "pushl %%ecx          \n\t"
        "pushl %%edx          \n\t"
        "movl %1, %%eax       \n\t"
        "movl %2, %%ebx       \n\t"
        "movl %3, %%ecx       \n\t"
        "movl %4, %%edx       \n\t"
        "int $0x80            \n\t"
        "popl %%edx           \n\t"
        "popl %%ecx           \n\t"
        "popl %%ebx           \n\t"
        : "=a" (return_value)
        : "m" (syscall_number), "m" (arg1_val), "m" (arg2_val), "m" (arg3_val)
        : "cc", "memory"
    );
    return return_value;
}

// --- Syscall Helpers ---
#define sys_exit(code)      syscall(SYS_EXIT, (code), 0, 0)
#define sys_read(fd,buf,n)  syscall(SYS_READ, (fd), (int32_t)(uintptr_t)(buf), (n))
#define sys_write(fd,buf,n) syscall(SYS_WRITE, (fd), (int32_t)(uintptr_t)(buf), (n))
#define sys_puts(p)         syscall(SYS_PUTS, (int32_t)(uintptr_t)(p), 0, 0)

// --- String Utilities ---
static size_t my_strlen(const char *s) {
    size_t i = 0;
    if (!s) return 0;
    while (s[i]) i++;
    return i;
}

static int my_strcmp(const char *s1, const char *s2) {
    if (!s1 && !s2) {
        return 0;
    }
    if (!s1) {
        return -1;
    }
    if (!s2) {
        return 1;
    }
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

#define CMD_BUFFER_SIZE 256
char cmd_buffer[CMD_BUFFER_SIZE];

int main(void) {
    sys_puts("UiAOS Shell v0.1 (Self-Contained) Initialized.\n");

    while (1) {
        sys_puts("UiAOS> ");

        ssize_t bytes_read = sys_read(STDIN_FILENO, cmd_buffer, CMD_BUFFER_SIZE - 1);

        if (bytes_read > 0) {
            if (cmd_buffer[bytes_read - 1] == '\n') {
                cmd_buffer[bytes_read - 1] = '\0';
            } else {
                cmd_buffer[bytes_read] = '\0';
            }

            if (my_strlen(cmd_buffer) == 0) {
                continue;
            }

            if (my_strcmp(cmd_buffer, "exit") == 0) {
                sys_puts("Exiting shell.\n");
                sys_exit(0);
            } else if (my_strcmp(cmd_buffer, "help") == 0) {
                sys_puts("Available commands:\n");
                sys_puts("  exit  - Exit the shell.\n");
                sys_puts("  help  - Display this help message.\n");
                sys_puts("  hello - (Conceptual) Run hello program.\n");
            } else if (my_strcmp(cmd_buffer, "hello") == 0) {
                sys_puts("Conceptual: Would try to run /hello.elf\n");
            } else {
                sys_puts("Unknown command: ");
                sys_puts(cmd_buffer);
                sys_puts("\n");
            }
        } else if (bytes_read == 0) { // EOF
            sys_puts("\nEOF on input. Exiting shell.\n");
            sys_exit(0);
        } else { // Error from sys_read
            sys_puts("Error reading input from terminal.\n");
        }
    }
    return 0;
}