#ifndef LIBC_SYS_SELECT_H
#define LIBC_SYS_SELECT_H

#include <libc/sys/time.h>

#define FD_SETSIZE 1024

/* Same layout as the Linux kernel: 1024 bits. */
typedef struct { unsigned long fds_bits[FD_SETSIZE / 64]; } fd_set;

#define FD_ZERO(s)   do { for (int _i = 0; _i < FD_SETSIZE / 64; _i++) (s)->fds_bits[_i] = 0; } while (0)
#define FD_SET(fd, s)   ((s)->fds_bits[(fd) / 64] |= 1UL << ((fd) % 64))
#define FD_CLR(fd, s)   ((s)->fds_bits[(fd) / 64] &= ~(1UL << ((fd) % 64)))
#define FD_ISSET(fd, s) (((s)->fds_bits[(fd) / 64] >> ((fd) % 64)) & 1)

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *timeout);

#endif
