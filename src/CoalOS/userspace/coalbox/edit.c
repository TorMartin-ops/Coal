/* edit: a small full-screen text editor.
 * Keys: arrows, Home/End, PgUp/PgDn, Backspace/Delete, Enter, ^S save, ^Q quit, ^F find (^F again: next). */

#include "cb.h"
#include <libc/termios.h>
#include <libc/poll.h>
#include <libc/sys/ioctl.h>

#define KEY_UP    1000
#define KEY_DOWN  1001
#define KEY_RIGHT 1002
#define KEY_LEFT  1003
#define KEY_HOME  1004
#define KEY_END   1005
#define KEY_PGUP  1006
#define KEY_PGDN  1007
#define KEY_DEL   1008
#define CTRL(c)   ((c) & 31)
#define MAX_LINES 65536

static char **lines;
static int nlines, cx, cy, top, left, rows = 24, cols = 80, dirty, quit_armed;
static const char *path;
static char msg[80], needle[64];

static void load(void) {
    FILE *f = fopen(path, "r");
    char *buf = NULL;
    size_t cap = 0;
    ssize_t n;
    lines = calloc(MAX_LINES, sizeof *lines);
    if (f) {
        while (nlines < MAX_LINES && (n = getline(&buf, &cap, f)) >= 0) {
            if (n && buf[n - 1] == '\n') buf[--n] = 0;
            lines[nlines++] = strdup(buf);
        }
        free(buf);
        fclose(f);
    }
    if (!nlines) lines[nlines++] = strdup("");
}

static int save(void) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { snprintf(msg, sizeof msg, "cannot write %s: %s", path, strerror(errno)); return -1; }
    for (int i = 0; i < nlines; i++)
        if (cb_write_all(fd, lines[i], strlen(lines[i])) < 0 || cb_write_all(fd, "\n", 1) < 0) {
            close(fd);
            snprintf(msg, sizeof msg, "write error: %s", strerror(errno));
            return -1;
        }
    close(fd);
    dirty = 0;
    snprintf(msg, sizeof msg, "saved %d lines", nlines);
    return 0;
}

/* Reads one key; escape sequences become KEY_*. */
static int read_key(void) {
    unsigned char c, s[4];
    while (read(0, &c, 1) != 1)
        if (errno != EINTR) return CTRL('Q');
    if (c != 27) return c;
    struct pollfd pf = { 0, POLLIN, 0 };
    if (poll(&pf, 1, 50) <= 0 || read(0, &s[0], 1) != 1) return 27;
    if (s[0] != '[' && s[0] != 'O') return 27;
    if (read(0, &s[1], 1) != 1) return 27;
    if (s[1] >= '0' && s[1] <= '9') {
        if (read(0, &s[2], 1) != 1) return 27;
        switch (s[1]) {
        case '1': case '7': return KEY_HOME;
        case '3': return KEY_DEL;
        case '4': case '8': return KEY_END;
        case '5': return KEY_PGUP;
        case '6': return KEY_PGDN;
        }
        return 27;
    }
    switch (s[1]) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    }
    return 27;
}

static void insert_line(int at, const char *text) {
    if (nlines >= MAX_LINES) return;
    memmove(lines + at + 1, lines + at, (size_t)(nlines - at) * sizeof *lines);
    lines[at] = strdup(text);
    nlines++;
}

static void insert_char(int c) {
    size_t len = strlen(lines[cy]);
    char *l = realloc(lines[cy], len + 2);
    if (!l) return;
    memmove(l + cx + 1, l + cx, len - (size_t)cx + 1);
    l[cx++] = (char)c;
    lines[cy] = l;
    dirty = 1;
}

static void split_line(void) {
    char *tail = strdup(lines[cy] + cx);
    lines[cy][cx] = 0;
    insert_line(cy + 1, tail);
    free(tail);
    cy++;
    cx = 0;
    dirty = 1;
}

static void join_with_next(int y) {
    if (y + 1 >= nlines) return;
    size_t a = strlen(lines[y]), b = strlen(lines[y + 1]);
    char *l = realloc(lines[y], a + b + 1);
    if (!l) return;
    memcpy(l + a, lines[y + 1], b + 1);
    lines[y] = l;
    free(lines[y + 1]);
    memmove(lines + y + 1, lines + y + 2, (size_t)(nlines - y - 2) * sizeof *lines);
    nlines--;
    dirty = 1;
}

static void backspace(void) {
    if (cx > 0) {
        memmove(lines[cy] + cx - 1, lines[cy] + cx, strlen(lines[cy]) - (size_t)cx + 1);
        cx--;
        dirty = 1;
    } else if (cy > 0) {
        cx = (int)strlen(lines[cy - 1]);
        join_with_next(--cy);
    }
}

static void delete_char(void) {
    if (lines[cy][cx]) {
        memmove(lines[cy] + cx, lines[cy] + cx + 1, strlen(lines[cy]) - (size_t)cx);
        dirty = 1;
    } else join_with_next(cy);
}

