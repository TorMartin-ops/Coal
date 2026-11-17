/* sh: a small POSIX-style shell. Pipelines, redirections, && || ; &, quoting,
 * $VAR expansion, wildcards, history and line editing on a raw terminal. */

#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/unistd.h>
#include <libc/fcntl.h>
#include <libc/errno.h>
#include <libc/ctype.h>
#include <libc/signal.h>
#include <libc/termios.h>
#include <libc/dirent.h>
#include <libc/sys/stat.h>
#include <libc/sys/wait.h>
#include "../coalbox/applets.h"

#define LINE_MAX_  256
#define MAX_TOKENS 128
#define MAX_ARGS   128
#define MAX_CMDS   16
#define ARENA_SIZE 4096
#define HIST_SIZE  32
#define MAX_CAND   64

static int last_status;
static int interactive;
static char cwd[256];

/* ------------------------------------------------------------------ lexer */

enum { T_WORD, T_PIPE, T_AND, T_OR, T_SEMI, T_BG, T_IN, T_OUT, T_APP, T_ERR, T_ERRAPP, T_ERR2OUT };

struct tok {
    int type;
    int len;            /* T_WORD: the word's source text s[0..len), expanded when its command runs */
    const char *s;
};

static struct tok toks[MAX_TOKENS];
static int ntok;
static char arena[ARENA_SIZE];
static int apos, overflow;

static void put(char c) {
    if (apos < ARENA_SIZE - 1) arena[apos++] = c;
    else overflow = 1;
}

static void put_str(const char *s) {
    while (s && *s) put(*s++);
}

static void put_num(unsigned v) {
    char b[12];
    int n = 0;
    do b[n++] = (char)('0' + v % 10); while (v /= 10);
    while (n) put(b[--n]);
}

/* Expands the variable after the '$' at *pp. */
static void expand(const char **pp) {
    const char *p = *pp + 1;
    if (*p == '?') { put_num((unsigned)last_status); p++; }
    else if (*p == '$') { put_num((unsigned)getpid()); p++; }
    else if (*p == '{' || isalpha(*p) || *p == '_') {
        char name[64];
        int n = 0, brace = *p == '{';
        p += brace;
        while ((isalnum(*p) || *p == '_') && n < 63) name[n++] = *p++;
        name[n] = 0;
        if (brace && *p == '}') p++;
        put_str(getenv(name));
    } else {
        put('$');
    }
    *pp = p;
}

static int add_tok(int type, const char *s, int len) {
    if (ntok == MAX_TOKENS) return -1;
    toks[ntok].type = type;
    toks[ntok].s = s;
    toks[ntok++].len = len;
    return 0;
}

/* End of the word starting at p (quotes and escapes respected); NULL on an unterminated quote. */
static const char *skip_word(const char *p) {
    while (*p && !strchr(" \t|&;<>", *p)) {
        if (*p == '\'') {
            p = strchr(p + 1, '\'');
            if (!p) return NULL;
            p++;
        } else if (*p == '"') {
            for (p++; *p && *p != '"'; p += (*p == '\\' && p[1]) ? 2 : 1)
                ;
            if (!*p) return NULL;
            p++;
        } else {
            p += (*p == '\\' && p[1]) ? 2 : 1;
        }
    }
    return p;
}

/* Splits a line into tokens; returns -1 on an unterminated quote or too many tokens.
   Variables are not expanded here: `false; echo $?` must see the status of `false`. */
static int lex(const char *p) {
    ntok = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') break;
        int op = 0;
        switch (*p) {
        case '|': op = p[1] == '|' ? T_OR : T_PIPE; p += op == T_OR ? 2 : 1; break;
        case '&': op = p[1] == '&' ? T_AND : T_BG; p += op == T_AND ? 2 : 1; break;
        case ';': op = T_SEMI; p++; break;
        case '<': op = T_IN; p++; break;
        case '>': op = p[1] == '>' ? T_APP : T_OUT; p += op == T_APP ? 2 : 1; break;
        case '2':
            if (p[1] != '>') break;
            if (p[2] == '&' && p[3] == '1') { op = T_ERR2OUT; p += 4; }
            else if (p[2] == '>') { op = T_ERRAPP; p += 3; }
            else { op = T_ERR; p += 2; }
            break;
        }
        if (op) {
            if (add_tok(op, NULL, 0)) return -1;
            continue;
        }
        const char *end = skip_word(p);
        if (!end || add_tok(T_WORD, p, (int)(end - p))) return -1;
        p = end;
    }
    return 0;
}

