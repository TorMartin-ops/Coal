/*
 * hello.c – UiAOS User-Space Kernel Test Suite (v3.9 - Full Suite)
 *
 * Purpose: Rigorously test kernel syscalls, focusing on PID, file I/O,
 * and lseek operations. Uses standard "=a" for syscall return.
 *
 * Build: i686-elf-gcc -m32 -Wall -Wextra -nostdlib -fno-builtin \
 * -fno-stack-protector -std=gnu99 -o hello.elf hello.c
 * (Ensure entry.asm is linked if it calls main)
 */

/* ==== Typedefs and Basic Definitions ===================================== */
typedef signed   char      int8_t;
typedef unsigned char      uint8_t;
typedef signed   short     int16_t;
typedef unsigned short     uint16_t;
typedef signed   int       int32_t;
typedef unsigned int       uint32_t;

typedef uint32_t           size_t;
typedef int32_t            ssize_t;
typedef int32_t            pid_t;
typedef int32_t            off_t;

#ifndef _UINTPTR_T_DEFINED_HELLO_C
#define _UINTPTR_T_DEFINED_HELLO_C
typedef uint32_t uintptr_t;
#endif

typedef int32_t bool;
#define true  1
#define false 0
#define NULL  ((void*)0)

#ifndef INT32_MIN
#define INT32_MIN (-2147483647 - 1)
#endif
#ifndef SSIZE_MAX
#define SSIZE_MAX (2147483647)
#endif


/* ==== Kernel ABI Constants (MUST MATCH YOUR KERNEL) ===================== */
#define SYS_EXIT    1
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_PUTS    7
#define SYS_LSEEK   19
#define SYS_GETPID  20

#define O_RDONLY     0x0000
#define O_WRONLY     0x0001
#define O_RDWR       0x0002
#define O_CREAT      0x0040 // If file does not exist, create it.
#define O_TRUNC      0x0200 // Truncate file to zero length if it exists.
#define O_APPEND     0x0400 // Append to file instead of overwriting.
#define O_EXCL       0x0080 // With O_CREAT, fail if file exists.

#define DEFAULT_MODE 0666u // User r/w, Group r/w, Other r/w

#ifndef SEEK_SET
#define SEEK_SET    0 // Seek from beginning of file.
#define SEEK_CUR    1 // Seek from current position.
#define SEEK_END    2 // Seek from end of file.
#endif

// Expected error codes (negative values from kernel)
#define EBADF        9
#define ENOENT       2
#define EACCES      13
#define EINVAL      22
#define EEXIST      17

/* ==== Syscall Wrapper (Standard "=a" output) =========================== */
static inline int32_t syscall(int32_t syscall_number,
                              int32_t arg1_val,
                              int32_t arg2_val,
                              int32_t arg3_val) {
    int32_t return_value;
    // This wrapper uses explicit mov instructions from memory operands for inputs,
    // and the standard "=a" for output.
    __asm__ volatile (
        "pushl %%ebx          \n\t" // Save original EBX
        "pushl %%ecx          \n\t" // Save original ECX
        "pushl %%edx          \n\t" // Save original EDX

        "movl %1, %%eax       \n\t" // syscall_number from memory operand %1
        "movl %2, %%ebx       \n\t" // arg1_val from memory operand %2
        "movl %3, %%ecx       \n\t" // arg2_val from memory operand %3
        "movl %4, %%edx       \n\t" // arg3_val from memory operand %4
        "int $0x80            \n\t" // Kernel call; EAX now holds the result

        "popl %%edx           \n\t" // Restore original EDX
        "popl %%ecx           \n\t" // Restore original ECX
        "popl %%ebx           \n\t" // Restore original EBX
        // EAX still holds the kernel's return value here
        : "=a" (return_value)        // Output %0: value in EAX goes to C var return_value
        : "m" (syscall_number),      // Input %1
          "m" (arg1_val),          // Input %2
          "m" (arg2_val),          // Input %3
          "m" (arg3_val)           // Input %4
        : "cc", "memory"             // EAX is output. EBX, ECX, EDX are clobbered by movl but restored.
    );
    return return_value;
}

