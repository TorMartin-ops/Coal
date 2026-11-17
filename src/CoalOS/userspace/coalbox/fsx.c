/* More file applets: ln chmod stat find du df */

#include "cb.h"
#include <libc/time.h>
#include <libc/sys/statfs.h>

/* Prints why an operation is missing on this kernel/filesystem. */
static int unsupported(const char *what, const char *path) {
    if (errno != ENOSYS && errno != EPERM && errno != EOPNOTSUPP) return cb_err(what, path);
    fprintf(stderr, "%s: %s: not supported by this kernel or filesystem\n", prog, what);
    return 1;
}

int ln_main(int argc, char **argv) {
    int c, sym = 0, force = 0;
    while ((c = getopt(argc, argv, "sf")) != -1) {
        if (c == '?') return 1;
        if (c == 's') sym = 1; else force = 1;
    }
    if (argc - optind != 2) return cb_usage("[-sf] TARGET LINK");
    const char *to = argv[optind + 1];
    if (force) unlink(to);
    if ((sym ? symlink(argv[optind], to) : link(argv[optind], to)) < 0)
        return unsupported(sym ? "symbolic links" : "hard links", to);
    return 0;
}

/* FAT has no permission bits: a refused chmod is reported but is not an error. */
int chmod_main(int argc, char **argv) {
    if (argc < 3) return cb_usage("OCTAL-MODE FILE...");
    char *end;
    long mode = strtol(argv[1], &end, 8);
    if (*end || end == argv[1] || mode < 0 || mode > 07777) {
        fprintf(stderr, "chmod: invalid mode '%s' (only octal modes are supported)\n", argv[1]);
        return 1;
    }
    int rc = 0, noted = 0;
    for (int i = 2; i < argc; i++) {
        if (chmod(argv[i], (mode_t)mode) == 0) continue;
        if (errno == ENOSYS || errno == EPERM || errno == EOPNOTSUPP) {
            if (!noted++) fprintf(stderr, "chmod: note: permissions are not stored on this filesystem, nothing changed\n");
        } else rc = cb_err("cannot change mode of", argv[i]);
    }
    return rc;
}

int stat_main(int argc, char **argv) {
    int rc = 0;
    if (argc < 2) return cb_usage("FILE...");
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (lstat(argv[i], &st) < 0) { rc = cb_err("cannot stat", argv[i]); continue; }
        const char *type = S_ISDIR(st.st_mode) ? "directory" : S_ISCHR(st.st_mode) ? "character device"
                         : S_ISBLK(st.st_mode) ? "block device" : S_ISLNK(st.st_mode) ? "symbolic link"
                         : S_ISREG(st.st_mode) ? "regular file" : "other";
        struct tm tm;
        char when[32];
        time_t t = st.st_mtime;
        gmtime_r(&t, &tm);
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
        printf("  File: %s\n  Size: %llu\tBlocks: %llu\t%s\nMode: %04o\tLinks: %lu\tUid: %u\tGid: %u\nModify: %s UTC\n",
               argv[i], (unsigned long long)st.st_size, (unsigned long long)st.st_blocks, type,
               st.st_mode & 07777, st.st_nlink, st.st_uid, st.st_gid, when);
    }
    return rc;
}

/* --- find --- */

static const char *f_name;
static char f_type;
static int f_depth = 1 << 30;

static int f_match(const char *path, const struct stat *st) {
    if (f_name && !pattern_match(f_name, cb_base(path))) return 0;
    if (!f_type) return 1;
    return f_type == 'd' ? S_ISDIR(st->st_mode) : f_type == 'f' ? S_ISREG(st->st_mode) : S_ISLNK(st->st_mode);
}

static int f_walk(const char *path, int depth) {
    struct stat st;
    if (lstat(path, &st) < 0) return cb_err(NULL, path);
    if (f_match(path, &st)) puts(path);
    if (!S_ISDIR(st.st_mode) || depth >= f_depth) return 0;
    int n, rc = 0;
    char **v = cb_list_dir(path, &n);
    if (!v) return cb_err(NULL, path);
    for (int i = 0; i < n; i++) {
        char *child = cb_join(path, v[i]);
        if (child) rc |= f_walk(child, depth + 1);
        free(child);
    }
    cb_free_names(v, n);
    return rc;
}

