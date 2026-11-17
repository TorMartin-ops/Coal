/**
 * @file tty.c
 * @brief Console line discipline (canonical + raw mode) on top of the keyboard
 *        and the VGA terminal.
 *
 * Input is processed in the keyboard interrupt. Readers sleep on a wait queue;
 * on a single CPU, keeping IRQs off across "check, then block" avoids lost
 * wakeups.
 */
#include <kernel/drivers/display/tty.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/drivers/display/vga_hardware.h>
#include <kernel/drivers/input/keyboard.h>
#include <kernel/drivers/wait.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/sync/spinlock.h>

#define SIGINT_NO  2
#define SIGQUIT_NO 3
#define SIGTSTP_NO 20

#define RING_SIZE 1024            /* power of two */
#define LINE_MAX  256

/* Ring entries: character plus record flags. */
#define E_CHAR 0x00FF
#define E_EOR  0x0100             /* last character of a canonical read record */
#define E_EOF  0x0200             /* end-of-file marker (Ctrl-D on an empty line) */

#define CTRL(c) ((c) & 0x1F)

static struct {
    tty_termios_t tio;
    uint16_t ring[RING_SIZE];
    uint32_t head, tail;          /* free-running indices */
    uint8_t  line[LINE_MAX];      /* line being edited (canonical mode) */
    uint32_t line_len;
    uint32_t fg_pgrp;
    drv_waitq_t waiters;
} g;

static const tty_termios_t default_termios = {
    .c_iflag = TTY_ICRNL | 0002000 /* IXON */,
    .c_oflag = 0000001 | 0000004   /* OPOST | ONLCR */,
    .c_cflag = 0000017 | 0000060 | 0000200 /* B38400 | CS8 | CREAD */,
    .c_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE | TTY_ECHOK | 0100000 /* IEXTEN */,
    .c_cc = {
        [TTY_VINTR] = CTRL('C'), [TTY_VQUIT] = CTRL('\\'), [TTY_VERASE] = 0x7F,
        [TTY_VKILL] = CTRL('U'), [TTY_VEOF] = CTRL('D'), [TTY_VMIN] = 1,
        [8] = CTRL('Q'), [9] = CTRL('S'), [TTY_VSUSP] = CTRL('Z'),
    },
};

/* ---- Interrupt side (IRQs are off) ------------------------------------ */

static void echo(const char *s, size_t n) {
    terminal_write_bytes(s, n);
}

static void echo_char(uint8_t c) {
    if (!(g.tio.c_lflag & TTY_ECHO)) return;
    if (c < 0x20 && c != '\n' && c != '\t') {
        char s[2] = { '^', (char)(c + 0x40) };
        echo(s, 2);
    } else {
        char ch = (char)c;
        echo(&ch, 1);
    }
}

static void ring_push(uint16_t entry) {
    if (g.head - g.tail < RING_SIZE) {
        g.ring[g.head++ & (RING_SIZE - 1)] = entry;
    } /* else: input overrun, drop */
}

/** Moves the edited line into the read ring; its last character ends a read record. */
static void commit_line(void) {
    for (uint32_t i = 0; i < g.line_len; i++) {
        uint16_t e = g.line[i];
        if (i + 1 == g.line_len) e |= E_EOR;
        ring_push(e);
    }
    g.line_len = 0;
}

static void erase_one(void) {
    uint8_t c = g.line[--g.line_len];
    if ((g.tio.c_lflag & (TTY_ECHO | TTY_ECHOE)) == (TTY_ECHO | TTY_ECHOE)) {
        echo("\b \b\b \b", c < 0x20 && c != '\t' ? 6 : 3); /* control characters echo as two columns */
    }
}

static void flush_input(void) {
    g.head = g.tail = 0;
    g.line_len = 0;
}

static void signal_key(int sig, uint8_t c) {
    echo_char(c);
    echo("\n", 1);
    if (!(g.tio.c_lflag & TTY_NOFLSH)) flush_input();
    tty_signal_foreground(sig);
}

