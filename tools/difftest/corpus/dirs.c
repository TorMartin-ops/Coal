/* Directories: mkdir/rmdir, listing, chdir/getcwd, rename of directories, symlinks. */
#include "trace.h"

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void ls(const char *label, const char *path) {
    long fd = K(SYS_open, path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) { tr(label, fd, NULL); return; }
    static char buf[2048], names[64][64];
    char *list[64];
    int n = 0;
    long total, last = 0;
    while ((total = K(SYS_getdents64, fd, buf, sizeof buf)) > 0) {
        last = total;
        for (long o = 0; o < total;) {
            struct dent_ *d = (struct dent_ *)(buf + o);
            o += d->len;
            if (n < 64) { snprintf(names[n], 64, "%s%s", d->name, d->type == 4 ? "/" : d->type == 10 ? "@" : ""); list[n] = names[n]; n++; }
        }
    }
    qsort(list, (size_t)n, sizeof *list, cmp);
    char out[400] = "";
    for (int i = 0; i < n; i++) { strcat(out, list[i]); strcat(out, " "); }
    tr(label, total < 0 ? total : 0, "%s", out);
    (void)last;
    K(SYS_close, fd);
}

int main(void) {
    init();
    scratch("dirs");
    R("mkdir(d)", K(SYS_mkdir, "d", 0755));
    R("mkdir(exists)", K(SYS_mkdir, "d", 0755));
    R("mkdir(noent parent)", K(SYS_mkdir, "x/y", 0755));
    STAT("stat(d)", "d");
    R("mkdir(d/e)", K(SYS_mkdir, "d/e", 0700));
    STAT("stat(d/e mode)", "d/e");
    STAT("stat(d nlink)", "d");
    long fd = R("creat(d/f)", K(SYS_creat, "d/f", 0644));
    K(SYS_close, fd);
    ls("getdents64(d)", "d");
    R("getdents64(on file)", ({ long f = K(SYS_open, "d/f", O_RDONLY); long q = K(SYS_getdents64, f, scratch_, 100); K(SYS_close, f); q; }));
    R("open(file O_DIRECTORY)", K(SYS_open, "d/f", O_RDONLY | O_DIRECTORY));
    R("open(dir O_WRONLY)", K(SYS_open, "d", O_WRONLY));
    R("read(dir)", ({ long f = K(SYS_open, "d", O_RDONLY); long q = K(SYS_read, f, scratch_, 10); K(SYS_close, f); q; }));
    R("rmdir(notempty)", K(SYS_rmdir, "d"));
    R("rmdir(file)", K(SYS_rmdir, "d/f"));
    R("unlink(dir)", K(SYS_unlink, "d"));
    R("rmdir(d/e)", K(SYS_rmdir, "d/e"));
    R("rmdir(missing)", K(SYS_rmdir, "d/e"));
    R("rmdir(dot)", K(SYS_rmdir, "d/."));
    R("rename(d,g)", K(SYS_rename, "d", "g"));
    ls("getdents64(g)", "g");
    R("rename(dir over file)", K(SYS_rename, "g", "g/f"));
    R("rename(dir into itself)", K(SYS_rename, "g", "g/sub"));
    R("mkdir(h)", K(SYS_mkdir, "h", 0755));
    R("rename(dir over empty dir)", K(SYS_rename, "g", "h"));
    ls("getdents64(h)", "h");

    char cwd[100];
    RF("getcwd", K(SYS_getcwd, cwd, sizeof cwd), "%s", cwd);
    R("chdir(h)", K(SYS_chdir, "h"));
    RF("getcwd(in h)", K(SYS_getcwd, cwd, sizeof cwd), "%s", cwd);
    R("getcwd(small)", K(SYS_getcwd, cwd, 3));
    R("chdir(..)", K(SYS_chdir, ".."));
    R("chdir(file)", K(SYS_chdir, "h/f"));
    R("chdir(missing)", K(SYS_chdir, "zzz"));
    R("fchdir", ({ long f = K(SYS_open, "h", O_RDONLY); long q = K(SYS_fchdir, f); K(SYS_close, f); q; }));
    RF("getcwd(after fchdir)", K(SYS_getcwd, cwd, sizeof cwd), "%s", cwd);
    R("chdir(back)", K(SYS_chdir, scratch_));

    R("symlink(h/f)", K(SYS_symlink, "h/f", "s"));
    R("symlink(exists)", K(SYS_symlink, "h/f", "s"));
    LSTAT("lstat(s)", "s");
    STAT("stat(s)", "s");
    char l[64];
    memset(l, 0, sizeof l);
    RF("readlink(s)", K(SYS_readlink, "s", l, sizeof l), "%s", l);
    R("readlink(file)", K(SYS_readlink, "h/f", l, sizeof l));
    R("readlink(missing)", K(SYS_readlink, "zz", l, sizeof l));
    memset(l, 0, sizeof l);
    RF("readlink(short buf)", K(SYS_readlink, "s", l, 3), "%s", l);
    R("symlink(dangling)", K(SYS_symlink, "nowhere", "dang"));
    STAT("stat(dangling)", "dang");
    LSTAT("lstat(dangling)", "dang");
    R("open(dangling)", K(SYS_open, "dang", O_RDONLY));
    R("open(dangling creat)", K(SYS_open, "dang", O_WRONLY | O_CREAT, 0644));
    STAT("stat(nowhere created via link)", "nowhere");
    R("unlink(s)", K(SYS_unlink, "s"));
    STAT("stat(target stays)", "h/f");
    ls("getdents64(.)", ".");
    return 0;
}
