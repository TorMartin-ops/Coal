#ifndef LIBC_TERMIOS_H
#define LIBC_TERMIOS_H

/* Linux kernel struct termios (what TCGETS/TCSETS exchange). */
#define NCCS 19
typedef unsigned int tcflag_t;
typedef unsigned char cc_t;

struct termios {
    tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
    cc_t c_line;
    cc_t c_cc[NCCS];
};

#define VINTR 0
#define VEOF  4
#define VTIME 5
#define VMIN  6

#define ICRNL  0x100
#define IXON   0x400
#define OPOST  1
#define ONLCR  4
#define ISIG   1
#define ICANON 2
#define ECHO   8

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int when, const struct termios *t);

#endif
