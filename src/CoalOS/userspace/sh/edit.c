/* Line editing on a raw terminal: cursor keys, history, Tab completion of commands and files. */

#include "sh.h"
#include <libc/termios.h>
#include <libc/dirent.h>

#define LINE_MAX_  256
#define HIST_SIZE  32
#define MAX_CAND   64

static char hist[HIST_SIZE][LINE_MAX_];
static int hist_n;

void history_add(const char *line) {
    if (!*line || (hist_n && !strcmp(hist[hist_n - 1], line))) return;
    if (hist_n == HIST_SIZE) {
        memmove(hist[0], hist[1], sizeof hist - sizeof hist[0]);
        hist_n--;
    }
    strcpy(hist[hist_n++], line);
}

void history_print(void) {
    for (int i = 0; i < hist_n; i++) printf("%4d  %s\n", i + 1, hist[i]);
}

static struct termios cooked;
static char buf[LINE_MAX_];
static int len, cur;
static const char *prompt;

static int cmp_cand(const void *a, const void *b) {
    return strcmp(a, b);
}

static void out(const char *s, int n) {
    write(1, s, (size_t)n);
}

static void out_rep(char c, int n) {
    char tmp[16];
    memset(tmp, c, sizeof tmp);
    while (n > 0) {
        int k = n < (int)sizeof tmp ? n : (int)sizeof tmp;
        out(tmp, k);
        n -= k;
    }
}

/* Reprints buf[from..len) plus `erase` blanks, then returns the cursor to cur. */
static void redraw_tail(int from, int erase) {
    out(buf + from, len - from);
    out_rep(' ', erase);
    out_rep('\b', len + erase - cur);
}

static void insert(const char *s, int n) {
    if (len + n >= LINE_MAX_) return;
    memmove(buf + cur + n, buf + cur, (size_t)(len - cur));
    memcpy(buf + cur, s, (size_t)n);
    len += n;
    cur += n;
    out(buf + cur - n, n);
    redraw_tail(cur, 0);
}

static void remove_range(int from, int to) {       /* deletes buf[from..to), cursor at or after `from` */
    out_rep('\b', cur - from);
    memmove(buf + from, buf + to, (size_t)(len - to));
    len -= to - from;
    cur = from;
    redraw_tail(cur, to - from);
}

static void set_line(const char *s) {
    int old = len;
    out_rep('\b', cur);
    len = cur = (int)strlen(s);
    memcpy(buf, s, (size_t)len);
    out(buf, len);
    if (old > len) {
        out_rep(' ', old - len);
        out_rep('\b', old - len);
    }
}

static void redraw_all(void) {
    out(prompt, (int)strlen(prompt));
    out(buf, len);
    out_rep('\b', len - cur);
}

/* --- tab completion --- */

static char cand[MAX_CAND][64];
static int ncand;

static void add_cand(const char *name, int dir) {
    size_t l = strlen(name);
    if (ncand == MAX_CAND || l + 2 > sizeof cand[0]) return;
    for (int i = 0; i < ncand; i++)
        if (!strncmp(cand[i], name, l) && (!cand[i][l] || (cand[i][l] == '/' && !cand[i][l + 1]))) return;
    memcpy(cand[ncand], name, l);
    cand[ncand][l] = dir ? '/' : 0;
    cand[ncand++][l + (dir != 0)] = 0;
}

static void scan_dir(const char *dir, const char *prefix, int cmds) {
    DIR *d = opendir(dir);
    if (!d) return;
    size_t pl = strlen(prefix);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, prefix, pl) || (e->d_name[0] == '.' && prefix[0] != '.')) continue;
        if (cmds) {
            add_cand(e->d_name, 0);
        } else {
            char full[160];
            struct stat st;
            snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
            add_cand(e->d_name, stat(full, &st) == 0 && S_ISDIR(st.st_mode));
        }
    }
    closedir(d);
}