/* Expands a word into the arena. Returns the string, or NULL if an unquoted expansion left it empty.
   *glob is set when it has an unquoted * or ?. */
static char *expand_word(const struct tok *t, int *glob) {
    const char *p = t->s, *end = t->s + t->len;
    char *w = arena + apos;
    int quoted = 0;
    *glob = 0;
    while (p < end) {
        if (*p == '\'') {
            quoted = 1;
            for (p++; *p != '\''; p++) put(*p);
            p++;
        } else if (*p == '"') {
            quoted = 1;
            for (p++; *p != '"';) {
                if (*p == '$') expand(&p);
                else if (*p == '\\' && strchr("\"\\$", p[1]) && p[1]) { put(p[1]); p += 2; }
                else put(*p++);
            }
            p++;
        } else if (*p == '\\' && p[1]) {
            quoted = 1;
            put(p[1]);
            p += 2;
        } else if (*p == '$') {
            expand(&p);
        } else if (*p == '~' && arena + apos == w && (p + 1 == end || p[1] == '/')) {
            put_str(getenv("HOME"));
            p++;
        } else {
            if (*p == '*' || *p == '?') *glob = 1;
            put(*p++);
        }
    }
    put(0);
    if (arena + apos - 1 == w && !quoted) { apos--; return NULL; }
    return w;
}

/* ---------------------------------------------------------------- globbing */

static char garena[2048];
static int gpos;

static int wild(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') {
        for (;; s++) {
            if (wild(p + 1, s)) return 1;
            if (!*s) return 0;
        }
    }
    return *s && (*p == '?' || *p == *s) && wild(p + 1, s + 1);
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int cmp_cand(const void *a, const void *b) {
    return strcmp(a, b);
}

/* Appends the matches of pattern to out; returns the count (0: no match). */
static int glob_word(const char *pat, char **out, int room) {
    const char *slash = strrchr(pat, '/');
    size_t dl = slash ? (size_t)(slash - pat) + 1 : 0;
    const char *base = pat + dl;
    char dir[128];
    if (dl >= sizeof dir) return 0;
    for (size_t i = 0; i < dl; i++)
        if (pat[i] == '*' || pat[i] == '?') return 0;      /* wildcards only in the last component */
    memcpy(dir, pat, dl);
    dir[dl] = 0;
    DIR *d = opendir(dl ? dir : ".");
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < room) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' && base[0] != '.') continue;
        if (!wild(base, e->d_name) || gpos + (int)(dl + len) + 1 > (int)sizeof garena) continue;
        out[n] = garena + gpos;
        memcpy(out[n], dir, dl);
        memcpy(out[n] + dl, e->d_name, len + 1);
        gpos += (int)(dl + len) + 1;
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, cmp_str);
    return n;
}

/* --------------------------------------------------------------- execution */

struct cmd {
    char **argv;        /* NULL terminated, after leading NAME=value words */
    char **assign;
    int nassign;
    const char *in, *out, *err;
    int out_append, err_append, err_to_out;
};

static char *argv_pool[MAX_ARGS + MAX_CMDS];
static int pool_n;
static const char *const applet_names[] = {
#define NAME(n) #n,
    APPLET_LIST(NAME)
#undef NAME
};
static const char *const builtins[] = { "cd", "exit", "export", "history", "unset" };

static int is_applet(const char *name) {
    for (unsigned i = 0; i < sizeof applet_names / sizeof *applet_names; i++)
        if (!strcmp(name, applet_names[i])) return 1;
    return 0;
}

static int is_builtin(const char *name) {
    for (unsigned i = 0; i < sizeof builtins / sizeof *builtins; i++)
        if (!strcmp(name, builtins[i])) return 1;
    return 0;
}

static int is_assignment(const char *w) {
    if (!isalpha(*w) && *w != '_') return 0;
    while (isalnum(*w) || *w == '_') w++;
    return *w == '=';
}

