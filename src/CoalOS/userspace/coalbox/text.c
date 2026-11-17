/* Text applets: echo head tail wc grep sort hexdump yes basename dirname */

#include "cb.h"

/* --- echo --- */

static int unescape(const char *s) {
    for (; *s; s++) {
        if (*s != '\\' || !s[1]) { putchar(*s); continue; }
        switch (*++s) {
        case 'n': putchar('\n'); break;
        case 't': putchar('\t'); break;
        case 'r': putchar('\r'); break;
        case 'a': putchar('\a'); break;
        case 'b': putchar('\b'); break;
        case 'e': putchar(27); break;
        case 'c': return 1;
        case '0': {
            int v = 0, k = 0;
            while (k < 3 && s[1] >= '0' && s[1] <= '7') v = v * 8 + (*++s - '0'), k++;
            putchar(v);
            break;
        }
        default: putchar('\\'); putchar(*s); break;
        }
    }
    return 0;
}

int echo_main(int argc, char **argv) {
    int nl = 1, esc = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *f = argv[i] + 1;
        while (*f == 'n' || *f == 'e' || *f == 'E') f++;
        if (*f) break;
        for (f = argv[i] + 1; *f; f++) {
            if (*f == 'n') nl = 0;
            else esc = *f == 'e';
        }
    }
    for (; i < argc; i++) {
        if (esc ? unescape(argv[i]) : fputs(argv[i], stdout) < 0) return 0;
        if (i + 1 < argc) putchar(' ');
    }
    if (nl) putchar('\n');
    return 0;
}

/* --- head / tail --- */

/* Parses -n N, -nN and -N; returns the index of the first operand. */
static int parse_count(int argc, char **argv, long *n) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i] + 1;
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (*a == 'n') {
            a = a[1] ? a + 1 : (i + 1 < argc ? argv[++i] : "");
        }
        if (!isdigit(*a)) {
            fprintf(stderr, "%s: invalid option or number of lines: '%s'\n", prog, argv[i]);
            exit(1);
        }
        *n = strtol(a, NULL, 10);
    }
    return i;
}

