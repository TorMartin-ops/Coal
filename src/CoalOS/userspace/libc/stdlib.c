#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/unistd.h>
#include <libc/errno.h>
#include <libc/ctype.h>
#include <libc/syscall.h>
#include <libc/signal.h>
#include <libc/limits.h>

/* --- heap: size classes with per-page free lists (after mimalloc, MSR-TR-2019-18) ---
 *
 * Small blocks come from 4 KiB pages taken from brk. Every page serves one size class and keeps two
 * lists: `free`, which malloc pops, and `local_free`, which free pushes; they are merged when `free`
 * runs dry. A fresh page hands out blocks by bumping an offset. A page that empties goes to a pool
 * any class can reuse. Blocks above MAX_SMALL get their own mmap. The page header sits at the start
 * of every page and of every large block, so free() finds it by masking the pointer. */

#define PAGE   4096u
#define HDR    48u                              /* header size; keeps blocks 16-byte aligned */
#define LARGE  0xFF
#define GROW   (16 * PAGE)

struct page {
    unsigned char cls, full;                    /* full: on no list, nothing left to allocate */
    unsigned short used, bump;                  /* blocks handed out; offset of the first never-used block */
    union { struct page *next; size_t len; };   /* partial list / bytes mapped for a large block */
    struct page *prev;
    void *free, *local_free;
};

_Static_assert(sizeof(struct page) <= HDR, "page header does not fit");

static const unsigned short csize[] = { 16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384,
                                        448, 512, 640, 768, 896, 1008, 1344, 2016 };
#define NCLS      ((unsigned)(sizeof csize / sizeof csize[0]))
#define MAX_SMALL 2016u

static struct page *cur[NCLS], *partial[NCLS];
static struct page *spare;                      /* empty pages, linked through next */
static char *heap_top, *heap_end;

static unsigned class_of(size_t n) {
    if (n <= 128) return n ? (unsigned)(n - 1) >> 4 : 0;
    unsigned c = 8;
    while (csize[c] < n) c++;
    return c;
}

static struct page *page_get(void) {
    struct page *pg = spare;
    if (pg) {
        spare = pg->next;
        return pg;
    }
    if (heap_top == heap_end) {
        long brk = __syscall_ret(__SC1(SYS_BRK, 0));
        if (brk < 0) return NULL;
        size_t base = ((size_t)brk + PAGE - 1) & ~(size_t)(PAGE - 1), size = GROW;
        long end = __syscall_ret(__SC1(SYS_BRK, base + size));
        if (end < 0 || (size_t)end < base + size) {
            size = PAGE;
            end = __syscall_ret(__SC1(SYS_BRK, base + size));
            if (end < 0 || (size_t)end < base + size) return NULL;
        }
        heap_top = (char *)base;
        heap_end = heap_top + size;
    }
    pg = (struct page *)heap_top;
    heap_top += PAGE;
    return pg;
}

static void partial_unlink(unsigned c, struct page *pg) {
    if (pg->prev) pg->prev->next = pg->next; else partial[c] = pg->next;
    if (pg->next) pg->next->prev = pg->prev;
}

/* Pop a block from the page's lists, merging the two when `free` is empty. */
static inline void *page_pop(struct page *pg) {
    void *b = pg->free;
    if (!b) {
        b = pg->local_free;
        pg->local_free = NULL;
    }
    if (b) {
        pg->free = *(void **)b;
        pg->used++;
    }
    return b;
}

/* Both lists of the current page of class c are empty: bump, or move to another page. */
static void *malloc_slow(unsigned c) {
    for (;;) {
        struct page *pg = cur[c];
        if (pg) {
            void *b = page_pop(pg);
            if (b) return b;
            if (pg->bump <= PAGE - csize[c]) {
                b = (char *)pg + pg->bump;
                pg->bump += csize[c];
                pg->used++;
                return b;
            }
            pg->full = 1;
        }
        pg = partial[c];
        if (pg) {
            partial_unlink(c, pg);
        } else {
            pg = page_get();
            if (!pg) { cur[c] = NULL; errno = ENOMEM; return NULL; }
            pg->cls = (unsigned char)c;
            pg->full = 0;
            pg->used = 0;
            pg->bump = HDR;
            pg->free = pg->local_free = NULL;
        }
        cur[c] = pg;
    }
}

static void *malloc_large(size_t n) {
    size_t len = (n + HDR + PAGE - 1) & ~(size_t)(PAGE - 1);
    long r = __sc6(SYS_MMAP, 0, (long)len, 3 /* RW */, 0x22 /* PRIVATE|ANON */, -1, 0);
    if ((unsigned long)r > (unsigned long)-4096) {
        errno = ENOMEM;
        return NULL;
    }
    struct page *pg = (struct page *)r;
    pg->cls = LARGE;
    pg->len = len;
    return (char *)pg + HDR;
}

void *malloc(size_t n) {
    if (n > 0x40000000) {
        errno = ENOMEM;
        return NULL;
    }
    if (n > MAX_SMALL) return malloc_large(n);

    unsigned c = class_of(n);
    struct page *pg = cur[c];
    void *b = pg ? page_pop(pg) : NULL;
    return b ? b : malloc_slow(c);
}

void free(void *p) {
    if (!p) return;
    struct page *pg = (struct page *)((size_t)p & ~(size_t)(PAGE - 1));
    if (pg->cls == LARGE) {
        __SC3(SYS_MUNMAP, pg, pg->len, 0);
        return;
    }
    unsigned c = pg->cls;
    *(void **)p = pg->local_free;
    pg->local_free = p;
    if (--pg->used == 0 && pg != cur[c]) {
        if (!pg->full) partial_unlink(c, pg);
        pg->next = spare;
        spare = pg;
    } else if (pg->full) {
        pg->full = 0;
        pg->prev = NULL;
        pg->next = partial[c];
        if (partial[c]) partial[c]->prev = pg;
        partial[c] = pg;
    }
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
    struct page *pg = (struct page *)((size_t)p & ~(size_t)(PAGE - 1));
    size_t have = pg->cls == LARGE ? pg->len - HDR : csize[pg->cls];
    if (have >= n) return p;
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, have);
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

#define AT_SYSINFO_EHDR 33

void *__vdso_base;

void __libc_start(long *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **env;
    environ = argv + argc + 1;
    for (env = environ; *env; env++) { }
    for (unsigned long *a = (unsigned long *)(env + 1); a[0]; a += 2) {
        if (a[0] == AT_SYSINFO_EHDR) __vdso_base = (void *)a[1];
    }
    exit(main(argc, argv, environ));
}