/* Builds commands from tokens [from, to); returns the count or -1 (syntax error). */
static int build_cmds(int from, int to, struct cmd *cmds) {
    int n = 0;
    pool_n = gpos = apos = overflow = 0;
    for (int i = from; i <= to; n++) {
        if (n == MAX_CMDS) return -1;
        struct cmd *c = &cmds[n];
        memset(c, 0, sizeof *c);
        char **words = argv_pool + pool_n;
        int nw = 0;
        for (; i < to && toks[i].type != T_PIPE; i++) {
            struct tok *t = &toks[i];
            if (t->type == T_WORD) {
                char *m[MAX_ARGS];
                int glob;
                char *w = expand_word(t, &glob);
                if (overflow) return -1;
                if (!w) continue;
                int k = glob && nw > 0 ? glob_word(w, m, MAX_ARGS - 2 - pool_n - nw) : 0;
                if (!k) { m[0] = w; k = 1; }
                if (pool_n + nw + k >= MAX_ARGS) return -1;
                for (int j = 0; j < k; j++) words[nw++] = m[j];
                continue;
            }
            if (t->type == T_ERR2OUT) { c->err_to_out = 1; continue; }
            if (i + 1 >= to || toks[i + 1].type != T_WORD) return -1;
            int fglob;
            const char *f = expand_word(&toks[++i], &fglob);
            if (overflow || !f) return -1;
            switch (t->type) {
            case T_IN: c->in = f; break;
            case T_OUT: c->out = f; c->out_append = 0; break;
            case T_APP: c->out = f; c->out_append = 1; break;
            case T_ERR: c->err = f; c->err_append = 0; break;
            case T_ERRAPP: c->err = f; c->err_append = 1; break;
            default: return -1;
            }
        }
        int na = 0;
        while (na < nw && is_assignment(words[na])) na++;
        c->assign = words;
        c->nassign = na;
        c->argv = words + na;
        words[nw++] = NULL;
        pool_n += nw;
        if (!c->argv[0] && !na && !c->in && !c->out && !c->err) return -1;
        i++;
    }
    return n;
}

static void exec_path(const char *path, char **av) {
    execve(path, av, environ);
    if (errno == ENOEXEC) {                     /* no shebang support: run it as a script */
        char *sv[MAX_ARGS + 2];
        int k = 0;
        sv[k++] = "sh";
        sv[k++] = (char *)path;
        for (int i = 1; av[i] && k < MAX_ARGS; i++) sv[k++] = av[i];
        sv[k] = NULL;
        execve("/bin/sh", sv, environ);
        errno = ENOEXEC;
    }
}

static void exec_cmd(char **av) {
    const char *name = av[0];
    int err = ENOENT;
    if (strchr(name, '/')) {
        exec_path(name, av);
        err = errno;
    } else {
        const char *path = getenv("PATH");
        if (!path) path = "/bin:/usr/bin";
        size_t nl = strlen(name);
        while (*path) {
            const char *e = strchr(path, ':');
            size_t dl = e ? (size_t)(e - path) : strlen(path);
            char buf[160];
            if (dl + nl + 2 <= sizeof buf) {
                memcpy(buf, path, dl);
                buf[dl] = '/';
                memcpy(buf + dl + 1, name, nl + 1);
                exec_path(buf, av);
                if (errno != ENOENT && errno != ENOTDIR) err = errno;
            }
            path += dl + (e != NULL);
        }
        if (is_applet(name)) {
            char *cv[MAX_ARGS + 2];
            int k = 0;
            cv[k++] = "coalbox";
            for (int i = 0; av[i] && k < MAX_ARGS; i++) cv[k++] = av[i];
            cv[k] = NULL;
            execve("/bin/coalbox", cv, environ);
            err = errno;
        }
    }
    fprintf(stderr, "sh: %s: %s\n", name, err == ENOENT ? "command not found" : strerror(err));
    _exit(err == ENOENT ? 127 : 126);
}

static void redirect(int fd, const char *file, int flags) {
    int f = open(file, flags, 0644);
    if (f < 0) {
        fprintf(stderr, "sh: %s: %s\n", file, strerror(errno));
        _exit(1);
    }
    dup2(f, fd);
    close(f);
}