static int each_file(int argc, char **argv, int first, int (*fn)(FILE *, const char *)) {
    int rc = 0, multi = argc - first > 1;
    if (first == argc) return fn(stdin, "standard input");
    for (int i = first; i < argc; i++) {
        FILE *f = strcmp(argv[i], "-") ? fopen(argv[i], "r") : stdin;
        if (!f) {
            fflush(stdout);
            fprintf(stderr, "%s: cannot open '%s' for reading: %s\n", prog, argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (multi) printf("%s==> %s <==\n", i > first ? "\n" : "", argv[i]);
        if (fn(f, argv[i])) rc = 1;
        if (f != stdin) fclose(f);
    }
    return rc;
}

static long line_count = 10;

static int head_file(FILE *f, const char *name) {
    int c;
    for (long n = line_count; n > 0 && (c = fgetc(f)) != EOF;) {
        putchar(c);
        if (c == '\n') n--;
    }
    return 0;
}

int head_main(int argc, char **argv) {
    return each_file(argc, argv, parse_count(argc, argv, &line_count), head_file);
}

static int tail_file(FILE *f, const char *name) {
    size_t len = 0, cap = 4096;
    char *buf = malloc(cap);
    int c;
    while (buf && (c = fgetc(f)) != EOF) {
        if (len == cap) {
            char *nb = realloc(buf, cap *= 2);
            if (!nb) break;
            buf = nb;
        }
        buf[len++] = (char)c;
    }
    if (!buf) return 1;
    /* a final newline ends the last line; it does not start another */
    size_t i = len && buf[len - 1] == '\n' ? len - 1 : len, start = len;
    if (line_count > 0) {
        long seen = 0;
        for (; i > 0; i--)
            if (buf[i - 1] == '\n' && ++seen == line_count) break;
        start = i;
    }
    fwrite(buf + start, 1, len - start, stdout);
    free(buf);
    return 0;
}

int tail_main(int argc, char **argv) {
    return each_file(argc, argv, parse_count(argc, argv, &line_count), tail_file);
}

/* --- wc --- */

static int wc_l, wc_w, wc_c, wc_width;

static void wc_print(unsigned long l, unsigned long w, unsigned long c, const char *name) {
    if (wc_l) printf("%*lu", wc_width, l);
    if (wc_w) printf("%s%*lu", wc_l ? " " : "", wc_width, w);
    if (wc_c) printf("%s%*lu", wc_l || wc_w ? " " : "", wc_width, c);
    if (name) printf(" %s", name);
    putchar('\n');
}

int wc_main(int argc, char **argv) {
    int o, rc = 0;
    while ((o = getopt(argc, argv, "lwcm")) != -1) {
        if (o == 'l') wc_l = 1;
        else if (o == 'w') wc_w = 1;
        else if (o == 'c' || o == 'm') wc_c = 1;
        else return 1;
    }
    if (!wc_l && !wc_w && !wc_c) wc_l = wc_w = wc_c = 1;
    int nfiles = argc - optind;
    wc_width = (wc_l + wc_w + wc_c > 1 || nfiles > 1 || !nfiles) ? 7 : 0;
    unsigned long tl = 0, tw = 0, tc = 0;
    for (int i = optind; i < argc || i == optind; i++) {
        const char *name = i < argc ? argv[i] : NULL;
        FILE *f = !name ? stdin : strcmp(name, "-") ? fopen(name, "r") : stdin;
        if (!f) { rc = cb_err(NULL, name); continue; }
        unsigned long l = 0, w = 0, c = 0;
        int ch, in_word = 0;
        while ((ch = fgetc(f)) != EOF) {
            c++;
            if (ch == '\n') l++;
            if (isspace(ch)) in_word = 0;
            else if (!in_word) in_word = 1, w++;
        }
        if (f != stdin) fclose(f);
        wc_print(l, w, c, name);
        tl += l, tw += w, tc += c;
    }
    if (nfiles > 1) wc_print(tl, tw, tc, "total");
    return rc;
}

/* --- grep: Kernighan/Pike matcher with ^ $ . * [...] and backslash escapes --- */

static int g_icase, g_fixed;

static int eq(int a, int b) {
    return g_icase ? tolower(a) == tolower(b) : a == b;
}

/* The pattern atom at re ends here: one character, an escape or a complete [...] class. */
static const char *atom_end(const char *re) {
    if (g_fixed) return re + 1;
    if (*re == '\\' && re[1]) return re + 2;
    if (*re == '[') {
        const char *q = re + 1;
        q += *q == '^';
        q += *q == ']';
        while (*q && *q != ']') q++;
        if (*q) return q + 1;
    }
    return re + 1;
}

static int atom_match(const char *re, const char *end, int t) {
    if (g_fixed) return eq(*re, t);
    if (*re == '\\' && re[1]) return eq(re[1], t);
    if (*re == '.') return 1;
    if (*re != '[' || end - re < 3) return eq(*re, t);
    const char *q = re + 1;
    int neg = *q == '^', hit = 0;
    q += neg;
    for (const char *c = q; c < end - 1; c++) {
        if (c[1] == '-' && c + 2 < end - 1) { hit |= eq(c[0], t) || (t >= c[0] && t <= c[2]); c += 2; }
        else hit |= eq(*c, t);
    }
    return hit != neg;
}

static int match_here(const char *re, const char *t) {
    if (!*re) return 1;
    if (*re == '$' && !re[1] && !g_fixed) return !*t;
    const char *end = atom_end(re);
    if (*end == '*' && !g_fixed) {
        for (;; t++) {
            if (match_here(end + 1, t)) return 1;
            if (!*t || !atom_match(re, end, *t)) return 0;
        }
    }
    return *t && atom_match(re, end, *t) && match_here(end, t + 1);
}

static int match(const char *re, const char *t) {
    if (*re == '^' && !g_fixed) return match_here(re + 1, t);
    do {
        if (match_here(re, t)) return 1;
    } while (*t++);
    return 0;
}

int grep_main(int argc, char **argv) {
    int o, invert = 0, number = 0, count = 0, quiet = 0, rc = 1;
    const char *re = NULL;
    while ((o = getopt(argc, argv, "ivncqFe:")) != -1) {
        if (o == 'i') g_icase = 1;
        else if (o == 'v') invert = 1;
        else if (o == 'n') number = 1;
        else if (o == 'c') count = 1;
        else if (o == 'q') quiet = 1;
        else if (o == 'F') g_fixed = 1;
        else if (o == 'e') re = optarg;
        else return 2;
    }
    if (!re) {
        if (optind == argc) return cb_usage("[-ivncqF] PATTERN [FILE...]");
        re = argv[optind++];
    }
    int nfiles = argc - optind;
    char *line = NULL;
    size_t cap = 0;
    for (int i = optind; i < argc || i == optind; i++) {
        const char *name = i < argc ? argv[i] : "(standard input)";
        FILE *f = i >= argc || !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
        if (!f) { cb_err(NULL, argv[i]); rc = 2; continue; }
        long ln = 0, hits = 0;
        ssize_t len;
        while ((len = getline(&line, &cap, f)) >= 0) {
            ln++;
            if (len && line[len - 1] == '\n') line[len - 1] = 0;
            if (match(re, line) == invert) continue;
            hits++;
            rc = rc == 2 ? 2 : 0;
            if (quiet) exit(0);
            if (count) continue;
            if (nfiles > 1) printf("%s:", name);
            if (number) printf("%ld:", ln);
            puts(line);
        }
        if (count) {
            if (nfiles > 1) printf("%s:", name);
            printf("%ld\n", hits);
        }
        if (f != stdin) fclose(f);
    }
    return rc;
}

/* --- sort --- */

static int s_rev, s_num;

static int cmp_lines(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int r = 0;
    if (s_num) {
        long nx = strtol(x, NULL, 10), ny = strtol(y, NULL, 10);
        r = nx < ny ? -1 : nx > ny;
    }
    if (!r) r = strcmp(x, y);
    return s_rev ? -r : r;
}

int sort_main(int argc, char **argv) {
    int o, uniq = 0, rc = 0;
    while ((o = getopt(argc, argv, "rnu")) != -1) {
        if (o == 'r') s_rev = 1;
        else if (o == 'n') s_num = 1;
        else if (o == 'u') uniq = 1;
        else return 2;
    }
    char **v = NULL;
    size_t n = 0, cap = 0;
    for (int i = optind; i < argc || i == optind; i++) {
        FILE *f = i >= argc || !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
        if (!f) { rc = 2 * cb_err("cannot read", argv[i]); continue; }
        char *line = NULL;
        size_t lcap = 0;
        ssize_t len;
        while ((len = getline(&line, &lcap, f)) >= 0) {
            if (len && line[len - 1] == '\n') line[len - 1] = 0;
            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                v = realloc(v, cap * sizeof *v);
                if (!v) return 2;
            }
            v[n++] = line;
            line = NULL;
            lcap = 0;
        }
        free(line);
        if (f != stdin) fclose(f);
    }
    qsort(v, n, sizeof *v, cmp_lines);
    for (size_t i = 0; i < n; i++)
        if (!uniq || !i || cmp_lines(&v[i - 1], &v[i])) puts(v[i]);
    return rc;
}

/* --- hexdump (canonical format) --- */

static int dump_fd(int fd, unsigned long *off) {
    unsigned char b[16];
    ssize_t n;
    for (;;) {
        size_t have = 0;
        while (have < 16 && (n = read(fd, b + have, 16 - have)) > 0) have += (size_t)n;
        if (!have) return 0;
        printf("%08lx ", *off);
        for (int i = 0; i < 16; i++) {
            if (i == 8) putchar(' ');
            if ((size_t)i < have) printf(" %02x", b[i]);
            else fputs("   ", stdout);
        }
        fputs("  |", stdout);
        for (size_t i = 0; i < have; i++) putchar(isprint(b[i]) ? b[i] : '.');
        puts("|");
        *off += have;
        if (have < 16) return 0;
    }
}

int hexdump_main(int argc, char **argv) {
    int rc = 0, first = 1;
    unsigned long off = 0;
    if (argc > 1 && !strcmp(argv[1], "-C")) first = 2;
    if (first == argc) dump_fd(0, &off);
    for (int i = first; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { rc = cb_err(NULL, argv[i]); continue; }
        dump_fd(fd, &off);
        close(fd);
    }
    printf("%08lx\n", off);
    return rc;
}

/* --- yes / basename / dirname --- */

int yes_main(int argc, char **argv) {
    char buf[4096];
    size_t len = 0;
    for (int i = 1; i < argc || (i == 1 && argc == 1); i++) {
        const char *w = argc == 1 ? "y" : argv[i];
        size_t wl = strlen(w);
        if (len + wl + 2 > sizeof buf) break;
        memcpy(buf + len, w, wl);
        len += wl;
        buf[len++] = i + 1 < argc ? ' ' : '\n';
    }
    size_t unit = len, total = len;
    while (total + unit <= sizeof buf) {
        memcpy(buf + total, buf, unit);
        total += unit;
    }
    while (cb_write_all(1, buf, total) == 0) {}
    return 1;
}

/* Copies path without trailing slashes into a static buffer. */
static char *trim_slashes(const char *path) {
    static char buf[256];
    size_t n = strlen(path);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, path, n);
    while (n > 1 && buf[n - 1] == '/') n--;
    buf[n] = 0;
    return buf;
}

int basename_main(int argc, char **argv) {
    if (argc < 2) return cb_usage("NAME [SUFFIX]");
    char *p = trim_slashes(argv[1]);
    const char *b = p[1] ? cb_base(p) : p;
    size_t bl = strlen(b);
    if (argc > 2) {
        size_t sl = strlen(argv[2]);
        if (sl < bl && !strcmp(b + bl - sl, argv[2])) bl -= sl;
    }
    printf("%.*s\n", (int)bl, b);
    return 0;
}

int dirname_main(int argc, char **argv) {
    if (argc < 2) return cb_usage("NAME");
    char *p = trim_slashes(argv[1]);
    char *s = strrchr(p, '/');
    if (!s) puts(".");
    else {
        while (s > p && s[-1] == '/') s--;
        if (s == p) puts("/");
        else printf("%.*s\n", (int)(s - p), p);
    }
    return 0;
}
