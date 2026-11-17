/* Script interpreter for generated file/descriptor sequences (see fuzz.py). Not part of the fixed corpus:
 * difftest.py skips it (prefix "fsops" is listed in SKIP) and fuzz.py drives it with a script file.
 *     fsops SCRIPT WORKDIR
 * Script lines: open S PATH FLAGS | close S | write S N CH | read S N | pread S N OFF | pwrite S N OFF CH |
 * lseek S OFF WHENCE | dup S T | dup2 S T | ftruncate S N | truncate PATH N | unlink PATH | mkdir PATH |
 * rmdir PATH | rename A B | link A B | symlink TARGET PATH | stat PATH | fstat S | readdir PATH |
 * fsync S | fcntl S CMD [ARG] | getfl S | access PATH. S and T are slots 0..7. */
#include "trace.h"

static long slot[8];
static char name[64];

static int flags_of(const char *f) {
    int fl = strchr(f, 'w') ? (strchr(f, 'r') ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (strchr(f, 'c')) fl |= O_CREAT;
    if (strchr(f, 't')) fl |= O_TRUNC;
    if (strchr(f, 'a')) fl |= O_APPEND;
    if (strchr(f, 'x')) fl |= O_EXCL;
    if (strchr(f, 'd')) fl |= O_DIRECTORY;
    if (strchr(f, 'n')) fl |= O_NONBLOCK;
    return fl;
}
static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static long fd(int s) { return s >= 0 && s < 8 ? slot[s] : -1; }
static void show_data(const char *label, long r, const unsigned char *b) {
    char h[80] = "";
    for (long i = 0; i < r && i < 12; i++) { char t[4]; snprintf(t, sizeof t, "%02x", b[i]); strcat(h, t); }
    tr(label, r, r > 0 ? "%s" : NULL, h);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    init();
    for (int i = 0; i < 8; i++) slot[i] = -1;
    char dir[100];
    snprintf(dir, sizeof dir, "%s", argv[2]);
    static char rm[130];
    snprintf(rm, sizeof rm, "%s", dir);
    scratch(dir + (strncmp(dir, "/tmp/dt_", 8) == 0 ? 8 : 0));
    FILE *f = fopen(argv[1], "r");
    if (!f) return 3;
    char line[200];
    static unsigned char buf[8192];
    while (fgets(line, sizeof line, f)) {
        char op[16] = "", a[64] = "", b[64] = "", c[64] = "";
        long n1 = 0, n2 = 0, n3 = 0;
        line[strcspn(line, "\n")] = 0;
        sscanf(line, "%15s %63s %63s %63s", op, a, b, c);
        n1 = atol(a); n2 = atol(b); n3 = atol(c);
        snprintf(name, sizeof name, "%s", line);
        if (!strcmp(op, "open")) { long r = K(SYS_open, b, flags_of(c), 0644); if (r >= 0) { if (fd((int)n1) >= 0) K(SYS_close, fd((int)n1)); slot[n1 & 7] = r; r = r; } tr(name, r, NULL); }
        else if (!strcmp(op, "close")) { long r = K(SYS_close, fd((int)n1)); if (r == 0) slot[n1 & 7] = -1; tr(name, r, NULL); }
        else if (!strcmp(op, "write")) { memset(buf, c[0], (size_t)n2); tr(name, K(SYS_write, fd((int)n1), buf, n2), NULL); }
        else if (!strcmp(op, "read")) { memset(buf, 0, 16); long r = K(SYS_read, fd((int)n1), buf, n2); show_data(name, r, buf); }
        else if (!strcmp(op, "pread")) { memset(buf, 0, 16); long r = K(SYS_pread64, fd((int)n1), buf, n2, n3); show_data(name, r, buf); }
        else if (!strcmp(op, "pwrite")) { memset(buf, atol(c) ? 'p' : 'q', (size_t)n2); tr(name, K(SYS_pwrite64, fd((int)n1), buf, n2, n3), NULL); }
        else if (!strcmp(op, "lseek")) tr(name, K(SYS_lseek, fd((int)n1), n2, n3), NULL);
        else if (!strcmp(op, "dup")) { long r = K(SYS_dup, fd((int)n1)); if (r >= 0) { if (fd((int)n2) >= 0) K(SYS_close, fd((int)n2)); slot[n2 & 7] = r; } tr(name, r, NULL); }
        else if (!strcmp(op, "dup2")) { long r = K(SYS_dup2, fd((int)n1), 20 + (n2 & 7)); if (r >= 0) { if (fd((int)n2) >= 0) K(SYS_close, fd((int)n2)); slot[n2 & 7] = r; } tr(name, r, NULL); }
        else if (!strcmp(op, "ftruncate")) tr(name, K(SYS_ftruncate, fd((int)n1), n2), NULL);
        else if (!strcmp(op, "truncate")) tr(name, K(SYS_truncate, a, n2), NULL);
        else if (!strcmp(op, "unlink")) tr(name, K(SYS_unlink, a), NULL);
        else if (!strcmp(op, "mkdir")) tr(name, K(SYS_mkdir, a, 0755), NULL);
        else if (!strcmp(op, "rmdir")) tr(name, K(SYS_rmdir, a), NULL);
        else if (!strcmp(op, "rename")) tr(name, K(SYS_rename, a, b), NULL);
        else if (!strcmp(op, "link")) tr(name, K(SYS_link, a, b), NULL);
        else if (!strcmp(op, "symlink")) tr(name, K(SYS_symlink, a, b), NULL);
        else if (!strcmp(op, "access")) tr(name, K(SYS_access, a, F_OK), NULL);
        else if (!strcmp(op, "fsync")) tr(name, K(SYS_fsync, fd((int)n1)), NULL);
        else if (!strcmp(op, "getfl")) tr(name, K(SYS_fcntl, fd((int)n1), F_GETFL) & (O_ACCMODE | O_APPEND), NULL);
        else if (!strcmp(op, "fcntl")) tr(name, K(SYS_fcntl, fd((int)n1), n2, n3), NULL);
        else if (!strcmp(op, "stat")) { struct kstat_ s; memset(&s, 0, sizeof s); long r = K(SYS_stat, a, &s); if (r < 0) tr(name, r, NULL); else tr(name, r, "type=%s size=%ld nlink=%lu", ftype(s.mode), ((s.mode & 0170000) == 040000) ? 0L : (long)s.size, ((s.mode & 0170000) == 040000) ? 0UL : (unsigned long)s.nlink); }
        else if (!strcmp(op, "fstat")) { struct kstat_ s; memset(&s, 0, sizeof s); long r = K(SYS_fstat, fd((int)n1), &s); if (r < 0) tr(name, r, NULL); else tr(name, r, "type=%s size=%ld nlink=%lu", ftype(s.mode), ((s.mode & 0170000) == 040000) ? 0L : (long)s.size, ((s.mode & 0170000) == 040000) ? 0UL : (unsigned long)s.nlink); }
        else if (!strcmp(op, "readdir")) {
            long d = K(SYS_open, a, O_RDONLY | O_DIRECTORY);
            if (d < 0) { tr(name, d, NULL); continue; }
            static char db[2048], nm[32][64];
            char *lst[32]; int n = 0; long t;
            while ((t = K(SYS_getdents64, d, db, sizeof db)) > 0)
                for (long o = 0; o < t;) { struct dent_ *e = (struct dent_ *)(db + o); o += e->len; if (strcmp(e->name, ".") && strcmp(e->name, "..") && n < 32) { snprintf(nm[n], 64, "%s", e->name); lst[n] = nm[n]; n++; } }
            qsort(lst, (size_t)n, sizeof *lst, cmp);
            char out[300] = "";
            for (int i = 0; i < n; i++) { strcat(out, lst[i]); strcat(out, " "); }
            tr(name, t < 0 ? t : 0, "%s", out);
            K(SYS_close, d);
        } else NOTE("%s: unknown op", name);
    }
    return 0;
}
