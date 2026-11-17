/* Shell wildcard matching (* ? [...] and backslash escapes). Shared by sh, case and find -name. */

#include <libc/string.h>

/* Matches the bracket expression at p ("[...]") against c. Returns the text after it, NULL if it
 * is not terminated (then the [ is an ordinary character). */
static const char *class_match(const char *p, int c, int *ok) {
    const char *q = p + 1;
    int neg = *q == '!' || *q == '^', hit = 0;
    q += neg;
    for (int first = 1; *q && (*q != ']' || first); first = 0) {
        int lo = *q == '\\' && q[1] ? *++q : *q;
        q++;
        int hi = lo;
        if (*q == '-' && q[1] && q[1] != ']') {
            hi = q[1] == '\\' && q[2] ? q[2] : q[1];
            q += q[1] == '\\' && q[2] ? 3 : 2;
        }
        hit |= c >= lo && c <= hi;
    }
    if (*q != ']') return NULL;
    *ok = hit != neg;
    return q + 1;
}

int pattern_match(const char *p, const char *s) {
    while (*p) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            for (; *s; s++)
                if (pattern_match(p, s)) return 1;
            return 0;
        }
        if (!*s) return 0;
        int ok = 0;
        const char *after;
        if (*p == '?') {
            p++;
        } else if (*p == '[' && (after = class_match(p, (unsigned char)*s, &ok))) {
            if (!ok) return 0;
            p = after;
        } else {
            if (*p == '\\' && p[1]) p++;
            if (*p++ != *s) return 0;
        }
        s++;
    }
    return !*s;
}
