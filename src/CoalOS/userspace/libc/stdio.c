#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/unistd.h>
#include <libc/fcntl.h>
#include <libc/errno.h>

struct FILE {
    int fd;
    int flags;              /* F_* below */
    int rpos, rlen;         /* read buffer window */
    int wlen;               /* pending write bytes */
    int cap;
    char *buf;
};

#define F_ERR  1
#define F_EOF  2
#define F_LINE 4            /* flush on newline (tty) */
#define F_OWN  8            /* buf and FILE are malloc'd */
#define F_WR   16
#define F_PROBED 32         /* stdout checked for a terminal */
#define F_UNBUF 64          /* flush after every call (stderr) */

static char out_buf[BUFSIZ], in_buf[BUFSIZ], err_buf[128];
static FILE f_in  = { 0, 0, 0, 0, 0, BUFSIZ, in_buf };
static FILE f_out = { 1, F_WR, 0, 0, 0, BUFSIZ, out_buf };
static FILE f_err = { 2, F_WR | F_UNBUF, 0, 0, 0, sizeof err_buf, err_buf };
FILE *stdin = &f_in, *stdout = &f_out, *stderr = &f_err;
static FILE *opened[8];

int fflush(FILE *f) {
    if (!f) {
        for (int i = 0; i < 8; i++) if (opened[i]) fflush(opened[i]);
        return fflush(stdout);
    }
    int off = 0;
    while (off < f->wlen) {
        ssize_t n = write(f->fd, f->buf + off, (size_t)(f->wlen - off));
        if (n < 0) {
            if (errno == EINTR) continue;
            f->flags |= F_ERR;
            f->wlen = 0;
            return EOF;
        }
        off += (int)n;
    }
    f->wlen = 0;
    return 0;
}

void __stdio_flush_all(void) { fflush(NULL); }

/* stdout becomes line buffered the first time it is found to be a terminal. */
static void bput(FILE *f, int c) {
    if (f == &f_out && !(f->flags & F_PROBED))
        f->flags |= F_PROBED | (isatty(1) ? F_LINE : 0);
    f->buf[f->wlen++] = (char)c;
    if (f->wlen == f->cap || ((f->flags & F_LINE) && c == '\n')) fflush(f);
}

static void done(FILE *f) {
    if (f->flags & F_UNBUF) fflush(f);
}

int fputc(int c, FILE *f) {
    bput(f, c);
    done(f);
    return (unsigned char)c;
}

size_t fwrite(const void *p, size_t size, size_t n, FILE *f) {
    const char *s = p;
    for (size_t i = 0, total = size * n; i < total; i++) bput(f, s[i]);
    done(f);
    return n;
}

int fputs(const char *s, FILE *f) {
    fwrite(s, 1, strlen(s), f);
    return (f->flags & F_ERR) ? EOF : 0;
}

int putchar(int c) { return fputc(c, stdout); }
int puts(const char *s) { fputs(s, stdout); fputc('\n', stdout); return 0; }

/* --- reading --- */

static int fill(FILE *f) {
    if (f->flags & (F_EOF | F_ERR)) return 0;
    if (f == &f_in) fflush(stdout);     /* show prompts before blocking */
    for (;;) {
        ssize_t n = read(f->fd, f->buf, (size_t)f->cap);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            f->flags |= n < 0 ? F_ERR : F_EOF;
            return 0;
        }
        f->rpos = 0;
        f->rlen = (int)n;
        return 1;
    }
}

int fgetc(FILE *f) {
    if (f->rpos >= f->rlen && !fill(f)) return EOF;
    return (unsigned char)f->buf[f->rpos++];
}

size_t fread(void *p, size_t size, size_t n, FILE *f) {
    char *d = p;
    size_t total = size * n, i = 0;
    for (; i < total; i++) {
        int c = fgetc(f);
        if (c == EOF) break;
        d[i] = (char)c;
    }
    return size ? i / size : 0;
}

char *fgets(char *s, int n, FILE *f) {
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (!i) return NULL;
    s[i] = 0;
    return s;
}

ssize_t getline(char **line, size_t *cap, FILE *f) {
    size_t len = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF && !len) return -1;
        if (len + 2 > *cap) {
            size_t nc = *cap ? *cap * 2 : 128;
            char *p = realloc(*line, nc);
            if (!p) return -1;
            *line = p;
            *cap = nc;
        }
        if (c == EOF) break;
        (*line)[len++] = (char)c;
        if (c == '\n') break;
    }
    (*line)[len] = 0;
    return (ssize_t)len;
}

