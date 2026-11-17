#ifndef LIBC_SYS_SELECT_H
#define LIBC_SYS_SELECT_H

#include <libc/sys/time.h>

#define FD_SETSIZE 1024

/* Same layout as the Linux i386 kernel: 1024 bits. */
typedef struct { unsigned long fds_bits[FD_SETSIZE / 32]; } fd_set;

#define FD_ZERO(s)   do { for (int _i = 0; _i < FD_SETSIZE / 32; _i++) (s)->fds_bits[_i] = 0; } while (0)
#define FD_SET(fd, s)   ((s)->fds_bits[(fd) / 32] |= 1UL << ((fd) % 32))
#define FD_CLR(fd, s)   ((s)->fds_bits[(fd) / 32] &= ~(1UL << ((fd) % 32)))
#define FD_ISSET(fd, s) (((s)->fds_bits[(fd) / 32] >> ((fd) % 32)) & 1)

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *timeout);

#endif
