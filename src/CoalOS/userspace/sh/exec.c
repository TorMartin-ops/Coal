/* Command evaluation: simple commands, pipelines, compound commands, functions, redirections
 * and the builtins. */

#include "sh.h"
#include "../coalbox/applets.h"

#define MAXRD     16            /* redirections applied to one in-process command */
#define MAX_DEPTH 200           /* nested function calls */
#define HEREDOC_MAX 4096        /* a here-document is written to a pipe before the command starts */
#define SAVE_FD   10            /* saved descriptors live from here up, close-on-exec */

int last_status, opt_errexit, opt_xtrace, interactive, last_bg;

static int cond_depth;          /* > 0 while a condition runs: set -e does not apply */
static int brk_n, cont_n;       /* pending break / continue levels */
static int ret_flag, ret_val, func_depth, loop_depth;

const char *const applet_names[] = {
#define NAME(n) #n,
    APPLET_LIST(NAME)
#undef NAME
};
const unsigned n_applets = sizeof applet_names / sizeof *applet_names;

struct func {
    struct func *next;
    char *name;
    struct node *body;
};
static struct func *funcs;

static int eval(struct node *n);

/* ------------------------------------------------------------------ processes */

static char *exit_trap;

static pid_t spawn(void) {
    fflush(stdout);                     /* the child must not repeat buffered output */
    pid_t pid = fork();
    if (!pid) exit_trap = NULL;         /* traps belong to the shell, not to its subshells */
    return pid;
}

static int wait_for(pid_t pid) {
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) return 1;
    return WIFSIGNALED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st);
}

/* Runs a shebang line's interpreter, or sh for a file without one (the kernel returned ENOEXEC). */
static void exec_script(const char *path, char **av) {
    char head[128];
    int fd = open(path, O_RDONLY), k = 0, n = 0, ac = 0;
    if (fd >= 0) {
        n = (int)read(fd, head, sizeof head - 1);
        close(fd);
    }
    const char *interp = "/bin/sh";
    char *arg = NULL;
    if (n > 2 && head[0] == '#' && head[1] == '!') {
        head[n] = 0;
        head[strcspn(head, "\n")] = 0;
        char *p = head + 2 + strspn(head + 2, " \t");
        interp = p;
        p += strcspn(p, " \t");
        if (*p) {
            *p++ = 0;
            p += strspn(p, " \t");
            if (*p) arg = p;
        }
    }
    while (av[ac]) ac++;
    char **argv = xmalloc((size_t)(ac + 4) * sizeof *argv);
    argv[k++] = (char *)(!strcmp(interp, "/bin/sh") ? "sh" : interp);
    if (arg) argv[k++] = arg;
    argv[k++] = (char *)path;
    for (int i = 1; i < ac; i++) argv[k++] = av[i];
    argv[k] = NULL;
    execve(interp, argv, environ);
    fprintf(stderr, "sh: %s: bad interpreter: %s\n", path, strerror(errno));
    _exit(126);
}

static void exec_path(const char *path, char **av) {
    execve(path, av, environ);
    if (errno == ENOEXEC) exec_script(path, av);
}

static int is_applet(const char *name) {
    for (unsigned i = 0; i < n_applets; i++)
        if (!strcmp(name, applet_names[i])) return 1;
    return 0;
}

/* Replaces the process with the command: a path, a program on PATH or a coalbox applet. */
static void exec_cmd(char **av) {
    const char *name = av[0];
    int err = ENOENT;
    if (strchr(name, '/')) {
        exec_path(name, av);
        err = errno;
    } else {
        const char *path = var_get("PATH");
        if (!path) path = "/bin:/usr/bin";
        size_t nl = strlen(name);
        while (*path) {
            const char *e = strchr(path, ':');
            size_t dl = e ? (size_t)(e - path) : strlen(path);
            char buf[512];
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
            int ac = 0;
            while (av[ac]) ac++;
            char **cv = xmalloc((size_t)(ac + 2) * sizeof *cv);
            cv[0] = "coalbox";
            memcpy(cv + 1, av, (size_t)(ac + 1) * sizeof *cv);
            execve("/bin/coalbox", cv, environ);
            err = errno;
        }
    }
    fprintf(stderr, "sh: %s: %s\n", name, err == ENOENT ? "command not found" : strerror(err));
    _exit(err == ENOENT ? 127 : 126);
}

