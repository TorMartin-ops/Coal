/* Word expansion: ~, parameters, command substitution, arithmetic, field splitting, wildcards
 * and quote removal.
 *
 * While a word is expanded its text is kept in "pattern form": a quoted * ? [ or backslash is
 * preceded by a backslash. Wildcards that are left unescaped are live; the escapes are removed
 * at the end (or consumed by pattern_match). */

#include "sh.h"
#include <libc/dirent.h>

enum { M_FIELDS, M_STR, M_PATTERN };

struct sb {
    char *p;
    int n, cap;
};

struct exp {
    int mode;
    int dq;                     /* inside double quotes */
    int qseen;                  /* the current field contained a quote, so it exists even if empty */
    int zat;                    /* a "$@" without parameters was seen */
    struct sb cur;
    struct vec *out;
};

int cmdsub_status;

void vec_push(struct vec *v, char *s) {
    if (v->n == v->cap) v->v = xrealloc(v->v, (size_t)(v->cap = v->cap ? v->cap * 2 : 16) * sizeof *v->v);
    v->v[v->n++] = s;
}

void vec_free(struct vec *v) {
    for (int i = 0; i < v->n; i++) free(v->v[i]);
    free(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

static void sb_put(struct sb *s, int c) {
    if (s->n + 1 >= s->cap) s->p = xrealloc(s->p, (size_t)(s->cap = s->cap ? s->cap * 2 : 32));
    s->p[s->n++] = (char)c;
}

/* The finished string (malloc'd); the builder is empty afterwards. */
static char *sb_take(struct sb *s) {
    sb_put(s, 0);
    char *r = s->p;
    s->p = NULL;
    s->n = s->cap = 0;
    return r;
}

/* ----------------------------------------------------------------- patterns */

static int has_wildcard(const char *s) {
    for (; *s; s++) {
        if (*s == '\\' && s[1]) s++;
        else if (*s == '*' || *s == '?' || (*s == '[' && strchr(s + 1, ']'))) return 1;
    }
    return 0;
}

static void unescape(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) s++;
        *o++ = *s;
    }
    *o = 0;
}

/* -------------------------------------------------------------------- globbing */

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Matches pat (components separated by /) below prefix, which is empty or ends in a slash. */
static void glob_walk(const char *prefix, const char *pat, struct vec *out) {
    const char *slash = pat;
    while (*slash && *slash != '/') slash += *slash == '\\' && slash[1] ? 2 : 1;
    size_t cl = (size_t)(slash - pat), pl = strlen(prefix);
    const char *rest = *slash ? slash + 1 : NULL;       /* NULL: this is the last component */
    char comp[256];
    if (cl >= sizeof comp) return;
    memcpy(comp, pat, cl);
    comp[cl] = 0;
    struct vec names = { 0 };
    if (has_wildcard(comp)) {
        DIR *d = opendir(*prefix ? prefix : ".");
        struct dirent *e;
        while (d && (e = readdir(d)))
            if ((e->d_name[0] != '.' || comp[0] == '.') && strcmp(e->d_name, ".") && strcmp(e->d_name, "..")
                && pattern_match(comp, e->d_name))
                vec_push(&names, xstrdup(e->d_name));
        if (d) closedir(d);
        qsort(names.v, (size_t)names.n, sizeof *names.v, cmp_names);
    } else {
        unescape(comp);
        vec_push(&names, xstrdup(comp));
    }
    for (int i = 0; i < names.n; i++) {
        char *path = xmalloc(pl + strlen(names.v[i]) + 3);
        sprintf(path, "%s%s", prefix, names.v[i]);
        if (rest && *rest) {
            strcat(path, "/");
            if (is_dir(path)) glob_walk(path, rest, out);
            free(path);
        } else if (rest ? is_dir(path) : access(path, F_OK) == 0) {
            if (rest) strcat(path, "/");
            vec_push(out, path);
        } else {
            free(path);
        }
    }
    vec_free(&names);
}