static void receive(uint8_t c) {
    uint32_t iflag = g.tio.c_iflag, lflag = g.tio.c_lflag;
    const uint8_t *cc = g.tio.c_cc;

    if (c == '\r') {
        if (iflag & TTY_IGNCR) return;
        if (iflag & TTY_ICRNL) c = '\n';
    } else if (c == '\n' && (iflag & TTY_INLCR)) {
        c = '\r';
    }

    if (lflag & TTY_ISIG) {
        if (c == cc[TTY_VINTR])  { signal_key(SIGINT_NO, c);  return; }
        if (c == cc[TTY_VQUIT])  { signal_key(SIGQUIT_NO, c); return; }
        if (c == cc[TTY_VSUSP])  { signal_key(SIGTSTP_NO, c); return; }
    }

    if (!(lflag & TTY_ICANON)) {
        echo_char(c);
        ring_push(c);
        return;
    }

    if (c == cc[TTY_VERASE] || c == '\b') {
        if (g.line_len) erase_one();
    } else if (c == cc[TTY_VKILL]) {
        while (g.line_len) erase_one();
    } else if (c == CTRL('W')) {
        while (g.line_len && g.line[g.line_len - 1] == ' ') erase_one();
        while (g.line_len && g.line[g.line_len - 1] != ' ') erase_one();
    } else if (c == cc[TTY_VEOF]) {
        if (g.line_len) commit_line(); else ring_push(E_EOF);
    } else if (c == '\n') {
        if (lflag & TTY_ECHO) echo("\n", 1);
        g.line[g.line_len++] = '\n'; /* regular characters leave room for this */
        commit_line();
    } else if (g.line_len < LINE_MAX - 1) {
        g.line[g.line_len++] = c;
        echo_char(c);
    }
}

static void feed_string(const char *s) {
    while (*s) receive((uint8_t)*s++);
}

static const char *const key_sequences[] = {
    [KEY_F1] = "\033OP",  [KEY_F2] = "\033OQ",  [KEY_F3] = "\033OR",  [KEY_F4] = "\033OS",
    [KEY_F5] = "\033[15~", [KEY_F6] = "\033[17~", [KEY_F7] = "\033[18~", [KEY_F8] = "\033[19~",
    [KEY_F9] = "\033[20~", [KEY_F10] = "\033[21~", [KEY_F11] = "\033[23~", [KEY_F12] = "\033[24~",
    [KEY_UP] = "\033[A", [KEY_DOWN] = "\033[B", [KEY_RIGHT] = "\033[C", [KEY_LEFT] = "\033[D",
    [KEY_HOME] = "\033[1~", [KEY_END] = "\033[4~", [KEY_PAGE_UP] = "\033[5~",
    [KEY_PAGE_DOWN] = "\033[6~", [KEY_INSERT] = "\033[2~", [KEY_DELETE] = "\033[3~",
};

static void on_key(const KeyEvent *ev) {
    /* IRQ context: IRQs are already off. */
    if (ev->ch) {
        receive(ev->ch);
    } else if (ev->key < sizeof(key_sequences) / sizeof(key_sequences[0]) && key_sequences[ev->key]) {
        feed_string(key_sequences[ev->key]);
    }
    drv_wake_all(&g.waiters);
}

/* ---- Task side --------------------------------------------------------- */

void tty_init(void) {
    g.tio = default_termios;
    keyboard_set_handler(on_key);
}

size_t tty_input_available(void) {
    uintptr_t flags = local_irq_save();
    size_t n = 0;
    for (uint32_t i = g.tail; i != g.head; i++) {
        uint16_t e = g.ring[i & (RING_SIZE - 1)];
        n += !(e & E_EOF);
        if ((g.tio.c_lflag & TTY_ICANON) && (e & (E_EOR | E_EOF))) break;
    }
    local_irq_restore(flags);
    return n;
}

