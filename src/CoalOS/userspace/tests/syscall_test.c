/* Checks the Linux x86_64 system call ABI through the libc wrappers. Uses /tmp
 * only, so it runs unchanged on a Linux host and on Coal OS. */

#include <libc/stdio.h>
#include <libc/string.h>
#include <libc/stdlib.h>
#include <libc/unistd.h>
#include <libc/fcntl.h>
#include <libc/errno.h>
#include <libc/dirent.h>
#include <libc/signal.h>
#include <libc/termios.h>
#include <libc/time.h>
#include <libc/sys/stat.h>
#include <libc/sys/wait.h>
#include <libc/sys/utsname.h>
#include "check.h"

#define FILE_PATH "/tmp/coal_sc_test"
#define DIR_PATH  "/tmp/coal_sc_dir"

static void test_process(void) {
    CHECK(getpid() > 0);
    CHECK(getppid() >= 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) _exit(7);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7);
    CHECK(waitpid(-1, &status, 0) < 0 && errno == ECHILD);
}

static void test_file_io(void) {
    char buf[8];
    struct stat st;

    int fd = open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, "hello world", 11) == 11);
    CHECK(lseek(fd, 6, SEEK_SET) == 6);
    CHECK(read(fd, buf, 5) == 5 && memcmp(buf, "world", 5) == 0);
    CHECK(fstat(fd, &st) == 0 && st.st_size == 11 && S_ISREG(st.st_mode));
    CHECK(close(fd) == 0);
    CHECK(close(fd) < 0 && errno == EBADF);
    CHECK(stat(FILE_PATH, &st) == 0 && st.st_size == 11);
    CHECK(stat("/tmp", &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(stat("/no/such/file", &st) < 0 && errno == ENOENT);
    CHECK(access(FILE_PATH, R_OK) == 0);
    CHECK(open("/no/such/file", O_RDONLY) < 0 && errno == ENOENT);
    CHECK(rename(FILE_PATH, FILE_PATH "2") == 0 && access(FILE_PATH, F_OK) < 0);
    CHECK(unlink(FILE_PATH "2") == 0);
}

static void test_dir(void) {
    char cwd[128];
    int seen = 0;

    CHECK(mkdir(DIR_PATH, 0755) == 0);
    CHECK(mkdir(DIR_PATH, 0755) < 0 && errno == EEXIST);
    int fd = open(DIR_PATH "/a", O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0);
    close(fd);
    DIR *d = opendir(DIR_PATH);
    CHECK(d != NULL);
    struct dirent *e;
    while (d && (e = readdir(d))) seen += !strcmp(e->d_name, "a");
    CHECK(seen == 1);
    if (d) closedir(d);
    CHECK(rmdir(DIR_PATH) < 0);                 /* not empty */
    CHECK(unlink(DIR_PATH "/a") == 0 && rmdir(DIR_PATH) == 0);
    CHECK(chdir("/tmp") == 0 && getcwd(cwd, sizeof cwd) && strcmp(cwd, "/tmp") == 0);
}

static void test_pipe_dup(void) {
    int fds[2];
    char c;

    CHECK(pipe(fds) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        write(fds[1], "Q", 1);
        _exit(0);
    }
    close(fds[1]);
    CHECK(read(fds[0], &c, 1) == 1 && c == 'Q');
    CHECK(read(fds[0], &c, 1) == 0);            /* writer gone: end of file */
    waitpid(pid, NULL, 0);
    int d = dup(fds[0]);
    CHECK(d >= 0 && dup2(fds[0], 20) == 20);
    close(d); close(20); close(fds[0]);
}

static volatile int got_signal;
static void on_usr1(int sig) { got_signal = sig; }

static void test_signal(void) {
    CHECK(signal(SIGUSR1, on_usr1) != (sighandler_t)-1);
    CHECK(kill(getpid(), SIGUSR1) == 0);
    CHECK(got_signal == SIGUSR1);
    CHECK(kill(0x7ffffff0, 0) < 0 && errno == ESRCH);

    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGUSR1, SIG_DFL);
        for (;;) sleep(1);
    }
    CHECK(kill(pid, SIGKILL) == 0);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}

static void test_misc(void) {
    struct utsname u;
    CHECK(uname(&u) == 0 && u.sysname[0] && u.machine[0]);
    CHECK(time(NULL) > 1700000000);
    struct timespec ts = { 0, 10 * 1000 * 1000 };
    CHECK(nanosleep(&ts, NULL) == 0);
    struct termios t;
    CHECK(tcgetattr(1234, &t) < 0 && errno == EBADF);
    char *p = malloc(1 << 20);                  /* brk must grow the heap */
    CHECK(p != NULL);
    if (p) { memset(p, 1, 1 << 20); free(p); }
}

int main(void) {
    test_process();
    test_file_io();
    test_dir();
    test_pipe_dup();
    test_signal();
    test_misc();
    return check_report("syscall_test");
}