/* ------------------------------------------------------------------- arithmetic */

static const char *ap;
static int arith_err;

static long arith_expr(int level);

static long arith_primary(void) {
    while (*ap == ' ' || *ap == '\t') ap++;
    if (*ap == '(') {
        ap++;
        long v = arith_expr(0);
        while (*ap == ' ') ap++;
        if (*ap == ')') ap++;
        else arith_err = 1;
        return v;
    }
    if (*ap == '-') { ap++; return -arith_primary(); }
    if (*ap == '+') { ap++; return arith_primary(); }
    if (*ap == '!') { ap++; return !arith_primary(); }
    if (*ap == '~') { ap++; return ~arith_primary(); }
    if (isdigit(*ap)) {
        char *end;
        long v = strtol(ap, &end, 0);
        ap = end;
        return v;
    }
    if (isalpha(*ap) || *ap == '_') {
        char name[64];
        int n = 0;
        while ((isalnum(*ap) || *ap == '_') && n < 63) name[n++] = *ap++;
        name[n] = 0;
        const char *v = var_get(name);
        return v ? strtol(v, NULL, 0) : 0;
    }
    arith_err = 1;
    return 0;
}

/* Binary operators by precedence level, loosest first. */
static const char *const arith_ops[][4] = {
    { "||" }, { "&&" }, { "|" }, { "^" }, { "&" }, { "==", "!=" }, { "<=", ">=", "<", ">" },
    { "<<", ">>" }, { "+", "-" }, { "*", "/", "%" },
};

static long arith_expr(int level) {
    if (level == (int)(sizeof arith_ops / sizeof *arith_ops)) return arith_primary();
    long v = arith_expr(level + 1);
    for (;;) {
        while (*ap == ' ' || *ap == '\t') ap++;
        const char *op = NULL;
        for (int i = 0; i < 4 && arith_ops[level][i]; i++) {
            size_t l = strlen(arith_ops[level][i]);
            if (strncmp(ap, arith_ops[level][i], l)) continue;
            if (l == 1 && strchr("|&<>", *ap) && ap[1] == *ap) continue;      /* part of || && << >> */
            op = arith_ops[level][i];
            break;
        }
        if (!op) return v;
        ap += strlen(op);
        long r = arith_expr(level + 1);
        switch (op[0] | op[1] << 8) {
        case '|' | '|' << 8: v = v || r; break;
        case '&' | '&' << 8: v = v && r; break;
        case '|': v |= r; break;
        case '^': v ^= r; break;
        case '&': v &= r; break;
        case '=' | '=' << 8: v = v == r; break;
        case '!' | '=' << 8: v = v != r; break;
        case '<' | '=' << 8: v = v <= r; break;
        case '>' | '=' << 8: v = v >= r; break;
        case '<': v = v < r; break;
        case '>': v = v > r; break;
        case '<' | '<' << 8: v = (long)((unsigned long)v << (r & 31)); break;
        case '>' | '>' << 8: v >>= r & 31; break;
        case '+': v += r; break;
        case '-': v -= r; break;
        case '*': v *= r; break;
        default:
            if (!r) {
                fputs("sh: arithmetic: division by zero\n", stderr);
                arith_err = 1;
                return 0;
            }
            v = op[0] == '/' ? v / r : v % r;
        }
    }
}

/* ------------------------------------------------------------------ expansion */

static void expand_text(struct exp *e, const char *p, const char *end, int word_start);

/* A fatal expansion error ends a script; an interactive shell just fails the command. */
static void expansion_failed(void) {
    if (!interactive) exit(2);
    cmdsub_status = 1;
}

/* One character that must stay literal. */
static void put_quoted(struct exp *e, int c) {
    if (c == '*' || c == '?' || c == '[' || c == '\\') sb_put(&e->cur, '\\');
    sb_put(&e->cur, c);
}