/** True when a read can return now (a complete record in canonical mode, any byte in raw mode). */
static bool ring_readable(void) {
    if (!(g.tio.c_lflag & TTY_ICANON)) return g.head != g.tail;
    for (uint32_t i = g.tail; i != g.head; i++) {
        if (g.ring[i & (RING_SIZE - 1)] & (E_EOR | E_EOF)) return true;
    }
    return false;
}

bool tty_readable(void) {
    uintptr_t flags = local_irq_save();
    bool r = ring_readable();
    local_irq_restore(flags);
    return r;
}

ssize_t tty_read(void *buf, size_t len) {
    uint8_t *out = buf;
    if (!out || !len) return 0;

    uintptr_t flags = local_irq_save();
    while (!ring_readable()) {
        if (!(g.tio.c_lflag & TTY_ICANON) && g.tio.c_cc[TTY_VMIN] == 0) {
            local_irq_restore(flags);
            return 0;
        }
        drv_wait(&g.waiters);
    }

    size_t n = 0;
    while (n < len && g.head != g.tail) {
        uint16_t e = g.ring[g.tail & (RING_SIZE - 1)];
        if (e & E_EOF) {
            if (n == 0) g.tail++;  /* consume the marker: this read returns 0 */
            break;
        }
        out[n++] = (uint8_t)(e & E_CHAR);
        g.tail++;
        if ((g.tio.c_lflag & TTY_ICANON) && (e & E_EOR)) break;
    }
    local_irq_restore(flags);
    return (ssize_t)n;
}

ssize_t tty_write(const void *buf, size_t len) {
    terminal_write_bytes(buf, len);
    return (ssize_t)len;
}

void tty_get_termios(tty_termios_t *out) {
    uintptr_t flags = local_irq_save();
    *out = g.tio;
    local_irq_restore(flags);
}

void tty_set_termios(const tty_termios_t *in) {
    uintptr_t flags = local_irq_save();
    bool was_canon = g.tio.c_lflag & TTY_ICANON;
    g.tio = *in;
    if (was_canon && !(in->c_lflag & TTY_ICANON)) commit_line(); /* hand over a half-typed line */
    drv_wake_all(&g.waiters);
    local_irq_restore(flags);
}

uint32_t tty_get_fg_pgrp(void) { return g.fg_pgrp; }

int tty_ioctl(unsigned request, void *arg) {
    switch (request) {
    case TTY_TCGETS:
        tty_get_termios(arg);
        return 0;
    case TTY_TCSETS: case TTY_TCSETSW: case TTY_TCSETSF:
        if (request == TTY_TCSETSF) {
            uintptr_t flags = local_irq_save();
            flush_input();
            local_irq_restore(flags);
        }
        tty_set_termios(arg);
        return 0;
    case TTY_TIOCGWINSZ: {
        tty_winsize_t *ws = arg;
        *ws = (tty_winsize_t){ .ws_row = VGA_ROWS, .ws_col = VGA_COLS };
        return 0;
    }
    case TTY_TIOCGPGRP:
        *(uint32_t *)arg = g.fg_pgrp;
        return 0;
    case TTY_TIOCSPGRP:
        g.fg_pgrp = *(uint32_t *)arg;
        return 0;
    case TTY_FIONREAD:
        *(int *)arg = (int)tty_input_available();
        return 0;
    default:
        return -ENOTTY;
    }
}

__attribute__((weak)) void tty_signal_foreground(int sig) {
    (void)sig;
}

/* Legacy line API used by the current read(2) path: a line without its newline,
 * NUL-terminated. Superseded by tty_read(). */
ssize_t terminal_read_line_blocking(char *kbuf, size_t len) {
    if (!kbuf || len < 2) return -EINVAL;
    ssize_t n = tty_read(kbuf, len - 1);
    if (n > 0 && kbuf[n - 1] == '\n') n--;
    kbuf[n > 0 ? n : 0] = '\0';
    return n;
}