int find_main(int argc, char **argv) {
    int i = 1, rc = 0, npaths = 0;
    while (i < argc && argv[i][0] != '-') i++, npaths++;
    for (int k = i; k < argc; k += 2) {
        if (k + 1 >= argc) return cb_usage("[PATH...] [-name PATTERN] [-type f|d|l] [-maxdepth N]");
        if (!strcmp(argv[k], "-name")) f_name = argv[k + 1];
        else if (!strcmp(argv[k], "-type") && strchr("fdl", argv[k + 1][0])) f_type = argv[k + 1][0];
        else if (!strcmp(argv[k], "-maxdepth")) f_depth = atoi(argv[k + 1]);
        else return fprintf(stderr, "find: unsupported expression '%s'\n", argv[k]), 1;
    }
    if (!npaths) return f_walk(".", 0);
    for (int p = 1; p <= npaths; p++) rc |= f_walk(argv[p], 0);
    return rc;
}

/* --- du --- */

static int du_summary, du_human;

static void du_print(unsigned long long kb, const char *path) {
    if (!du_human) printf("%llu\t%s\n", kb, path);
    else if (kb >= 1048576) printf("%llu.%lluG\t%s\n", kb >> 20, (kb & 1048575) * 10 >> 20, path);
    else if (kb >= 1024) printf("%llu.%lluM\t%s\n", kb >> 10, (kb & 1023) * 10 >> 10, path);
    else printf("%lluK\t%s\n", kb, path);
}

static unsigned long long du_walk(const char *path, int top, int *rc) {
    struct stat st;
    if (lstat(path, &st) < 0) { *rc = cb_err("cannot access", path); return 0; }
    unsigned long long bytes = st.st_blocks ? (unsigned long long)st.st_blocks * 512 : (unsigned long long)st.st_size;
    unsigned long long kb = (bytes + 1023) / 1024;
    if (S_ISDIR(st.st_mode)) {
        int n;
        char **v = cb_list_dir(path, &n);
        if (!v) { *rc = cb_err(NULL, path); return kb; }
        for (int i = 0; i < n; i++) {
            char *child = cb_join(path, v[i]);
            if (child) kb += du_walk(child, 0, rc);
            free(child);
        }
        cb_free_names(v, n);
        if (!du_summary || top) du_print(kb, path);
    }
    return kb;
}

int du_main(int argc, char **argv) {
    int c, rc = 0;
    while ((c = getopt(argc, argv, "shk")) != -1) {
        if (c == 's') du_summary = 1;
        else if (c == 'h') du_human = 1;
        else if (c != 'k') return 1;
    }
    for (int i = optind; i < argc || i == optind; i++) {
        const char *path = i < argc ? argv[i] : ".";
        struct stat st;
        unsigned long long kb = du_walk(path, 1, &rc);
        if (lstat(path, &st) == 0 && !S_ISDIR(st.st_mode)) du_print(kb, path);
    }
    return rc;
}

/* --- df --- */

/* Blocks of `unit` bytes in KiB, without 64-bit division. */
static unsigned long kib(unsigned long blocks, unsigned long unit) {
    return unit >= 1024 ? blocks * (unit >> 10) : blocks / (1024 / (unit ? unit : 512));
}

int df_main(int argc, char **argv) {
    const char *def[] = { "/" };
    int rc = 0;
    if (argc < 2) argv = (char **)def - 1, argc = 2;
    printf("Filesystem          1K-blocks      Used Available Use%% Mounted on\n");
    for (int i = 1; i < argc; i++) {
        struct statfs s;
        if (statfs(argv[i], &s) < 0) {
            if (errno != ENOSYS) { rc = cb_err("cannot stat", argv[i]); continue; }
            char buf[1024];                         /* no statfs: show what the mount list knows */
            if (cb_slurp("/proc/mounts", buf, sizeof buf) < 0) {
                fprintf(stderr, "df: this kernel has no statfs and no mount list\n");
                return 1;
            }
            fputs(buf, stdout);
            return 0;
        }
        unsigned long unit = (unsigned long)(s.f_frsize ? s.f_frsize : s.f_bsize);
        unsigned long total = kib(s.f_blocks, unit), avail = kib(s.f_bavail, unit);
        unsigned long used = total - kib(s.f_bfree, unit);
        printf("%-16s %10lu %9lu %9lu %3lu%% %s\n", "-", total, used, avail,
               used + avail ? used / ((used + avail) / 100 + 1) : 0, argv[i]);
    }
    return rc;
}