static void child(struct cmd *c) {
    signal(SIGINT, SIG_DFL);
    for (int i = 0; i < c->nassign; i++) {
        char *eq = strchr(c->assign[i], '=');
        *eq = 0;
        setenv(c->assign[i], eq + 1, 1);
    }
    if (c->in) redirect(0, c->in, O_RDONLY);
    if (c->out) redirect(1, c->out, O_WRONLY | O_CREAT | (c->out_append ? O_APPEND : O_TRUNC));
    if (c->err) redirect(2, c->err, O_WRONLY | O_CREAT | (c->err_append ? O_APPEND : O_TRUNC));
    if (c->err_to_out) dup2(1, 2);
    if (!c->argv[0]) _exit(0);
    exec_cmd(c->argv);
}

static int wait_for(pid_t pid) {
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) return 1;
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return WEXITSTATUS(st);
}

static void set_cwd(void) {
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "?");
}

static void print_history(void);

static int run_builtin(char **av) {
    const char *c = av[0];
    if (!strcmp(c, "exit")) {
        exit(av[1] ? atoi(av[1]) : last_status);
    } else if (!strcmp(c, "cd")) {
        const char *dir = av[1] ? av[1] : getenv("HOME");
        if (dir && !strcmp(dir, "-")) dir = getenv("OLDPWD");
        if (!dir) dir = "/";
        char old[256];
        strcpy(old, cwd);
        if (chdir(dir) < 0) {
            fprintf(stderr, "sh: cd: %s: %s\n", dir, strerror(errno));
            return 1;
        }
        set_cwd();
        setenv("OLDPWD", old, 1);
        setenv("PWD", cwd, 1);
    } else if (!strcmp(c, "export")) {
        if (!av[1]) for (char **e = environ; e && *e; e++) printf("export %s\n", *e);
        for (int i = 1; av[i]; i++) {
            char *eq = strchr(av[i], '=');
            if (eq) { *eq = 0; setenv(av[i], eq + 1, 1); }
        }
    } else if (!strcmp(c, "unset")) {
        for (int i = 1; av[i]; i++) unsetenv(av[i]);
    } else {
        print_history();
    }
    return 0;
}

static int run_pipeline(int from, int to, int background) {
    struct cmd cmds[MAX_CMDS];
    int n = build_cmds(from, to, cmds);
    if (n < 0) {
        fprintf(stderr, "sh: syntax error\n");
        return 2;
    }
    if (n == 1 && !background) {
        struct cmd *c = &cmds[0];
        if (c->argv[0] && is_builtin(c->argv[0])) return run_builtin(c->argv);
        if (!c->argv[0] && c->nassign && !c->in && !c->out && !c->err) {
            for (int i = 0; i < c->nassign; i++) {
                char *eq = strchr(c->assign[i], '=');
                *eq = 0;
                setenv(c->assign[i], eq + 1, 1);
            }
            return 0;
        }
    }
    pid_t pids[MAX_CMDS];
    int prev = -1, status = 0;
    fflush(stdout);                 /* the children must not inherit buffered output */
    for (int i = 0; i < n; i++) {
        int fds[2] = { -1, -1 };
        if (i + 1 < n && pipe(fds) < 0) {
            fprintf(stderr, "sh: pipe: %s\n", strerror(errno));
            status = 1;
            n = i;
            break;
        }
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "sh: fork: %s\n", strerror(errno));
            status = 1;
            n = i;
            if (fds[0] >= 0) { close(fds[0]); close(fds[1]); }
            break;
        }
        if (pid == 0) {
            if (prev >= 0) { dup2(prev, 0); close(prev); }
            if (fds[1] >= 0) { dup2(fds[1], 1); close(fds[0]); close(fds[1]); }
            child(&cmds[i]);
        }
        pids[i] = pid;
        if (prev >= 0) close(prev);
        if (fds[1] >= 0) close(fds[1]);
        prev = fds[0];
    }
    if (prev >= 0) close(prev);
    if (background) {
        if (interactive && n) fprintf(stderr, "[%d]\n", pids[n - 1]);
        return 0;
    }
    for (int i = 0; i < n; i++) status = wait_for(pids[i]);
    return status;
}