/* ---------------------------------------------------------------- redirections */

struct saved {
    int fd, copy;               /* copy is -1 if fd was closed */
};

static void restore_redirs(struct saved *sv, int n) {
    fflush(stdout);
    while (n--) {
        if (sv[n].copy >= 0) {
            dup2(sv[n].copy, sv[n].fd);
            close(sv[n].copy);
        } else {
            close(sv[n].fd);
        }
    }
}

static int redir_error(const char *what, const char *why) {
    fprintf(stderr, "sh: %s: %s\n", what, why);
    return 1;
}

/* Opens the source of one redirection: a descriptor to dup2 onto r->fd, or -1 to close it. *own
 * says whether the descriptor must be closed afterwards. Returns -2 after printing an error. */
static int redir_source(const struct redir *r, int *own) {
    static const int flags[] = { O_RDONLY, O_WRONLY | O_CREAT | O_TRUNC, O_WRONLY | O_CREAT | O_APPEND };
    *own = 1;
    if (r->op <= RD_APP) {
        char *file = expand_str(r->word);
        int fd = open(file, flags[r->op], 0644);
        if (fd < 0) redir_error(file, strerror(errno));
        free(file);
        return fd < 0 ? -2 : fd;
    }
    if (r->op == RD_DUPIN || r->op == RD_DUPOUT) {
        char *w = expand_str(r->word), *end;
        long fd = strtol(w, &end, 10);
        int closing = !strcmp(w, "-"), bad = !closing && (end == w || *end);
        if (bad) redir_error(w, "bad file descriptor");
        free(w);
        *own = 0;
        return bad ? -2 : closing ? -1 : (int)fd;
    }
    char *body = r->quoted ? r->word : expand_heredoc(r->word);
    size_t len = strlen(body);
    int p[2], ok = len <= HEREDOC_MAX || (errno = EFBIG, 0);
    if (ok && (ok = pipe(p) == 0)) {
        if (write(p[1], body, len) == (ssize_t)len) {
            close(p[1]);
        } else {
            close(p[0]);
            close(p[1]);
            ok = 0;
        }
    }
    if (body != r->word) free(body);
    if (!ok) {
        redir_error("here-document", strerror(errno));
        return -2;
    }
    return p[0];
}

/* Applies redirections. With sv the previous descriptors are saved for restore_redirs. */
static int apply_redirs(const struct redir *r, struct saved *sv, int *nsv) {
    for (; r; r = r->next) {
        int own, src = redir_source(r, &own);
        if (src == -2) return 1;
        if (sv) {
            if (*nsv == MAXRD) return redir_error("redirection", "too many");
            sv[*nsv].fd = r->fd;
            sv[(*nsv)++].copy = fcntl(r->fd, F_DUPFD_CLOEXEC, SAVE_FD);
        }
        if (src < 0) close(r->fd);
        else if (src != r->fd) {
            if (dup2(src, r->fd) < 0) {
                redir_error("redirection", strerror(errno));
                if (own) close(src);
                return 1;
            }
            if (own) close(src);
        }
    }
    return 0;
}

/* -------------------------------------------------------------------- builtins */

char *read_all(int fd) {
    size_t n = 0, cap = 1024;
    char *buf = xmalloc(cap);
    ssize_t r;
    while ((r = read(fd, buf + n, cap - n - 1)) != 0) {
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        n += (size_t)r;
        if (cap - n < 512) buf = xrealloc(buf, cap *= 2);
    }
    buf[n] = 0;
    return buf;
}

static int b_colon(int argc, char **argv) { return 0; }
static int b_false(int argc, char **argv) { return 1; }
static int b_history(int argc, char **argv) { history_print(); return 0; }
static void run_exit_trap(void) {
    char *cmd = exit_trap;
    exit_trap = NULL;
    if (cmd) sh_run(cmd);
}

