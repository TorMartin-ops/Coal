/**
 * @file cmdline.c
 * @brief Kernel command line (see cmdline.h).
 */
#include <kernel/core/cmdline.h>
#include <kernel/lib/string.h>

#define CMDLINE_MAX 256

static char g_line[CMDLINE_MAX];

void cmdline_init(const char *line) {
    if (!line) return;
    size_t n = MIN(strlen(line), (size_t)CMDLINE_MAX - 1);
    memcpy(g_line, line, n);
    g_line[n] = '\0';
}

const char *cmdline_raw(void) { return g_line; }

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n'; }

/* Finds the nth word whose name is `key`; *value points at its value ("" for a flag) and *len is that value's length. */
static bool find_word(const char *key, unsigned nth, const char **value, size_t *len) {
    size_t klen = strlen(key);
    for (const char *p = g_line; *p; ) {
        while (is_space(*p)) p++;
        const char *w = p;
        while (*p && !is_space(*p)) p++;
        if ((size_t)(p - w) < klen || memcmp(w, key, klen) != 0) continue;
        if (w + klen != p && w[klen] != '=') continue;
        if (nth--) continue;
        *value = w + klen + (w + klen != p);
        *len = (size_t)(p - *value);
        return true;
    }
    return false;
}

bool cmdline_get(const char *key, unsigned nth, char *out, size_t cap) {
    const char *v;
    size_t len;
    if (!cap || !find_word(key, nth, &v, &len)) return false;
    len = MIN(len, cap - 1);
    memcpy(out, v, len);
    out[len] = '\0';
    return true;
}

bool cmdline_has(const char *key) {
    const char *v;
    size_t len;
    return find_word(key, 0, &v, &len);
}

bool cmdline_number(const char *s, const char **end, uint64_t *out) {
    unsigned base = 10;
    uint64_t v = 0;
    const char *p = s;

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    if (!*p) return false;
    for (; *p; p++) {
        unsigned d = *p >= '0' && *p <= '9' ? (unsigned)(*p - '0')
                   : base == 16 && *p >= 'a' && *p <= 'f' ? (unsigned)(*p - 'a' + 10)
                   : base == 16 && *p >= 'A' && *p <= 'F' ? (unsigned)(*p - 'A' + 10) : base;
        if (d >= base) break;
        if (v >> 60) return false;                          /* far beyond any address or size; no overflow to check */
        v = v * base + d;
    }
    if (p == s) return false;
    *out = v;
    if (end) *end = p;
    return true;
}