static int is_list_op(int t) {
    return t == T_AND || t == T_OR || t == T_SEMI || t == T_BG;
}

static void run_line(const char *line) {
    if (lex(line) < 0) {
        fprintf(stderr, "sh: syntax error: unterminated quote or line too long\n");
        last_status = 2;
        return;
    }
    int skip = 0;
    for (int i = 0; i < ntok;) {
        int j = i;
        while (j < ntok && !is_list_op(toks[j].type)) j++;
        int op = j < ntok ? toks[j].type : T_SEMI;
        if (j > i || op != T_SEMI) {
            if (!skip) last_status = run_pipeline(i, j, op == T_BG);
            skip = (op == T_AND && last_status) || (op == T_OR && !last_status);
        }
        i = j + 1;
    }
}

/* ------------------------------------------------------------ line editing */

static char hist[HIST_SIZE][LINE_MAX_];
static int hist_n;

static void history_add(const char *line) {
    if (!*line || (hist_n && !strcmp(hist[hist_n - 1], line))) return;
    if (hist_n == HIST_SIZE) {
        memmove(hist[0], hist[1], sizeof hist - sizeof hist[0]);
        hist_n--;
    }
    strcpy(hist[hist_n++], line);
}

static void print_history(void) {
    for (int i = 0; i < hist_n; i++) printf("%4d  %s\n", i + 1, hist[i]);
}

static struct termios cooked;
static char buf[LINE_MAX_];
static int len, pos;
static char prompt[300];

static void out(const char *s, int n) {
    write(1, s, (size_t)n);
}

static void out_rep(char c, int n) {
    char tmp[16];
    memset(tmp, c, sizeof tmp);
    while (n > 0) {
        int k = n < (int)sizeof tmp ? n : (int)sizeof tmp;
        out(tmp, k);
        n -= k;
    }
}

/* Reprints buf[from..len) plus `erase` blanks, then returns the cursor to pos. */
static void redraw_tail(int from, int erase) {
    out(buf + from, len - from);
    out_rep(' ', erase);
    out_rep('\b', len + erase - pos);
}

static void insert(const char *s, int n) {
    if (len + n >= LINE_MAX_) return;
    memmove(buf + pos + n, buf + pos, (size_t)(len - pos));
    memcpy(buf + pos, s, (size_t)n);
    len += n;
    pos += n;
    out(buf + pos - n, n);
    redraw_tail(pos, 0);
}

static void remove_range(int from, int to) {       /* deletes buf[from..to), cursor at or after `from` */
    out_rep('\b', pos - from);
    memmove(buf + from, buf + to, (size_t)(len - to));
    len -= to - from;
    pos = from;
    redraw_tail(pos, to - from);
}

static void set_line(const char *s) {
    int old = len;
    out_rep('\b', pos);
    len = pos = (int)strlen(s);
    memcpy(buf, s, (size_t)len);
    out(buf, len);
    if (old > len) {
        out_rep(' ', old - len);
        out_rep('\b', old - len);
    }
}

static void redraw_all(void) {
    out(prompt, (int)strlen(prompt));
    out(buf, len);
    out_rep('\b', len - pos);
}

/* --- tab completion --- */

static char cand[MAX_CAND][64];
static int ncand;

static void add_cand(const char *name, int dir) {
    size_t l = strlen(name);
    if (ncand == MAX_CAND || l + 2 > sizeof cand[0]) return;
    for (int i = 0; i < ncand; i++)
        if (!strncmp(cand[i], name, l) && (!cand[i][l] || (cand[i][l] == '/' && !cand[i][l + 1]))) return;
    memcpy(cand[ncand], name, l);
    cand[ncand][l] = dir ? '/' : 0;
    cand[ncand++][l + (dir != 0)] = 0;
}

static void scan_dir(const char *dir, const char *prefix, int cmds) {
    DIR *d = opendir(dir);
    if (!d) return;
    size_t pl = strlen(prefix);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, prefix, pl) || (e->d_name[0] == '.' && prefix[0] != '.')) continue;
        if (cmds) {
            add_cand(e->d_name, 0);
        } else {
            char full[160];
            struct stat st;
            snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
            add_cand(e->d_name, stat(full, &st) == 0 && S_ISDIR(st.st_mode));
        }
    }
    closedir(d);
}

