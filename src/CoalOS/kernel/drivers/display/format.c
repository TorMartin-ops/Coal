/**
 * @file format.c
 * @brief Kernel snprintf: %s %c %d %i %u %x %X %o %p %%, flags - 0 #, width,
 *        precision, length modifiers h hh l ll z t j, including 64-bit values.
 */
#include <kernel/drivers/display/terminal.h>
#include <kernel/lib/string.h>

typedef struct {
    char  *buf;
    size_t cap; /* usable bytes, excluding the terminator */
    size_t len;
} out_t;

static void put(out_t *o, char c) {
    if (o->len < o->cap) o->buf[o->len++] = c;
}

static void pad(out_t *o, char c, int n) {
    while (n-- > 0) put(o, c);
}

/* v /= base, returns the remainder. 64/32 division with divl: the kernel is
 * not linked against libgcc, so there is no __udivdi3. */
static uint32_t divmod(uint64_t *v, uint32_t base) {
    uint32_t hi = (uint32_t)(*v >> 32), lo = (uint32_t)*v;
    uint32_t qhi = hi / base, rem = hi % base, qlo;
    __asm__("divl %4" : "=a"(qlo), "=d"(rem) : "a"(lo), "d"(rem), "rm"(base));
    *v = ((uint64_t)qhi << 32) | qlo;
    return rem;
}

typedef struct {
    int  width, prec;
    bool left, zero;
} spec_t;

static void put_number(out_t *o, uint64_t v, uint32_t base, bool upper, bool neg,
                       const char *prefix, const spec_t *sp) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    if (!(v == 0 && sp->prec == 0)) {
        do { tmp[n++] = digits[divmod(&v, base)]; } while (v);
    }
    int zeros = sp->prec > n ? sp->prec - n : 0;
    int head = (int)strlen(prefix) + (neg ? 1 : 0);
    int total = head + zeros + n;
    if (sp->zero && !sp->left && sp->prec < 0 && sp->width > total) {
        zeros += sp->width - total;
        total = sp->width;
    }
    int spaces = sp->width > total ? sp->width - total : 0;

    if (!sp->left) pad(o, ' ', spaces);
    if (neg) put(o, '-');
    for (; *prefix; prefix++) put(o, *prefix);
    pad(o, '0', zeros);
    while (n > 0) put(o, tmp[--n]);
    if (sp->left) pad(o, ' ', spaces);
}

int _vsnprintf(char *str, size_t size, const char *fmt, va_list args) {
    if (!str || size == 0) return 0;
    out_t o = { str, size - 1, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;

        spec_t sp = { 0, -1, false, false };
        bool alt = false;
        for (;; fmt++) {
            if (*fmt == '-') sp.left = true;
            else if (*fmt == '0') sp.zero = true;
            else if (*fmt == '#') alt = true;
            else if (*fmt != ' ' && *fmt != '+') break;
        }
        if (*fmt == '*') {
            sp.width = va_arg(args, int);
            if (sp.width < 0) { sp.left = true; sp.width = -sp.width; }
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') sp.width = sp.width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++;
            sp.prec = 0;
            if (*fmt == '*') { sp.prec = va_arg(args, int); fmt++; }
            while (*fmt >= '0' && *fmt <= '9') sp.prec = sp.prec * 10 + (*fmt++ - '0');
        }

        int longs = 0; /* 2 = 64-bit; everything else is 32-bit on i386 */
        for (;; fmt++) {
            if (*fmt == 'l') longs++;
            else if (*fmt == 'j') longs = 2;
            else if (*fmt != 'h' && *fmt != 'z' && *fmt != 't') break;
        }

        switch (*fmt) {
        case 'c':
            if (!sp.left) pad(&o, ' ', sp.width - 1);
            put(&o, (char)va_arg(args, int));
            if (sp.left) pad(&o, ' ', sp.width - 1);
            break;
        case 's': {
            const char *s = va_arg(args, const char *);
            if (!s) s = "(null)";
            int n = 0;
            while (s[n] && (sp.prec < 0 || n < sp.prec)) n++;
            if (!sp.left) pad(&o, ' ', sp.width - n);
            for (int i = 0; i < n; i++) put(&o, s[i]);
            if (sp.left) pad(&o, ' ', sp.width - n);
            break;
        }
        case 'd': case 'i': {
            int64_t v = longs >= 2 ? va_arg(args, long long) : (int64_t)va_arg(args, int);
            bool neg = v < 0;
            put_number(&o, neg ? 0 - (uint64_t)v : (uint64_t)v, 10, false, neg, "", &sp);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = longs >= 2 ? va_arg(args, unsigned long long) : (uint64_t)va_arg(args, unsigned int);
            uint32_t base = *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16;
            const char *prefix = "";
            if (alt && v) prefix = base == 16 ? (*fmt == 'X' ? "0X" : "0x") : base == 8 ? "0" : "";
            put_number(&o, v, base, *fmt == 'X', false, prefix, &sp);
            break;
        }
        case 'p': {
            sp.prec = 8;
            put_number(&o, (uintptr_t)va_arg(args, void *), 16, false, false, "0x", &sp);
            break;
        }
        case '%':
            put(&o, '%');
            break;
        case '\0':
            fmt--; /* dangling '%' at the end */
            break;
        default:
            put(&o, '%');
            put(&o, *fmt);
            break;
        }
    }
    str[o.len] = '\0';
    return (int)o.len;
}
