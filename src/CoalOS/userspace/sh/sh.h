/* sh: declarations shared by the parser, expander, evaluator and line editor. */
#ifndef SH_H
#define SH_H

#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/unistd.h>
#include <libc/fcntl.h>
#include <libc/errno.h>
#include <libc/ctype.h>
#include <libc/signal.h>
#include <libc/sys/stat.h>
#include <libc/sys/wait.h>

/* ---------------------------------------------------------------- syntax tree */

enum {
    N_SIMPLE, N_LIST, N_AND, N_OR, N_NOT, N_BG, N_PIPE, N_SUBSHELL, N_GROUP,
    N_IF, N_WHILE, N_UNTIL, N_FOR, N_CASE, N_FUNC
};

enum { RD_IN, RD_OUT, RD_APP, RD_HERE, RD_HERE_TAB, RD_DUPIN, RD_DUPOUT };

struct redir {
    struct redir *next;
    int op, fd;
    int quoted;                 /* here-document: delimiter was quoted, so the body is literal */
    char *word;                 /* target, or the here-document body */
};

struct citem {                  /* one `pattern|pattern) commands ;;` of a case */
    struct citem *next;
    char **pat;
    int npat;
    struct node *body;
};

/* Words are kept as source text; quotes and expansions are processed when the command runs.
 * N_SIMPLE: w. N_FOR: name, w (nw < 0: "$@"), l. N_CASE: w[0], items. N_FUNC: name, l.
 * N_IF: l cond, r then, x else. N_WHILE/N_UNTIL: l cond, r body. N_PIPE: l is the first stage,
 * stages are chained by next. N_LIST: l is the first command, chained by next. */
struct node {
    int type;
    struct node *next, *l, *r, *x;
    char **w;
    int nw;
    char *name;
    struct redir *rd;
    struct citem *items;
};

struct parser;

/* parse.c */
struct parser *parse_open(const char *text);            /* returns the previous parser to hand back */
void parse_close(struct parser *prev);
int parse_next(struct node **out);                      /* 1: command, 0: end of text, -1: error */
void parse_keep(void);                                  /* the tree just parsed must outlive the next parse */
const char *skip_nested(const char *p, char open, char close);   /* text after the matching close */
extern int parse_incomplete;                            /* the error is only that the text ends too early */
extern const char *parse_error;

/* vars.c */
struct params { char **v; int n; };
extern struct params pos;                               /* $1 ... */
extern const char *sh_name;                             /* $0 */
void set_params(char **v, int n);
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
const char *var_get(const char *name);
void var_set(const char *name, const char *val);
void var_export(const char *name);
void var_unset(const char *name);
int var_valid(const char *name);
char **var_environ(void);
void var_import(void);
void var_print(int only_exported);

/* expand.c */
struct vec { char **v; int n, cap; };
void vec_push(struct vec *v, char *s);
void vec_free(struct vec *v);
void expand_fields(const char *word, struct vec *out);  /* splitting and wildcards */
char *expand_str(const char *word);                     /* one string, no splitting */
char *expand_pattern(const char *word);                 /* one string, quoted wildcards escaped */
char *expand_heredoc(const char *text);

/* exec.c */
extern int last_status, opt_errexit, opt_xtrace, interactive;
extern int cmdsub_status;
extern int last_bg;                                     /* $! */
int sh_run(const char *text);                           /* runs every command in text */
char *read_all(int fd);                                 /* the rest of a descriptor, NUL terminated */
int sh_incomplete(const char *text);                    /* does text end inside a command? */
char *run_capture(const char *text);                    /* output of a command substitution */
const char *builtin_name(unsigned i);
extern const char *const applet_names[];
extern const unsigned n_applets;
int test_main(int argc, char **argv);
int pattern_match(const char *pat, const char *s);        /* coalbox/match.c */

/* edit.c */
int edit_line(const char *prompt, char *line);          /* 1: line, 0: end of input, 2: cancelled */
void history_print(void);
void history_add(const char *line);

#endif
