/**
 * @file tty.h
 * @brief Console line discipline: keyboard input, canonical/raw modes, echo,
 *        job-control signals. Linux termios semantics (i386 struct termios).
 */
#ifndef TTY_H
#define TTY_H

#include <kernel/core/types.h>

/* c_iflag */
#define TTY_IGNCR  0000200
#define TTY_ICRNL  0000400
#define TTY_INLCR  0000100
/* c_lflag */
#define TTY_ISIG   0000001
#define TTY_ICANON 0000002
#define TTY_ECHO   0000010
#define TTY_ECHOE  0000020
#define TTY_ECHOK  0000040
#define TTY_NOFLSH 0000200
/* c_cc indices */
#define TTY_VINTR  0
#define TTY_VQUIT  1
#define TTY_VERASE 2
#define TTY_VKILL  3
#define TTY_VEOF   4
#define TTY_VMIN   6
#define TTY_VSUSP  10
#define TTY_NCCS   19

/** Layout of the kernel's struct termios (TCGETS/TCSETS argument) on i386. */
typedef struct {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[TTY_NCCS];
} tty_termios_t;

/** struct winsize (TIOCGWINSZ argument). */
typedef struct {
    uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel;
} tty_winsize_t;

/* Linux ioctl request numbers handled by tty_ioctl() */
#define TTY_TCGETS      0x5401
#define TTY_TCSETS      0x5402
#define TTY_TCSETSW     0x5403
#define TTY_TCSETSF     0x5404
#define TTY_TIOCGPGRP   0x540F
#define TTY_TIOCSPGRP   0x5410
#define TTY_TIOCGWINSZ  0x5413
#define TTY_FIONREAD    0x541B

/** Hooks the keyboard up to the line discipline. */
void tty_init(void);

/**
 * Reads input. Canonical mode: blocks for a full line and returns at most one
 * line per call (including its '\n'); returns 0 at end of file (Ctrl-D on an
 * empty line). Raw mode: returns what is available, blocking only for the
 * first byte (VMIN=0 never blocks).
 */
ssize_t tty_read(void *buf, size_t len);

/** True when a read would not block (including end of file). */
bool tty_readable(void);

/** Writes to the screen. Returns len. */
ssize_t tty_write(const void *buf, size_t len);

/** Number of bytes a read would return without blocking. */
size_t tty_input_available(void);

void tty_get_termios(tty_termios_t *out);
void tty_set_termios(const tty_termios_t *in);

/** Raw mode toggle: no line editing, no echo, no signal keys. */
void tty_set_raw(bool raw);

/**
 * ioctl on the console with kernel-space arguments. Handles TCGETS, TCSETS*,
 * TIOCGWINSZ, TIOCGPGRP, TIOCSPGRP and FIONREAD; returns 0 or a negative errno
 * (-ENOTTY for unknown requests).
 */
int tty_ioctl(unsigned request, void *arg);

uint32_t tty_get_fg_pgrp(void);
void tty_set_fg_pgrp(uint32_t pgid);

/**
 * Hook called from the keyboard interrupt when a signal key (Ctrl-C = SIGINT,
 * Ctrl-\ = SIGQUIT, Ctrl-Z = SIGTSTP) is typed. The default does nothing; the
 * process code overrides it to signal the foreground process group
 * (tty_get_fg_pgrp()).
 */
void tty_signal_foreground(int sig);

#endif /* TTY_H */