/* ==== Syscall Helper Macros ============================================== */
#define sys_exit(code)      syscall(SYS_EXIT, (code), 0, 0)
#define sys_read(fd,buf,n)  syscall(SYS_READ, (fd), (int32_t)(uintptr_t)(buf), (n))
#define sys_write(fd,buf,n) syscall(SYS_WRITE, (fd), (int32_t)(uintptr_t)(buf), (n))
#define sys_open(p,f,m)     syscall(SYS_OPEN, (int32_t)(uintptr_t)(p), (f), (m))
#define sys_close(fd)       syscall(SYS_CLOSE, (fd), 0, 0)
#define sys_puts(p)         syscall(SYS_PUTS, (int32_t)(uintptr_t)(p), 0, 0)
#define sys_getpid()        syscall(SYS_GETPID, 0, 0, 0)
#define sys_lseek(fd,off,wh) syscall(SYS_LSEEK, (fd), (off), (wh))

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
static char* my_strcpy(char *dest, const char *src) {
    char *ret = dest;
    if (!dest || !src) return dest; // Or handle error
    while ((*dest++ = *src++));
    return ret;
}
static char* my_strcat(char *dest, const char *src) {
    char *ret = dest;
    if (!dest || !src) return dest;
    while (*dest) dest++; // Find end of dest
    while ((*dest++ = *src++)); // Append src
    return ret;
}


static void print_char(char c) { char b[2]={c,0}; sys_puts(b); }
static void print_str(const char *s) { if (s) sys_puts(s); }
static void print_nl() { print_char('\n'); }

static void print_sdec(int32_t v) {
    char buf[12]; char *p = buf + 11; *p = '\0';
    if (v == 0) { if (p > buf) *--p = '0'; }
    else {
        bool neg = v < 0;
        uint32_t n = neg ? ((v == INT32_MIN) ? 2147483648U : (uint32_t)-v) : (uint32_t)v;
        while (n > 0) { if (p == buf) break; *--p = (n % 10) + '0'; n /= 10; }
        if (neg) { if (p > buf) *--p = '-'; }
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

/* ==== Test Framework ===================================================== */
static int tests_run = 0;
static int tests_failed = 0;
static char fail_msg_buf[128]; // Buffer for formatting failure messages

#define TC_START(desc) do { \
    tests_run++; \
    print_str("Test: "); print_str(desc); print_str(" ..."); \
} while(0)

#define TC_RESULT_MSG(cond, msg_on_fail) do { \
    if (cond) { \
        print_str(" [PASS]\n"); \
    } else { \
        tests_failed++; \
        print_str(" [FAIL] "); \
        if (msg_on_fail) print_str((const char*)msg_on_fail); \
        print_nl(); \
    } \
} while(0)

#define TC_EXPECT_EQ_DETAIL(val, exp, detail_label) do { \
    bool _cond = ((val) == (exp)); \
    if (!_cond) { \
        my_strcpy(fail_msg_buf, detail_label); \
        my_strcat(fail_msg_buf, ": Expected "); \
        /* Simple itoa for fail_msg_buf */ \
        char temp_buf[12]; char *tp = temp_buf+11; *tp = '\0'; \
        int32_t _exp = (exp); \
        if(_exp == 0) *--tp = '0'; else { bool _neg = _exp < 0; uint32_t _n = _neg ? -_exp : _exp; \
        while(_n>0) {*--tp=(_n%10)+'0'; _n/=10;} if(_neg) *--tp = '-';} \
        my_strcat(fail_msg_buf, tp); \
        my_strcat(fail_msg_buf, ", Got "); \
        tp = temp_buf+11; *tp = '\0'; \
        int32_t _val = (val); \
        if(_val == 0) *--tp = '0'; else { bool _neg = _val < 0; uint32_t _n = _neg ? -_val : _val; \
        while(_n>0) {*--tp=(_n%10)+'0'; _n/=10;} if(_neg) *--tp = '-';} \
        my_strcat(fail_msg_buf, tp); \
        TC_RESULT_MSG(_cond, fail_msg_buf); \
    } else { \
        TC_RESULT_MSG(_cond, NULL); \
    } \
} while(0)

#define TC_EXPECT_TRUE(cond, msg_on_fail) TC_RESULT_MSG(cond, msg_on_fail)
#define TC_EXPECT_FALSE(cond, msg_on_fail) TC_RESULT_MSG(!(cond), msg_on_fail)
#define TC_EXPECT_NE(val, exp, msg_on_fail) TC_RESULT_MSG((val) != (exp), msg_on_fail)
#define TC_EXPECT_GE(val, exp, msg_on_fail) TC_RESULT_MSG((val) >= (exp), msg_on_fail)
#define TC_EXPECT_LT(val, exp, msg_on_fail) TC_RESULT_MSG((val) < (exp), msg_on_fail)


/* ==== Individual Test Cases ============================================== */
void test_pid_syscall() {
    print_str("\n--- PID Tests ---\n");
    TC_START("sys_getpid returns a non-negative PID");
    pid_t pid = sys_getpid();
    TC_EXPECT_GE(pid, 0, "PID was negative!");
    if(pid >= 0) { print_str(" (Note: PID is "); print_sdec(pid); print_str(")\n"); }
}

