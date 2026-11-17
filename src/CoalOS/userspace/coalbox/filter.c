/* Stream filters: tee cut tr rev fold nl seq printf expr cmp diff xxd */

#include "cb.h"
#include <libc/limits.h>

/* --- tee --- */

int tee_main(int argc, char **argv) {
    int c, append = 0, rc = 0, n = 0, fds[16];
    while ((c = getopt(argc, argv, "ai")) != -1) {
        if (c == '?') return 1;
        append |= c == 'a';
    }
    for (int i = optind; i < argc && n < 16; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) rc = cb_err(NULL, argv[i]);
        else fds[n++] = fd;
    }
    char buf[4096];
    ssize_t r;
    while ((r = read(0, buf, sizeof buf)) != 0) {
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 || cb_write_all(1, buf, (size_t)r) < 0) break;
        for (int i = 0; i < n; i++)
            if (cb_write_all(fds[i], buf, (size_t)r) < 0) rc = cb_err("write error", argv[optind + i]);
    }
    for (int i = 0; i < n; i++) close(fds[i]);
    return rc;
}

/* --- cut --- */

static struct { int lo, hi; } ranges[32];
static int nranges;

static int in_ranges(int n) {
    for (int i = 0; i < nranges; i++)
        if (n >= ranges[i].lo && n <= ranges[i].hi) return 1;
    return 0;
}

/* "1,3-5,7-,-2": returns 0 on success. */
static int parse_ranges(const char *s) {
    for (; *s && nranges < 32; nranges++) {
        int lo = 1;
        if (isdigit(*s)) lo = (int)strtol(s, (char **)&s, 10);
        int hi = lo;
        if (*s == '-') {
            s++;
            hi = isdigit(*s) ? (int)strtol(s, (char **)&s, 10) : INT_MAX;
        }
        ranges[nranges].lo = lo;
        ranges[nranges].hi = hi;
        if (*s == ',') s++;
        else if (*s) return -1;
    }
    return nranges ? 0 : -1;
}

int cut_main(int argc, char **argv) {
    int c, chars = 0, only_delimited = 0;
    char delim = '\t';
    while ((c = getopt(argc, argv, "d:f:c:b:s")) != -1) {
        if (c == 'd') delim = optarg[0];
        else if (c == 'f' || c == 'c' || c == 'b') {
            chars = c != 'f';
            if (parse_ranges(optarg)) return fprintf(stderr, "cut: invalid list '%s'\n", optarg), 1;
        } else if (c == 's') only_delimited = 1;
        else return 1;
    }
    if (!nranges) return cb_usage("-f LIST [-d DELIM] [-s] | -c LIST [FILE...]");
    int rc = 0;
    char *line = NULL;
    size_t cap = 0;
    for (int i = optind; i < argc || i == optind; i++) {
        FILE *f = i >= argc || !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
        if (!f) { rc = cb_err(NULL, argv[i]); continue; }
        ssize_t len;
        while ((len = getline(&line, &cap, f)) >= 0) {
            if (len && line[len - 1] == '\n') line[--len] = 0;
            if (chars) {
                for (int k = 0; k < len; k++)
                    if (in_ranges(k + 1)) putchar(line[k]);
            } else if (!strchr(line, delim)) {
                if (only_delimited) continue;
                fputs(line, stdout);
            } else {
                int field = 1, first = 1;
                for (char *p = line;; field++) {
                    char *e = strchr(p, delim);
                    if (in_ranges(field)) {
                        if (!first) putchar(delim);
                        fwrite(p, 1, e ? (size_t)(e - p) : strlen(p), stdout);
                        first = 0;
                    }
                    if (!e) break;
                    p = e + 1;
                }
            }
            putchar('\n');
        }
        if (f != stdin) fclose(f);
    }
    return rc;
}

/* --- tr --- */

