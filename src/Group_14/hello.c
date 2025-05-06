/*
 * hello.c – UiAOS User-Space Kernel Test Suite (v3.1 - Focused)
 *
 * Purpose: Rigorously test kernel syscalls with clear, concise pass/fail.
 * Focuses on PID and core file I/O operations.
 *
 * Build: i686-elf-gcc -m32 -Wall -Wextra -nostdlib -fno-builtin \
 * -fno-stack-protector -std=gnu99 -o hello.elf hello.c
 */

/* ==== Typedefs and Basic Definitions ===================================== */
typedef signed   char      int8_t;
typedef unsigned char      uint8_t;
typedef signed   short     int16_t;
typedef unsigned short     uint16_t;
typedef signed   int       int32_t;
typedef unsigned int       uint32_t;
//typedef signed   long long int64_t; // Not used in this version
//typedef unsigned long long uint64_t; // Not used in this version

typedef uint32_t           size_t;
typedef int32_t            ssize_t;
typedef int32_t            pid_t;
typedef int32_t            off_t;

#ifndef _UINTPTR_T_DEFINED_HELLO_C
#define _UINTPTR_T_DEFINED_HELLO_C
typedef uint32_t uintptr_t;
#endif

typedef int32_t  bool;
#define true  1
#define false 0
#define NULL  ((void*)0)

#ifndef INT32_MIN
#define INT32_MIN (-2147483647 - 1)
#endif

/* ==== Kernel ABI Constants (MUST MATCH YOUR KERNEL) ===================== */
#define SYS_EXIT    1
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_PUTS    7
#define SYS_LSEEK   19 // Update if different or comment out lseek tests
#define SYS_GETPID  20

#define O_RDONLY     0x0000
#define O_WRONLY     0x0001
#define O_RDWR       0x0002
#define O_CREAT      0x0040
#define O_TRUNC      0x0200
#define DEFAULT_MODE 0666u

#ifndef SEEK_SET
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2
#endif

// Expected error codes (negative values)
#define EBADF        9
#define ENOENT       2

/* ==== Syscall Wrapper (Robust EBX preservation) ========================== */
static inline int32_t syscall3(int32_t syscall_number,
                               int32_t arg1, int32_t arg2, int32_t arg3) {
    int32_t return_value;
    __asm__ volatile (
        "pushl %%ebx      \n\t"
        "movl %2, %%ebx   \n\t" // %2 is arg1 due to "m"(arg1)
        "int $0x80        \n\t"
        "popl %%ebx       \n\t"
        : "=a" (return_value)
        : "a" (syscall_number), "m" (arg1), "c" (arg2), "d" (arg3)
        : "cc", "memory"
    );
    return return_value;
}

/* ==== Syscall Helper Macros ============================================== */
#define sys_exit(code)      syscall3(SYS_EXIT, (code), 0, 0)
#define sys_read(fd,buf,n)  syscall3(SYS_READ, (fd), (int32_t)(uintptr_t)(buf), (n))
#define sys_write(fd,buf,n) syscall3(SYS_WRITE, (fd), (int32_t)(uintptr_t)(buf), (n))
#define sys_open(p,f,m)     syscall3(SYS_OPEN, (int32_t)(uintptr_t)(p), (f), (m))
#define sys_close(fd)       syscall3(SYS_CLOSE, (fd), 0, 0)
#define sys_puts(p)         syscall3(SYS_PUTS, (int32_t)(uintptr_t)(p), 0, 0)
#define sys_getpid()        syscall3(SYS_GETPID, 0, 0, 0)
#define sys_lseek(fd,off,wh) syscall3(SYS_LSEEK, (fd), (off), (wh))

/* ==== Minimal Libc-like Utilities ======================================== */
static size_t my_strlen(const char *s) {
    size_t i = 0; if (!s) return 0; while (s[i]) i++; return i;
}
static int my_strcmp(const char *s1, const char *s2) {
    if (!s1 && !s2) return 0; if (!s1) return -1; if (!s2) return 1;
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}
static void my_memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s; while (n-- > 0) *p++ = (unsigned char)c;
}

// --- Printing Utilities (Corrected) ---
static void print_char(char c) { char b[2]={c,0}; sys_puts(b); }
static void print_str(const char *s) { if (s) sys_puts(s); }
static void print_nl() { print_char('\n'); }

static void print_sdec(int32_t v) {
    char buf[12]; char *p = buf + 11; *p = '\0';
    if (v == 0) { *--p = '0'; }
    else {
        bool neg = v < 0;
        uint32_t n = neg ? ((v == INT32_MIN) ? 2147483648U : (uint32_t)-v) : (uint32_t)v;
        while (n > 0) { *--p = (n % 10) + '0'; n /= 10; }
        if (neg) *--p = '-';
    }
    print_str(p);
}
static void print_hex32(uint32_t v) {
    print_str("0x");
    for (int i = 7; i >= 0; i--) {
        uint8_t nibble = (v >> (i * 4)) & 0xF;
        print_char(nibble < 10 ? ('0' + nibble) : ('A' + nibble - 10));
    }
}

/* ==== Test Framework (Simplified Logging) ================================ */
static int tests_run = 0;
static int tests_failed = 0;

#define TC_START(desc) do { \
    tests_run++; \
    print_str("Test: "); print_str(desc); print_str(" ..."); \
} while(0)

#define TC_RESULT(cond, fail_detail_fmt, ...) do { \
    if (cond) { \
        print_str(" [PASS]\n"); \
    } else { \
        tests_failed++; \
        print_str(" [FAIL] "); \
        /* This simplified version won't use print_fmt for fail_detail */ \
        if (fail_detail_fmt) print_str((const char*)fail_detail_fmt); \
        print_nl(); \
    } \
} while(0)

