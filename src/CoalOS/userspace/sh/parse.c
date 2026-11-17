/* Lexer and recursive-descent parser. Words stay as source text; the tree lives in an arena that
 * is released when the next command is parsed (unless it defined a function). */

#include "sh.h"

#define MAXW    256             /* words in one simple command */
#define MAXHERE 8               /* here-documents pending on one line */
#define BLOCK   4096

enum { T_EOF, T_NL, T_WORD, T_IONUM, T_SEMI, T_DSEMI, T_AMP, T_AND, T_OR, T_PIPE, T_LP, T_RP,
       T_LT, T_GT, T_DGT, T_DLT, T_DLTD, T_LTAMP, T_GTAMP };

struct ablk {
    struct ablk *next;
    size_t used;
    char data[BLOCK];
};

struct parser {
    const char *p;
    struct ablk *blocks;
    int keep;
    struct redir *here[MAXHERE];
    int nhere;
    struct parser *prev;
};

static struct parser *P;
static struct { int type, quoted, num; char *s; } tok;
int parse_incomplete;
const char *parse_error;

/* --------------------------------------------------------------------- arena */

static void *anew(size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (!P->blocks || P->blocks->used + n > BLOCK) {
        struct ablk *b = xmalloc(n > BLOCK ? sizeof *b - BLOCK + n : sizeof *b);
        b->next = P->blocks;
        b->used = 0;
        P->blocks = b;
    }
    void *r = P->blocks->data + P->blocks->used;
    P->blocks->used += n;
    return memset(r, 0, n);
}

static char *astrn(const char *s, size_t n) {
    char *r = anew(n + 1);
    memcpy(r, s, n);
    return r;
}

static void release(void) {
    while (P->blocks && !P->keep) {
        struct ablk *b = P->blocks;
        P->blocks = b->next;
        free(b);
    }
    P->blocks = NULL;       /* a kept tree is never freed */
    P->keep = 0;
}

void parse_keep(void) {
    if (P) P->keep = 1;
}

struct parser *parse_open(const char *text) {
    struct parser *np = xmalloc(sizeof *np), *prev = P;
    memset(np, 0, sizeof *np);
    np->p = text;
    np->prev = prev;
    P = np;
    return prev;
}

void parse_close(struct parser *prev) {
    release();
    free(P);
    P = prev;
}

/* --------------------------------------------------------------------- lexer */

static int fail(const char *msg, int incomplete) {
    if (!parse_error) {
        parse_error = msg;
        parse_incomplete = incomplete;
    }
    return 0;
}

/* The skip_* helpers take the text after the opening character and return the text after the
 * closing one, or NULL when the input ends first. */
static const char *skip_dq(const char *p);
static const char *skip_bq(const char *p);

static const char *skip_sq(const char *p) {
    p = strchr(p, '\'');
    return p ? p + 1 : NULL;
}

const char *skip_nested(const char *p, char open, char close) {
    for (int depth = 1; *p;) {
        if (*p == '\'') p = skip_sq(p + 1);
        else if (*p == '"') p = skip_dq(p + 1);
        else if (*p == '`') p = skip_bq(p + 1);
        else if (*p == '\\' && p[1]) p += 2;
        else if (*p == open) { depth++; p++; }
        else if (*p == close && !--depth) return p + 1;
        else p++;
        if (!p) return NULL;
    }
    return NULL;
}

static const char *skip_dq(const char *p) {
    while (*p) {
        if (*p == '"') return p + 1;
        if (*p == '\\' && p[1]) p += 2;
        else if (*p == '`') p = skip_bq(p + 1);
        else if (*p == '$' && p[1] == '(') p = skip_nested(p + 2, '(', ')');
        else if (*p == '$' && p[1] == '{') p = skip_nested(p + 2, '{', '}');
        else p++;
        if (!p) return NULL;
    }
    return NULL;
}

static const char *skip_bq(const char *p) {
    for (; *p; p += *p == '\\' && p[1] ? 2 : 1)
        if (*p == '`') return p + 1;
    return NULL;
}

/* End of the word starting at p, or NULL if a quote or expansion is left open. */
static const char *scan_word(const char *p, int *quoted) {
    while (*p && !strchr(" \t\n|&;<>()", *p)) {
        if (*p == '\'' || *p == '"') {
            *quoted = 1;
            p = *p == '"' ? skip_dq(p + 1) : skip_sq(p + 1);
        } else if (*p == '`') {
            p = skip_bq(p + 1);
        } else if (*p == '\\' && p[1]) {
            *quoted = 1;
            p += 2;
        } else if (*p == '$' && p[1] == '(') {
            p = skip_nested(p + 2, '(', ')');
        } else if (*p == '$' && p[1] == '{') {
            p = skip_nested(p + 2, '{', '}');
        } else {
            p++;
        }
        if (!p) return NULL;
    }
    return p;
}

