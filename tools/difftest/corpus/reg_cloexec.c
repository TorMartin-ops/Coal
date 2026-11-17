/* Regression: close-on-exec requested at open, pipe2, socket and dup3 time, and its effect across execve. */
#include "trace.h"
#include <sys/wait.h>

int main(int argc, char **argv) {
    if (argc > 1) {                       /* exec target: which of the inherited descriptors are open? */
        NOTE("after exec: fd10=%s fd11=%s", yn(K(SYS_fcntl, 10, F_GETFD) >= 0), yn(K(SYS_fcntl, 11, F_GETFD) >= 0));
        return 0;
    }
    init();
    long keep = K(SYS_open, "/etc/passwd", O_RDONLY);
    long cl = K(SYS_open, "/etc/passwd", O_RDONLY | O_CLOEXEC);
    R("fcntl(GETFD plain open)", K(SYS_fcntl, keep, F_GETFD));
    R("fcntl(GETFD O_CLOEXEC open)", K(SYS_fcntl, cl, F_GETFD));
    int p[2];
    K(SYS_pipe2, p, O_CLOEXEC);
    R("fcntl(GETFD pipe2 read end)", K(SYS_fcntl, p[0], F_GETFD));
    R("fcntl(GETFD pipe2 write end)", K(SYS_fcntl, p[1], F_GETFD));
    long s = K(SYS_socket, AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    R("fcntl(GETFD SOCK_CLOEXEC)", K(SYS_fcntl, s, F_GETFD));
    R("dup3(CLOEXEC)", K(SYS_dup3, keep, 10, O_CLOEXEC));
    R("dup2 leaves flag clear", K(SYS_dup2, keep, 11));
    R("fcntl(GETFD dup3)", K(SYS_fcntl, 10, F_GETFD));
    R("fcntl(GETFD dup2)", K(SYS_fcntl, 11, F_GETFD));
    R("fcntl(SETFD 0)", K(SYS_fcntl, cl, F_SETFD, 0));
    R("fcntl(GETFD cleared)", K(SYS_fcntl, cl, F_GETFD));
    R("fcntl(DUPFD_CLOEXEC)", K(SYS_fcntl, keep, 1030, 20));
    R("fcntl(GETFD dupfd_cloexec)", K(SYS_fcntl, 20, F_GETFD));
    char *args[] = {argv[0], "child", 0}, *env[] = {0};
    long c = K(SYS_fork);
    if (c == 0) { K(SYS_execve, argv[0], args, env); K(SYS_exit, 99); }
    int st = 0;
    K(SYS_wait4, c, &st, 0, 0);
    NOTE("exec child exited %d", (st >> 8) & 0xff);
    return 0;
}
