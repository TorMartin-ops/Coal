/* File and directory applets: ls cat cp mv rm mkdir rmdir touch pwd sync */

#include "cb.h"
#include <libc/time.h>
#include <libc/syscall.h>

/* --- ls --- */

static int ls_long, ls_all, ls_one;

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void print_long(const char *name, const struct stat *st) {
    char mode[11] = "----------";
    unsigned m = st->st_mode;
    mode[0] = S_ISDIR(m) ? 'd' : S_ISCHR(m) ? 'c' : S_ISBLK(m) ? 'b' : S_ISLNK(m) ? 'l' : '-';
    for (int i = 0; i < 9; i++)
        if (m & (0400u >> i)) mode[1 + i] = "rwx"[i % 3];
    time_t t = st->st_mtime, now = time(NULL);
    struct tm tm;
    char when[24];
    gmtime_r(&t, &tm);
    strftime(when, sizeof when, (t > now || now - t > 182 * 86400L) ? "%b %e  %Y" : "%b %e %H:%M", &tm);
    char who[2][12];
    snprintf(who[0], sizeof who[0], st->st_uid ? "%u" : "root", st->st_uid);
    snprintf(who[1], sizeof who[1], st->st_gid ? "%u" : "root", st->st_gid);
    printf("%s %3u %-5s %-5s %8llu %s %s\n", mode, st->st_nlink, who[0], who[1],
           (unsigned long long)st->st_size, when, name);
}

/* Prints names in columns on a terminal, one per line otherwise. */
static void print_names(char **v, int n) {
    int w = 0, cols = 1;
    if (!ls_one && isatty(1)) {
        for (int i = 0; i < n; i++) if ((int)strlen(v[i]) > w) w = (int)strlen(v[i]);
        cols = 80 / (w + 2);
        if (cols < 1) cols = 1;
    }
    int rows = (n + cols - 1) / cols;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols && r + c * rows < n; c++) {
            const char *s = v[r + c * rows];
            if (r + (c + 1) * rows < n) printf("%-*s", w + 2, s);
            else printf("%s", s);
        }
        putchar('\n');
    }
}

static int ls_dir(const char *path) {
    int n;
    char **v = cb_list_dir(path, &n);
    if (!v) return 2 * cb_err("cannot open directory", path);
    if (ls_all) {
        v = realloc(v, (size_t)(n + 2) * sizeof *v);
        v[n++] = strdup(".");
        v[n++] = strdup("..");
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (!ls_all && v[i][0] == '.') free(v[i]);
        else v[k++] = v[i];
    }
    qsort(v, (size_t)k, sizeof *v, cmp_names);
    if (!ls_long) {
        print_names(v, k);
    } else {
        for (int i = 0; i < k; i++) {
            char *full = cb_join(path, v[i]);
            struct stat st;
            if (lstat(full, &st) == 0) print_long(v[i], &st);
            else cb_err("cannot access", full);
            free(full);
        }
    }
    cb_free_names(v, k);
    return 0;
}

int ls_main(int argc, char **argv) {
    int c, rc = 0;
    while ((c = getopt(argc, argv, "la1")) != -1) {
        if (c == 'l') ls_long = 1;
        else if (c == 'a') ls_all = 1;
        else if (c == '1') ls_one = 1;
        else return 2;
    }
    int nargs = argc - optind;
    if (!nargs) return ls_dir(".");

    /* files first, then directories, like GNU ls */
    int first = 1;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = optind; i < argc; i++) {
            struct stat st;
            if (lstat(argv[i], &st) < 0) {
                if (!pass) rc = 2 * cb_err("cannot access", argv[i]);
                continue;
            }
            if (S_ISDIR(st.st_mode) != pass) continue;
            if (!pass) {
                if (ls_long) print_long(argv[i], &st);
                else puts(argv[i]);
            } else {
                if (nargs > 1) printf("%s%s:\n", first ? "" : "\n", argv[i]);
                if (ls_dir(argv[i])) rc = 2;
            }
            first = 0;
        }
    }
    return rc;
}

/* --- cat --- */

static char iobuf[4096];

static int cat_fd(int fd, const char *name) {
    ssize_t n;
    while ((n = read(fd, iobuf, sizeof iobuf)) != 0) {
        if (n < 0) {
            if (errno == EINTR) continue;
            return cb_err(NULL, name);
        }
        if (cb_write_all(1, iobuf, (size_t)n) < 0) {
            fprintf(stderr, "cat: write error: %s\n", strerror(errno));
            exit(1);
        }
    }
    return 0;
}

int cat_main(int argc, char **argv) {
    int rc = 0;
    if (argc < 2) return cat_fd(0, "-");
    for (int i = 1; i < argc; i++) {
        int fd = strcmp(argv[i], "-") ? open(argv[i], O_RDONLY) : 0;
        if (fd < 0) { rc = cb_err(NULL, argv[i]); continue; }
        if (cat_fd(fd, argv[i])) rc = 1;
        if (fd) close(fd);
    }
    return rc;
}

/* --- cp / mv --- */

static int cp_recursive;

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int copy_file(const char *src, const char *dst, unsigned mode) {
    int in = open(src, O_RDONLY);
    if (in < 0) return cb_err("cannot open", src);
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
    if (out < 0) {
        close(in);
        return cb_err("cannot create regular file", dst);
    }
    int rc = 0;
    ssize_t n;
    while ((n = read(in, iobuf, sizeof iobuf)) > 0)
        if (cb_write_all(out, iobuf, (size_t)n) < 0) { rc = cb_err("error writing", dst); break; }
    if (n < 0) rc = cb_err("error reading", src);
    close(in);
    close(out);
    return rc;
}