/* Reads the bodies of the pending here-documents; p is just after the newline. */
static const char *read_heredocs(const char *p) {
    for (int i = 0; i < P->nhere; i++) {
        struct redir *r = P->here[i];
        const char *delim = r->word;
        size_t dl = strlen(delim), cap = 64, len = 0;
        char *body = xmalloc(cap);
        int found = 0;
        while (*p && !found) {
            const char *e = strchr(p, '\n');
            size_t ll = e ? (size_t)(e - p) : strlen(p);
            const char *line = p;
            if (r->op == RD_HERE_TAB)
                while (ll && *line == '\t') line++, ll--;
            p += (e ? e - p + 1 : (long)strlen(p));
            if (ll == dl && !memcmp(line, delim, dl)) {
                found = 1;
                break;
            }
            if (len + ll + 2 > cap) body = xrealloc(body, cap = (len + ll + 2) * 2);
            memcpy(body + len, line, ll);
            len += ll;
            body[len++] = '\n';
        }
        r->word = astrn(body, len);
        free(body);
        if (!found) fail("here-document: unexpected end of input", 1);
    }
    P->nhere = 0;
    return p;
}

static void advance(void) {
    const char *p = P->p;
    for (;;) {
        if (*p == ' ' || *p == '\t') p++;
        else if (p[0] == '\\' && p[1] == '\n') p += 2;
        else break;
    }
    if (*p == '#')
        while (*p && *p != '\n') p++;
    tok.quoted = 0;
    tok.s = NULL;
    int op = -1;
    switch (*p) {
    case 0: op = T_EOF; break;
    case '\n':
        op = T_NL;
        p++;
        if (P->nhere) p = read_heredocs(p);
        P->p = p;
        tok.type = op;
        return;
    case '|': op = p[1] == '|' ? T_OR : T_PIPE; break;
    case '&': op = p[1] == '&' ? T_AND : T_AMP; break;
    case ';': op = p[1] == ';' ? T_DSEMI : T_SEMI; break;
    case '(': op = T_LP; break;
    case ')': op = T_RP; break;
    case '<':
        op = p[1] == '<' ? (p[2] == '-' ? T_DLTD : T_DLT) : p[1] == '&' ? T_LTAMP : T_LT;
        break;
    case '>': op = p[1] == '>' ? T_DGT : p[1] == '&' ? T_GTAMP : T_GT; break;
    }
    if (op >= 0) {
        static const char len[] = { [T_EOF] = 0, [T_PIPE] = 1, [T_AMP] = 1, [T_SEMI] = 1, [T_LP] = 1, [T_RP] = 1,
            [T_LT] = 1, [T_GT] = 1, [T_OR] = 2, [T_AND] = 2, [T_DSEMI] = 2, [T_DGT] = 2, [T_DLT] = 2,
            [T_LTAMP] = 2, [T_GTAMP] = 2, [T_DLTD] = 3 };
        p += len[op];
        if (op == T_GT && *p == '|') p++;       /* >| is > without noclobber */
        P->p = p;
        tok.type = op;
        return;
    }
    const char *q = p;
    while (isdigit(*q)) q++;
    if (q > p && (*q == '<' || *q == '>')) {            /* 2>file */
        tok.num = atoi(p);
        tok.type = T_IONUM;
        P->p = q;
        return;
    }
    const char *end = scan_word(p, &tok.quoted);
    if (!end) {
        fail("unterminated quote or expansion", 1);
        tok.type = T_EOF;
        P->p = p + strlen(p);
        return;
    }
    tok.type = T_WORD;
    tok.s = astrn(p, (size_t)(end - p));
    P->p = end;
}

/* -------------------------------------------------------------------- parser */

static struct node *parse_seq(int top);
static struct node *parse_command(void);

static struct node *mk(int type) {
    struct node *n = anew(sizeof *n);
    n->type = type;
    return n;
}

static struct node *syntax_error(void) {
    fail("syntax error", tok.type == T_EOF);
    return NULL;
}

static int is_kw(const char *k) {
    return tok.type == T_WORD && !tok.quoted && !strcmp(tok.s, k);
}

static int at_closer(void) {
    static const char *const kw[] = { "then", "elif", "else", "fi", "do", "done", "esac", "}" };
    for (unsigned i = 0; i < sizeof kw / sizeof *kw; i++)
        if (is_kw(kw[i])) return 1;
    return 0;
}

static int end_of_list(void) {
    return tok.type == T_EOF || tok.type == T_RP || tok.type == T_DSEMI || at_closer();
}

