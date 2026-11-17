#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/unistd.h>
#include <libc/errno.h>
#include <libc/ctype.h>
#include <libc/syscall.h>
#include <libc/signal.h>
#include <libc/limits.h>

/* --- heap: first-fit free list on brk, blocks coalesce with their successor --- */

struct block {
    size_t size;            /* payload bytes, multiple of 8 */
    struct block *next;     /* next block in address order */
    int free;
};

#define HDR ((size_t)sizeof(struct block))
static struct block *head, *tail;

static struct block *grow(size_t size) {
    long cur = __syscall_ret(__SC1(SYS_BRK, 0));
    if (cur < 0) return NULL;
    size_t want = (HDR + size + 4095) & ~(size_t)4095;
    long end = __syscall_ret(__SC1(SYS_BRK, cur + want));
    if (end < 0 || (size_t)end < (size_t)cur + want) return NULL;
    struct block *b = (struct block *)cur;
    b->size = want - HDR;
    b->next = NULL;
    b->free = 1;
    if (tail) tail->next = b; else head = b;
    tail = b;
    return b;
}

void *malloc(size_t n) {
    if (n > 0x40000000) {
        errno = ENOMEM;
        return NULL;
    }
    n = (n + 7) & ~(size_t)7;
    if (!n) n = 8;
    for (int pass = 0; pass < 2; pass++) {
        for (struct block *b = head; b; b = b->next) {
            while (b->free && b->next && b->next->free && (char *)b + HDR + b->size == (char *)b->next) {
                b->size += HDR + b->next->size;
                if (tail == b->next) tail = b;
                b->next = b->next->next;
            }
            if (!b->free || b->size < n) continue;
            if (b->size >= n + HDR + 8) {
                struct block *r = (struct block *)((char *)b + HDR + n);
                r->size = b->size - n - HDR;
                r->free = 1;
                r->next = b->next;
                if (tail == b) tail = r;
                b->next = r;
                b->size = n;
            }
            b->free = 0;
            return (char *)b + HDR;
        }
        if (pass == 0 && !grow(n)) break;
    }
    errno = ENOMEM;
    return NULL;
}

void free(void *p) {
    if (p) ((struct block *)((char *)p - HDR))->free = 1;
}

void *calloc(size_t n, size_t m) {
    if (m && n > (size_t)-1 / m) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = malloc(n * m);
    return p ? memset(p, 0, n * m) : NULL;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    struct block *b = (struct block *)((char *)p - HDR);
    if (b->size >= n) return p;
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, b->size);
    free(p);
    return q;
}

/* --- process exit --- */

extern void __stdio_flush_all(void);
static void (*exit_fns[8])(void);
static int exit_count;

int atexit(void (*fn)(void)) {
    if (exit_count == 8) return -1;
    exit_fns[exit_count++] = fn;
    return 0;
}

void exit(int status) {
    while (exit_count) exit_fns[--exit_count]();
    __stdio_flush_all();
    _exit(status);
}

void abort(void) {
    kill(getpid(), SIGABRT);
    _exit(134);
}

/* --- numbers --- */

/* Parses an integer with optional sign and base prefix into 64 bits. *ovf is set when it does not fit. */
static unsigned long long parse_int(const char *s, char **end, int base, int *neg, int *ovf) {
    const char *p = s;
    unsigned long long v = 0;
    int any = 0;
    *neg = *ovf = 0;
    while (isspace(*p)) p++;
    if (*p == '-' || *p == '+') *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x' && isxdigit(p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = *p == '0' ? 8 : 10;
    }
    for (;; p++) {
        int d = isdigit(*p) ? *p - '0' : isalpha(*p) ? (*p | 32) - 'a' + 10 : 99;
        if (d >= base) break;
        if (__builtin_mul_overflow(v, (unsigned long long)base, &v) || __builtin_add_overflow(v, (unsigned long long)d, &v))
            *ovf = 1;
        any = 1;
    }
    if (end) *end = (char *)(any ? p : s);
    return v;
}

/* Clamps to [lo, hi] (as magnitudes: hi for positive, lo_mag for negative) and sets ERANGE. */
static unsigned long long clamp_int(unsigned long long v, int neg, int ovf, unsigned long long pos_max) {
    if (ovf || v > pos_max + neg) {
        errno = ERANGE;
        v = pos_max + neg;
    }
    return neg ? -v : v;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    int neg, ovf;
    unsigned long long v = parse_int(s, end, base, &neg, &ovf);
    if (ovf) {
        errno = ERANGE;
        return ~0ULL;
    }
    return neg ? -v : v;
}