void test_core_file_operations() {
    print_str("\n--- Core File I/O Tests ---\n");
    const char* FNAME1 = "/testfile1.txt";
    const char* CONTENT1 = "Hello Kernel World!"; // 17 chars + null
    const char* CONTENT2 = " Appended data.";    // 15 chars + null
    char read_buf[128];
    int fd = -1;
    ssize_t ret_s;
    size_t content1_len = my_strlen(CONTENT1);
    size_t content2_len = my_strlen(CONTENT2);

    // 1. Create, Write, Close
    TC_START("Create, Write, Close");
    fd = sys_open(FNAME1, O_CREAT | O_WRONLY | O_TRUNC, DEFAULT_MODE);
    TC_EXPECT_GE(fd, 0, "sys_open for create/write failed");
    if (fd < 0) return; // Critical failure

    ret_s = sys_write(fd, CONTENT1, content1_len);
    TC_EXPECT_EQ_DETAIL(ret_s, (ssize_t)content1_len, "sys_write initial content");
    ret_s = sys_close(fd);
    TC_EXPECT_EQ_DETAIL(ret_s, 0, "sys_close after write");
    fd = -1;

    // 2. Re-open, Read, Verify
    TC_START("Re-open, Read, Verify");
    fd = sys_open(FNAME1, O_RDONLY, 0);
    TC_EXPECT_GE(fd, 0, "sys_open for read failed");
    if (fd < 0) return;

    my_memset(read_buf, 0, sizeof(read_buf));
    ret_s = sys_read(fd, read_buf, content1_len);
    TC_EXPECT_EQ_DETAIL(ret_s, (ssize_t)content1_len, "sys_read full content");
    if (ret_s == (ssize_t)content1_len) {
        TC_EXPECT_EQ_DETAIL(my_strcmp(read_buf, CONTENT1), 0, "Content verification");
    }
    // Try reading past EOF
    my_memset(read_buf, 0, sizeof(read_buf));
    ret_s = sys_read(fd, read_buf, 10);
    TC_EXPECT_EQ_DETAIL(ret_s, 0, "sys_read past EOF should return 0");

    ret_s = sys_close(fd);
    TC_EXPECT_EQ_DETAIL(ret_s, 0, "sys_close after read");
    fd = -1;

    // 3. Append Mode Test
    TC_START("Append Mode (O_APPEND)");
    fd = sys_open(FNAME1, O_WRONLY | O_APPEND, 0); // No O_CREAT, file must exist
    TC_EXPECT_GE(fd, 0, "sys_open for append failed");
    if (fd < 0) return;

    ret_s = sys_write(fd, CONTENT2, content2_len);
    TC_EXPECT_EQ_DETAIL(ret_s, (ssize_t)content2_len, "sys_write append content");
    ret_s = sys_close(fd);
    TC_EXPECT_EQ_DETAIL(ret_s, 0, "sys_close after append");
    fd = -1;

    // 4. Verify Appended Content
    TC_START("Verify Appended Content");
    fd = sys_open(FNAME1, O_RDONLY, 0);
    TC_EXPECT_GE(fd, 0, "sys_open for append verification failed");
    if (fd < 0) return;

    my_memset(read_buf, 0, sizeof(read_buf));
    size_t total_len = content1_len + content2_len;
    ret_s = sys_read(fd, read_buf, total_len);
    TC_EXPECT_EQ_DETAIL(ret_s, (ssize_t)total_len, "sys_read appended content length");

    if (ret_s == (ssize_t)total_len) {
        char expected_total_content[64]; // Ensure large enough
        my_strcpy(expected_total_content, CONTENT1);
        my_strcat(expected_total_content, CONTENT2);
        TC_EXPECT_EQ_DETAIL(my_strcmp(read_buf, expected_total_content), 0, "Appended content verification");
    }
    ret_s = sys_close(fd);
    TC_EXPECT_EQ_DETAIL(ret_s, 0, "sys_close after append verification");
    fd = -1;
}