/* trap 'commands' EXIT, trap - EXIT: only the exit trap exists; signals are not trapped. */
static int b_trap(int argc, char **argv) {
    if (argc < 3 || (strcmp(argv[2], "EXIT") && strcmp(argv[2], "0")) || argc > 3) {
        fprintf(stderr, "sh: trap: usage: trap COMMANDS EXIT (other signals are not supported)\n");
        return 2;
    }
    if (!exit_trap && strcmp(argv[1], "-")) atexit(run_exit_trap);
    free(exit_trap);
    exit_trap = strcmp(argv[1], "-") ? xstrdup(argv[1]) : NULL;
    return 0;
}

static int b_test(int argc, char **argv) { fflush(stdout); return test_main(argc, argv); }

static int b_exit(int argc, char **argv) {
    exit(argc > 1 ? atoi(argv[1]) & 255 : last_status);
}

static int b_cd(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : var_get("HOME");
    int show = dir && !strcmp(dir, "-");
    if (show) dir = var_get("OLDPWD");
    if (!dir) dir = "/";
    char old[512], now[512];
    if (!getcwd(old, sizeof old)) old[0] = 0;
    if (chdir(dir) < 0) {
        fprintf(stderr, "sh: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    if (getcwd(now, sizeof now)) {
        var_set("OLDPWD", old);
        var_set("PWD", now);
        var_export("PWD");
        var_export("OLDPWD");
        if (show) puts(now);
    }
    return 0;
}

static int b_export(int argc, char **argv) {
    if (argc == 1) var_print(1);
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) *eq = 0;
        if (!var_valid(argv[i])) {
            fprintf(stderr, "sh: export: %s: bad variable name\n", argv[i]);
            return 1;
        }
        if (eq) var_set(argv[i], eq + 1);
        var_export(argv[i]);
    }
    return 0;
}

static int b_unset(int argc, char **argv) {
    int fn = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) { fn = 1; continue; }
        if (!strcmp(argv[i], "-v")) { fn = 0; continue; }
        if (!fn) { var_unset(argv[i]); continue; }
        for (struct func **pp = &funcs; *pp; pp = &(*pp)->next)
            if (!strcmp((*pp)->name, argv[i])) { *pp = (*pp)->next; break; }
    }
    return 0;
}

static int b_set(int argc, char **argv) {
    if (argc == 1) {
        var_print(0);
        return 0;
    }
    int i = 1, dashdash = 0;
    for (; i < argc && (argv[i][0] == '-' || argv[i][0] == '+') && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            dashdash = 1;
            i++;
            break;
        }
        for (const char *c = argv[i] + 1; *c; c++) {
            if (*c == 'e') opt_errexit = argv[i][0] == '-';
            else if (*c == 'x') opt_xtrace = argv[i][0] == '-';
            else {
                fprintf(stderr, "sh: set: %c%c: unsupported option\n", argv[i][0], *c);
                return 2;
            }
        }
    }
    if (dashdash || i < argc) set_params(argv + i, argc - i);
    return 0;
}

static int b_shift(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1;
    if (n < 0 || n > pos.n) {
        fprintf(stderr, "sh: shift: can't shift that many\n");
        return 1;
    }
    pos.v += n;
    pos.n -= n;
    return 0;
}

static int b_break(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1;
    if (n < 1) {
        fprintf(stderr, "sh: %s: bad number\n", argv[0]);
        return 1;
    }
    if (!loop_depth) return 0;
    if (argv[0][0] == 'b') brk_n = n;
    else cont_n = n;
    return 0;
}

static int b_return(int argc, char **argv) {
    ret_val = argc > 1 ? atoi(argv[1]) & 255 : last_status;
    ret_flag = 1;
    return ret_val;
}

static int b_eval(int argc, char **argv) {
    size_t len = 1;
    for (int i = 1; i < argc; i++) len += strlen(argv[i]) + 1;
    char *text = xmalloc(len), *o = text;
    *o = 0;
    for (int i = 1; i < argc; i++) o += sprintf(o, "%s%s", i > 1 ? " " : "", argv[i]);
    int st = sh_run(text);
    free(text);
    return st;
}

