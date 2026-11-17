/**
 * @file syscall_test.c
 * @brief Checks the system calls that a user program can rely on. Run it on
 *        the booted system; it needs the disk image with /hello.elf on it.
 */

#include <libc/stdio.h>
#include <libc/string.h>
#include <libc/unistd.h>
#include "check.h"

static void test_process(void) {
    CHECK(getpid() > 0);
    CHECK(getppid() >= 0);
}

static void test_file_io(void) {
    char buf[4];

    int fd = open("/hello.elf", O_RDONLY);
    CHECK(fd >= 0);
    if (fd < 0) return;

    CHECK(read(fd, buf, 4) == 4);
    CHECK(buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F');
    CHECK(lseek(fd, 1, SEEK_SET) == 1);
    CHECK(read(fd, buf, 3) == 3 && memcmp(buf, "ELF", 3) == 0);
    CHECK(close(fd) == 0);
    CHECK(close(fd) < 0);                    /* already closed */
    CHECK(open("/no/such/file", O_RDONLY) < 0);
}

static void test_dup2(void) {
    int fd = open("/hello.elf", O_RDONLY);
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(dup2(fd, 10) == 10);
    char c;
    CHECK(read(10, &c, 1) == 1 && c == 0x7f);
    close(10);
    close(fd);
}

static void test_fork_wait(void) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) _exit(7);
    if (pid > 0) {
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid);
    }
}

int main(void) {
    test_process();
    test_file_io();
    test_dup2();
    test_fork_wait();
    return check_report("syscall_test");
}