static const char *class_chars(const char *name, size_t n, int c) {
    static const struct { const char *name; int (*is)(int); } cls[] = {
        { "upper", isupper }, { "lower", islower }, { "digit", isdigit }, { "alpha", isalpha },
        { "alnum", isalnum }, { "space", isspace }, { "xdigit", isxdigit }, { "print", isprint },
    };
    for (unsigned i = 0; i < sizeof cls / sizeof *cls; i++)
        if (strlen(cls[i].name) == n && !strncmp(cls[i].name, name, n)) return cls[i].is(c) ? "" : NULL;
    return NULL;
}

static int tr_escape(const char **p) {
    int c = *(*p)++;
    if (c != '\\' || !**p) return c;
    c = *(*p)++;
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': case '1': case '2': case '3': {
        int v = c - '0';
        for (int k = 0; k < 2 && **p >= '0' && **p <= '7'; k++) v = v * 8 + *(*p)++ - '0';
        return v & 255;
    }
    }
    return c;
}

/* Expands ranges, [:class:] and escapes into the characters of the set; returns how many. */
static int tr_set(const char *s, unsigned char *out) {
    int n = 0;
    while (*s && n < 256) {
        if (s[0] == '[' && s[1] == ':' && strstr(s, ":]")) {
            const char *e = strstr(s, ":]");
            for (int c = 0; c < 256 && n < 256; c++)
                if (class_chars(s + 2, (size_t)(e - s - 2), c)) out[n++] = (unsigned char)c;
            s = e + 2;
            continue;
        }
        int lo = tr_escape(&s), hi = lo;
        if (*s == '-' && s[1]) {
            s++;
            hi = tr_escape(&s);
        }
        for (int c = lo; c <= hi && n < 256; c++) out[n++] = (unsigned char)c;
    }
    return n;
}

int tr_main(int argc, char **argv) {
    int o, del = 0, squeeze = 0, comp = 0;
    while ((o = getopt(argc, argv, "dsc")) != -1) {
        if (o == 'd') del = 1;
        else if (o == 's') squeeze = 1;
        else if (o == 'c') comp = 1;
        else return 1;
    }
    int nsets = argc - optind;
    if (nsets < 1 || nsets > 2 || (!del && !squeeze && nsets < 2) || (del && !squeeze && nsets > 1))
        return cb_usage("[-cds] SET1 [SET2]");
    unsigned char s1[256], s2[256], map[256], drop[256] = { 0 }, sq[256] = { 0 };
    int n1 = tr_set(argv[optind], s1), n2 = nsets > 1 ? tr_set(argv[optind + 1], s2) : 0;
    if (comp) {
        unsigned char in[256] = { 0 };
        for (int i = 0; i < n1; i++) in[s1[i]] = 1;
        for (int c = n1 = 0; c < 256; c++)
            if (!in[c]) s1[n1++] = (unsigned char)c;
    }
    for (int c = 0; c < 256; c++) map[c] = (unsigned char)c;
    if (del) {
        for (int i = 0; i < n1; i++) drop[s1[i]] = 1;
    } else if (nsets > 1) {
        if (!n2) return fprintf(stderr, "tr: SET2 is empty\n"), 1;
        for (int i = 0; i < n1; i++) map[s1[i]] = s2[i < n2 ? i : n2 - 1];
    }
    const unsigned char *sqset = nsets > 1 && !del ? s2 : s1;
    for (int i = 0; i < (nsets > 1 && !del ? n2 : n1); i++) sq[sqset[i]] = 1;
    int c, last = -1;
    while ((c = getchar()) != EOF) {
        if (drop[c]) continue;
        c = map[c];
        if (squeeze && sq[c] && c == last) continue;
        putchar(last = c);
    }
    return 0;
}

/* --- rev / fold / nl --- */

/* Runs fn on every line (without its newline) of the files, or stdin. */
static int each_line(int argc, char **argv, int first, void (*fn)(char *, ssize_t, int)) {
    int rc = 0;
    char *line = NULL;
    size_t cap = 0;
    for (int i = first; i < argc || i == first; i++) {
        FILE *f = i >= argc || !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
        if (!f) { rc = cb_err(NULL, argv[i]); continue; }
        ssize_t len;
        while ((len = getline(&line, &cap, f)) >= 0) {
            int nl = len && line[len - 1] == '\n';
            if (nl) line[--len] = 0;
            fn(line, len, nl);
        }
        if (f != stdin) fclose(f);
    }
    free(line);
    return rc;
}

