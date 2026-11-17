/* test and [: POSIX conditional expressions. Linked into coalbox and into sh (builtin). */

#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/unistd.h>
#include <libc/errno.h>
#include <libc/sys/stat.h>

int test_main(int argc, char **argv);

static char **av;
static int ac, ai, bad;

static int fail(const char *msg) {
    if (!bad) fprintf(stderr, "test: %s\n", msg);
    bad = 1;
    return 0;
}

static int is_binary(const char *s) {
    static const char *const ops[] = { "=", "==", "!=", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", "-nt", "-ot" };
    for (unsigned i = 0; i < sizeof ops / sizeof *ops; i++)
        if (!strcmp(s, ops[i])) return 1;
    return 0;
}

static long to_int(const char *s) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end) fail("integer expression expected");
    return v;
}

static int binary(const char *a, const char *op, const char *b) {
    if (!strcmp(op, "=") || !strcmp(op, "==")) return !strcmp(a, b);
    if (!strcmp(op, "!=")) return strcmp(a, b) != 0;
    if (!strcmp(op, "-nt") || !strcmp(op, "-ot")) {
        struct stat x, y;
        if (stat(a, &x) < 0) return 0;
        if (stat(b, &y) < 0) return op[1] == 'n';
        return op[1] == 'n' ? x.st_mtime > y.st_mtime : x.st_mtime < y.st_mtime;
    }
    long x = to_int(a), y = to_int(b);
    switch (op[2] | op[1] << 8) {            /* the two letters of -eq -ne -lt -le -gt -ge */
    case 'q' | 'e' << 8: return x == y;
    case 'e' | 'n' << 8: return x != y;
    case 't' | 'l' << 8: return x < y;
    case 'e' | 'l' << 8: return x <= y;
    case 't' | 'g' << 8: return x > y;
    default: return x >= y;
    }
}

static int unary(char op, const char *a) {
    struct stat st;
    switch (op) {
    case 'z': return !*a;
    case 'n': return *a != 0;
    case 't': return isatty(atoi(a));
    case 'r': return access(a, R_OK) == 0;
    case 'w': return access(a, W_OK) == 0;
    case 'x': return access(a, X_OK) == 0;
    case 'L': case 'h': return lstat(a, &st) == 0 && S_ISLNK(st.st_mode);
    }
    if (stat(a, &st) < 0) return 0;
    switch (op) {
    case 'e': return 1;
    case 'f': return S_ISREG(st.st_mode);
    case 'd': return S_ISDIR(st.st_mode);
    case 'c': return S_ISCHR(st.st_mode);
    case 'b': return S_ISBLK(st.st_mode);
    case 'p': return (st.st_mode & S_IFMT) == S_IFIFO;
    case 's': return st.st_size > 0;
    }
    return fail("unknown operator");
}

static int parse_or(void);

static int primary(void) {
    if (ai >= ac) return fail("argument expected");
    const char *a = av[ai];
    if (!strcmp(a, "(") && ai + 1 < ac) {
        ai++;
        int v = parse_or();
        if (ai >= ac || strcmp(av[ai], ")")) return fail("missing ')'");
        ai++;
        return v;
    }
    if (ai + 2 < ac && is_binary(av[ai + 1])) {
        int v = binary(a, av[ai + 1], av[ai + 2]);
        ai += 3;
        return v;
    }
    if (a[0] == '-' && a[1] && !a[2] && strchr("zntrwxLhefdcbps", a[1]) && ai + 1 < ac) {
        int v = unary(a[1], av[ai + 1]);
        ai += 2;
        return v;
    }
    ai++;
    return *a != 0;
}

static int parse_not(void) {
    if (ai + 1 < ac && !strcmp(av[ai], "!")) {
        ai++;
        return !parse_not();
    }
    return primary();
}

static int parse_and(void) {
    int v = parse_not();
    while (ai < ac && !strcmp(av[ai], "-a")) {
        ai++;
        int r = parse_not();
        v = v && r;
    }
    return v;
}

static int parse_or(void) {
    int v = parse_and();
    while (ai < ac && !strcmp(av[ai], "-o")) {
        ai++;
        int r = parse_and();
        v = v || r;
    }
    return v;
}

int test_main(int argc, char **argv) {
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    av = argv + 1;
    ac = argc - 1;
    ai = bad = 0;
    if (!strcmp(name, "[")) {
        if (ac < 1 || strcmp(av[ac - 1], "]")) return fail("missing ']'"), 2;
        ac--;
    }
    if (!ac) return 1;
    int v = parse_or();
    if (!bad && ai < ac) fail("too many arguments");
    return bad ? 2 : !v;
}
