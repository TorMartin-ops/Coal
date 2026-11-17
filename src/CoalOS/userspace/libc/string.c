#include <libc/string.h>
#include <libc/stdlib.h>
#include <libc/errno.h>
#include <libc/ctype.h>

void *memcpy(void *d, const void *s, size_t n) {
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}

void *memset(void *d, int c, size_t n) {
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (dp <= sp || dp >= sp + n) return memcpy(d, s, n);
    while (n--) dp[n] = sp[n];
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (; n; n--, p++)
        if (*p == (unsigned char)c) return (void *)p;
    return NULL;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}

char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

char *strcat(char *d, const char *s) {
    strcpy(d + strlen(d), s);
    return d;
}

char *strncat(char *d, const char *s, size_t n) {
    char *e = d + strlen(d);
    while (n-- && *s) *e++ = *s++;
    *e = 0;
    return d;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) n++;
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) n++;
    return n;
}

char *strpbrk(const char *s, const char *accept) {
    s += strcspn(s, accept);
    return *s ? (char *)s : NULL;
}

char *strtok_r(char *s, const char *delim, char **save) {
    if (!s) s = *save;
    s += strspn(s, delim);
    if (!*s) return NULL;
    char *tok = s;
    s += strcspn(s, delim);
    if (*s) *s++ = 0;
    *save = s;
    return tok;
}

char *strtok(char *s, const char *delim) {
    static char *save;
    return strtok_r(s, delim, &save);
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) break;
    }
    return 0;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && tolower(*a) == tolower(*b)) a++, b++;
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c) {
    const char *r = NULL;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}

char *strstr(const char *h, const char *n) {
    size_t len = strlen(n);
    for (; *h; h++)
        if (!strncmp(h, n, len)) return (char *)h;
    return len ? NULL : (char *)h;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    return p ? memcpy(p, s, n) : NULL;
}

static const struct { unsigned char code; const char *msg; } errors[] = {
    { EPERM, "Operation not permitted" },
    { ENOENT, "No such file or directory" },
    { ESRCH, "No such process" },
    { EINTR, "Interrupted system call" },
    { EIO, "Input/output error" },
    { ENXIO, "No such device or address" },
    { E2BIG, "Argument list too long" },
    { ENOEXEC, "Exec format error" },
    { EBADF, "Bad file descriptor" },
    { ECHILD, "No child processes" },
    { EAGAIN, "Resource temporarily unavailable" },
    { ENOMEM, "Cannot allocate memory" },
    { EACCES, "Permission denied" },
    { EFAULT, "Bad address" },
    { EBUSY, "Device or resource busy" },
    { EEXIST, "File exists" },
    { EXDEV, "Invalid cross-device link" },
    { ENODEV, "No such device" },
    { ENOTDIR, "Not a directory" },
    { EISDIR, "Is a directory" },
    { EINVAL, "Invalid argument" },
    { ENFILE, "Too many open files in system" },
    { EMFILE, "Too many open files" },
    { ENOTTY, "Inappropriate ioctl for device" },
    { EFBIG, "File too large" },
    { ENOSPC, "No space left on device" },
    { ESPIPE, "Illegal seek" },
    { EROFS, "Read-only file system" },
    { EMLINK, "Too many links" },
    { EPIPE, "Broken pipe" },
    { ERANGE, "Numerical result out of range" },
    { ENAMETOOLONG, "File name too long" },
    { ENOSYS, "Function not implemented" },
    { ENOTEMPTY, "Directory not empty" },
    { ELOOP, "Too many levels of symbolic links" },
};

char *strerror(int err) {
    for (unsigned i = 0; i < sizeof errors / sizeof errors[0]; i++)
        if (errors[i].code == err) return (char *)errors[i].msg;
    return "Unknown error";
}