static void field_end(struct exp *e, int force) {
    if (e->mode != M_FIELDS || !(e->cur.n || e->qseen || force)) return;
    char *s = sb_take(&e->cur);
    e->qseen = 0;
    if (has_wildcard(s)) {
        struct vec m = { 0 };
        glob_walk(*s == '/' ? "/" : "", s + (*s == '/'), &m);
        if (m.n) {
            for (int i = 0; i < m.n; i++) vec_push(e->out, m.v[i]);
            free(m.v);
            free(s);
            return;
        }
    }
    unescape(s);
    vec_push(e->out, s);
}

/* Adds the value of an expansion: quoted text stays whole, unquoted text is split at IFS. */
static void add_text(struct exp *e, const char *s) {
    const char *ifs = var_get("IFS");
    if (!ifs) ifs = " \t\n";
    for (; *s; s++) {
        if (e->dq) {
            put_quoted(e, (unsigned char)*s);
            e->qseen = 1;
        } else if (e->mode != M_FIELDS || !strchr(ifs, *s)) {
            if (*s == '\\') sb_put(&e->cur, '\\');
            sb_put(&e->cur, *s);
        } else if (isspace((unsigned char)*s)) {
            field_end(e, 0);                    /* white space delimiters collapse */
        } else {
            field_end(e, 1);                    /* other delimiters keep empty fields */
        }
    }
}

static char *sub_expand(const char *p, const char *end, int mode, int dq) {
    struct exp e = { mode, dq, 0, 0, { 0 }, NULL };
    expand_text(&e, p, end, 0);
    char *s = sb_take(&e.cur);
    if (mode == M_STR) unescape(s);
    return s;
}

static char *dup_range(const char *p, const char *end) {
    char *s = xmalloc((size_t)(end - p) + 1);
    memcpy(s, p, (size_t)(end - p));
    s[end - p] = 0;
    return s;
}

static const char *param_value(const char *name, int len, char *tmp) {
    if (isdigit(*name)) {
        int i = 0;
        for (int k = 0; k < len && i < 100000; k++) i = i * 10 + name[k] - '0';
        return i == 0 ? sh_name : i <= pos.n ? pos.v[i - 1] : NULL;
    }
    switch (*name) {
    case '?': snprintf(tmp, 12, "%d", last_status); return tmp;
    case '#': snprintf(tmp, 12, "%d", pos.n); return tmp;
    case '$': snprintf(tmp, 12, "%d", getpid()); return tmp;
    case '!':
        if (!last_bg) return NULL;
        snprintf(tmp, 12, "%d", last_bg);
        return tmp;
    case '-': snprintf(tmp, 12, "%s%s", opt_errexit ? "e" : "", opt_xtrace ? "x" : ""); return tmp;
    }
    char buf[64];
    if (len >= (int)sizeof buf) return NULL;
    memcpy(buf, name, (size_t)len);
    buf[len] = 0;
    return var_get(buf);
}

/* $@ and $*: one field per parameter; one joined string inside quotes for $*, and wherever
 * fields do not exist (assignments, case words). */
static void expand_params(struct exp *e, int star) {
    if (e->mode != M_FIELDS || (e->dq && star)) {
        const char *ifs = var_get("IFS");
        int sep = star ? (ifs ? *ifs : ' ') : ' ';
        for (int i = 0; i < pos.n; i++) {
            if (i && sep) put_quoted(e, sep);
            add_text(e, pos.v[i]);
        }
        e->qseen = 1;
        return;
    }
    if (e->dq && !pos.n) e->zat = 1;
    for (int i = 0; i < pos.n; i++) {
        if (i) field_end(e, e->dq);
        add_text(e, pos.v[i]);
        if (e->dq) e->qseen = 1;
    }
}

