/* Regression: MAP_SHARED|MAP_ANONYMOUS pages stay shared across fork; untouched pages too. */
#include "trace.h"

#define PG 4096

int main(void) {
    init();
    long sh = K(SYS_mmap, 0, 2 * PG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    long pv = K(SYS_mmap, 0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    NOTE("mappings ok: %s", yn(sh > 0 && pv > 0));
    volatile char *s = (volatile char *)sh, *p = (volatile char *)pv;
    s[0] = 'p';
    p[0] = 'p';
    int st;
    long c = K(SYS_fork);
    if (c == 0) { s[0] = 'c'; s[PG] = 'k'; p[0] = 'c'; K(SYS_exit, 0); }
    K(SYS_wait4, c, &st, 0, 0);
    NOTE("touched shared page: parent sees %c", s[0]);
    NOTE("untouched shared page: parent sees %c", s[PG]);
    NOTE("private page: parent sees %c", p[0]);
    return 0;
}
