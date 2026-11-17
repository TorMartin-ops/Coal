#ifndef LIBC_SYS_IOCTL_H
#define LIBC_SYS_IOCTL_H

#define TIOCGWINSZ 0x5413

struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };

int ioctl(int fd, unsigned long request, ...);

#endif