static void complete(void) {
    int ws = cur;
    while (ws > 0 && !strchr(" \t|;&<>", buf[ws - 1])) ws--;
    char word[96];
    int wl = cur - ws;
    if (wl >= (int)sizeof word) return;
    memcpy(word, buf + ws, (size_t)wl);
    word[wl] = 0;
    int first = 1;
    for (int i = ws - 1; i >= 0; i--) {
        if (buf[i] == ' ' || buf[i] == '\t') continue;
        first = strchr("|;&", buf[i]) != NULL;
        break;
    }
    ncand = 0;
    const char *slash = strrchr(word, '/');
    const char *prefix = slash ? slash + 1 : word;
    if (first && !slash) {
        size_t pl = strlen(prefix);
        for (unsigned i = 0; builtin_name(i); i++)
            if (!strncmp(builtin_name(i), prefix, pl)) add_cand(builtin_name(i), 0);
        for (unsigned i = 0; i < n_applets; i++)
            if (!strncmp(applet_names[i], prefix, pl)) add_cand(applet_names[i], 0);
        const char *path = var_get("PATH");
        while (path && *path) {
            const char *e = strchr(path, ':');
            size_t dl = e ? (size_t)(e - path) : strlen(path);
            char dir[96];
            snprintf(dir, sizeof dir, "%.*s", (int)dl, path);
            scan_dir(dir, prefix, 1);
            path += dl + (e != NULL);
        }
    } else {
        char dir[96];
        if (!slash) strcpy(dir, ".");
        else if (slash == word) strcpy(dir, "/");
        else snprintf(dir, sizeof dir, "%.*s", (int)(slash - word), word);
        scan_dir(dir, prefix, 0);
    }
    if (!ncand) { out("\a", 1); return; }
    qsort(cand, (size_t)ncand, sizeof cand[0], cmp_cand);

    int common = (int)strlen(cand[0]), pl = (int)strlen(prefix);
    for (int i = 1; i < ncand; i++) {
        int k = 0;
        while (k < common && cand[0][k] == cand[i][k]) k++;
        common = k;
    }
    if (common > pl) {
        insert(cand[0] + pl, common - pl);
    } else if (ncand == 1) {
        /* nothing to add */
    } else {
        out("\n", 1);
        int col = 0;
        for (int i = 0; i < ncand; i++) {
            int l = (int)strlen(cand[i]) + 2;
            if (col + l > 79) { out("\n", 1); col = 0; }
            out(cand[i], l - 2);
            out("  ", 2);
            col += l;
        }
        out("\n", 1);
        redraw_all();
        return;
    }
    if (ncand == 1 && cand[0][common - 1] != '/') insert(" ", 1);
}

/* Reads one line in raw mode. Returns 1 for a line, 0 at end of input, 2 if cancelled with Ctrl-C. */
int edit_line(const char *p, char *line) {
    prompt = p;
    if (tcgetattr(0, &cooked) < 0) return 0;
    struct termios raw = cooked;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);
    len = cur = 0;
    int hi = hist_n, result = 1;
    char saved[LINE_MAX_] = "";
    out(prompt, (int)strlen(prompt));
    for (;;) {
        unsigned char c;
        ssize_t r = read(0, &c, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { result = 0; break; }
        if (c == '\r' || c == '\n') break;
        if (c == 3) {                                   /* Ctrl-C */
            out("^C", 2);
            len = cur = 0;
            result = 2;
            break;
        } else if (c == 4) {                            /* Ctrl-D */
            if (!len) { result = 0; break; }
            if (cur < len) remove_range(cur, cur + 1);
        } else if (c == 1) {                            /* Ctrl-A */
            out_rep('\b', cur);
            cur = 0;
        } else if (c == 5) {                            /* Ctrl-E */
            out(buf + cur, len - cur);
            cur = len;
        } else if (c == 21) {                           /* Ctrl-U */
            remove_range(0, cur);
        } else if (c == 11) {                           /* Ctrl-K */
            remove_range(cur, len);
        } else if (c == 23) {                           /* Ctrl-W */
            int s = cur;
            while (s > 0 && buf[s - 1] == ' ') s--;
            while (s > 0 && buf[s - 1] != ' ') s--;
            remove_range(s, cur);
        } else if (c == 12) {                           /* Ctrl-L */
            out("\033[H\033[2J", 7);
            redraw_all();
        } else if (c == 127 || c == 8) {                /* Backspace */
            if (cur > 0) remove_range(cur - 1, cur);
        } else if (c == '\t') {
            complete();
        } else if (c == 27) {                           /* escape sequence */
            unsigned char s[2] = { 0, 0 };
            if (read(0, &s[0], 1) != 1) continue;
            if (s[0] != '[' && s[0] != 'O') continue;
            if (read(0, &s[1], 1) != 1) continue;
            if (s[1] >= '0' && s[1] <= '9') {
                unsigned char t;
                if (read(0, &t, 1) != 1) continue;      /* the closing '~' */
                if (s[1] == '3' && cur < len) remove_range(cur, cur + 1);
                else if (s[1] == '1' || s[1] == '7') { out_rep('\b', cur); cur = 0; }
                else if (s[1] == '4' || s[1] == '8') { out(buf + cur, len - cur); cur = len; }
                continue;
            }
            switch (s[1]) {
            case 'A':
                if (hi > 0) {
                    if (hi == hist_n) { memcpy(saved, buf, (size_t)len); saved[len] = 0; }
                    set_line(hist[--hi]);
                }
                break;
            case 'B':
                if (hi < hist_n) {
                    hi++;
                    set_line(hi == hist_n ? saved : hist[hi]);
                }
                break;
            case 'C': if (cur < len) { out(buf + cur, 1); cur++; } break;
            case 'D': if (cur > 0) { out("\b", 1); cur--; } break;
            case 'H': out_rep('\b', cur); cur = 0; break;
            case 'F': out(buf + cur, len - cur); cur = len; break;
            }
        } else if (c >= 32 && c < 127) {
            char ch = (char)c;
            insert(&ch, 1);
        }
    }
    tcsetattr(0, TCSANOW, &cooked);
    out("\n", 1);
    buf[len] = 0;
    memcpy(line, buf, (size_t)len + 1);
    return result;
}