static int expect(const char *kw) {
    if (!is_kw(kw)) return syntax_error() != NULL;
    advance();
    return 1;
}

static void skip_newlines(void) {
    while (tok.type == T_NL) advance();
}

/* A non-empty command list for the body of a compound command. */
static struct node *parse_body(void) {
    struct node *l = parse_seq(0);
    if (l && !l->l) return syntax_error();
    return l;
}

static char **copy_words(char **w, int n) {
    char **r = anew((size_t)(n + 1) * sizeof *r);
    memcpy(r, w, (size_t)n * sizeof *r);
    return r;
}

/* Parses one redirection if the next token starts one: 1 = yes, 0 = no, -1 = error. */
static int parse_redir(struct redir ***tail) {
    int fd = -1;
    if (tok.type == T_IONUM) {
        fd = tok.num;
        advance();
    }
    int t = tok.type;
    if (t < T_LT) return 0;
    advance();
    if (tok.type != T_WORD) {
        syntax_error();
        return -1;
    }
    struct redir *r = anew(sizeof *r);
    static const char ops[] = { [T_LT] = RD_IN, [T_GT] = RD_OUT, [T_DGT] = RD_APP, [T_DLT] = RD_HERE,
                                [T_DLTD] = RD_HERE_TAB, [T_LTAMP] = RD_DUPIN, [T_GTAMP] = RD_DUPOUT };
    r->op = ops[t];
    r->fd = fd >= 0 ? fd : (t == T_LT || t == T_DLT || t == T_DLTD || t == T_LTAMP) ? 0 : 1;
    r->word = tok.s;
    if (t == T_DLT || t == T_DLTD) {
        if (P->nhere == MAXHERE) {
            fail("too many here-documents", 0);
            return -1;
        }
        char *d = r->word, *o = d;                      /* the delimiter without its quotes */
        for (; *d; d++)
            if (*d != '\'' && *d != '"' && *d != '\\') *o++ = *d;
        *o = 0;
        r->quoted = tok.quoted;
        P->here[P->nhere++] = r;
    }
    advance();
    **tail = r;
    *tail = &r->next;
    return 1;
}

static struct node *parse_funcdef(char *name) {
    advance();                                          /* ( */
    if (tok.type != T_RP) return syntax_error();
    advance();
    skip_newlines();
    if (!var_valid(name)) {
        fail("bad function name", 0);
        return NULL;
    }
    struct node *n = mk(N_FUNC);
    n->name = name;
    n->l = parse_command();
    return n->l ? n : NULL;
}

static struct node *parse_simple(void) {
    struct node *n = mk(N_SIMPLE);
    struct redir **rt = &n->rd;
    char *w[MAXW];
    int nw = 0;
    for (;;) {
        if (tok.type == T_WORD) {
            if (nw == MAXW) {
                fail("too many words in a command", 0);
                return NULL;
            }
            w[nw++] = tok.s;
            advance();
            if (nw == 1 && tok.type == T_LP && !n->rd) return parse_funcdef(w[0]);
        } else {
            int r = parse_redir(&rt);
            if (r < 0) return NULL;
            if (!r) break;
        }
    }
    if (!nw && !n->rd) return syntax_error();
    n->w = copy_words(w, nw);
    n->nw = nw;
    return n;
}

static struct node *parse_if(void) {                    /* after "if" or "elif" */
    advance();
    struct node *n = mk(N_IF);
    if (!(n->l = parse_body()) || !expect("then") || !(n->r = parse_body())) return NULL;
    if (is_kw("elif")) return (n->x = parse_if()) ? n : NULL;
    if (is_kw("else")) {
        advance();
        if (!(n->x = parse_body())) return NULL;
    }
    return n;
}

static struct node *parse_loop(int type) {
    advance();
    struct node *n = mk(type);
    if (!(n->l = parse_body()) || !expect("do") || !(n->r = parse_body()) || !expect("done")) return NULL;
    return n;
}

static struct node *parse_for(void) {
    advance();
    if (tok.type != T_WORD || !var_valid(tok.s)) return syntax_error();
    struct node *n = mk(N_FOR);
    n->name = tok.s;
    advance();
    skip_newlines();
    n->nw = -1;
    if (is_kw("in")) {
        char *w[MAXW];
        int nw = 0;
        for (advance(); tok.type == T_WORD && nw < MAXW; advance()) w[nw++] = tok.s;
        n->w = copy_words(w, nw);
        n->nw = nw;
    }
    if (tok.type != T_SEMI && tok.type != T_NL && !is_kw("do")) return syntax_error();
    skip_newlines();
    if (tok.type == T_SEMI) advance();
    skip_newlines();
    if (!expect("do") || !(n->l = parse_body()) || !expect("done")) return NULL;
    return n;
}

