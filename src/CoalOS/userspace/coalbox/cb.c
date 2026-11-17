#include "cb.h"
#include <libc/dirent.h>

const char *prog = "coalbox";

int cb_err(const char *what, const char *path) {
    fflush(stdout);
    if (what) fprintf(stderr, "%s: %s '%s': %s\n", prog, what, path, strerror(errno));
    else fprintf(stderr, "%s: %s: %s\n", prog, path, strerror(errno));
    return 1;
}

int cb_usage(const char *args) {
    fprintf(stderr, "%s: usage: %s %s\n", prog, prog, args);
    return 1;
}

int cb_write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

char *cb_join(const char *dir, const char *name) {
    size_t dl = strlen(dir);
    char *p = malloc(dl + strlen(name) + 2);
    if (!p) { fputs("out of memory\n", stderr); exit(1); }
    memcpy(p, dir, dl);
    if (dl && dir[dl - 1] != '/') p[dl++] = '/';
    strcpy(p + dl, name);
    return p;
}

const char *cb_base(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

long cb_slurp(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t len = 0;
    ssize_t n;
    while (len + 1 < cap && (n = read(fd, buf + len, cap - 1 - len)) > 0) len += (size_t)n;
    close(fd);
    buf[len] = 0;
    return (long)len;
}

char **cb_list_dir(const char *path, int *count) {
    DIR *d = opendir(path);
    if (!d) return NULL;
    int n = 0, cap = 16;
    char **v = malloc((size_t)cap * sizeof *v);
    struct dirent *e;
    while (v && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) {
            cap *= 2;
            char **nv = realloc(v, (size_t)cap * sizeof *v);
            if (!nv) break;
            v = nv;
        }
        v[n] = strdup(e->d_name);
        if (v[n]) n++;
    }
    closedir(d);
    *count = n;
    return v;
}

void cb_free_names(char **names, int count) {
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
}
