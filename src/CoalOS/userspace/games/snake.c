/* snake - the classic game for the Coal OS console.
 * Arrow keys or WASD to steer, p to pause, q to quit.
 * Usage: snake [-s ms]   (-s sets the starting step time, default 140 ms) */
#include <libc/poll.h>
#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/termios.h>
#include <libc/time.h>
#include <libc/unistd.h>

#define W 38               /* board cells; each cell is two screen columns wide */
#define H 20
#define MAXLEN (W * H)
#define TOP 3              /* screen row of the board's top border */
#define LEFT 2             /* screen column of the board's left border */

enum dir { UP, DOWN, LEFTD, RIGHTD };

static int sx[MAXLEN], sy[MAXLEN], len, head;
static int fx, fy, score, best;
static enum dir dir, next_dir;
static struct termios saved;

static void out(const char *s) { write(1, s, strlen(s)); }

static void at(int row, int col) {
    char b[24];
    snprintf(b, sizeof b, "\033[%d;%dH", row, col);
    out(b);
}

static void cell(int x, int y, const char *color, const char *glyph) {
    at(TOP + 1 + y, LEFT + 1 + 2 * x);
    out(color);
    out(glyph);
    out("\033[0m");
}

static int occupied(int x, int y) {
    for (int i = 0; i < len; i++) {
        int k = (head - i + MAXLEN) % MAXLEN;
        if (sx[k] == x && sy[k] == y) return 1;
    }
    return 0;
}

static void place_food(void) {
    do { fx = rand() % W; fy = rand() % H; } while (occupied(fx, fy));
    cell(fx, fy, "\033[1;31m", "()");
}

static void status(void) {
    char b[96];
    at(TOP + H + 2, LEFT);
    snprintf(b, sizeof b, "\033[1;37mScore %-5d Best %-5d\033[0m  arrows/WASD steer  p pause  q quit ", score, best);
    out(b);
}

static void draw_board(void) {
    out("\033[2J\033[H\033[1;32m  C O A L   S N A K E\033[0m");
    at(TOP, LEFT);
    out("\033[36m+");
    for (int i = 0; i < 2 * W; i++) out("-");
    out("+");
    for (int y = 0; y < H; y++) {
        at(TOP + 1 + y, LEFT);
        out("|");
        at(TOP + 1 + y, LEFT + 1 + 2 * W);
        out("|");
    }
    at(TOP + H + 1, LEFT);
    out("+");
    for (int i = 0; i < 2 * W; i++) out("-");
    out("+\033[0m");
}

static void new_game(void) {
    draw_board();
    len = 4;
    head = len - 1;
    for (int i = 0; i < len; i++) {
        sx[i] = W / 2 - (len - 1 - i);
        sy[i] = H / 2;
        cell(sx[i], sy[i], "\033[1;32m", i == head ? "@@" : "[]");
    }
    dir = next_dir = RIGHTD;
    score = 0;
    place_food();
    status();
}

static void raw_mode(int on) {
    if (on) {
        struct termios t;
        tcgetattr(0, &saved);
        t = saved;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
        out("\033[?25l");             /* hide cursor */
    } else {
        tcsetattr(0, TCSANOW, &saved);
        out("\033[0m\033[?25h\033[2J\033[H");
    }
}

/* Reads pending keys; returns 'q' to quit, 'p' to pause, else 0. */
static int read_keys(int timeout_ms) {
    struct pollfd p = { .fd = 0, .events = POLLIN };
    int ret = 0;
    if (poll(&p, 1, timeout_ms) <= 0) return 0;
    char b[16];
    int n = (int)read(0, b, sizeof b);
    for (int i = 0; i < n; i++) {
        char c = b[i];
        if (c == '\033' && i + 2 < n && b[i + 1] == '[') {
            c = b[i + 2];
            i += 2;
            if (c == 'A') c = 'w'; else if (c == 'B') c = 's';
            else if (c == 'C') c = 'd'; else if (c == 'D') c = 'a';
        }
        switch (c) {
        case 'w': case 'W': if (dir != DOWN) next_dir = UP; break;
        case 's': case 'S': if (dir != UP) next_dir = DOWN; break;
        case 'a': case 'A': if (dir != RIGHTD) next_dir = LEFTD; break;
        case 'd': case 'D': if (dir != LEFTD) next_dir = RIGHTD; break;
        case 'q': case 'Q': ret = 'q'; break;
        case 'p': case 'P': ret = 'p'; break;
        case 'r': case 'R': ret = 'r'; break;
        }
    }
    return ret;
}

/* Advances one step; returns 0 when the snake dies. */
static int step(void) {
    dir = next_dir;
    int nx = sx[head], ny = sy[head];
    if (dir == UP) ny--; else if (dir == DOWN) ny++;
    else if (dir == LEFTD) nx--; else nx++;
    if (nx < 0 || nx >= W || ny < 0 || ny >= H) return 0;

    int grow = (nx == fx && ny == fy);
    if (!grow) {                       /* the tail moves away first */
        int tail = (head - len + 1 + MAXLEN) % MAXLEN;
        cell(sx[tail], sy[tail], "", "  ");
        len--;
    }
    if (occupied(nx, ny)) return 0;
    cell(sx[head], sy[head], "\033[1;32m", "[]");
    head = (head + 1) % MAXLEN;
    sx[head] = nx;
    sy[head] = ny;
    len++;
    cell(nx, ny, "\033[1;32m", "@@");
    if (grow) {
        score += 10;
        if (score > best) best = score;
        if (len < MAXLEN) place_food();
        status();
    }
    return 1;
}

static int base_speed = 140;

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "-s") == 0) base_speed = atoi(argv[2]);
    if (base_speed < 30) base_speed = 30;
    srand((unsigned)time(NULL));
    raw_mode(1);
    new_game();
    for (;;) {
        int speed = base_speed - score / 2;   /* gets faster as you eat */
        if (speed < base_speed / 2) speed = base_speed / 2;
        int k = read_keys(speed);
        if (k == 'q') break;
        if (k == 'p') {
            at(TOP + H / 2 + 1, LEFT + W - 4);
            out("\033[1;33m PAUSED \033[0m");
            while ((k = read_keys(1000)) != 'p' && k != 'q') {}
            if (k == 'q') break;
            at(TOP + H / 2 + 1, LEFT + W - 4);
            out("        ");
        }
        if (!step()) {
            at(TOP + H / 2 + 1, LEFT + W - 8);
            out("\033[1;31m GAME OVER  r = again \033[0m");
            while ((k = read_keys(1000)) == 0) {}
            if (k == 'q') break;
            new_game();
        }
    }
    raw_mode(0);
    printf("Thanks for playing. Score %d, best %d.\n", score, best);
    return 0;
}
