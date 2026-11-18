/* Regression: creat, truncate and ftruncate (shrink, grow with zeros, error cases). */
#include "trace.h"

int main(void) {
    init();
    scratch("reg_truncate");
    long fd = R("creat", K(SYS_creat, "t", 0644));
    R("write", K(SYS_write, fd, "0123456789", 10));
    R("ftruncate(4)", K(SYS_ftruncate, fd, 4));
    FSTAT("fstat", fd);
    R("ftruncate(grow 8192)", K(SYS_ftruncate, fd, 8192));
    FSTAT("fstat(grown)", fd);
    R("pread(old data kept)", ({ char b[4]; K(SYS_pread64, fd, b, 4, 0); memcmp(b, "0123", 4) == 0 ? 0 : -1; }));
    R("pread(grown part is zero)", ({ char b[8]; memset(b, 1, 8); K(SYS_pread64, fd, b, 8, 5000); b[0] | b[7]; }));
    R("ftruncate(-1)", K(SYS_ftruncate, fd, -1));
    R("close", K(SYS_close, fd));
    R("truncate(2)", K(SYS_truncate, "t", 2));
    STAT("stat", "t");
    R("truncate(0)", K(SYS_truncate, "t", 0));
    R("truncate(missing)", K(SYS_truncate, "nope", 0));
    R("truncate(dir)", K(SYS_truncate, ".", 0));
    R("truncate(bad ptr)", K(SYS_truncate, 8, 0));
    R("truncate(no dir component)", K(SYS_truncate, "t/x", 0));
    fd = K(SYS_open, "t", O_RDONLY);
    R("ftruncate(read-only fd)", K(SYS_ftruncate, fd, 0));
    R("close", K(SYS_close, fd));
    R("ftruncate(bad fd)", K(SYS_ftruncate, 99, 0));
    R("ftruncate(bad fd, negative length)", K(SYS_ftruncate, 99, -1));
    fd = R("creat(existing truncates)", K(SYS_creat, "t", 0644));
    R("write", K(SYS_write, fd, "abc", 3));
    R("close", K(SYS_close, fd));
    R("creat(existing)", ({ long q = K(SYS_creat, "t", 0644); if (q >= 0) K(SYS_close, q); q >= 0 ? 0 : q; }));
    STAT("stat(truncated by creat)", "t");
    return 0;
}
