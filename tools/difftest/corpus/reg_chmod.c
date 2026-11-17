/* Regression: chmod and fchmod. Only the owner write bit is compared: FAT stores no other permission. */
#include "trace.h"

static void ownerw(const char *label, const char *path) {
    struct kstat_ s;
    long r = K(SYS_stat, path, &s);
    tr(label, r < 0 ? r : 0, r < 0 ? NULL : "owner write bit %s", yn(s.mode & 0200));
}

int main(void) {
    init();
    scratch("reg_chmod");
    K(SYS_close, K(SYS_creat, "f", 0644));
    ownerw("stat(new file)", "f");
    R("chmod(0400)", K(SYS_chmod, "f", 0400));
    ownerw("stat(after 0400)", "f");
    R("chmod(0644)", K(SYS_chmod, "f", 0644));
    ownerw("stat(after 0644)", "f");
    long fd = K(SYS_open, "f", O_RDONLY);
    R("fchmod(0444)", K(SYS_fchmod, fd, 0444));
    ownerw("stat(after fchmod 0444)", "f");
    R("fchmod(0600)", K(SYS_fchmod, fd, 0600));
    ownerw("stat(after fchmod 0600)", "f");
    R("close", K(SYS_close, fd));
    R("chmod(dir 0777)", K(SYS_chmod, ".", 0777));
    R("chmod(missing)", K(SYS_chmod, "nope", 0644));
    R("chmod(bad ptr)", K(SYS_chmod, 8, 0644));
    R("chmod(no dir component)", K(SYS_chmod, "f/x", 0644));
    R("fchmod(bad fd)", K(SYS_fchmod, 99, 0644));
    return 0;
}