static void rev_line(char *s, ssize_t n, int nl) {
    for (ssize_t i = 0; i < n / 2; i++) {
        char t = s[i];
        s[i] = s[n - 1 - i];
        s[n - 1 - i] = t;
    }
    fwrite(s, 1, (size_t)n, stdout);
    if (nl) putchar('\n');
}

int rev_main(int argc, char **argv) {
    return each_line(argc, argv, 1, rev_line);
}

static long fold_width = 80;
static int fold_spaces;

static void fold_line(char *s, ssize_t n, int nl) {
    ssize_t start = 0;
    while (n - start > fold_width) {
        ssize_t cut = start + fold_width;
        if (fold_spaces)
            for (ssize_t k = cut; k > start; k--)
                if (s[k - 1] == ' ') { cut = k; break; }
        fwrite(s + start, 1, (size_t)(cut - start), stdout);
        putchar('\n');
        start = cut;
    }
    fwrite(s + start, 1, (size_t)(n - start), stdout);
    if (nl) putchar('\n');
}

int fold_main(int argc, char **argv) {
    int c;
    while ((c = getopt(argc, argv, "w:s")) != -1) {
        if (c == 'w') fold_width = strtol(optarg, NULL, 10);
        else if (c == 's') fold_spaces = 1;
        else return 1;
    }
    if (fold_width < 1) return fprintf(stderr, "fold: invalid width\n"), 1;
    return each_line(argc, argv, optind, fold_line);
}

static int nl_all;
static long nl_count;

static void nl_line(char *s, ssize_t n, int nl) {
    if (n || nl_all) printf("%6ld\t", ++nl_count);
    else fputs("       ", stdout);
    fwrite(s, 1, (size_t)n, stdout);
    if (nl) putchar('\n');
}

int nl_main(int argc, char **argv) {
    int c;
    while ((c = getopt(argc, argv, "b:")) != -1) {
        if (c == 'b') nl_all = optarg[0] == 'a';
        else return 1;
    }
    return each_line(argc, argv, optind, nl_line);
}

/* --- seq --- */

int seq_main(int argc, char **argv) {
    long v[3] = { 1, 1, 0 };
    int n = argc - 1;
    if (n < 1 || n > 3) return cb_usage("[FIRST [INCREMENT]] LAST");
    for (int i = 0; i < n; i++) {
        char *end;
        long x = strtol(argv[i + 1], &end, 10);
        if (*end || end == argv[i + 1]) return fprintf(stderr, "seq: invalid integer argument: '%s'\n", argv[i + 1]), 1;
        if (n == 1) v[2] = x;
        else if (n == 2) v[i ? 2 : 0] = x;
        else v[i == 0 ? 0 : i == 1 ? 1 : 2] = x;
    }
    if (!v[1]) return fprintf(stderr, "seq: zero increment\n"), 1;
    for (long x = v[0]; v[1] > 0 ? x <= v[2] : x >= v[2]; x += v[1]) printf("%ld\n", x);
    return 0;
}

/* --- printf --- */

/* Decodes the escape after a backslash at *p (which points just past it) into *out; returns -1 for \c. */
static int pf_escape(const char **p) {
    int c = *(*p)++;
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'a': return '\a';
    case 'b': return '\b';
    case 'f': return '\f';
    case 'v': return '\v';
    case 'c': return -1;
    case 'x': {
        int v = 0, k = 0;
        for (; k < 2 && isxdigit(**p); k++) v = v * 16 + (isdigit(**p) ? **p - '0' : (**p | 32) - 'a' + 10), (*p)++;
        return k ? v : 'x';
    }
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
        int v = c - '0';
        for (int k = 0; k < 2 && **p >= '0' && **p <= '7'; k++) v = v * 8 + *(*p)++ - '0';
        return v & 255;
    }
    }
    return c;
}