/* ${val%pat} ${val%%pat} ${val#pat} ${val##pat}: drops the shortest or longest matching end. */
static void remove_affix(struct exp *e, const char *val, const char *pat, int suffix, int longest) {
    int len = (int)strlen(val), cut = -1;
    char *tmp = xmalloc((size_t)len + 1);
    for (int k = 0; k <= len; k++) {                    /* k characters would be removed */
        memcpy(tmp, suffix ? val + len - k : val, (size_t)k);
        tmp[k] = 0;
        if (!pattern_match(pat, tmp)) continue;
        cut = k;
        if (!longest) break;
    }
    if (cut < 0) {
        add_text(e, val);
    } else if (suffix) {
        memcpy(tmp, val, (size_t)(len - cut));
        tmp[len - cut] = 0;
        add_text(e, tmp);
    } else {
        add_text(e, val + cut);
    }
    free(tmp);
}

/* ${...}; p is after "${". Returns the text after the closing brace. */
static const char *expand_brace(struct exp *e, const char *p) {
    const char *close = skip_nested(p, '{', '}');
    if (!close) return p + strlen(p);
    const char *end = close - 1;
    int length = *p == '#' && p + 1 < end;
    p += length;
    const char *name = p;
    if (isdigit(*p)) while (isdigit(*p)) p++;
    else if (*p && strchr("@*#?$!-", *p)) p++;
    else while (isalnum(*p) || *p == '_') p++;
    int nl = (int)(p - name);
    char tmp[16];
    const char *val = nl ? param_value(name, nl, tmp) : NULL;
    if (length) {
        snprintf(tmp, sizeof tmp, "%d", (*name == '@' || *name == '*') ? pos.n : (int)strlen(val ? val : ""));
        add_text(e, tmp);
        return close;
    }
    if (p == end) {
        if (*name == '@' || *name == '*') expand_params(e, *name == '*');
        else if (val) add_text(e, val);
        return close;
    }
    int colon = *p == ':';
    p += colon;
    char op = *p++;
    int unset = !val || (colon && !*val);
    if (op == '%' || op == '#') {
        int longest = *p == op;
        p += longest;
        char *pat = sub_expand(p, end, M_PATTERN, 0);
        remove_affix(e, val ? val : "", pat, op == '%', longest);
        free(pat);
    } else if (op == '+') {
        if (!unset) { char *w = sub_expand(p, end, M_STR, e->dq); add_text(e, w); free(w); }
    } else if (op == '-' || op == '=' || op == '?') {
        if (!unset) {
            add_text(e, val);
        } else {
            char *w = sub_expand(p, end, M_STR, e->dq);
            if (op == '?') {
                fprintf(stderr, "sh: %.*s: %s\n", nl, name, *w ? w : "parameter null or not set");
                expansion_failed();
            }
            if (op == '=') {
                char *vn = dup_range(name, name + nl);
                if (var_valid(vn)) var_set(vn, w);
                free(vn);
            }
            add_text(e, w);
            free(w);
        }
    }
    return close;
}

static char *unbackslash(const char *p, const char *end) {      /* `...`: \` \\ \$ lose their backslash */
    char *s = xmalloc((size_t)(end - p) + 1), *o = s;
    for (; p < end; p++) {
        if (*p == '\\' && p + 1 < end && strchr("`\\$", p[1])) p++;
        *o++ = *p;
    }
    *o = 0;
    return s;
}