static struct node *parse_case(void) {
    advance();
    if (tok.type != T_WORD) return syntax_error();
    struct node *n = mk(N_CASE);
    n->w = copy_words(&tok.s, 1);
    n->nw = 1;
    advance();
    skip_newlines();
    if (!expect("in")) return NULL;
    skip_newlines();
    struct citem **tail = &n->items;
    while (!is_kw("esac")) {
        struct citem *it = anew(sizeof *it);
        char *pats[MAXW];
        if (tok.type == T_LP) advance();
        for (;;) {
            if (tok.type != T_WORD || it->npat == MAXW) return syntax_error();
            pats[it->npat++] = tok.s;
            advance();
            if (tok.type != T_PIPE) break;
            advance();
        }
        if (tok.type != T_RP) return syntax_error();
        advance();
        it->pat = copy_words(pats, it->npat);
        if (!(it->body = parse_seq(0))) return NULL;
        *tail = it;
        tail = &it->next;
        if (tok.type == T_DSEMI) advance();
        else if (!is_kw("esac")) return syntax_error();
        skip_newlines();
    }
    advance();
    return n;
}

static struct node *parse_command(void) {
    struct node *n;
    if (tok.type == T_LP) {
        advance();
        n = mk(N_SUBSHELL);
        if (!(n->l = parse_body())) return NULL;
        if (tok.type != T_RP) return syntax_error();
        advance();
    } else if (is_kw("{")) {
        advance();
        n = mk(N_GROUP);
        if (!(n->l = parse_body()) || !expect("}")) return NULL;
    } else if (is_kw("if")) {
        if (!(n = parse_if()) || !expect("fi")) return NULL;
    } else if (is_kw("while") || is_kw("until")) {
        if (!(n = parse_loop(tok.s[0] == 'w' ? N_WHILE : N_UNTIL))) return NULL;
    } else if (is_kw("for")) {
        if (!(n = parse_for())) return NULL;
    } else if (is_kw("case")) {
        if (!(n = parse_case())) return NULL;
    } else {
        return parse_simple();
    }
    struct redir **rt = &n->rd;
    for (int r; (r = parse_redir(&rt)) != 0;)
        if (r < 0) return NULL;
    return n;
}

static struct node *parse_pipeline(void) {
    int neg = is_kw("!");
    if (neg) advance();
    struct node *first = parse_command();
    if (!first) return NULL;
    if (tok.type == T_PIPE) {
        struct node *pl = mk(N_PIPE), **tail = &first->next;
        pl->l = first;
        while (tok.type == T_PIPE) {
            advance();
            skip_newlines();
            struct node *c = parse_command();
            if (!c) return NULL;
            *tail = c;
            tail = &c->next;
        }
        first = pl;
    }
    if (neg) {
        struct node *n = mk(N_NOT);
        n->l = first;
        first = n;
    }
    return first;
}

static struct node *parse_andor(void) {
    struct node *l = parse_pipeline();
    while (l && (tok.type == T_AND || tok.type == T_OR)) {
        struct node *n = mk(tok.type == T_AND ? N_AND : N_OR);
        advance();
        skip_newlines();
        n->l = l;
        if (!(n->r = parse_pipeline())) return NULL;
        l = n;
    }
    return l;
}

/* Commands separated by ; & or newlines. At the top level a newline ends the list. */
static struct node *parse_seq(int top) {
    struct node *list = mk(N_LIST), **tail = &list->l;
    for (;;) {
        if (!top) skip_newlines();
        if (end_of_list() || (top && tok.type == T_NL)) break;
        struct node *n = parse_andor();
        if (!n) return NULL;
        if (tok.type == T_AMP) {
            struct node *bg = mk(N_BG);
            bg->l = n;
            n = bg;
            advance();
        } else if (tok.type == T_SEMI) {
            advance();
        } else if (tok.type != T_NL && !end_of_list()) {
            return syntax_error();
        }
        *tail = n;
        tail = &n->next;
    }
    return list;
}

int parse_next(struct node **out) {
    release();
    parse_error = NULL;
    parse_incomplete = 0;
    advance();
    skip_newlines();
    if (parse_error) return -1;
    if (tok.type == T_EOF) return 0;
    struct node *n = parse_seq(1);
    if (n && tok.type != T_NL && tok.type != T_EOF) n = syntax_error();
    if (P->nhere) fail("here-document: unexpected end of input", 1);
    if (!n || parse_error) {
        if (!parse_error) fail("syntax error", 0);
        return -1;
    }
    *out = n;
    return 1;
}
