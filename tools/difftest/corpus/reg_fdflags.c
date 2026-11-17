/* Regression: dup3, umask, chown and the flags fcntl reports. */
#include "trace.h"

int main(void) {
    init();
    scratch("reg_fdflags");
    long fd = K(SYS_open, "f", O_RDWR | O_CREAT | O_EXCL | O_TRUNC, 0644);
    R("fcntl(GETFL hides creation flags)", K(SYS_fcntl, fd, F_GETFL));
    R("dup3(fd,10,0)", K(SYS_dup3, fd, 10, 0));
    R("fcntl(10,GETFD)", K(SYS_fcntl, 10, F_GETFD));
    R("dup3(fd,11,CLOEXEC)", K(SYS_dup3, fd, 11, O_CLOEXEC));
    R("fcntl(11,GETFD)", K(SYS_fcntl, 11, F_GETFD));
    R("dup3(fd,fd)", K(SYS_dup3, fd, fd, 0));
    R("dup3(bad flags)", K(SYS_dup3, fd, 12, 1));
    R("dup3(bad oldfd)", K(SYS_dup3, 77, 12, 0));
    R("dup3(newfd out of range)", K(SYS_dup3, fd, 100000, 0));
    R("dup3(replaces open fd)", K(SYS_dup3, fd, 10, O_CLOEXEC));
    R("fcntl(10,GETFD after replace)", K(SYS_fcntl, 10, F_GETFD));
    R("umask(0)", K(SYS_umask, 0));
    R("umask(0777 returns 0)", K(SYS_umask, 0777));
    R("umask(high bits masked)", K(SYS_umask, 0xfffff022));
    R("umask(readback)", K(SYS_umask, 022));
    long c = K(SYS_fork);
    if (c == 0) K(SYS_exit, (int)K(SYS_umask, 077));
    int st = 0; K(SYS_wait4, c, &st, 0, 0);
    R("child inherits umask", (st >> 8) & 0xff);
    R("umask(parent unchanged by child)", K(SYS_umask, 022));
    R("chown(root:root)", K(SYS_chown, "f", 0, 0));
    R("chown(-1,-1)", K(SYS_chown, "f", -1, -1));
    R("fchown(-1,0)", K(SYS_fchown, fd, -1, 0));
    R("lchown(file)", K(SYS_lchown, "f", 0, -1));
    R("chown(missing)", K(SYS_chown, "nope", 0, 0));
    R("fchown(bad fd)", K(SYS_fchown, 99, 0, 0));
    R("lseek(beyond max file size)", K(SYS_lseek, fd, 0x7fffffffffffffffL, SEEK_SET));
    R("access(bad mode)", K(SYS_access, "f", 99));
    R("access(rw mask ok)", K(SYS_access, "f", 6));
    return 0;
}