int feof(FILE *f)   { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
int fileno(FILE *f) { return f->fd; }

FILE *fopen(const char *path, const char *mode) {
    int flags = mode[0] == 'r' ? O_RDONLY
              : mode[0] == 'w' ? O_WRONLY | O_CREAT | O_TRUNC
              : O_WRONLY | O_CREAT | O_APPEND;
    if (strchr(mode, '+')) flags = (flags & ~3) | O_RDWR;
    int slot = 0;
    while (slot < 8 && opened[slot]) slot++;
    if (slot == 8) { errno = EMFILE; return NULL; }
    int fd = open(path, flags, 0644);
    if (fd < 0) return NULL;
    FILE *f = calloc(1, sizeof *f);
    char *b = malloc(BUFSIZ);
    if (!f || !b) {
        free(f); free(b);
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    f->fd = fd;
    f->buf = b;
    f->cap = BUFSIZ;
    f->flags = F_OWN | (mode[0] != 'r' ? F_WR : 0);
    opened[slot] = f;
    return f;
}

int fclose(FILE *f) {
    int r = (f->flags & F_WR) ? fflush(f) : 0;
    if (close(f->fd) < 0) r = EOF;
    for (int i = 0; i < 8; i++) if (opened[i] == f) opened[i] = NULL;
    if (f->flags & F_OWN) {
        free(f->buf);
        free(f);
    }
    return r;
}

int remove(const char *path) {
    return unlink(path) == 0 ? 0 : (errno == EISDIR ? rmdir(path) : -1);
}

void perror(const char *msg) {
    if (msg && *msg) fprintf(stderr, "%s: ", msg);
    fprintf(stderr, "%s\n", strerror(errno));
}

/* --- formatted output --- */

struct sink {
    FILE *f;                /* write to a stream, or ... */
    char *buf;              /* ... into a buffer of cap bytes */
    size_t cap, len;
};

static void put(struct sink *s, char c) {
    if (s->f) {
        bput(s->f, c);
    } else if (s->len + 1 < s->cap) {
        s->buf[s->len] = c;
    }
    s->len++;
}

static void pad(struct sink *s, int n, char c) {
    while (n-- > 0) put(s, c);
}

/* *v /= base, returns the remainder; avoids the libgcc 64-bit division helpers. */
static unsigned divmod64(unsigned long long *v, unsigned base) {
    unsigned hi = (unsigned)(*v >> 32), lo = (unsigned)*v, qh = hi / base, r = hi % base, ql;
    __asm__("divl %2" : "=a"(ql), "=d"(r) : "r"(base), "0"(lo), "1"(r));
    *v = ((unsigned long long)qh << 32) | ql;
    return r;
}

/* %f without the FPU: formats the IEEE-754 bits of a double with integer arithmetic. The fraction
 * lives in a 1088-bit binary register that is multiplied by ten per digit, so every digit is exact
 * (round to nearest, ties to even). At most 18 decimals; |x| >= 2^64 prints as "(big)". */
static int format_fixed(unsigned long long bits, int prec, int alt, char *out) {
    int exp = (int)(bits >> 52 & 0x7ff), n = 0;
    unsigned long long mant = bits & ((1ULL << 52) - 1), ip = 0;
    unsigned w[34] = { 0 };                         /* fraction bits, most significant limb first */
    if (exp == 0x7ff) {
        memcpy(out, mant ? "nan" : "inf", 3);
        return 3;
    }
    if (exp) {
        mant |= 1ULL << 52;
        exp -= 1075;
        if (exp >= 0) {
            if (exp > 11) { memcpy(out, "(big)", 5); return 5; }
            ip = mant << exp;
        } else {
            int shift = -exp;
            ip = shift < 64 ? mant >> shift : 0;
            for (int b = 0; b < 53 && b < shift; b++)
                if (mant >> b & 1) w[(shift - b - 1) / 32] |= 0x80000000u >> ((shift - b - 1) % 32);
        }
    }
    if (prec > 18) prec = 18;
    char dig[18];
    for (int i = 0; i < prec; i++) {
        unsigned long long carry = 0;
        for (int k = 33; k >= 0; k--) {
            carry += (unsigned long long)w[k] * 10;
            w[k] = (unsigned)carry;
            carry >>= 32;
        }
        dig[i] = (char)carry;
    }
    int rest = 0;
    for (int k = 1; k < 34; k++) rest |= w[k] != 0;
    int last = prec ? dig[prec - 1] : (int)(ip & 1);
    if (w[0] > 0x80000000u || (w[0] == 0x80000000u && (rest || (last & 1)))) {
        int i = prec - 1;
        while (i >= 0 && dig[i] == 9) dig[i--] = 0;
        if (i >= 0) dig[i]++;
        else ip++;
    }
    char num[20];
    int nd = 0;
    do num[nd++] = (char)('0' + divmod64(&ip, 10)); while (ip);
    while (nd) out[n++] = num[--nd];
    if (prec || alt) out[n++] = '.';
    for (int i = 0; i < prec; i++) out[n++] = (char)('0' + dig[i]);
    return n;
}

static int vformat(struct sink *s, const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(s, *fmt); continue; }
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0, width = 0, prec = -1, lng = 0;
        for (fmt++;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '#') alt = 1;
            else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; if (width < 0) { left = 1; width = -width; } }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            prec = 0;
            fmt++;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') {
            if (*fmt == 'l') lng++;
            fmt++;
        }
        char tmp[48];
        const char *str = tmp, *prefix = "";
        int len = 0;
        char c = *fmt;
        if (c == '%') { put(s, '%'); continue; }
        if (c == 'c') {
            tmp[0] = (char)va_arg(ap, int);
            len = 1;
        } else if (c == 's') {
            str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            while ((prec < 0 || len < prec) && str[len]) len++;
        } else if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X' || c == 'o' || c == 'p') {
            unsigned long long v;
            int neg = 0, base = c == 'o' ? 8 : (c == 'x' || c == 'X' || c == 'p') ? 16 : 10;
            if (c == 'd' || c == 'i') {
                long long sv = lng > 1 ? va_arg(ap, long long) : va_arg(ap, int);
                neg = sv < 0;
                v = neg ? -(unsigned long long)sv : (unsigned long long)sv;
            } else {
                v = lng > 1 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
                if (c == 'p') alt = 1;
            }
            if (neg) prefix = "-"; else if (plus && base == 10 && (c == 'd' || c == 'i')) prefix = "+";
            else if (space && (c == 'd' || c == 'i')) prefix = " ";
            else if (alt && base == 16 && v) prefix = c == 'X' ? "0X" : "0x";
            const char *digits = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char *e = tmp + sizeof tmp;
            while (v) *--e = digits[divmod64(&v, (unsigned)base)];
            str = e;
            len = (int)(tmp + sizeof tmp - e);
            if (prec >= 0) zero = 0;
            while (len < prec && e > tmp) { *--e = '0'; len++; }
            str = e;
            if (!len && prec != 0) { str = "0"; len = 1; }
        } else if (c == 'f' || c == 'F') {
            unsigned long long bits = va_arg(ap, unsigned long long);    /* a double travels as 8 stack bytes */
            if (bits >> 63) prefix = "-"; else if (plus) prefix = "+"; else if (space) prefix = " ";
            len = format_fixed(bits & ~(1ULL << 63), prec < 0 ? 6 : prec, alt, tmp);
            if (tmp[0] == 'i' || tmp[0] == 'n') zero = 0;
        } else {
            put(s, '%');
            if (c) put(s, c); else break;
            continue;
        }
        if (c == 's' || c == 'c') zero = 0;
        int plen = (int)strlen(prefix);
        int fill = width - len - plen;
        if (!left && !zero) pad(s, fill, ' ');
        for (int i = 0; i < plen; i++) put(s, prefix[i]);
        if (!left && zero) pad(s, fill, '0');
        for (int i = 0; i < len; i++) put(s, str[i]);
        if (left) pad(s, fill, ' ');
    }
    return (int)s->len;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    struct sink s = { f, NULL, 0, 0 };
    int r = vformat(&s, fmt, ap);
    done(f);
    return r;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    struct sink s = { NULL, buf, n, 0 };
    int r = vformat(&s, fmt, ap);
    if (n) buf[s.len < n ? s.len : n - 1] = 0;
    return r;
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, (size_t)-1 >> 1, fmt, ap);
}