static int copy_any(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) < 0) return cb_err("cannot stat", src);
    if (!S_ISDIR(st.st_mode)) return copy_file(src, dst, st.st_mode);
    if (!cp_recursive) {
        fprintf(stderr, "cp: -r not specified; omitting directory '%s'\n", src);
        return 1;
    }
    if (mkdir(dst, st.st_mode & 0777) < 0 && !(errno == EEXIST && is_dir(dst)))
        return cb_err("cannot create directory", dst);
    int n, rc = 0;
    char **v = cb_list_dir(src, &n);
    if (!v) return cb_err("cannot open directory", src);
    for (int i = 0; i < n; i++) {
        char *s = cb_join(src, v[i]), *d = cb_join(dst, v[i]);
        if (copy_any(s, d)) rc = 1;
        free(s);
        free(d);
    }
    cb_free_names(v, n);
    return rc;
}

static int move_any(const char *src, const char *dst) {
    if (rename(src, dst) == 0) return 0;
    fflush(stdout);
    fprintf(stderr, "mv: cannot move '%s' to '%s': %s\n", src, dst, strerror(errno));
    return 1;
}

/* Shared by cp and mv: SRC... DST, where DST must be a directory for several SRC. */
static int transfer(int argc, char **argv, int (*op)(const char *, const char *)) {
    if (argc - optind < 2) {
        fprintf(stderr, "%s: missing %s operand\n", prog, argc - optind ? "destination file" : "file");
        return 1;
    }
    const char *dst = argv[argc - 1];
    int dir = is_dir(dst), rc = 0;
    if (argc - optind > 2 && !dir) {
        fprintf(stderr, "%s: target '%s' is not a directory\n", prog, dst);
        return 1;
    }
    for (int i = optind; i < argc - 1; i++) {
        char *to = dir ? cb_join(dst, cb_base(argv[i])) : (char *)dst;
        if (op(argv[i], to)) rc = 1;
        if (dir) free(to);
    }
    return rc;
}

int cp_main(int argc, char **argv) {
    int c;
    while ((c = getopt(argc, argv, "rRa")) != -1) {
        if (c == '?') return 1;
        cp_recursive = 1;
    }
    return transfer(argc, argv, copy_any);
}

int mv_main(int argc, char **argv) {
    int c;
    while ((c = getopt(argc, argv, "f")) != -1)
        if (c == '?') return 1;
    return transfer(argc, argv, move_any);
}

/* --- rm / mkdir / rmdir --- */

static int rm_force, rm_recursive;

static int remove_path(const char *p) {
    struct stat st;
    if (lstat(p, &st) < 0) return rm_force && errno == ENOENT ? 0 : cb_err("cannot remove", p);
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(p) == 0 || rm_force) return 0;
        return cb_err("cannot remove", p);
    }
    if (!rm_recursive) {
        errno = EISDIR;
        return cb_err("cannot remove", p);
    }
    int n, rc = 0;
    char **v = cb_list_dir(p, &n);
    if (!v) return rm_force ? 0 : cb_err("cannot remove", p);
    for (int i = 0; i < n; i++) {
        char *child = cb_join(p, v[i]);
        if (remove_path(child)) rc = 1;
        free(child);
    }
    cb_free_names(v, n);
    if (rc || rmdir(p) == 0) return rc;
    return rm_force ? 0 : cb_err("cannot remove", p);
}

int rm_main(int argc, char **argv) {
    int c, rc = 0;
    while ((c = getopt(argc, argv, "rRf")) != -1) {
        if (c == 'f') rm_force = 1;
        else if (c == '?') return 1;
        else rm_recursive = 1;
    }
    if (optind == argc) return rm_force ? 0 : cb_usage("[-rf] FILE...");
    for (int i = optind; i < argc; i++)
        if (remove_path(argv[i])) rc = 1;
    return rc;
}

static int make_parents(char *path) {
    for (char *s = path + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(path, 0755) < 0 && errno != EEXIST) {
            int rc = cb_err("cannot create directory", path);
            *s = '/';
            return rc;
        }
        *s = '/';
    }
    return 0;
}

int mkdir_main(int argc, char **argv) {
    int c, parents = 0, rc = 0;
    while ((c = getopt(argc, argv, "p")) != -1) {
        if (c == '?') return 1;
        parents = 1;
    }
    if (optind == argc) return cb_usage("[-p] DIRECTORY...");
    for (int i = optind; i < argc; i++) {
        if (parents && make_parents(argv[i])) { rc = 1; continue; }
        if (mkdir(argv[i], 0755) < 0 && !(parents && errno == EEXIST && is_dir(argv[i])))
            rc = cb_err("cannot create directory", argv[i]);
    }
    return rc;
}

int rmdir_main(int argc, char **argv) {
    int rc = 0;
    if (argc < 2) return cb_usage("DIRECTORY...");
    for (int i = 1; i < argc; i++)
        if (rmdir(argv[i]) < 0) rc = cb_err("failed to remove", argv[i]);
    return rc;
}

/* --- touch / pwd / sync --- */

int touch_main(int argc, char **argv) {
    int rc = 0;
    if (argc < 2) return cb_usage("FILE...");
    for (int i = 1; i < argc; i++) {
        /* utime(path, NULL) sets the times to now; a missing file is created. */
        if (__syscall_ret(__SC3(SYS_UTIME, argv[i], 0, 0)) == 0 || errno != ENOENT) continue;
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) rc = cb_err("cannot touch", argv[i]);
        else close(fd);
    }
    return rc;
}

int pwd_main(int argc, char **argv) {
    char buf[256];
    if (!getcwd(buf, sizeof buf)) return cb_err(NULL, "getcwd");
    puts(buf);
    return 0;
}

int sync_main(int argc, char **argv) {
    sync();
    return 0;
}