static int pf_number(const char *arg, long *out) {
    char *end;
    *out = 0;
    if (!arg) return 0;
    if ((arg[0] == '\'' || arg[0] == '"') && arg[1]) {          /* 'A is the code of A */
        *out = (unsigned char)arg[1];
        return 0;
    }
    *out = strtol(arg, &end, 0);
    if (end == arg || *end) {
        fprintf(stderr, "printf: '%s': expected a numeric value\n", arg);
        return 1;
    }
    return 0;
}

int printf_main(int argc, char **argv) {
    if (argc < 2) return cb_usage("FORMAT [ARGUMENT...]");
    int ai = 2, rc = 0, used;
    do {
        used = 0;
        for (const char *p = argv[1]; *p; p++) {
            if (*p == '\\') {
                p++;
                int c = pf_escape(&p);
                if (c < 0) return rc;
                putchar(c);
                p--;
                continue;
            }
            if (*p != '%') { putchar(*p); continue; }
            if (p[1] == '%') { putchar('%'); p++; continue; }
            char spec[24];
            int n = 0;
            spec[n++] = *p++;
            while (*p && strchr("-+ #0123456789.", *p) && n < 18) spec[n++] = *p++;
            const char *arg = ai < argc ? argv[ai] : NULL;
            char conv = *p;
            if (!conv || !strchr("diuxXocsb", conv)) {
                fprintf(stderr, "printf: %%%c: invalid conversion\n", conv ? conv : ' ');
                return 1;
            }
            ai += arg != NULL;
            used += arg != NULL;
            long v;
            if (conv == 's' || conv == 'c' || conv == 'b') {
                const char *s = arg ? arg : "";
                if (conv == 'b') {
                    for (; *s; s++) {
                        if (*s != '\\' || !s[1]) { putchar(*s); continue; }
                        s++;
                        int c = pf_escape(&s);
                        if (c < 0) return rc;
                        putchar(c);
                        s--;
                    }
                    continue;
                }
                spec[n++] = conv;
                spec[n] = 0;
                if (conv == 'c') printf(spec, *s);
                else printf(spec, s);
                continue;
            }
            rc |= pf_number(arg, &v);
            spec[n++] = 'l';
            spec[n++] = conv == 'i' ? 'd' : conv;
            spec[n] = 0;
            printf(spec, v);
        }
    } while (ai < argc && used);
    return rc;
}

/* --- expr --- */

static char **ex_av;
static int ex_n, ex_i;

static void ex_die(const char *msg) {
    fprintf(stderr, "expr: %s\n", msg);
    exit(2);
}

static long ex_num(const char *s) {
    char *end;
    long v = strtol(s, &end, 10);
    if (!*s || *end) ex_die("non-integer argument");
    return v;
}

static int ex_is_num(const char *s) {
    char *end;
    strtol(s, &end, 10);
    return *s && !*end;
}

static const char *ex_fmt(long v) {
    static char buf[8][24];
    static int k;
    snprintf(buf[k & 7], sizeof buf[0], "%ld", v);
    return buf[k++ & 7];
}

static int ex_true(const char *s) {
    return *s && strcmp(s, "0");
}

static const char *ex_or(void);

static const char *ex_primary(void) {
    if (ex_i >= ex_n) ex_die("syntax error: missing argument");
    const char *t = ex_av[ex_i++];
    if (!strcmp(t, "(")) {
        const char *v = ex_or();
        if (ex_i >= ex_n || strcmp(ex_av[ex_i++], ")")) ex_die("syntax error: expecting ')'");
        return v;
    }
    if (!strcmp(t, "length") && ex_i < ex_n) return ex_fmt((long)strlen(ex_av[ex_i++]));
    return t;
}

static int ex_op(const char *const *ops) {
    if (ex_i >= ex_n) return 0;
    for (int k = 0; ops[k]; k++)
        if (!strcmp(ex_av[ex_i], ops[k])) return k + 1;
    return 0;
}