static void complete(void) {
    int ws = pos;
    while (ws > 0 && !strchr(" \t|;&<>", buf[ws - 1])) ws--;
    char word[96];
    int wl = pos - ws;
    if (wl >= (int)sizeof word) return;
    memcpy(word, buf + ws, (size_t)wl);
    word[wl] = 0;
    int first = 1;
    for (int i = ws - 1; i >= 0; i--) {
        if (buf[i] == ' ' || buf[i] == '\t') continue;
        first = strchr("|;&", buf[i]) != NULL;
        break;
    }
    ncand = 0;
    const char *slash = strrchr(word, '/');
    const char *prefix = slash ? slash + 1 : word;
    if (first && !slash) {
        size_t pl = strlen(prefix);
        for (unsigned i = 0; i < sizeof builtins / sizeof *builtins; i++)
            if (!strncmp(builtins[i], prefix, pl)) add_cand(builtins[i], 0);
        for (unsigned i = 0; i < sizeof applet_names / sizeof *applet_names; i++)
            if (!strncmp(applet_names[i], prefix, pl)) add_cand(applet_names[i], 0);
        const char *path = getenv("PATH");
        while (path && *path) {
            const char *e = strchr(path, ':');
            size_t dl = e ? (size_t)(e - path) : strlen(path);
            char dir[96];
            snprintf(dir, sizeof dir, "%.*s", (int)dl, path);
            scan_dir(dir, prefix, 1);
            path += dl + (e != NULL);
        }
    } else {
        char dir[96];
        if (!slash) strcpy(dir, ".");
        else if (slash == word) strcpy(dir, "/");
        else snprintf(dir, sizeof dir, "%.*s", (int)(slash - word), word);
        scan_dir(dir, prefix, 0);
    }
    if (!ncand) { out("\a", 1); return; }
    qsort(cand, (size_t)ncand, sizeof cand[0], cmp_cand);

    int common = (int)strlen(cand[0]), pl = (int)strlen(prefix);
    for (int i = 1; i < ncand; i++) {
        int k = 0;
        while (k < common && cand[0][k] == cand[i][k]) k++;
        common = k;
    }
    if (common > pl) {
        insert(cand[0] + pl, common - pl);
    } else if (ncand == 1) {
        /* nothing to add */
    } else {
        out("\n", 1);
        int col = 0;
        for (int i = 0; i < ncand; i++) {
            int l = (int)strlen(cand[i]) + 2;
            if (col + l > 79) { out("\n", 1); col = 0; }
            out(cand[i], l - 2);
            out("  ", 2);
            col += l;
        }
        out("\n", 1);
        redraw_all();
        return;
    }
    if (ncand == 1 && cand[0][common - 1] != '/') insert(" ", 1);
}

