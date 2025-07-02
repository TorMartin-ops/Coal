/*
 * hello.c – Coal OS User-Space Test Program
 *
 * Purpose: This program provides a test environment for the Coal OS kernel's
 * system call layer. It specifically targets PID management, file I/O operations
 * (create, read, write, close), and lseek functionality. The tests expect
 * standard POSIX negative error codes for error conditions.
 */

/* ==== Core Type Definitions & Constants ================================= */
/*
 * Standard integer types are defined here for portability and clarity
 * across the test suite. These ensure consistent data sizes.
 */
 typedef signed   char      int8_t;
 typedef unsigned char      uint8_t;
 typedef signed   short     int16_t;
 typedef unsigned short     uint16_t;
 typedef signed   int       int32_t;
 typedef unsigned int       uint32_t;
 
 /*
  * Common Unsigned Integer Types for Sizing and Addressing.
  * 'off_t' is specifically int32_t to align with the kernel's syscall return values
  * for file offsets, a change from a potential 64-bit definition to match current kernel.
  */
 typedef uint32_t           size_t;
 typedef int32_t            ssize_t; /* For return values that can be negative (errors) or counts. */
 typedef int32_t            pid_t;   /* Process ID type. */
 typedef int32_t            off_t;   /* File offset type, crucial for lseek. */
 
 #ifndef _UINTPTR_T_DEFINED_HELLO_C
 #define _UINTPTR_T_DEFINED_HELLO_C
 /* Unsigned integer type capable of holding a pointer. Essential for casting. */
 typedef uint32_t uintptr_t;
 #endif
 
 /* Boolean type definition for clarity, maintaining int32_t for ABI consistency. */
 typedef int32_t bool;
 #define true  1
 #define false 0
 #define NULL  ((void*)0)
 
 /* Standard integer limits, useful for boundary condition testing. */
 #ifndef INT32_MIN
 #define INT32_MIN (-2147483647 - 1)
 #endif
 #ifndef SSIZE_MAX
 /* Maximum value for ssize_t, typically the max of a signed 32-bit integer. */
 #define SSIZE_MAX (2147483647)
 #endif
 
 
 /* ==== Kernel Application Binary Interface (ABI) Constants =============== */
 /* These values *must* align precisely with the syscall numbers defined in the Coal OS kernel. */
 #define SYS_EXIT    1  /* Terminate current process. */
 #define SYS_READ    3  /* Read from a file descriptor. */
 #define SYS_WRITE   4  /* Write to a file descriptor. */
 #define SYS_OPEN    5  /* Open or create a file. */
 #define SYS_CLOSE   6  /* Close a file descriptor. */
 #define SYS_PUTS    7  /* Write string to console (kernel-specific convenience). */
 #define SYS_LSEEK   19 /* Reposition file offset. */
 #define SYS_GETPID  20 /* Get current process ID. */
 
 /* File open flags, mirroring standard POSIX definitions. */
 #define O_RDONLY     0x0000 /* Open for reading only. */
 #define O_WRONLY     0x0001 /* Open for writing only. */
 #define O_RDWR       0x0002 /* Open for reading and writing. */
 #define O_CREAT      0x0040 /* Create file if it does not exist. */
 #define O_TRUNC      0x0200 /* Truncate file to zero length if it exists. */
 #define O_APPEND     0x0400 /* Append to file instead of overwriting. */
 #define O_EXCL       0x0080 /* With O_CREAT, fail if file exists. */
 
 /* Default file creation mode (permissions). Kernel might override or ignore. */
 #define DEFAULT_MODE 0666u
 
 /* Standard lseek 'whence' values, defining the reference point for offset. */
 #ifndef SEEK_SET
 #define SEEK_SET    0 /* Seek from beginning of file. */
 #define SEEK_CUR    1 /* Seek from current position. */
 #define SEEK_END    2 /* Seek from end of file. */
 #endif
 
 /*
  * Expected NEGATIVE error codes from syscalls (POSIX style).
  * The kernel's fs_errno.h defines positive error constants (e.g., EBADF as 9).
  * The syscall interface layer is responsible for returning the negated value.
  */
 #define NEG_EBADF        (-9)  /* Bad file descriptor. */
 #define NEG_ENOENT       (-2)  /* No such file or directory. */
 #define NEG_EACCES       (-13) /* Permission denied. */
 #define NEG_EINVAL       (-22) /* Invalid argument. */
 #define NEG_EEXIST       (-17) /* File exists (used with O_EXCL). */
 #define NEG_EMFILE       (-24) /* Too many open files (system-wide or per-process). */
 #define NEG_ENOSPC       (-28) /* No space left on device. */
 #define NEG_EISDIR       (-21) /* Operation on a directory, expected file. */
 #define NEG_ENOTDIR      (-20) /* Operation on a file, expected directory. */
 #define NEG_EFAULT       (-14) /* Bad address (invalid pointer from user space). */
 
 /* ==== Syscall Wrapper Function ========================================== */
 /*
  * Provides a C-callable interface for making system calls via `int $0x80`.
  * Arguments: syscall_number, arg1, arg2, arg3.
  * Return: Value from kernel (typically status or data).
  * This wrapper handles register setup and restoration.
  */
 static inline int32_t syscall(int32_t syscall_number,
                               int32_t arg1_val,
                               int32_t arg2_val,
                               int32_t arg3_val) {
     int32_t return_value;
     /*
      * This inline assembly block defines the low-level mechanism for transitioning
      * to kernel mode. It saves caller-preserved registers, loads arguments into
      * the expected registers (EAX, EBX, ECX, EDX), triggers the software interrupt,
      * and then restores registers. The kernel's return value (from EAX) is moved
      * to the 'return_value' C variable.
      */
     __asm__ volatile (
         "pushl %%ebx          \n\t" /* Save original EBX, callee-saved in some conventions */
         "pushl %%ecx          \n\t" /* Save original ECX */
         "pushl %%edx          \n\t" /* Save original EDX */
 
         "movl %1, %%eax       \n\t" /* syscall_number -> EAX */
         "movl %2, %%ebx       \n\t" /* arg1_val -> EBX */
         "movl %3, %%ecx       \n\t" /* arg2_val -> ECX */
         "movl %4, %%edx       \n\t" /* arg3_val -> EDX */
         "int $0x80            \n\t" /* Kernel interrupt; result in EAX */
 
         "popl %%edx           \n\t" /* Restore original EDX */
         "popl %%ecx           \n\t" /* Restore original ECX */
         "popl %%ebx           \n\t" /* Restore original EBX */
         /* EAX now holds kernel's return value. */
         : "=a" (return_value)        /* Output: EAX to 'return_value' */
         : "m" (syscall_number),      /* Inputs from memory operands */
           "m" (arg1_val),
           "m" (arg2_val),
           "m" (arg3_val)
         : "cc", "memory"             /* Clobbers: condition codes, memory (due to int) */
     );
     return return_value;
 }
 
 /* ==== Syscall Helper Macros ============================================== */
 /* Convenience macros for specific system calls, improving readability. */
 #define sys_exit(code)      syscall(SYS_EXIT, (code), 0, 0)
 #define sys_read(fd,buf,n)  syscall(SYS_READ, (fd), (int32_t)(uintptr_t)(buf), (n))
 #define sys_write(fd,buf,n) syscall(SYS_WRITE, (fd), (int32_t)(uintptr_t)(buf), (n))
 #define sys_open(p,f,m)     syscall(SYS_OPEN, (int32_t)(uintptr_t)(p), (f), (m))
 #define sys_close(fd)       syscall(SYS_CLOSE, (fd), 0, 0)
 #define sys_puts(p)         syscall(SYS_PUTS, (int32_t)(uintptr_t)(p), 0, 0)
 #define sys_getpid()        syscall(SYS_GETPID, 0, 0, 0)
 #define sys_lseek(fd,off,wh) syscall(SYS_LSEEK, (fd), (off), (wh))
 
 /* ==== Minimal Libc-like Utilities ======================================== */
 /*
  * These are minimal, self-contained implementations of common C library
  * functions. They are necessary because this test suite is built with
  * -nostdlib, meaning it cannot link against a standard C library.
  */
 static size_t my_strlen(const char *s) {
     size_t i = 0; if (!s) return 0; while (s[i]) i++; return i;
 }
 static int my_strcmp(const char *s1, const char *s2) {
     if (!s1 && !s2) return 0;
     if (!s1) return -1;
     if (!s2) return 1;
     while (*s1 && (*s1 == *s2)) { s1++; s2++; }
     return *(const unsigned char*)s1 - *(const unsigned char*)s2;
 }
 static void my_memset(void *s, int c, size_t n) {
     unsigned char *p = (unsigned char *)s; while (n-- > 0) *p++ = (unsigned char)c;
 }
 static char* my_strcpy(char *dest, const char *src) {
     char *ret = dest;
     if (!dest || !src) return dest;
     while ((*dest++ = *src++));
     return ret;
 }
 static char* my_strcat(char *dest, const char *src) {
     char *ret = dest;
     if (!dest || !src) return dest;
     while (*dest) dest++;
     while ((*dest++ = *src++));
     return ret;
 }
 
 /* Basic character and string printing functions using the 'sys_puts' syscall. */
 static void print_char(char c) { char b[2]={c,0}; sys_puts(b); }
 static void print_str(const char *s) { if (s) sys_puts(s); }
 static void print_nl() { print_char('\n'); }
 
 /* Prints a signed decimal integer. Handles INT32_MIN. */
 static void print_sdec(int32_t v) {
     char buf[12]; char *p = buf + 11; *p = '\0'; /* Work backwards from end of buffer. */
     if (v == 0) { if (p > buf) *--p = '0'; }
     else {
         bool neg = v < 0;
         /* Handle INT32_MIN carefully to avoid overflow when negating. */
         uint32_t n = neg ? ((v == INT32_MIN) ? 2147483648U : (uint32_t)-v) : (uint32_t)v;
         while (n > 0) { if (p == buf) break; *--p = (n % 10) + '0'; n /= 10; }
         if (neg) { if (p > buf) *--p = '-'; }
     }
     print_str(p);
 }
 /* Prints an unsigned 32-bit integer in hexadecimal format. */
 static void print_hex32(uint32_t v) {
     print_str("0x");
     for (int i = 7; i >= 0; i--) { /* Iterate through nibbles. */
         uint8_t nibble = (v >> (i * 4)) & 0xF;
         print_char(nibble < 10 ? ('0' + nibble) : ('A' + nibble - 10));
     }
 }
 
 /* ==== Test Framework Primitives ========================================== */
 /* Simple framework for running tests and reporting pass/fail status. */
 static int tests_run = 0;
 static int tests_failed = 0;
 static char fail_msg_buf[128]; /* Buffer for detailed failure messages. */
 
 /* Macro to mark the start of a test case and print its description. */
 #define TC_START(desc) do { \
     tests_run++; \
     print_str("Test: "); print_str(desc); print_str(" ..."); \
 } while(0)
 
 /* Macro to report the result of a test case (PASS/FAIL) with an optional message. */
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
 
 /* Macro to assert equality, providing detailed output on failure. */
 #define TC_EXPECT_EQ_DETAIL(val, exp, detail_label_prefix) do { \
     bool _cond = ((val) == (exp)); \
     if (!_cond) { \
         my_strcpy(fail_msg_buf, detail_label_prefix); \
         my_strcat(fail_msg_buf, ": Expected "); \
         /* Convert expected value to string. */ \
         char temp_buf_exp[12]; char *tp_exp = temp_buf_exp+11; *tp_exp = '\0'; \
         int32_t _exp = (exp); \
         if(_exp == 0) *--tp_exp = '0'; else { bool _neg_exp = _exp < 0; uint32_t _n_exp = _neg_exp ? ((_exp == INT32_MIN) ? 2147483648U : (uint32_t)-_exp) : (uint32_t)_exp; \
         while(_n_exp>0) {*--tp_exp=(_n_exp%10)+'0'; _n_exp/=10;} if(_neg_exp && _exp != INT32_MIN) *--tp_exp = '-';} \
         my_strcat(fail_msg_buf, tp_exp); \
         my_strcat(fail_msg_buf, ", Got "); \
         /* Convert actual value to string. */ \
         char temp_buf_val[12]; char *tp_val = temp_buf_val+11; *tp_val = '\0'; \
         int32_t _val = (val); \
         if(_val == 0) *--tp_val = '0'; else { bool _neg_val = _val < 0; uint32_t _n_val = _neg_val ? ((_val == INT32_MIN) ? 2147483648U : (uint32_t)-_val) : (uint32_t)_val; \
         while(_n_val>0) {*--tp_val=(_n_val%10)+'0'; _n_val/=10;} if(_neg_val && _val != INT32_MIN) *--tp_val = '-';} \
         my_strcat(fail_msg_buf, tp_val); \
         TC_RESULT_MSG(_cond, fail_msg_buf); \
     } else { \
         TC_RESULT_MSG(_cond, NULL); \
     } \
 } while(0)
 
 /* Macro to assert a condition is true, providing a message on failure. */
 #define TC_EXPECT_TRUE(cond, msg_on_fail) TC_RESULT_MSG(cond, msg_on_fail)
 
 
 /* ==== Individual Test Cases ============================================== */
 
 /*
  * Tests the `sys_getpid` system call.
  * Verifies that the returned PID is non-negative. PIDs 0 and 1 are typically
  * special (kernel/idle and first user process, respectively).
  */
 void test_pid_syscall() {
     print_str("\n--- PID Tests ---\n");
     TC_START("sys_getpid returns a non-negative PID");
     pid_t pid = sys_getpid();
     bool cond = (pid >= 0); /* PID 0 is valid (e.g. idle/kernel) */
     if (!cond) { my_strcpy(fail_msg_buf, "PID was negative!"); }
     else { /* Construct pass message with PID value */
         my_strcpy(fail_msg_buf, " (Note: PID is ");
         char temp_pid_buf[12]; char *tp_pid = temp_pid_buf+11; *tp_pid = '\0';
         if(pid == 0) *--tp_pid = '0'; else { uint32_t _p = (uint32_t)pid; while(_p>0) {*--tp_pid=(_p%10)+'0'; _p/=10;} }
         my_strcat(fail_msg_buf, tp_pid);
         my_strcat(fail_msg_buf, ")");
     }
     /* Report PASS/FAIL. Only print custom message on failure. */
     if (cond) {
         TC_RESULT_MSG(cond, NULL);
         print_str(fail_msg_buf); /* Print PID note on pass */
     } else {
         TC_RESULT_MSG(cond, fail_msg_buf);
     }
     print_nl();
 }
 
 /*
  * Tests core file operations: create, write, close, re-open, read, verify, append.
  * Uses `testfile1.txt`.
  */
 void test_core_file_operations() {
     print_str("\n--- Core File I/O Tests ---\n");
     const char* FNAME1 = "/testfile1.txt";
     const char* CONTENT1 = "Hello Kernel FS!"; /* 16 chars + null -> 16 to write */
     const char* CONTENT2 = " Appended Text.";  /* 14 chars + null -> 14 to write */
     char read_buf[128]; /* Sufficiently large buffer for reading content. */
     int fd = -1;
     ssize_t ret_s;
     size_t content1_len = my_strlen(CONTENT1);
     size_t content2_len = my_strlen(CONTENT2);
 
     /* Test 1: Create a new file, write initial content, and close it. */
     TC_START("Create, Write, Close");
     fd = sys_open(FNAME1, O_CREAT | O_WRONLY | O_TRUNC, DEFAULT_MODE);
     TC_EXPECT_TRUE(fd >= 0, "sys_open for create/write failed");
     if (fd < 0) return; /* Cannot proceed if open failed. */
 
     ret_s = sys_write(fd, CONTENT1, content1_len);
     TC_EXPECT_EQ_DETAIL(ret_s, (ssize_t)content1_len, "sys_write initial content");
     ret_s = sys_close(fd);
     TC_EXPECT_EQ_DETAIL(ret_s, 0, "sys_close after write");
     fd = -1;
 
     /* Test 2: Re-open the file for reading, verify its content, and test reading past EOF. */
     TC_START("Re-open, Read, Verify");
     fd = sys_open(FNAME1, O_RDONLY, 0); /* Mode is irrelevant for O_RDONLY without O_CREAT */
     TC_EXPECT_TRUE(fd >= 0, "sys_open for read failed");
     if (fd < 0) return;
 
     my_memset(read_buf, 0, sizeof(read_buf)); /* Clear buffer before read. */
     ret_s = sys_read(fd, read_buf, content1_len);
     TC_EXPECT_EQ_DETAIL(ret_s, (ssize_t)content1_len, "sys_read full content");
     if (ret_s == (ssize_t)content1_len) {
         TC_EXPECT_EQ_DETAIL(my_strcmp(read_buf, CONTENT1), 0, "Content verification");
     }
     /* Attempt to read past the End-Of-File. Should return 0 bytes read. */
     my_memset(read_buf, 0, sizeof(read_buf));
     ret_s = sys_read(fd, read_buf, 10);
     TC_EXPECT_EQ_DETAIL(ret_s, 0, "sys_read past EOF should return 0");
 
     ret_s = sys_close(fd);
     TC_EXPECT_EQ_DETAIL(ret_s, 0, "sys_close after read");
     fd = -1;
 
     /* Test 3: Open the file in append mode and write additional content. */
     TC_START("Append Mode (O_APPEND)");
     /* File must exist for O_APPEND without O_CREAT. */
     fd = sys_open(FNAME1, O_WRONLY | O_APPEND, 0);
     TC_EXPECT_TRUE(fd >= 0, "sys_open for append failed");
     if (fd < 0) return;
 
     ret_s = sys_write(fd, CONTENT2, content2_len);
     TC_EXPECT_EQ_DETAIL(ret_s, (ssize_t)content2_len, "sys_write append content");
     ret_s = sys_close(fd);
     TC_EXPECT_EQ_DETAIL(ret_s, 0, "sys_close after append");
     fd = -1;
 
     /* Test 4: Verify the appended content by reading the entire file. */
     TC_START("Verify Appended Content");
     fd = sys_open(FNAME1, O_RDONLY, 0);
     TC_EXPECT_TRUE(fd >= 0, "sys_open for append verification failed");
     if (fd < 0) return;
 
     my_memset(read_buf, 0, sizeof(read_buf));
     size_t total_len = content1_len + content2_len;
     ret_s = sys_read(fd, read_buf, total_len);
     TC_EXPECT_EQ_DETAIL(ret_s, (ssize_t)total_len, "sys_read appended content length");
 
     if (ret_s == (ssize_t)total_len) {
         /* Construct expected full content. */
         char expected_total_content[64]; /* Ensure buffer is large enough. */
         my_strcpy(expected_total_content, CONTENT1);
         my_strcat(expected_total_content, CONTENT2);
         TC_EXPECT_EQ_DETAIL(my_strcmp(read_buf, expected_total_content), 0, "Appended content verification");
     }
     ret_s = sys_close(fd);
     TC_EXPECT_EQ_DETAIL(ret_s, 0, "sys_close after append verification");
     fd = -1;
 }
 
 /*
  * Tests `sys_lseek` operations: SEEK_SET, SEEK_CUR, SEEK_END, and writing after SEEK_END.
  * Uses `lseektest.txt`.
  */
 void test_lseek_operations() {
     print_str("\n--- Lseek Tests ---\n");
     const char* FNAME_LSEEK = "/lseektest.txt";
     const char* DATA1 = "0123456789"; /* 10 bytes */
     const char* DATA2 = "XYZ";        /* 3 bytes */
     char buf[32];
     int fd = -1;
     ssize_t ret_s; /* For read/write results. */
     off_t ret_o;   /* For lseek results. */
 
     /* Setup: Create file with initial content. */
     fd = sys_open(FNAME_LSEEK, O_CREAT | O_RDWR | O_TRUNC, DEFAULT_MODE);
     TC_EXPECT_TRUE(fd >= 0, "lseek test: sys_open for setup failed");
     if (fd < 0) return;
     ret_s = sys_write(fd, DATA1, my_strlen(DATA1));
     TC_EXPECT_EQ_DETAIL(ret_s, (ssize_t)my_strlen(DATA1), "lseek test: initial write");
 
     /* Test 1: SEEK_SET - Seek to a specific offset from the beginning. */
     TC_START("lseek with SEEK_SET");
     ret_o = sys_lseek(fd, 5, SEEK_SET); /* Seek to offset 5 (to '5'). */
     TC_EXPECT_EQ_DETAIL(ret_o, 5, "lseek SEEK_SET to 5");
     my_memset(buf, 0, sizeof(buf));
     ret_s = sys_read(fd, buf, 3); /* Read "567". */
     TC_EXPECT_EQ_DETAIL(ret_s, 3, "lseek test: read after SEEK_SET");
     if (ret_s == 3) {
         TC_EXPECT_EQ_DETAIL(my_strcmp(buf, "567"), 0, "lseek test: content after SEEK_SET");
     }
 
     /* Test 2: SEEK_CUR - Seek relative to the current position. */
     TC_START("lseek with SEEK_CUR");
     /* Current position is 5 (start of "567") + 3 (bytes read) = 8. */
     ret_o = sys_lseek(fd, -2, SEEK_CUR); /* Seek back 2 bytes to offset 6 (to '6'). */
     TC_EXPECT_EQ_DETAIL(ret_o, 6, "lseek SEEK_CUR to 6");
     my_memset(buf, 0, sizeof(buf));
     ret_s = sys_read(fd, buf, 2); /* Read "67". */
     TC_EXPECT_EQ_DETAIL(ret_s, 2, "lseek test: read after SEEK_CUR");
     if (ret_s == 2) {
         TC_EXPECT_EQ_DETAIL(my_strcmp(buf, "67"), 0, "lseek test: content after SEEK_CUR");
     }
 
     /* Test 3: SEEK_END - Seek relative to the end of the file. */
     TC_START("lseek with SEEK_END");
     /* File size is 10. Current pos is 6 + 2 = 8. */
     ret_o = sys_lseek(fd, 0, SEEK_END); /* Seek to end of file (offset 10). */
     TC_EXPECT_EQ_DETAIL(ret_o, 10, "lseek SEEK_END to 10 (EOF)");
     ret_s = sys_read(fd, buf, 1); /* Read at EOF should return 0. */
     TC_EXPECT_EQ_DETAIL(ret_s, 0, "lseek test: read at EOF after SEEK_END");
 
     /* Test 4: Write after SEEK_END - Should extend the file. */
     TC_START("lseek write after SEEK_END");
     ret_o = sys_lseek(fd, 0, SEEK_END); /* Ensure at end of file (offset 10). */
     TC_EXPECT_EQ_DETAIL(ret_o, 10, "lseek SEEK_END before extend");
     ret_s = sys_write(fd, DATA2, my_strlen(DATA2)); /* Write "XYZ". */
     TC_EXPECT_EQ_DETAIL(ret_s, (ssize_t)my_strlen(DATA2), "lseek test: write to extend file");
 
     size_t expected_new_size = my_strlen(DATA1) + my_strlen(DATA2); /* 10 + 3 = 13. */
     ret_o = sys_lseek(fd, 0, SEEK_END); /* Check new file size. */
     TC_EXPECT_EQ_DETAIL(ret_o, (off_t)expected_new_size, "lseek test: new file size after extend");
 
     /* Verify the extended content. */
     ret_o = sys_lseek(fd, 0, SEEK_SET); /* Seek to start for full read. */
     TC_EXPECT_EQ_DETAIL(ret_o, 0, "lseek test: seek to start for verification");
     my_memset(buf, 0, sizeof(buf));
     ret_s = sys_read(fd, buf, sizeof(buf)-1); /* Read up to buffer capacity. */
     TC_EXPECT_EQ_DETAIL(ret_s, (ssize_t)expected_new_size, "lseek test: read full extended content");
     if (ret_s == (ssize_t)expected_new_size) {
         char expected_content[32];
         my_strcpy(expected_content, DATA1);
         my_strcat(expected_content, DATA2); /* Expected: "0123456789XYZ" */
         TC_EXPECT_EQ_DETAIL(my_strcmp(buf, expected_content), 0, "lseek test: verify extended content");
     }
 
     ret_s = sys_close(fd);
     TC_EXPECT_EQ_DETAIL(ret_s, 0, "lseek test: final close");
     fd = -1;
 }
 
 /*
  * Tests various error conditions for file and lseek operations.
  * Focuses on correct negative POSIX error codes.
  */
 void test_error_conditions() {
     print_str("\n--- Error Condition Tests ---\n");
     char buf[10]; /* Small buffer for read tests. */
     int fd = -1;
     ssize_t ret_s;
 
     /* Test opening a non-existent file without O_CREAT. Expect -ENOENT. */
     TC_START("Open non-existent file (no O_CREAT)");
     fd = sys_open("/no_such_file.txt", O_RDONLY, 0);
     TC_EXPECT_EQ_DETAIL(fd, NEG_ENOENT, "sys_open non-existent (expected -ENOENT)");
     if (fd >= 0) sys_close(fd); fd = -1; /* Close if accidentally opened. */
 
     /* Test opening an existing file with O_CREAT | O_EXCL. Expect -EEXIST. */
     TC_START("Open existing file with O_CREAT | O_EXCL");
     /* First, create a file to ensure it exists. */
     fd = sys_open("/exist_test.txt", O_CREAT | O_WRONLY | O_TRUNC, DEFAULT_MODE);
     TC_EXPECT_TRUE(fd >= 0, "Error test: setup open for O_EXCL failed");
     if (fd < 0) return; /* Cannot proceed this specific test path. */
     sys_close(fd); fd = -1;
     /* Now, attempt to open with O_EXCL. */
     fd = sys_open("/exist_test.txt", O_CREAT | O_EXCL, DEFAULT_MODE);
     TC_EXPECT_EQ_DETAIL(fd, NEG_EEXIST, "sys_open O_EXCL on existing (expected -EEXIST)");
     if (fd >= 0) sys_close(fd); fd = -1;
 
     /* Test operations on invalid file descriptors. Expect -EBADF. */
     TC_START("Write to invalid FD (-1)");
     ret_s = sys_write(-1, "data", 4);
     TC_EXPECT_EQ_DETAIL(ret_s, NEG_EBADF, "sys_write to FD -1 (expected -EBADF)");
 
     TC_START("Read from invalid FD (999)");
     ret_s = sys_read(999, buf, 1);
     TC_EXPECT_EQ_DETAIL(ret_s, NEG_EBADF, "sys_read from FD 999 (expected -EBADF)");
 
     TC_START("Close invalid FD (-5)");
     ret_s = sys_close(-5);
     TC_EXPECT_EQ_DETAIL(ret_s, NEG_EBADF, "sys_close FD -5 (expected -EBADF)");
 
     TC_START("Lseek on invalid FD (123)");
     /* Cast off_t result to ssize_t for TC_EXPECT_EQ_DETAIL, assuming error codes fit. */
     ret_s = (ssize_t)sys_lseek(123, 0, SEEK_SET);
     TC_EXPECT_EQ_DETAIL(ret_s, NEG_EBADF, "sys_lseek on FD 123 (expected -EBADF)");
 
     /* Test writing to a file opened read-only. Expect -EACCES. */
     TC_START("Write to RDONLY file descriptor");
     /* Create and write something, then re-open O_RDONLY. */
     fd = sys_open("/rdonly_test.txt", O_CREAT | O_RDWR | O_TRUNC, DEFAULT_MODE);
     TC_EXPECT_TRUE(fd >= 0, "Error test: RDONLY setup open RDWR failed");
     if(fd < 0) return;
     sys_write(fd, "tmp", 3);
     sys_close(fd); fd = -1;
     /* Reopen as read-only. */
     fd = sys_open("/rdonly_test.txt", O_RDONLY, 0);
     TC_EXPECT_TRUE(fd >= 0, "Error test: RDONLY setup open O_RDONLY failed");
     if(fd < 0) return;
     ret_s = sys_write(fd, "test", 4); /* Attempt write. */
     TC_EXPECT_EQ_DETAIL(ret_s, NEG_EACCES, "sys_write to RDONLY fd (expected -EACCES)");
     sys_close(fd); fd = -1;
 
     /* Test reading from a file opened write-only. Expect -EACCES. */
     TC_START("Read from WRONLY file descriptor");
     fd = sys_open("/wronly_test.txt", O_CREAT | O_WRONLY | O_TRUNC, DEFAULT_MODE);
     TC_EXPECT_TRUE(fd >= 0, "Error test: WRONLY setup open failed");
     if(fd < 0) return;
     sys_write(fd, "tmp", 3); /* Write some data. */
     /* Some OS might require seek to 0 before read if WRONLY. Kernel might not support read at all. */
     /* sys_lseek(fd, 0, SEEK_SET); */
     ret_s = sys_read(fd, buf, 1); /* Attempt read. */
     TC_EXPECT_EQ_DETAIL(ret_s, NEG_EACCES, "sys_read from WRONLY fd (expected -EACCES)");
     sys_close(fd); fd = -1;
 }
 
 
 /* ==== Main Test Runner =================================================== */
 /* Executes all defined test suites and prints a summary. */
 int main(void) {
     print_str("=== Coal OS Kernel Test Suite v3.9.1 (POSIX Errors) ===\n");
 
     test_pid_syscall();
     test_core_file_operations();
     test_lseek_operations();
     test_error_conditions();
     /* Add calls to other test suites here as they are developed. */
 
     print_str("\n--- Test Summary ---\n");
     print_str("Total Tests: "); print_sdec(tests_run); print_nl();
     print_str("Passed: "); print_sdec(tests_run - tests_failed); print_nl();
     print_str("Failed: "); print_sdec(tests_failed); print_nl();
 
     if (tests_failed == 0) {
         print_str(">>> ALL TESTS PASSED! <<<\n");
     } else {
         print_str(">>> SOME TESTS FAILED! SEE DETAILS ABOVE. <<<\n");
     }
 
     /* Exit with status 0 if all passed, 1 if any failed. */
     sys_exit(tests_failed > 0 ? 1 : 0);
     return 0; /* Should not be reached due to sys_exit. */
 }