#define TC_EXPECT_EQ(val, exp, msg) do { \
    bool _cond = ((val) == (exp)); \
    if (!_cond) { \
        print_str(" Expected "); print_sdec(exp); print_str(", Got "); print_sdec(val); \
    } \
    TC_RESULT(_cond, msg); \
} while(0)

#define TC_EXPECT_TRUE(cond, msg) TC_RESULT(cond, msg)
#define TC_EXPECT_FALSE(cond, msg) TC_RESULT(!(cond), msg)


/* ==== Individual Test Cases ============================================== */
void test_pid_syscall() {
    print_str("\n--- PID Tests ---\n");
    TC_START("sys_getpid returns a sensible PID");
    pid_t pid = sys_getpid();
    // Check if PID is non-negative. PID 0 is often kernel/idle, PID 1 first user process.
    TC_EXPECT_TRUE(pid >= 0, "PID was negative!");
    if(pid >= 0) { print_str(" (PID: "); print_sdec(pid); print_str(")"); }
    print_nl();
}

void test_file_operations() {
    print_str("\n--- File I/O Tests ---\n");
    const char* FNAME1 = "/testfile1.txt";
    const char* CONTENT1 = "Hello Kernel FS! This is content for file 1.";
    char read_buf[128];
    int fd = -1;
    ssize_t ret_s;

    // 1. Create, Write, Close
    TC_START("Create, Write, Close");
    fd = sys_open(FNAME1, O_CREAT | O_WRONLY | O_TRUNC, DEFAULT_MODE);
    if (fd < 0) {
        print_str(" (sys_open failed, fd: "); print_sdec(fd); print_str(") ");
        TC_RESULT(false, "File open for create/write failed");
        return; // Critical failure for this sequence
    }
    print_str(" (fd_w: "); print_sdec(fd); print_str(") ");

    ret_s = sys_write(fd, CONTENT1, my_strlen(CONTENT1));
    if (ret_s != (ssize_t)my_strlen(CONTENT1)) {
        print_str(" (sys_write expected "); print_sdec(my_strlen(CONTENT1));
        print_str(", got "); print_sdec(ret_s); print_str(") ");
    }
    sys_close(fd); fd = -1; // Close regardless of write outcome for this test structure
    TC_EXPECT_EQ(ret_s, (ssize_t)my_strlen(CONTENT1), "Write operation");

    // 2. Re-open, Read, Verify
    TC_START("Re-open, Read, Verify");
    fd = sys_open(FNAME1, O_RDONLY, 0);
    if (fd < 0) {
        print_str(" (sys_open failed, fd: "); print_sdec(fd); print_str(") ");
        TC_RESULT(false, "File open for read failed");
        return;
    }
    print_str(" (fd_r: "); print_sdec(fd); print_str(") ");

    my_memset(read_buf, 0, sizeof(read_buf));
    ret_s = sys_read(fd, read_buf, sizeof(read_buf) - 1);
    if (ret_s != (ssize_t)my_strlen(CONTENT1)) {
         print_str(" (sys_read expected "); print_sdec(my_strlen(CONTENT1));
         print_str(", got "); print_sdec(ret_s); print_str(") ");
    }
    bool content_match = (ret_s == (ssize_t)my_strlen(CONTENT1)) && (my_strcmp(read_buf, CONTENT1) == 0);
    if (!content_match && ret_s > 0) {
        print_str("\n    Content Expected: '"); print_str(CONTENT1);
        print_str("'\n    Content Got     : '"); print_str(read_buf); print_str("'");
    }
    sys_close(fd); fd = -1;
    TC_EXPECT_TRUE(content_match, "Read content verification");


    // 3. Open non-existent file without O_CREAT
    TC_START("Open non-existent file (no O_CREAT)");
    fd = sys_open("/no_such_file_ever.txt", O_RDONLY, 0);
    TC_EXPECT_TRUE(fd < 0, "sys_open should fail for non-existent file");
    // Example: TC_EXPECT_EQ(fd, -ENOENT, "Error code for non-existent file");
    if (fd >= 0) sys_close(fd); fd = -1;

    // 4. Write to invalid FD
    TC_START("Write to invalid FD (-1)");
    ret_s = sys_write(-1, "data", 4);
    TC_EXPECT_TRUE(ret_s < 0, "sys_write to FD -1 should fail");
    // Example: TC_EXPECT_EQ(ret_s, -EBADF, "Error code for write to invalid FD");

    // 5. Read from invalid FD
    TC_START("Read from invalid FD (999)");
    ret_s = sys_read(999, read_buf, 10);
    TC_EXPECT_TRUE(ret_s < 0, "sys_read from FD 999 should fail");
}


/* ==== Main Test Runner =================================================== */
int main(void) {
    print_str("=== UiAOS Kernel Test Suite v3.1 (Focused) ===\n");

    test_pid_syscall();
    test_file_operations();
    // Add calls to other test suites (e.g., lseek) here if desired

    print_str("\n--- Test Summary ---\n");
    print_str("Total Tests: "); print_sdec(tests_run); print_nl();
    print_str("Passed: "); print_sdec(tests_run - tests_failed); print_nl();
    print_str("Failed: "); print_sdec(tests_failed); print_nl();

    if (tests_failed == 0) {
        print_str(">>> ALL TESTS PASSED! <<<\n");
    } else {
        print_str(">>> SOME TESTS FAILED! <<<\n");
    }

    sys_exit(tests_failed > 0 ? 1 : 0); // Exit with 0 on success, 1 if any test failed
    return 0; // Should not be reached
}