/* Handles the expansion starting at the '$' or '`' at p; returns the text after it. */
static const char *expand_dollar(struct exp *e, const char *p) {
    char tmp[16];
    if (*p == '`') {
        const char *q = p + 1;
        while (*q && *q != '`') q += *q == '\\' && q[1] ? 2 : 1;
        char *cmd = unbackslash(p + 1, q), *out = run_capture(cmd);
        add_text(e, out);
        free(cmd);
        free(out);
        return *q ? q + 1 : q;
    }
    p++;
    if (*p == '(') {
        const char *close = skip_nested(p + 1, '(', ')');
        if (!close) return p + strlen(p);
        if (p[1] == '(' && close[-2] == ')') {          /* $(( arithmetic )) */
            char *text = sub_expand(p + 2, close - 2, M_STR, 1);
            ap = text;
            arith_err = 0;
            long v = arith_expr(0);
            while (*ap == ' ') ap++;
            if (*ap || arith_err) {
                if (!arith_err) fprintf(stderr, "sh: arithmetic: syntax error: \"%s\"\n", text);
                expansion_failed();
                v = 0;
            }
            snprintf(tmp, sizeof tmp, "%ld", v);
            add_text(e, tmp);
            free(text);
        } else {
            char *cmd = dup_range(p + 1, close - 1), *out = run_capture(cmd);
            add_text(e, out);
            free(cmd);
            free(out);
        }
        return close;
    }
    if (*p == '{') return expand_brace(e, p + 1);
    if (*p == '@' || *p == '*') {
        expand_params(e, *p == '*');
        return p + 1;
    }
    const char *name = p;
    if (isdigit(*p) || (*p && strchr("?#$!-", *p))) p++;
    else while (isalnum(*p) || *p == '_') p++;
    if (p == name) {
        sb_put(&e->cur, '$');
        return p;
    }
    const char *v = param_value(name, (int)(p - name), tmp);
    if (v) add_text(e, v);
    return p;
}

static void expand_dq(struct exp *e, const char **pp, char term) {
    const char *p = *pp;
    e->dq = 1;
    while (*p && *p != term) {
        if (*p == '$' || *p == '`') {
            p = expand_dollar(e, p);
        } else if (*p == '\\' && p[1] && strchr(term ? "$`\"\\\n" : "$`\\\n", p[1])) {
            if (p[1] != '\n') put_quoted(e, p[1]);
            p += 2;
        } else {
            put_quoted(e, (unsigned char)*p++);
        }
    }
    e->dq = 0;
    *pp = p;
}

static void expand_text(struct exp *e, const char *p, const char *end, int word_start) {
    for (const char *w = p; p < end;) {
        if (*p == '\\') {
            if (p + 1 < end && p[1] != '\n') { put_quoted(e, (unsigned char)p[1]); e->qseen = 1; }
            else if (p + 1 >= end) put_quoted(e, '\\');
            p += p + 1 < end ? 2 : 1;
        } else if (*p == '\'') {
            e->qseen = 1;
            for (p++; p < end && *p != '\''; p++) put_quoted(e, (unsigned char)*p);
            p++;
        } else if (*p == '"') {
            e->zat = 0;
            const char *q = p + 1;
            expand_dq(e, &q, '"');
            if (!e->zat) e->qseen = 1;
            p = q + 1;
        } else if (*p == '$' || *p == '`') {
            p = expand_dollar(e, p);
        } else if (*p == '~' && word_start && p == w && (p + 1 == end || p[1] == '/')) {
            const char *home = var_get("HOME");
            if (home) add_text(e, home);
            p++;
        } else {
            sb_put(&e->cur, *p++);
        }
    }
}

/* ------------------------------------------------------------------ interface */

void expand_fields(const char *word, struct vec *out) {
    struct exp e = { M_FIELDS, 0, 0, 0, { 0 }, out };
    expand_text(&e, word, word + strlen(word), 1);
    field_end(&e, 0);
    free(e.cur.p);
}

char *expand_str(const char *word) {
    struct exp e = { M_STR, 0, 0, 0, { 0 }, NULL };
    expand_text(&e, word, word + strlen(word), 1);
    char *s = sb_take(&e.cur);
    unescape(s);
    return s;
}

char *expand_pattern(const char *word) {
    struct exp e = { M_PATTERN, 0, 0, 0, { 0 }, NULL };
    expand_text(&e, word, word + strlen(word), 0);
    return sb_take(&e.cur);
}

/* The body of an unquoted here-document: expansions, but quotes are literal. */
char *expand_heredoc(const char *text) {
    struct exp e = { M_STR, 1, 0, 0, { 0 }, NULL };
    const char *p = text;
    expand_dq(&e, &p, 0);
    char *s = sb_take(&e.cur);
    unescape(s);
    return s;
}