/* Reads one line in raw mode. Returns 0 on end of input, 1 otherwise. */
static int read_line(char *line) {
    struct termios raw = cooked;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);
    len = pos = 0;
    int hi = hist_n, result = 1;
    char saved[LINE_MAX_] = "";
    out(prompt, (int)strlen(prompt));
    for (;;) {
        unsigned char c;
        ssize_t r = read(0, &c, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { result = 0; break; }
        if (c == '\r' || c == '\n') break;
        if (c == 3) {                                   /* Ctrl-C */
            out("^C", 2);
            len = pos = 0;
            break;
        } else if (c == 4) {                            /* Ctrl-D */
            if (!len) { result = 0; break; }
            if (pos < len) remove_range(pos, pos + 1);
        } else if (c == 1) {                            /* Ctrl-A */
            out_rep('\b', pos);
            pos = 0;
        } else if (c == 5) {                            /* Ctrl-E */
            out(buf + pos, len - pos);
            pos = len;
        } else if (c == 21) {                           /* Ctrl-U */
            remove_range(0, pos);
        } else if (c == 11) {                           /* Ctrl-K */
            remove_range(pos, len);
        } else if (c == 23) {                           /* Ctrl-W */
            int s = pos;
            while (s > 0 && buf[s - 1] == ' ') s--;
            while (s > 0 && buf[s - 1] != ' ') s--;
            remove_range(s, pos);
        } else if (c == 12) {                           /* Ctrl-L */
            out("\033[H\033[2J", 7);
            redraw_all();
        } else if (c == 127 || c == 8) {                /* Backspace */
            if (pos > 0) remove_range(pos - 1, pos);
        } else if (c == '\t') {
            complete();
        } else if (c == 27) {                           /* escape sequence */
            unsigned char s[2] = { 0, 0 };
            if (read(0, &s[0], 1) != 1) continue;
            if (s[0] != '[' && s[0] != 'O') continue;
            if (read(0, &s[1], 1) != 1) continue;
            if (s[1] >= '0' && s[1] <= '9') {
                unsigned char t;
                if (read(0, &t, 1) != 1) continue;      /* the closing '~' */
                if (s[1] == '3' && pos < len) remove_range(pos, pos + 1);
                else if (s[1] == '1' || s[1] == '7') { out_rep('\b', pos); pos = 0; }
                else if (s[1] == '4' || s[1] == '8') { out(buf + pos, len - pos); pos = len; }
                continue;
            }
            switch (s[1]) {
            case 'A':
                if (hi > 0) {
                    if (hi == hist_n) { memcpy(saved, buf, (size_t)len); saved[len] = 0; }
                    set_line(hist[--hi]);
                }
                break;
            case 'B':
                if (hi < hist_n) {
                    hi++;
                    set_line(hi == hist_n ? saved : hist[hi]);
                }
                break;
            case 'C': if (pos < len) { out(buf + pos, 1); pos++; } break;
            case 'D': if (pos > 0) { out("\b", 1); pos--; } break;
            case 'H': out_rep('\b', pos); pos = 0; break;
            case 'F': out(buf + pos, len - pos); pos = len; break;
            }
        } else if (c >= 32 && c < 127) {
            char ch = (char)c;
            insert(&ch, 1);
        }
    }
    tcsetattr(0, TCSANOW, &cooked);
    out("\n", 1);
    buf[len] = 0;
    memcpy(line, buf, (size_t)len + 1);
    return result;
}

static void make_prompt(void) {
    const char *user = getenv("USER"), *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    const char *shown = cwd;
    char tilde[260];
    if (hl > 1 && !strncmp(cwd, home, hl) && (!cwd[hl] || cwd[hl] == '/')) {
        snprintf(tilde, sizeof tilde, "~%s", cwd + hl);
        shown = tilde;
    }
    snprintf(prompt, sizeof prompt, "%s@coal:%s$ ", user ? user : "user", shown);
}

static void print_motd(void) {
    char b[512];
    int fd = open("/etc/motd", O_RDONLY);
    if (fd < 0) return;
    ssize_t n;
    while ((n = read(fd, b, sizeof b)) > 0) write(1, b, (size_t)n);
    close(fd);
}

static void on_sigint(int sig) {}

int main(int argc, char **argv) {
    const char *command = NULL, *script = NULL;
    int login = argv[0][0] == '-';
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) { command = argv[++i]; break; }
        if (!strcmp(argv[i], "-l")) login = 1;
        else { script = argv[i]; break; }
    }
    set_cwd();
    signal(SIGINT, on_sigint);                  /* survive Ctrl-C; children reset it on exec */
    if (command) {
        run_line(command);
        return last_status;
    }
    if (script) {
        int fd = open(script, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "sh: %s: %s\n", script, strerror(errno));
            return 127;
        }
        dup2(fd, 0);
        close(fd);
    }

    interactive = !script && tcgetattr(0, &cooked) == 0;
    if (!interactive) {
        char line[LINE_MAX_ * 2];
        while (fgets(line, sizeof line, stdin)) {
            line[strlen(line) - (line[strlen(line) - 1] == '\n')] = 0;
            run_line(line);
        }
        return last_status;
    }
    if (login) print_motd();
    char line[LINE_MAX_];
    for (;;) {
        while (waitpid(-1, NULL, WNOHANG) > 0) {}      /* reap finished background jobs */
        make_prompt();
        if (!read_line(line)) {
            out("exit\n", 5);
            return last_status;
        }
        history_add(line);
        run_line(line);
        set_cwd();
    }
}
