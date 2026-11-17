/* Shell variables and allocation helpers. Exported variables are handed to programs as their environment. */

#include "sh.h"

struct var {
    struct var *next;
    char *name, *val;           /* val is NULL for a variable that is only marked for export */
    int exported;
};

static struct var *vars;
struct params pos;
const char *sh_name = "sh";

void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) {
        fputs("sh: out of memory\n", stderr);
        _exit(2);
    }
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n);
    if (!p) {
        fputs("sh: out of memory\n", stderr);
        _exit(2);
    }
    return p;
}

char *xstrdup(const char *s) {
    return strcpy(xmalloc(strlen(s) + 1), s);
}

static struct var *find(const char *name) {
    for (struct var *v = vars; v; v = v->next)
        if (!strcmp(v->name, name)) return v;
    return NULL;
}

static struct var *create(const char *name) {
    struct var *v = xmalloc(sizeof *v);
    v->name = xstrdup(name);
    v->val = NULL;
    v->exported = 0;
    v->next = vars;
    return vars = v;
}

const char *var_get(const char *name) {
    struct var *v = find(name);
    return v ? v->val : NULL;
}

void var_set(const char *name, const char *val) {
    struct var *v = find(name);
    char *copy = xstrdup(val);
    if (!v) v = create(name);
    free(v->val);
    v->val = copy;
}

void var_export(const char *name) {
    struct var *v = find(name);
    (v ? v : create(name))->exported = 1;
}

void var_unset(const char *name) {
    for (struct var **pp = &vars; *pp; pp = &(*pp)->next) {
        struct var *v = *pp;
        if (strcmp(v->name, name)) continue;
        *pp = v->next;
        free(v->name);
        free(v->val);
        free(v);
        return;
    }
}

int var_valid(const char *name) {
    if (!isalpha(*name) && *name != '_') return 0;
    while (isalnum(*name) || *name == '_') name++;
    return !*name;
}

/* The environment for a program: "name=value" for every exported variable. Allocated for one exec. */
char **var_environ(void) {
    int n = 0;
    for (struct var *v = vars; v; v = v->next) n += v->exported && v->val;
    char **env = xmalloc((size_t)(n + 1) * sizeof *env), **e = env;
    for (struct var *v = vars; v; v = v->next) {
        if (!v->exported || !v->val) continue;
        size_t nl = strlen(v->name);
        *e = xmalloc(nl + strlen(v->val) + 2);
        memcpy(*e, v->name, nl);
        (*e)[nl] = '=';
        strcpy(*e + nl + 1, v->val);
        e++;
    }
    *e = NULL;
    return env;
}

void var_import(void) {
    for (char **e = environ; e && *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq || eq == *e) continue;
        char *name = xstrdup(*e);
        name[eq - *e] = 0;
        var_set(name, eq + 1);
        var_export(name);
        free(name);
    }
}

/* set (all variables) or export (only the exported ones), in a form the shell can read back. */
void var_print(int only_exported) {
    for (struct var *v = vars; v; v = v->next) {
        if (!v->val || (only_exported && !v->exported)) continue;
        printf("%s%s='", only_exported ? "export " : "", v->name);
        for (const char *p = v->val; *p; p++) {
            if (*p == '\'') fputs("'\\''", stdout);
            else putchar(*p);
        }
        puts("'");
    }
}

/* Replaces $1 ... with copies of v[0..n). The old vector is not freed: a function may still borrow it. */
void set_params(char **v, int n) {
    char **copy = xmalloc((size_t)(n + 1) * sizeof *copy);
    for (int i = 0; i < n; i++) copy[i] = xstrdup(v[i]);
    copy[n] = NULL;
    pos.v = copy;
    pos.n = n;
}
