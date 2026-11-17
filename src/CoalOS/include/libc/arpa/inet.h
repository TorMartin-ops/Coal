#ifndef LIBC_ARPA_INET_H
#define LIBC_ARPA_INET_H

#include <libc/netinet/in.h>

#define INET_ADDRSTRLEN 16

int         inet_aton(const char *s, struct in_addr *out);
in_addr_t   inet_addr(const char *s);
char       *inet_ntoa(struct in_addr a);
int         inet_pton(int af, const char *s, void *dst);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);

#endif