static const char *ex_mul(void) {
    static const char *const ops[] = { "*", "/", "%", NULL };
    const char *l = ex_primary();
    int op;
    while ((op = ex_op(ops))) {
        ex_i++;
        long a = ex_num(l), b = ex_num(ex_primary());
        if (op > 1 && !b) ex_die("division by zero");
        l = ex_fmt(op == 1 ? a * b : op == 2 ? a / b : a % b);
    }
    return l;
}

static const char *ex_add(void) {
    static const char *const ops[] = { "+", "-", NULL };
    const char *l = ex_mul();
    int op;
    while ((op = ex_op(ops))) {
        ex_i++;
        long a = ex_num(l), b = ex_num(ex_mul());
        l = ex_fmt(op == 1 ? a + b : a - b);
    }
    return l;
}

static const char *ex_cmp(void) {
    static const char *const ops[] = { "=", "!=", "<", "<=", ">", ">=", NULL };
    const char *l = ex_add();
    int op;
    while ((op = ex_op(ops))) {
        ex_i++;
        const char *r = ex_add();
        int c = ex_is_num(l) && ex_is_num(r) ? (ex_num(l) > ex_num(r)) - (ex_num(l) < ex_num(r)) : strcmp(l, r);
        static const int want[] = { 0, 0, -1, -1, 1, 1 };       /* sign wanted; "=" and "<=" ">=" also accept 0 */
        int ok = op == 1 ? c == 0 : op == 2 ? c != 0 : op == 4 ? c <= 0 : op == 6 ? c >= 0 : c == want[op - 1];
        l = ex_fmt(ok);
    }
    return l;
}

static const char *ex_and(void) {
    static const char *const ops[] = { "&", NULL };
    const char *l = ex_cmp();
    while (ex_op(ops)) {
        ex_i++;
        const char *r = ex_cmp();
        if (!ex_true(l) || !ex_true(r)) l = "0";
    }
    return l;
}

static const char *ex_or(void) {
    static const char *const ops[] = { "|", NULL };
    const char *l = ex_and();
    while (ex_op(ops)) {
        ex_i++;
        const char *r = ex_and();
        if (!ex_true(l)) l = r;
    }
    return l;
}

int expr_main(int argc, char **argv) {
    ex_av = argv + 1;
    ex_n = argc - 1;
    if (ex_n < 1) ex_die("missing operand");
    const char *v = ex_or();
    if (ex_i < ex_n) ex_die(!strcmp(ex_av[ex_i], ":") ? "':' is not supported" : "syntax error");
    puts(v);
    return !ex_true(v);
}

/* --- cmp --- */

int cmp_main(int argc, char **argv) {
    int c, quiet = 0;
    while ((c = getopt(argc, argv, "s")) != -1) {
        if (c == '?') return 2;
        quiet = 1;
    }
    if (argc - optind != 2) return cb_usage("[-s] FILE1 FILE2");
    FILE *f[2];
    for (int i = 0; i < 2; i++) {
        f[i] = !strcmp(argv[optind + i], "-") ? stdin : fopen(argv[optind + i], "r");
        if (!f[i]) return 2 * cb_err(NULL, argv[optind + i]);
    }
    long byte = 1, line = 1;
    for (;; byte++) {
        int a = fgetc(f[0]), b = fgetc(f[1]);
        if (a == b && a == EOF) return 0;
        if (a != b) {
            if (!quiet) {
                if (a == EOF || b == EOF) printf("cmp: EOF on %s after byte %ld\n", argv[optind + (a == EOF ? 0 : 1)], byte - 1);
                else printf("%s %s differ: byte %ld, line %ld\n", argv[optind], argv[optind + 1], byte, line);
            }
            return 1;
        }
        line += a == '\n';
    }
}

/* --- diff: normal format, longest common subsequence --- */

