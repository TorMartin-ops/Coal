/* Descriptor semantics: dup family, fcntl, close-on-exec, shared offsets, lowest-free-fd rule. */
#include "trace.h"

int main(void) {
    init();
    scratch("fd");
    long a = R("open(a)", K(SYS_open, "a", O_RDWR | O_CREAT, 0644));
    R("write(a)", K(SYS_write, a, "0123456789", 10));
    R("lseek(a,0)", K(SYS_lseek, a, 0, SEEK_SET));
    long b = R("dup", K(SYS_dup, a));
    rd("read(a,3)", a, 3);
    R("lseek(b cur shared)", K(SYS_lseek, b, 0, SEEK_CUR));
    long c = R("dup2(a,10)", K(SYS_dup2, a, 10));
    R("dup2(a,a)", K(SYS_dup2, a, a));
    R("dup2(bad,10)", K(SYS_dup2, 50, 11));
    R("dup2(a,-1)", K(SYS_dup2, a, -1));
    R("dup2(a,huge)", K(SYS_dup2, a, 100000));
    R("dup3(a,12,0)", K(SYS_dup3, a, 12, 0));
    R("dup3(a,a)", K(SYS_dup3, a, a, 0));
    R("dup3(a,13,badflag)", K(SYS_dup3, a, 13, 1));
    R("dup3(a,14,CLOEXEC)", K(SYS_dup3, a, 14, O_CLOEXEC));
    R("fcntl(14,GETFD)", K(SYS_fcntl, 14, F_GETFD));
    R("fcntl(12,GETFD)", K(SYS_fcntl, 12, F_GETFD));
    R("fcntl(12,SETFD)", K(SYS_fcntl, 12, F_SETFD, FD_CLOEXEC));
    R("fcntl(12,GETFD after)", K(SYS_fcntl, 12, F_GETFD));
    R("dup(12) clears cloexec", ({ long d = K(SYS_dup, 12); long q = K(SYS_fcntl, d, F_GETFD); K(SYS_close, d); q; }));
    R("fcntl(DUPFD,20)", K(SYS_fcntl, a, F_DUPFD, 20));
    R("fcntl(DUPFD_CLOEXEC,30)", K(SYS_fcntl, a, F_DUPFD_CLOEXEC, 30));
    R("fcntl(30,GETFD)", K(SYS_fcntl, 30, F_GETFD));
    R("fcntl(DUPFD,bad min)", K(SYS_fcntl, a, F_DUPFD, -1));
    R("fcntl(bad fd)", K(SYS_fcntl, 77, F_GETFD));
    R("fcntl(bad cmd)", K(SYS_fcntl, a, 9999, 0));
    R("fcntl(GETFL)", K(SYS_fcntl, a, F_GETFL));
    R("fcntl(SETFL NONBLOCK)", K(SYS_fcntl, a, F_SETFL, O_NONBLOCK));
    R("fcntl(GETFL after)", K(SYS_fcntl, a, F_GETFL));
    R("fcntl(SETFL APPEND)", K(SYS_fcntl, a, F_SETFL, O_APPEND | O_NONBLOCK));
    R("fcntl(GETFL append)", K(SYS_fcntl, a, F_GETFL));
    R("fcntl(SETFL cannot change access)", K(SYS_fcntl, a, F_SETFL, O_RDONLY));
    R("fcntl(GETFL access kept)", K(SYS_fcntl, a, F_GETFL) & O_ACCMODE);
    R("fcntl(b GETFL shared flags)", K(SYS_fcntl, b, F_GETFL));

    R("close(a)", K(SYS_close, a));
    rd("read(b after close a)", b, 3);
    long n1 = R("open(lowest free)", K(SYS_open, "a", O_RDONLY));
    long n2 = R("open(next)", K(SYS_open, "a", O_RDONLY));
    R("close(n1)", K(SYS_close, n1));
    R("open(reuses hole)", K(SYS_open, "a", O_RDONLY | O_CLOEXEC));
    R("fcntl(O_CLOEXEC GETFD)", K(SYS_fcntl, n1, F_GETFD));
    (void)n2; (void)c;
    /* closing stdin leaves 0 free */
    long s0 = K(SYS_dup, 0);
    R("close(0)", K(SYS_close, 0));
    R("open(takes 0)", K(SYS_open, "a", O_RDONLY));
    R("dup2(s0,0 restore)", K(SYS_dup2, s0, 0));
    R("read(bad ptr)", K(SYS_read, b, 8, 4));
    R("read(negative fd)", K(SYS_read, -1, scratch_, 4));
    R("write(negative count)", K(SYS_write, b, scratch_, -1));
    R("lseek(bad fd)", K(SYS_lseek, 99, 0, SEEK_SET));
    R("close(-1)", K(SYS_close, -1));
    R("close(huge)", K(SYS_close, 1 << 20));
    return 0;
}
