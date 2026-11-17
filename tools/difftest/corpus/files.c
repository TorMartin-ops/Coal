/* Regular files: open flags, read/write/seek, truncate, stat, unlink, rename, hard links. */
#include "trace.h"

int main(void) {
    init();
    scratch("files");
    long fd = R("open(creat)", K(SYS_open, "a", O_RDWR | O_CREAT | O_EXCL, 0644));
    R("write(5)", K(SYS_write, fd, "hello", 5));
    R("lseek(cur0)", K(SYS_lseek, fd, 0, SEEK_CUR));
    R("lseek(set0)", K(SYS_lseek, fd, 0, SEEK_SET));
    rd("read(all)", fd, 100);
    rd("read(eof)", fd, 100);
    R("lseek(end)", K(SYS_lseek, fd, 0, SEEK_END));
    R("lseek(end-2)", K(SYS_lseek, fd, -2, SEEK_END));
    rd("read(tail)", fd, 10);
    R("lseek(neg)", K(SYS_lseek, fd, -100, SEEK_SET));
    R("lseek(badwhence)", K(SYS_lseek, fd, 0, 9));
    R("lseek(hole)", K(SYS_lseek, fd, 100, SEEK_SET));
    R("write(after hole)", K(SYS_write, fd, "Z", 1));
    FSTAT("fstat(sparse)", fd);
    R("lseek(set90)", K(SYS_lseek, fd, 90, SEEK_SET));
    rd("read(zeros)", fd, 12);
    R("pwrite64(at3)", K(SYS_pwrite64, fd, "XY", 2, 3));
    R("lseek(cur after pwrite)", K(SYS_lseek, fd, 0, SEEK_CUR));
    { char b[8] = {0}; RF("pread64(at0)", K(SYS_pread64, fd, b, 6, 0), "\"%.6s\"", b); }
    R("pread64(neg off)", K(SYS_pread64, fd, (long)scratch_, 1, -1));
    R("ftruncate(3)", K(SYS_ftruncate, fd, 3));
    FSTAT("fstat(truncated)", fd);
    R("ftruncate(grow 10)", K(SYS_ftruncate, fd, 10));
    FSTAT("fstat(grown)", fd);
    R("ftruncate(neg)", K(SYS_ftruncate, fd, -1));
    R("fsync", K(SYS_fsync, fd));
    R("fdatasync", K(SYS_fdatasync, fd));
    R("close", K(SYS_close, fd));
    R("close(again)", K(SYS_close, fd));
    R("write(closed)", K(SYS_write, fd, "x", 1));

    R("open(excl exists)", K(SYS_open, "a", O_RDWR | O_CREAT | O_EXCL, 0644));
    fd = R("open(trunc)", K(SYS_open, "a", O_WRONLY | O_TRUNC));
    STAT("stat(after trunc)", "a");
    rd("read(wronly)", fd, 4);
    R("write(wronly)", K(SYS_write, fd, "abc", 3));
    R("close", K(SYS_close, fd));
    fd = R("open(rdonly)", K(SYS_open, "a", O_RDONLY));
    R("write(rdonly)", K(SYS_write, fd, "x", 1));
    R("ftruncate(rdonly)", K(SYS_ftruncate, fd, 0));
    R("close", K(SYS_close, fd));
    fd = R("open(append)", K(SYS_open, "a", O_WRONLY | O_APPEND));
    R("lseek(set0)", K(SYS_lseek, fd, 0, SEEK_SET));
    R("write(append)", K(SYS_write, fd, "DEF", 3));
    R("lseek(cur)", K(SYS_lseek, fd, 0, SEEK_CUR));
    R("close", K(SYS_close, fd));
    fd = R("open(rdonly)", K(SYS_open, "a", O_RDONLY));
    rd("read(content)", fd, 100);
    R("close", K(SYS_close, fd));

    R("truncate(2)", K(SYS_truncate, "a", 2));
    STAT("stat(truncate)", "a");
    R("truncate(missing)", K(SYS_truncate, "nope", 2));
    R("open(noent)", K(SYS_open, "nope", O_RDONLY));
    R("open(creat noent dir)", K(SYS_open, "nodir/x", O_CREAT | O_WRONLY, 0644));

    R("rename(a,b)", K(SYS_rename, "a", "b"));
    STAT("stat(a)", "a");
    STAT("stat(b)", "b");
    R("link(b,c)", K(SYS_link, "b", "c"));
    STAT("stat(c nlink)", "c");
    R("link(exists)", K(SYS_link, "b", "c"));
    R("unlink(b)", K(SYS_unlink, "b"));
    STAT("stat(c after unlink b)", "c");
    R("rename(over)", K(SYS_rename, "c", "c"));
    fd = R("creat(d)", K(SYS_creat, "d", 0600));
    R("close", K(SYS_close, fd));
    R("rename(c over d)", K(SYS_rename, "c", "d"));
    STAT("stat(d)", "d");
    R("unlink(missing)", K(SYS_unlink, "c"));
    R("chmod(d 0400)", K(SYS_chmod, "d", 0400));
    STAT("stat(d chmod)", "d");
    R("fchmod", ({ long f = K(SYS_open, "d", O_RDONLY); long q = K(SYS_fchmod, f, 0640); K(SYS_close, f); q; }));
    STAT("stat(d fchmod)", "d");
    R("access(R_OK)", K(SYS_access, "d", R_OK));
    R("access(F_OK missing)", K(SYS_access, "zz", F_OK));
    R("umask(077)", K(SYS_umask, 077));
    fd = R("creat(umask)", K(SYS_creat, "e", 0666));
    R("close", K(SYS_close, fd));
    STAT("stat(e umask)", "e");
    R("umask(restore)", K(SYS_umask, 022));

    /* unlink of an open file keeps it readable */
    fd = R("open(u)", K(SYS_open, "u", O_RDWR | O_CREAT, 0644));
    R("write(u)", K(SYS_write, fd, "keep", 4));
    R("unlink(u open)", K(SYS_unlink, "u"));
    FSTAT("fstat(unlinked)", fd);
    R("lseek(set0)", K(SYS_lseek, fd, 0, SEEK_SET));
    rd("read(unlinked)", fd, 10);
    R("close", K(SYS_close, fd));

    /* scatter/gather */
    fd = R("open(v)", K(SYS_open, "v", O_RDWR | O_CREAT, 0644));
    struct iovec { void *b; size_t l; } w[3] = {{"ab", 2}, {"", 0}, {"cde", 3}};
    R("writev(3 vecs)", K(SYS_writev, fd, w, 3));
    R("lseek(set0)", K(SYS_lseek, fd, 0, SEEK_SET));
    char p[3], q[8] = {0};
    struct iovec rv[2] = {{p, 3}, {q, 8}};
    RF("readv", K(SYS_readv, fd, rv, 2), "\"%.3s\" \"%.2s\"", p, q);
    R("writev(iovcnt -1)", K(SYS_writev, fd, w, -1));
    R("read(count 0)", K(SYS_read, fd, p, 0));
    R("close", K(SYS_close, fd));
    return 0;
}