void test_lseek_operations() {
    print_str("\n--- Lseek Tests ---\n");
    const char* FNAME_LSEEK = "/lseektest.txt";
    const char* DATA1 = "0123456789"; // 10 bytes
    const char* DATA2 = "ABCDE";    // 5 bytes
    char buf[32];
    int fd = -1;
    ssize_t ret_s;
    off_t ret_o;

    // Setup: Create a file with known content
    fd = sys_open(FNAME_LSEEK, O_CREAT | O_RDWR | O_TRUNC, DEFAULT_MODE);
    TC_EXPECT_GE(fd, 0, "lseek test: sys_open for setup failed");
    if (fd < 0) return;
    ret_s = sys_write(fd, DATA1, my_strlen(DATA1));
    TC_EXPECT_EQ_DETAIL(ret_s, (ssize_t)my_strlen(DATA1), "lseek test: initial write");
    
    // 1. SEEK_SET
    TC_START("lseek with SEEK_SET");
    ret_o = sys_lseek(fd, 5, SEEK_SET); // Seek to offset 5
    TC_EXPECT_EQ_DETAIL(ret_o, 5, "lseek SEEK_SET to 5");
    my_memset(buf, 0, sizeof(buf));
    ret_s = sys_read(fd, buf, 3); // Read "567"
    TC_EXPECT_EQ_DETAIL(ret_s, 3, "lseek test: read after SEEK_SET");
    if (ret_s == 3) {
        TC_EXPECT_EQ_DETAIL(my_strcmp(buf, "567"), 0, "lseek test: content after SEEK_SET");
    }

    // 2. SEEK_CUR
    TC_START("lseek with SEEK_CUR");
    // Current pos is 5 + 3 = 8
    ret_o = sys_lseek(fd, -2, SEEK_CUR); // Seek back 2 bytes (to offset 6)
    TC_EXPECT_EQ_DETAIL(ret_o, 6, "lseek SEEK_CUR to 6");
    my_memset(buf, 0, sizeof(buf));
    ret_s = sys_read(fd, buf, 2); // Read "67"
    TC_EXPECT_EQ_DETAIL(ret_s, 2, "lseek test: read after SEEK_CUR");
    if (ret_s == 2) {
        TC_EXPECT_EQ_DETAIL(my_strcmp(buf, "67"), 0, "lseek test: content after SEEK_CUR");
    }
    
    // 3. SEEK_END
    TC_START("lseek with SEEK_END");
    // File size is 10. Current pos is 6 + 2 = 8
    ret_o = sys_lseek(fd, 0, SEEK_END); // Seek to end (offset 10)
    TC_EXPECT_EQ_DETAIL(ret_o, 10, "lseek SEEK_END to 10 (EOF)");
    ret_s = sys_read(fd, buf, 1); // Read at EOF
    TC_EXPECT_EQ_DETAIL(ret_s, 0, "lseek test: read at EOF after SEEK_END");

    // 4. Write after SEEK_END (should extend)
    TC_START("lseek write after SEEK_END");
    ret_o = sys_lseek(fd, 0, SEEK_END); // Ensure at end
    TC_EXPECT_EQ_DETAIL(ret_o, 10, "lseek SEEK_END before extend");
    ret_s = sys_write(fd, DATA2, my_strlen(DATA2)); // Write "ABCDE"
    TC_EXPECT_EQ_DETAIL(ret_s, (ssize_t)my_strlen(DATA2), "lseek test: write to extend file");
    
    ret_o = sys_lseek(fd, 0, SEEK_END); // Check new size
    TC_EXPECT_EQ_DETAIL(ret_o, 10 + (off_t)my_strlen(DATA2), "lseek test: new file size after extend");

    // Verify extended content
    ret_o = sys_lseek(fd, 0, SEEK_SET);
    TC_EXPECT_EQ_DETAIL(ret_o, 0, "lseek test: seek to start for verification");
    my_memset(buf, 0, sizeof(buf));
    ret_s = sys_read(fd, buf, sizeof(buf)-1);
    TC_EXPECT_EQ_DETAIL(ret_s, (ssize_t)(my_strlen(DATA1) + my_strlen(DATA2)), "lseek test: read full extended content");
    if (ret_s == (ssize_t)(my_strlen(DATA1) + my_strlen(DATA2))) {
        char expected_content[32];
        my_strcpy(expected_content, DATA1);
        my_strcat(expected_content, DATA2);
        TC_EXPECT_EQ_DETAIL(my_strcmp(buf, expected_content), 0, "lseek test: verify extended content");
    }

    ret_s = sys_close(fd);
    TC_EXPECT_EQ_DETAIL(ret_s, 0, "lseek test: final close");
    fd = -1;
}