long long strtoll(const char *s, char **end, int base) {
    int neg, ovf;
    unsigned long long v = parse_int(s, end, base, &neg, &ovf);
    return (long long)clamp_int(v, neg, ovf, (unsigned long long)LLONG_MAX);
}

unsigned long strtoul(const char *s, char **end, int base) {
    int neg, ovf;
    unsigned long long v = parse_int(s, end, base, &neg, &ovf);
    if (ovf || v > ULONG_MAX) {
        errno = ERANGE;
        return ULONG_MAX;
    }
    return neg ? -(unsigned long)v : (unsigned long)v;
}

long strtol(const char *s, char **end, int base) {
    int neg, ovf;
    unsigned long long v = parse_int(s, end, base, &neg, &ovf);
    return (long)clamp_int(v, neg, ovf, LONG_MAX);
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
int abs(int x) { return x < 0 ? -x : x; }

static unsigned long rand_state = 1;
void srand(unsigned seed) { rand_state = seed; }
int rand(void) {
    rand_state = rand_state * 1103515245UL + 12345UL;
    return (int)((rand_state >> 1) & RAND_MAX);
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    char *a = base, tmp[64];
    if (size > sizeof tmp) return;
    /* shell sort: small, no recursion, no allocation */
    for (size_t gap = n / 2; gap; gap /= 2)
        for (size_t i = gap; i < n; i++) {
            memcpy(tmp, a + i * size, size);
            size_t j = i;
            for (; j >= gap && cmp(a + (j - gap) * size, tmp) > 0; j -= gap)
                memcpy(a + j * size, a + (j - gap) * size, size);
            memcpy(a + j * size, tmp, size);
        }
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    const char *a = base;
    while (n) {
        size_t half = n / 2;
        const char *mid = a + half * size;
        int c = cmp(key, mid);
        if (!c) return (void *)mid;
        if (c > 0) {
            a = mid + size;
            n -= half + 1;
        } else {
            n = half;
        }
    }
    return NULL;
}

/* --- environment --- */

static int env_owned;   /* environ points to a malloc'd copy */

static int env_find(const char *name, size_t len) {
    if (!environ) return -1;
    for (int i = 0; environ[i]; i++)
        if (!strncmp(environ[i], name, len) && environ[i][len] == '=') return i;
    return -1;
}

char *getenv(const char *name) {
    int i = env_find(name, strlen(name));
    return i < 0 ? NULL : environ[i] + strlen(name) + 1;
}

int setenv(const char *name, const char *value, int overwrite) {
    size_t nl = strlen(name), vl = strlen(value);
    if (!nl || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    int i = env_find(name, nl);
    if (i >= 0 && !overwrite) return 0;
    char *kv = malloc(nl + vl + 2);
    if (!kv) return -1;
    memcpy(kv, name, nl);
    kv[nl] = '=';
    memcpy(kv + nl + 1, value, vl + 1);
    if (i >= 0) {
        environ[i] = kv;
        return 0;
    }
    int n = 0;
    while (environ && environ[n]) n++;
    char **ne = malloc((size_t)(n + 2) * sizeof *ne);
    if (!ne) return -1;
    for (int k = 0; k < n; k++) ne[k] = environ[k];
    ne[n] = kv;
    ne[n + 1] = NULL;
    if (env_owned) free(environ);
    environ = ne;
    env_owned = 1;
    return 0;
}

int unsetenv(const char *name) {
    int i = env_find(name, strlen(name));
    if (i < 0) return 0;
    for (; environ[i]; i++) environ[i] = environ[i + 1];
    return 0;
}

/* --- program start: called by _start with the initial stack pointer --- */

extern int main(int argc, char **argv, char **envp);

#define AT_SYSINFO      32
#define AT_SYSINFO_EHDR 33

void *__vdso_base;

void __libc_start(long *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **env;
    environ = argv + argc + 1;
    for (env = environ; *env; env++) { }
    for (unsigned long *a = (unsigned long *)(env + 1); a[0]; a += 2) {
        if (a[0] == AT_SYSINFO) __vsyscall = (void (*)(void))a[1];
        else if (a[0] == AT_SYSINFO_EHDR) __vdso_base = (void *)a[1];
    }
    exit(main(argc, argv, environ));
}
