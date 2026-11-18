/* Regression: set*uid/set*gid transitions, run in children so that the parent stays root. */
#include "trace.h"

static void child(const char *label, long (*f)(void)) {
    long c = K(SYS_fork);
    if (c == 0) K(SYS_exit, (int)f());
    int st = 0;
    K(SYS_wait4, c, &st, 0, 0);
    NOTE("%s: %d", label, (st >> 8) & 0xff);
}

static long drop_keep_saved(void) {   /* root: setreuid(1000,1000) also sets saved, so root cannot return */
    K(SYS_setreuid, 1000, 1000);
    return K(SYS_setuid, 0) == -EPERM ? 1 : 0;
}
static long resuid_then_back(void) {  /* setresuid(1000,1000,0) keeps root in saved */
    K(SYS_setresuid, 1000, 1000, 0);
    long a = K(SYS_getuid), b = K(SYS_setuid, 0);
    return (a == 1000 ? 1 : 0) | (b == 0 ? 2 : 0) | (K(SYS_geteuid) == 0 ? 4 : 0);
}
static long unprivileged_res(void) {
    K(SYS_setresuid, 1000, 1000, 1000);
    return (K(SYS_setresuid, 1, -1, -1) == -EPERM ? 1 : 0) | (K(SYS_setresuid, -1, 1000, -1) == 0 ? 2 : 0);
}
static long getres(void) {
    K(SYS_setresuid, 5, 6, 7);
    unsigned r = 0, e = 0, s = 0;
    long rc = K(SYS_getresuid, &r, &e, &s);
    return rc == 0 && r == 5 && e == 6 && s == 7 ? 1 : 0;
}
static long gid_by_root(void) {
    K(SYS_setgid, 50);
    unsigned r, e, s;
    K(SYS_getresgid, &r, &e, &s);
    return r == 50 && e == 50 && s == 50 && K(SYS_getgid) == 50 ? 1 : 0;
}

int main(void) {
    init();
    child("setreuid(1000,1000) drops saved id too", drop_keep_saved);
    child("setresuid keeps root in saved id", resuid_then_back);
    child("unprivileged setresuid limits", unprivileged_res);
    child("getresuid reports all three", getres);
    child("setgid as root sets all", gid_by_root);
    R("getresuid(bad ptr)", K(SYS_getresuid, 8, 8, 8));
    return 0;
}