/* The file for `.`: a path as given, else the first match on PATH, else the current directory. */
static int open_source(const char *name) {
    if (strchr(name, '/')) return open(name, O_RDONLY);
    const char *path = var_get("PATH");
    while (path && *path) {
        const char *e = strchr(path, ':');
        size_t dl = e ? (size_t)(e - path) : strlen(path);
        char buf[512];
        snprintf(buf, sizeof buf, "%.*s/%s", (int)dl, path, name);
        int fd = open(buf, O_RDONLY);
        if (fd >= 0) return fd;
        path += dl + (e != NULL);
    }
    return open(name, O_RDONLY);
}

static int b_dot(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "sh: %s: filename argument required\n", argv[0]);
        return 2;
    }
    int fd = open_source(argv[1]);
    if (fd < 0) {
        fprintf(stderr, "sh: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    char *text = read_all(fd);
    close(fd);
    int st = sh_run(text);
    if (ret_flag) {
        st = ret_val;
        ret_flag = 0;
    }
    free(text);
    return st;
}

static int b_wait(int argc, char **argv) {
    int st = 0;
    if (argc == 1) {
        int s;
        while (waitpid(-1, &s, 0) > 0 || errno == EINTR) {}
        return 0;
    }
    for (int i = 1; i < argc; i++) st = wait_for(atoi(argv[i]));
    return st;
}

static int b_read(int argc, char **argv) {
    int i = 1, raw = argc > 1 && !strcmp(argv[1], "-r");
    i += raw;
    char *line = xmalloc(128);
    size_t n = 0, cap = 128;
    int eof = 0;
    for (;;) {
        char c;
        ssize_t r = read(0, &c, 1);             /* unbuffered: the next command sees the rest of the input */
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { eof = 1; break; }
        if (c == '\n') break;
        if (c == '\\' && !raw) {
            if (read(0, &c, 1) != 1) { eof = 1; break; }
            if (c == '\n') continue;
        }
        if (n + 2 > cap) line = xrealloc(line, cap *= 2);
        line[n++] = c;
    }
    line[n] = 0;
    const char *ifs = var_get("IFS");
    if (!ifs) ifs = " \t\n";
    char *p = line;
    int count = argc - i ? argc - i : 1;
    for (int k = 0; k < count; k++) {
        const char *name = argc - i ? argv[i + k] : "REPLY";
        int last = k == count - 1;
        while (*p && strchr(ifs, *p) && isspace((unsigned char)*p)) p++;
        char *start = p;
        if (last) {
            p += strlen(p);
            while (p > start && strchr(ifs, p[-1]) && isspace((unsigned char)p[-1])) p--;
        } else {
            while (*p && !strchr(ifs, *p)) p++;
        }
        char saved = *p;
        *p = 0;
        var_set(name, start);
        *p = saved;
        if (saved && !last) p++;
    }
    free(line);
    return eof;
}

static int b_exec(int argc, char **argv) {
    if (argc > 1) {
        fflush(stdout);
        environ = var_environ();
        exec_cmd(argv + 1);
    }
    return 0;
}

static const struct builtin {
    const char *name;
    int (*fn)(int, char **);
} builtins[] = {
    { ":", b_colon }, { ".", b_dot }, { "[", b_test }, { "break", b_break }, { "cd", b_cd },
    { "continue", b_break }, { "eval", b_eval }, { "exec", b_exec }, { "exit", b_exit },
    { "export", b_export }, { "false", b_false }, { "history", b_history }, { "read", b_read },
    { "return", b_return }, { "set", b_set }, { "shift", b_shift }, { "source", b_dot },
    { "test", b_test }, { "trap", b_trap }, { "true", b_colon }, { "unset", b_unset }, { "wait", b_wait },
};

const char *builtin_name(unsigned i) {
    return i < sizeof builtins / sizeof *builtins ? builtins[i].name : NULL;
}

static const struct builtin *find_builtin(const char *name) {
    for (unsigned i = 0; i < sizeof builtins / sizeof *builtins; i++)
        if (!strcmp(name, builtins[i].name)) return &builtins[i];
    return NULL;
}

/* ------------------------------------------------------------ simple commands */

static int is_assignment(const char *w) {
    if (!isalpha(*w) && *w != '_') return 0;
    while (isalnum(*w) || *w == '_') w++;
    return *w == '=';
}

/* Sets NAME=value from the source word w; the value is expanded without splitting. */
static void assign(const char *w) {
    const char *eq = strchr(w, '=');
    char *name = xstrdup(w), *val = expand_str(eq + 1);
    name[eq - w] = 0;
    if (opt_xtrace) fprintf(stderr, "+ %s=%s\n", name, val);
    var_set(name, val);
    free(name);
    free(val);
}

struct tmpvar {
    char *name, *old;           /* old is NULL if the variable was unset */
};

/* Assignments that only last for one builtin or function call. */
static int assign_temp(struct node *n, int count, struct tmpvar *tv) {
    for (int i = 0; i < count; i++) {
        const char *eq = strchr(n->w[i], '=');
        tv[i].name = xstrdup(n->w[i]);
        tv[i].name[eq - n->w[i]] = 0;
        const char *old = var_get(tv[i].name);
        tv[i].old = old ? xstrdup(old) : NULL;
        assign(n->w[i]);
    }
    return count;
}

static void restore_temp(struct tmpvar *tv, int count) {
    for (int i = 0; i < count; i++) {
        if (tv[i].old) var_set(tv[i].name, tv[i].old);
        else var_unset(tv[i].name);
        free(tv[i].name);
        free(tv[i].old);
    }
}

static void trace(char **v, int n) {
    fputs("+", stderr);
    for (int i = 0; i < n; i++) fprintf(stderr, " %s", v[i]);
    fputc('\n', stderr);
}

static int call_func(struct func *f, struct vec *argv) {
    if (func_depth >= MAX_DEPTH) {
        fprintf(stderr, "sh: %s: function nesting too deep\n", f->name);
        return 1;
    }
    struct params saved = pos;
    pos.v = argv->v + 1;
    pos.n = argv->n - 1;
    func_depth++;
    int st = eval(f->body);
    func_depth--;
    if (ret_flag) {
        st = ret_val;
        ret_flag = 0;
    }
    pos = saved;
    return st;
}

static void child_exec(struct node *n, int nassign, struct vec *argv) {
    signal(SIGINT, SIG_DFL);
    for (int i = 0; i < nassign; i++) {
        char *name = xstrdup(n->w[i]);
        name[strchr(n->w[i], '=') - n->w[i]] = 0;
        assign(n->w[i]);
        var_export(name);
        free(name);
    }
    if (apply_redirs(n->rd, NULL, NULL)) _exit(1);
    environ = var_environ();
    exec_cmd(argv->v);
}

/* forked: this process is already a pipeline stage or background job, so exec directly. */
static int run_simple(struct node *n, int forked) {
    struct vec argv = { 0 };
    int ia = 0;
    while (ia < n->nw && is_assignment(n->w[ia])) ia++;
    cmdsub_status = -1;
    for (int i = ia; i < n->nw; i++) expand_fields(n->w[i], &argv);
    int st = 0;
    if (!argv.n) {                                      /* only assignments and redirections */
        struct saved sv[MAXRD];
        int nsv = 0;
        for (int i = 0; i < ia; i++) assign(n->w[i]);
        if (apply_redirs(n->rd, sv, &nsv)) st = 1;
        else st = cmdsub_status > 0 ? cmdsub_status : 0;
        restore_redirs(sv, nsv);
        vec_free(&argv);
        return st;
    }
    if (opt_xtrace) trace(argv.v, argv.n);
    vec_push(&argv, NULL);
    argv.n--;
    const char *name = argv.v[0];
    struct func *f = NULL;
    for (f = funcs; f && strcmp(f->name, name); f = f->next) {}
    const struct builtin *b = f ? NULL : find_builtin(name);
    if (f || b) {
        struct saved sv[MAXRD];
        struct tmpvar *tv = xmalloc((size_t)(ia + 1) * sizeof *tv);
        int nsv = 0, ntv = assign_temp(n, ia, tv);
        if (apply_redirs(n->rd, sv, &nsv)) {
            st = 1;
        } else {
            fflush(stdout);
            st = f ? call_func(f, &argv) : b->fn(argv.n, argv.v);
        }
        if (b && b->fn == b_exec && argv.n == 1 && !st) {   /* exec with only redirections: they stay */
            for (int i = 0; i < nsv; i++) if (sv[i].copy >= 0) close(sv[i].copy);
        } else {
            restore_redirs(sv, nsv);
        }
        restore_temp(tv, ntv);
        free(tv);
        vec_free(&argv);
        return st;
    }
    if (forked) child_exec(n, ia, &argv);
    pid_t pid = spawn();
    if (pid < 0) {
        fprintf(stderr, "sh: fork: %s\n", strerror(errno));
        st = 1;
    } else if (!pid) {
        child_exec(n, ia, &argv);
    } else {
        st = wait_for(pid);
    }
    vec_free(&argv);
    return st;
}

/* ----------------------------------------------------------- pipelines and jobs */

/* Runs a node as a forked pipeline stage or background job and exits with its status. */
static void stage_main(struct node *c) {
    signal(SIGINT, SIG_DFL);
    exit(c->type == N_SIMPLE ? run_simple(c, 1) : eval(c));
}

static int run_pipeline(struct node *n) {
    int stages = 0;
    for (struct node *c = n->l; c; c = c->next) stages++;
    pid_t *pids = xmalloc((size_t)stages * sizeof *pids);
    int prev = -1, started = 0, failed = 0;
    for (struct node *c = n->l; c; c = c->next, started++) {
        int fds[2] = { -1, -1 };
        if (c->next && pipe(fds) < 0) {
            fprintf(stderr, "sh: pipe: %s\n", strerror(errno));
            failed = 1;
            break;
        }
        pid_t pid = spawn();
        if (pid < 0) {
            fprintf(stderr, "sh: fork: %s\n", strerror(errno));
            if (fds[0] >= 0) { close(fds[0]); close(fds[1]); }
            failed = 1;
            break;
        }
        if (!pid) {
            if (prev >= 0) { dup2(prev, 0); close(prev); }
            if (fds[1] >= 0) { dup2(fds[1], 1); close(fds[0]); close(fds[1]); }
            stage_main(c);
        }
        pids[started] = pid;
        if (prev >= 0) close(prev);
        if (fds[1] >= 0) close(fds[1]);
        prev = fds[0];
    }
    if (prev >= 0) close(prev);
    int st = 0;
    for (int i = 0; i < started; i++) st = wait_for(pids[i]);      /* the status is the last stage's */
    free(pids);
    return failed ? 1 : st;
}

static int run_background(struct node *n) {
    pid_t pid = spawn();
    if (pid < 0) {
        fprintf(stderr, "sh: fork: %s\n", strerror(errno));
        return 1;
    }
    if (!pid) {
        if (!interactive) {
            int fd = open("/dev/null", O_RDONLY);
            if (fd >= 0) { dup2(fd, 0); close(fd); }
        }
        stage_main(n->l);
    }
    last_bg = pid;
    if (interactive) fprintf(stderr, "[%d]\n", pid);
    return 0;
}

/* ------------------------------------------------------------ compound commands */

static int pending(void) {
    return brk_n || cont_n || ret_flag;
}

/* After a loop body: 1 if the loop must stop. */
static int loop_done(void) {
    if (ret_flag) return 1;
    if (brk_n) { brk_n--; return 1; }
    return cont_n && --cont_n;
}

static int eval_cond(struct node *n) {
    cond_depth++;
    int st = eval(n);
    cond_depth--;
    return st;
}

static int run_for(struct node *n) {
    struct vec items = { 0 };
    if (n->nw < 0) for (int i = 0; i < pos.n; i++) vec_push(&items, xstrdup(pos.v[i]));
    else for (int i = 0; i < n->nw; i++) expand_fields(n->w[i], &items);
    int st = 0;
    loop_depth++;
    for (int i = 0; i < items.n; i++) {
        var_set(n->name, items.v[i]);
        st = eval(n->l);
        if (loop_done()) break;
    }
    loop_depth--;
    vec_free(&items);
    return st;
}

static int run_while(struct node *n) {
    int st = 0, until = n->type == N_UNTIL;
    loop_depth++;
    for (;;) {
        int c = eval_cond(n->l);
        if (pending() || (c == 0) == until) break;
        st = eval(n->r);
        if (loop_done()) break;
    }
    loop_depth--;
    return st;
}

static int run_case(struct node *n) {
    char *subject = expand_str(n->w[0]);
    int st = 0;
    for (struct citem *it = n->items; it; it = it->next) {
        int hit = 0;
        for (int i = 0; i < it->npat && !hit; i++) {
            char *pat = expand_pattern(it->pat[i]);
            hit = pattern_match(pat, subject);
            free(pat);
        }
        if (hit) {
            st = eval(it->body);
            break;
        }
    }
    free(subject);
    return st;
}

static int run_subshell(struct node *n) {
    pid_t pid = spawn();
    if (pid < 0) {
        fprintf(stderr, "sh: fork: %s\n", strerror(errno));
        return 1;
    }
    if (!pid) {
        signal(SIGINT, SIG_DFL);
        exit(eval(n->l));
    }
    return wait_for(pid);
}

static int eval_node(struct node *n) {
    int st = 0;
    switch (n->type) {
    case N_SIMPLE: return run_simple(n, 0);
    case N_LIST:
        for (struct node *c = n->l; c && !pending(); c = c->next) st = eval(c);
        return st;
    case N_AND: case N_OR:
        st = eval_cond(n->l);
        if (!pending() && (n->type == N_AND) == (st == 0)) st = eval(n->r);
        return st;
    case N_NOT: return !eval_cond(n->l);
    case N_BG: return run_background(n);
    case N_PIPE: return run_pipeline(n);
    case N_SUBSHELL: return run_subshell(n);
    case N_GROUP: return eval(n->l);
    case N_IF:
        st = eval_cond(n->l);
        if (pending()) return st;
        if (!st) return eval(n->r);
        return n->x ? eval(n->x) : 0;
    case N_WHILE: case N_UNTIL: return run_while(n);
    case N_FOR: return run_for(n);
    case N_CASE: return run_case(n);
    case N_FUNC: {
        struct func *f = funcs;
        while (f && strcmp(f->name, n->name)) f = f->next;
        if (!f) {
            f = xmalloc(sizeof *f);
            f->name = xstrdup(n->name);
            f->next = funcs;
            funcs = f;
        }
        f->body = n->l;
        parse_keep();                   /* the body outlives the command that defined it */
        return 0;
    }
    }
    return 0;
}

static int eval(struct node *n) {
    struct saved sv[MAXRD];
    int nsv = 0, st;
    if (n->rd && n->type != N_SIMPLE) {         /* simple commands apply their own */
        fflush(stdout);
        if (apply_redirs(n->rd, sv, &nsv)) {
            restore_redirs(sv, nsv);
            return last_status = 1;
        }
    }
    st = eval_node(n);
    if (nsv) restore_redirs(sv, nsv);
    last_status = st;
    if (st && opt_errexit && !cond_depth && (n->type == N_SIMPLE || n->type == N_PIPE || n->type == N_SUBSHELL))
        exit(st);
    return st;
}

/* ------------------------------------------------------------------- interface */

int sh_run(const char *text) {
    struct parser *prev = parse_open(text);
    struct node *n;
    int st = 0, r;
    while ((r = parse_next(&n)) > 0) {
        st = eval(n);
        brk_n = cont_n = 0;
        if (ret_flag) break;
    }
    if (r < 0) {
        fprintf(stderr, "sh: %s%s\n", parse_error, parse_incomplete ? ": unexpected end of input" : "");
        st = last_status = 2;
    }
    parse_close(prev);
    return st;
}

int sh_incomplete(const char *text) {
    struct parser *prev = parse_open(text);
    struct node *n;
    int r;
    while ((r = parse_next(&n)) > 0) {}
    int incomplete = r < 0 && parse_incomplete;
    parse_close(prev);
    return incomplete;
}

char *run_capture(const char *text) {
    int fd[2];
    if (pipe(fd) < 0) return xstrdup("");
    pid_t pid = spawn();
    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        return xstrdup("");
    }
    if (!pid) {
        close(fd[0]);
        dup2(fd[1], 1);
        close(fd[1]);
        signal(SIGINT, SIG_DFL);
        interactive = 0;
        exit(sh_run(text));
    }
    close(fd[1]);
    char *out = read_all(fd[0]);
    close(fd[0]);
    cmdsub_status = wait_for(pid);
    size_t len = strlen(out);
    while (len && out[len - 1] == '\n') out[--len] = 0;
    return out;
}
