/* Pipes and FIFOs: capacity rules, EOF, EPIPE, nonblocking, pipe2 flags, fork-shared ends. */
#include "trace.h"

int main(void) {
    init();
    scratch("pipes");
    int fds[2] = {-1, -1};
    R("pipe", K(SYS_pipe, fds));
    NOTE("fds are %d %d", fds[0], fds[1]);
    R("write(5)", K(SYS_write, fds[1], "hello", 5));
    rd("read(2)", fds[0], 2);
    rd("read(rest)", fds[0], 100);
    R("fcntl(GETFL r)", K(SYS_fcntl, fds[0], F_GETFL));
    R("fcntl(GETFL w)", K(SYS_fcntl, fds[1], F_GETFL));
    R("lseek(pipe)", K(SYS_lseek, fds[0], 0, SEEK_CUR));
    R("write(to read end)", K(SYS_write, fds[0], "x", 1));
    R("read(from write end)", K(SYS_read, fds[1], scratch_, 1));
    FSTAT("fstat(pipe)", fds[0]);
    R("fcntl(SETFL NONBLOCK)", K(SYS_fcntl, fds[0], F_SETFL, O_NONBLOCK));
    R("read(empty nonblock)", K(SYS_read, fds[0], scratch_, 1));
    R("close(w)", K(SYS_close, fds[1]));
    R("read(eof)", K(SYS_read, fds[0], scratch_, 1));
    R("close(r)", K(SYS_close, fds[0]));

    R("pipe2(NONBLOCK|CLOEXEC)", K(SYS_pipe2, fds, O_NONBLOCK | O_CLOEXEC));
    R("fcntl(GETFL pipe2)", K(SYS_fcntl, fds[0], F_GETFL) & O_NONBLOCK);
    R("fcntl(GETFD pipe2)", K(SYS_fcntl, fds[0], F_GETFD));
    R("pipe2(bad flags)", K(SYS_pipe2, fds, 0x40000000));
    R("pipe(bad ptr)", K(SYS_pipe, 8));
    /* fill it: Linux gives 65536 bytes; only whether progress stops with EAGAIN is compared */
    static char blk[4096];
    memset(blk, 'q', sizeof blk);
    long total = 0, w;
    while ((w = K(SYS_write, fds[1], blk, sizeof blk)) > 0) total += w;
    tr("write(until full)", w, "capacity_ge_4096=%s capacity_ge_65536=%s", yn(total >= 4096), yn(total >= 65536));
    rd("read(one)", fds[0], 3);
    R("close(r)", K(SYS_close, fds[0]));
    long k[2];
    (void)k;
    /* SIGPIPE ignored: EPIPE */
    ignsig(SIGPIPE);
    R("write(no reader)", K(SYS_write, fds[1], "x", 1));
    R("close(w)", K(SYS_close, fds[1]));

    /* data through fork */
    R("pipe", K(SYS_pipe, fds));
    long pid = K(SYS_fork);
    if (pid == 0) {
        K(SYS_close, fds[0]);
        K(SYS_write, fds[1], "from child", 10);
        K(SYS_exit, 7);
    }
    NOTE("fork: child=%s", pidn(pid));
    K(SYS_close, fds[1]);
    rd("read(from child)", fds[0], 100);
    rd("read(eof after child exit)", fds[0], 100);
    int st = 0;
    R("wait4", K(SYS_wait4, pid, &st, 0, 0) == pid ? 0 : -1);
    NOTE("child status exited=%d code=%d", (st & 0x7f) == 0, (st >> 8) & 0xff);
    R("close(r)", K(SYS_close, fds[0]));

    /* FIFO */
    R("mknod(fifo)", K(SYS_mknod, "ff", S_IFIFO | 0644, 0));
    STAT("stat(fifo)", "ff");
    long r = R("open(fifo rdonly nonblock)", K(SYS_open, "ff", O_RDONLY | O_NONBLOCK));
    long wr = R("open(fifo wronly)", K(SYS_open, "ff", O_WRONLY));
    R("write(fifo)", K(SYS_write, wr, "fifo!", 5));
    rd("read(fifo)", r, 10);
    R("close", K(SYS_close, r));
    R("close", K(SYS_close, wr));
    R("open(fifo wronly nonblock no reader)", K(SYS_open, "ff", O_WRONLY | O_NONBLOCK));
    return 0;
}
