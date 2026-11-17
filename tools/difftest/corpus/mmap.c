/* Memory mappings: anonymous mmap, mprotect, munmap, fixed placement, faults, brk, mremap, madvise. */
#include "trace.h"
#include <setjmp.h>
#include <sys/wait.h>

#define PG 4096
static const char *ad(long r) { return r >= -4095 && r < 0 ? errname(-r) : (r & (PG - 1)) ? "UNALIGNED" : "ADDR"; }
#define M(label, expr) ({ long r_ = (expr); NOTE("%s: %s", label, ad(r_)); r_; })

/* Does touching an address kill a child with SIGSEGV (1), or succeed (0)? */
static int faults(volatile char *p, int write) {
    long c = K(SYS_fork);
    if (c == 0) { if (write) *p = 1; else (void)*p; K(SYS_exit, 0); }
    int st = 0;
    K(SYS_wait4, c, &st, 0, 0);
    return WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV;
}

int main(void) {
    init();
    scratch("mmap");
    long a = M("mmap(anon rw 3 pages)", K(SYS_mmap, 0, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    volatile char *p = (char *)a;
    NOTE("fresh anonymous memory is zero: %s", yn(p[0] == 0 && p[3 * PG - 1] == 0));
    p[0] = 'a'; p[3 * PG - 1] = 'z';
    NOTE("write/read back: %c%c", p[0], p[3 * PG - 1]);
    R("mprotect(page1 none)", K(SYS_mprotect, a + PG, PG, PROT_NONE));
    NOTE("read of PROT_NONE page faults: %s", yn(faults(p + PG, 0)));
    NOTE("write of rw page 0 faults: %s", yn(faults(p, 1)));
    R("mprotect(page0 read-only)", K(SYS_mprotect, a, PG, PROT_READ));
    NOTE("write to read-only page faults: %s", yn(faults(p, 1)));
    NOTE("read of read-only page faults: %s", yn(faults(p, 0)));
    R("mprotect(unaligned)", K(SYS_mprotect, a + 1, PG, PROT_READ));
    R("mprotect(unmapped)", K(SYS_mprotect, 0x10000, PG, PROT_READ));
    R("mprotect(len 0)", K(SYS_mprotect, a, 0, PROT_READ));
    R("munmap(middle page)", K(SYS_munmap, a + PG, PG));
    NOTE("read of unmapped page faults: %s", yn(faults(p + PG, 0)));
    NOTE("neighbour page still readable: %s", yn(!faults(p + 2 * PG, 0)));
    R("munmap(unaligned)", K(SYS_munmap, a + 1, PG));
    R("munmap(len 0)", K(SYS_munmap, a, 0));
    R("munmap(not mapped is ok)", K(SYS_munmap, 0x20000000, PG));
    R("munmap(all)", K(SYS_munmap, a, 3 * PG));

    M("mmap(len 0)", K(SYS_mmap, 0, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    M("mmap(no PRIVATE/SHARED)", K(SYS_mmap, 0, PG, PROT_READ, MAP_ANONYMOUS, -1, 0));
    M("mmap(huge)", K(SYS_mmap, 0, 1UL << 62, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    M("mmap(bad fd)", K(SYS_mmap, 0, PG, PROT_READ, MAP_PRIVATE, 99, 0));
    M("mmap(unaligned offset)", K(SYS_mmap, 0, PG, PROT_READ, MAP_PRIVATE, 0, 1));
    long f = K(SYS_open, "m", O_RDWR | O_CREAT, 0644);
    K(SYS_write, f, "file-mapped-data", 16);
    long fm = M("mmap(file private read)", K(SYS_mmap, 0, PG, PROT_READ, MAP_PRIVATE, f, 0));
    if (fm > 0) NOTE("file content visible: %.16s", (char *)fm);
    long sm = M("mmap(file shared rw)", K(SYS_mmap, 0, PG, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0));
    if (sm > 0) { ((char *)sm)[0] = 'F'; char b[4]; K(SYS_pread64, f, b, 1, 0); NOTE("shared write reaches the file: %s", yn(b[0] == 'F')); }
    M("mmap(file wronly fd)", ({ long w = K(SYS_open, "m", O_WRONLY); long q = K(SYS_mmap, 0, PG, PROT_READ, MAP_PRIVATE, w, 0); K(SYS_close, w); q; }));
    K(SYS_close, f);

    long fx = M("mmap(FIXED)", K(SYS_mmap, 0x30000000, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0));
    NOTE("FIXED placed exactly: %s", yn(fx == 0x30000000));
    M("mmap(FIXED unaligned)", K(SYS_mmap, 0x30000001, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0));
    M("mmap(FIXED_NOREPLACE existing)", K(SYS_mmap, 0x30000000, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | 0x100000, -1, 0));
    ((char *)fx)[5] = 'k';
    M("mmap(FIXED replaces)", K(SYS_mmap, 0x30000000, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0));
    NOTE("replaced page is zero again: %s", yn(((char *)fx)[5] == 0));
    long hint = M("mmap(hint honoured)", K(SYS_mmap, 0x31000000, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    NOTE("hint used: %s", yn(hint == 0x31000000));

    /* copy on write across fork */
    long cw = K(SYS_mmap, 0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    *(volatile char *)cw = 'p';
    long c = K(SYS_fork);
    if (c == 0) { *(volatile char *)cw = 'c'; K(SYS_exit, 0); }
    int st; K(SYS_wait4, c, &st, 0, 0);
    NOTE("private mapping after fork child write: parent sees %c", *(volatile char *)cw);
    long sh = M("mmap(anon shared)", K(SYS_mmap, 0, PG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (sh > 0) {
        *(volatile char *)sh = 'p';
        c = K(SYS_fork);
        if (c == 0) { *(volatile char *)sh = 'c'; K(SYS_exit, 0); }
        K(SYS_wait4, c, &st, 0, 0);
        NOTE("shared anonymous after fork child write: parent sees %c", *(volatile char *)sh);
    }

    /* brk */
    long b0 = K(SYS_brk, 0);
    NOTE("brk(0) is a plausible address: %s", yn(b0 > 0x1000));
    long b1 = K(SYS_brk, b0 + 3 * PG);
    NOTE("brk grows: %s", yn(b1 >= b0 + 3 * PG));
    ((volatile char *)b0)[100] = 1;
    NOTE("brk memory usable and zero start: %s", yn(((volatile char *)b0)[0] == 0));
    long b2 = K(SYS_brk, b0);
    NOTE("brk shrinks back: %s", yn(b2 == b0));
    NOTE("brk(too high) leaves break unchanged: %s", yn(K(SYS_brk, 1UL << 62) == b0));

    long ra = K(SYS_mmap, 0, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ((char *)ra)[0] = 'r';
    long rb = M("mremap(grow, MAYMOVE)", K(SYS_mremap, ra, 2 * PG, 8 * PG, 1));
    if (rb > 0) NOTE("mremap kept contents: %s", yn(((char *)rb)[0] == 'r'));
    long rc = M("mremap(shrink)", K(SYS_mremap, rb > 0 ? rb : ra, 8 * PG, PG, 1));
    if (rc > 0) ra = rc;
    M("mremap(unmapped)", K(SYS_mremap, 0x50000000, PG, 2 * PG, 1));
    R("madvise(DONTNEED)", K(SYS_madvise, ra, PG, MADV_DONTNEED));
    R("madvise(bad advice)", K(SYS_madvise, ra, PG, 9999));
    R("madvise(unaligned)", K(SYS_madvise, ra + 1, PG, MADV_NORMAL));
    R("msync(anon)", K(SYS_msync, ra, PG, 4));
    R("mlock", K(SYS_mlock, ra, PG));
    R("munlock", K(SYS_munlock, ra, PG));
    return 0;
}