static void find_next(void) {
    for (int k = 0; k <= nlines; k++) {
        int y = (cy + k) % nlines;
        const char *from = lines[y] + (k == 0 ? cx + 1 : 0);
        if (k == 0 && cx + 1 > (int)strlen(lines[y])) continue;
        const char *hit = strstr(from, needle);
        if (hit) { cy = y; cx = (int)(hit - lines[y]); return; }
    }
    snprintf(msg, sizeof msg, "'%s' not found", needle);
}

/* Prompts on the status line; returns the length typed, or -1 if cancelled. */
static int prompt(const char *label, char *out, size_t cap) {
    size_t n = 0;
    for (;;) {
        printf("\033[%d;1H\033[7m%s%s\033[K\033[0m", rows, label, out);
        fflush(stdout);
        int k = read_key();
        if (k == '\r' || k == '\n') return (int)n;
        if (k == 27 || k == CTRL('Q') || k == CTRL('C')) return -1;
        if ((k == 127 || k == 8) && n) out[--n] = 0;
        else if (k >= 32 && k < 127 && n + 1 < cap) { out[n++] = (char)k; out[n] = 0; }
    }
}

static void draw(void) {
    if (cy < top) top = cy;
    if (cy >= top + rows - 1) top = cy - rows + 2;
    if (cx < left) left = cx;
    if (cx >= left + cols) left = cx - cols + 1;
    fputs("\033[H", stdout);
    for (int r = 0; r < rows - 1; r++) {
        int y = top + r;
        if (y < nlines && (int)strlen(lines[y]) > left)
            fwrite(lines[y] + left, 1, strlen(lines[y] + left) > (size_t)cols ? (size_t)cols : strlen(lines[y] + left), stdout);
        else if (y >= nlines) fputc('~', stdout);
        fputs("\033[K", stdout);
        if (r < rows - 2) fputs("\r\n", stdout);
    }
    char st[100];
    snprintf(st, sizeof st, " %s%s  Ln %d, Col %d  %s", path, dirty ? " [+]" : "", cy + 1, cx + 1,
             msg[0] ? msg : "^S save  ^Q quit  ^F find");
    printf("\033[%d;1H\033[7m%-*.*s\033[0m", rows, cols, cols, st);
    printf("\033[%d;%dH", cy - top + 1, cx - left + 1);
    fflush(stdout);
}

static void clamp_cx(void) {
    int len = (int)strlen(lines[cy]);
    if (cx > len) cx = len;
}

int edit_main(int argc, char **argv) {
    if (argc != 2) return cb_usage("FILE");
    path = argv[1];
    if (!isatty(0)) return fprintf(stderr, "edit: needs a terminal\n"), 1;
    load();
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row >= 3 && ws.ws_col >= 20) rows = ws.ws_row, cols = ws.ws_col;
    struct termios old, raw;
    tcgetattr(0, &old);
    raw = old;
    raw.c_iflag &= ~(tcflag_t)(ICRNL | IXON);
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);
    fputs("\033[2J", stdout);
    for (;;) {
        draw();
        int k = read_key();
        msg[0] = 0;
        if (k != CTRL('Q')) quit_armed = 0;
        if (k == CTRL('Q')) {
            if (dirty && !quit_armed++) { snprintf(msg, sizeof msg, "unsaved changes: ^Q again to discard"); continue; }
            break;
        }
        switch (k) {
        case CTRL('S'): save(); break;
        case CTRL('F'): {
            char q[sizeof needle] = "";
            int n = prompt("find (empty = repeat): ", q, sizeof q);
            if (n < 0) break;
            if (n) strcpy(needle, q);
            if (needle[0]) find_next();
            break;
        }
        case KEY_UP: if (cy > 0) cy--; break;
        case KEY_DOWN: if (cy + 1 < nlines) cy++; break;
        case KEY_LEFT:
            if (cx > 0) cx--;
            else if (cy > 0) cx = (int)strlen(lines[--cy]);
            break;
        case KEY_RIGHT:
            if (lines[cy][cx]) cx++;
            else if (cy + 1 < nlines) cy++, cx = 0;
            break;
        case KEY_HOME: cx = 0; break;
        case KEY_END: cx = (int)strlen(lines[cy]); break;
        case KEY_PGUP: cy = cy > rows - 2 ? cy - (rows - 2) : 0; break;
        case KEY_PGDN: cy = cy + rows - 2 < nlines ? cy + rows - 2 : nlines - 1; break;
        case KEY_DEL: delete_char(); break;
        case 127: case 8: backspace(); break;
        case '\r': case '\n': split_line(); break;
        case '\t': for (int i = 0; i < 4; i++) insert_char(' '); break;
        default: if (k >= 32 && k < 256 && k != 127) insert_char(k);
        }
        clamp_cx();
    }
    tcsetattr(0, TCSANOW, &old);
    fputs("\033[2J\033[H", stdout);
    return 0;
}