void test_error_conditions() {
    print_str("\n--- Error Condition Tests ---\n");
    char buf[10];
    int fd = -1;
    ssize_t ret_s;

    TC_START("Open non-existent file (no O_CREAT)");
    fd = sys_open("/no_such_file.txt", O_RDONLY, 0);
    TC_EXPECT_EQ_DETAIL(fd, -ENOENT, "sys_open non-existent (expected -ENOENT)");
    if (fd >= 0) sys_close(fd);

    TC_START("Open existing file with O_CREAT | O_EXCL");
    // First, create a file
    fd = sys_open("/exist_test.txt", O_CREAT | O_WRONLY | O_TRUNC, DEFAULT_MODE);
    TC_EXPECT_GE(fd, 0, "Error test: setup open");
    if (fd < 0) return;
    sys_close(fd);
    // Now try to open with O_EXCL
    fd = sys_open("/exist_test.txt", O_CREAT | O_EXCL, DEFAULT_MODE);
    TC_EXPECT_EQ_DETAIL(fd, -EEXIST, "sys_open O_EXCL on existing (expected -EEXIST)");
    if (fd >= 0) sys_close(fd);

    TC_START("Write to invalid FD (-1)");
    ret_s = sys_write(-1, "data", 4);
    TC_EXPECT_EQ_DETAIL(ret_s, -EBADF, "sys_write to FD -1 (expected -EBADF)");

    TC_START("Read from invalid FD (999)");
    ret_s = sys_read(999, buf, 1);
    TC_EXPECT_EQ_DETAIL(ret_s, -EBADF, "sys_read from FD 999 (expected -EBADF)");
    
    TC_START("Close invalid FD (-5)");
    ret_s = sys_close(-5);
    TC_EXPECT_EQ_DETAIL(ret_s, -EBADF, "sys_close FD -5 (expected -EBADF)");

    TC_START("Lseek on invalid FD (123)");
    ret_s = (ssize_t)sys_lseek(123, 0, SEEK_SET); // Cast off_t to ssize_t for TC_EXPECT_EQ_DETAIL
    TC_EXPECT_EQ_DETAIL(ret_s, -EBADF, "sys_lseek on FD 123 (expected -EBADF)");

    // Test write to RDONLY file
    TC_START("Write to RDONLY file descriptor");
    fd = sys_open("/rdonly_test.txt", O_CREAT | O_RDWR | O_TRUNC, DEFAULT_MODE); // Create with RDWR
    TC_EXPECT_GE(fd, 0, "Error test: RDONLY setup open RDWR");
    if(fd < 0) return;
    sys_write(fd, "tmp", 3);
    sys_close(fd);
    fd = sys_open("/rdonly_test.txt", O_RDONLY, 0); // Reopen RDONLY
    TC_EXPECT_GE(fd, 0, "Error test: RDONLY setup open RDONLY");
    if(fd < 0) return;
    ret_s = sys_write(fd, "test", 4);
    TC_EXPECT_EQ_DETAIL(ret_s, -EACCES, "sys_write to RDONLY fd (expected -EACCES or -EBADF)");
    sys_close(fd);

    // Test read from WRONLY file (Note: some systems might allow this, POSIX is underspecified)
    TC_START("Read from WRONLY file descriptor");
    fd = sys_open("/wronly_test.txt", O_CREAT | O_WRONLY | O_TRUNC, DEFAULT_MODE);
    TC_EXPECT_GE(fd, 0, "Error test: WRONLY setup open");
    if(fd < 0) return;
    sys_write(fd, "tmp", 3); // Write something so it's not empty
    // Some systems require lseek to 0 before read if opened WRONLY then trying to read
    // sys_lseek(fd, 0, SEEK_SET); // Not strictly WRONLY anymore if lseek works
    ret_s = sys_read(fd, buf, 1);
    TC_EXPECT_EQ_DETAIL(ret_s, -EACCES, "sys_read from WRONLY fd (expected -EACCES or -EBADF)");
    sys_close(fd);
}


/* ==== Main Test Runner =================================================== */
int main(void) {
    print_str("=== UiAOS Kernel Test Suite v3.9 (Full Suite) ===\n");

    test_pid_syscall();
    test_core_file_operations();
    test_lseek_operations();
    test_error_conditions();
    // Add calls to other test suites here

    print_str("\n--- Test Summary ---\n");
    print_str("Total Tests: "); print_sdec(tests_run); print_nl();
    print_str("Passed: "); print_sdec(tests_run - tests_failed); print_nl();
    print_str("Failed: "); print_sdec(tests_failed); print_nl();

    if (tests_failed == 0) {
        print_str(">>> ALL TESTS PASSED! <<<\n");
    } else {
        print_str(">>> SOME TESTS FAILED! SEE DETAILS ABOVE. <<<\n");
    }

    sys_exit(tests_failed > 0 ? 1 : 0); 
    return 0; 
}