static char **read_lines(const char *path, int *n) {
    FILE *f = !strcmp(path, "-") ? stdin : fopen(path, "r");
    if (!f) { cb_err(NULL, path); return NULL; }
    char **v = NULL, *line = NULL;
    size_t cap = 0, vcap = 0;
    ssize_t len;
    *n = 0;
    while ((len = getline(&line, &cap, f)) >= 0) {
        if (len && line[len - 1] == '\n') line[len - 1] = 0;
        if ((size_t)*n == vcap) v = realloc(v, (vcap = vcap ? vcap * 2 : 64) * sizeof *v);
        v[(*n)++] = strdup(line);
    }
    free(line);
    if (f != stdin) fclose(f);
    return v ? v : malloc(sizeof *v);
}

static void diff_range(int from, int count) {      /* 1-based, inclusive */
    if (count == 1) printf("%d", from);
    else printf("%d,%d", from, from + count - 1);
}

int diff_main(int argc, char **argv) {
    if (argc != 3) return cb_usage("FILE1 FILE2");
    int n, m;
    char **a = read_lines(argv[1], &n), **b = a ? read_lines(argv[2], &m) : NULL;
    if (!a || !b) return 2;
    if ((long)(n + 1) * (m + 1) > 4000000) return fprintf(stderr, "diff: files are too large\n"), 2;
    /* lcs[i][j]: length of the longest common subsequence of a[i..] and b[j..] */
    unsigned short *lcs = calloc((size_t)(n + 1) * (size_t)(m + 1), sizeof *lcs);
    if (!lcs) return fprintf(stderr, "diff: out of memory\n"), 2;
#define L(i, j) lcs[(size_t)(i) * (size_t)(m + 1) + (size_t)(j)]
    for (int i = n - 1; i >= 0; i--)
        for (int j = m - 1; j >= 0; j--)
            L(i, j) = !strcmp(a[i], b[j]) ? L(i + 1, j + 1) + 1 : L(i + 1, j) > L(i, j + 1) ? L(i + 1, j) : L(i, j + 1);
    int i = 0, j = 0, differ = 0;
    while (i < n || j < m) {
        if (i < n && j < m && !strcmp(a[i], b[j])) { i++; j++; continue; }
        int i0 = i, j0 = j;                         /* a hunk: the run of lines that do not match */
        while ((i < n || j < m) && !(i < n && j < m && !strcmp(a[i], b[j]))) {
            if (j >= m || (i < n && L(i + 1, j) >= L(i, j + 1))) i++;
            else j++;
        }
        differ = 1;
        int da = i - i0, db = j - j0;
        if (da) diff_range(i0 + 1, da);
        else printf("%d", i0);
        putchar(da && db ? 'c' : da ? 'd' : 'a');
        if (db) diff_range(j0 + 1, db);
        else printf("%d", j0);
        putchar('\n');
        for (int k = i0; k < i; k++) printf("< %s\n", a[k]);
        if (da && db) puts("---");
        for (int k = j0; k < j; k++) printf("> %s\n", b[k]);
    }
    return differ;
}

/* --- xxd --- */

int xxd_main(int argc, char **argv) {
    int plain = 0, i = 1;
    if (i < argc && !strcmp(argv[i], "-p")) plain = i++;
    int fd = i < argc ? open(argv[i], O_RDONLY) : 0;
    if (fd < 0) return cb_err(NULL, argv[i]);
    unsigned char b[16];
    unsigned long off = 0;
    for (;;) {
        size_t have = 0;
        ssize_t n;
        while (have < sizeof b && (n = read(fd, b + have, sizeof b - have)) > 0) have += (size_t)n;
        if (!have) return 0;
        if (plain) {
            for (size_t k = 0; k < have; k++) printf("%02x", b[k]);
            putchar('\n');
        } else {
            printf("%08lx:", off);
            for (size_t k = 0; k < sizeof b; k++) {
                if (k % 2 == 0) putchar(' ');
                if (k < have) printf("%02x", b[k]);
                else fputs("  ", stdout);
            }
            fputs("  ", stdout);
            for (size_t k = 0; k < have; k++) putchar(isprint(b[k]) ? b[k] : '.');
            putchar('\n');
        }
        off += have;
        if (have < sizeof b) return 0;
    }
}
