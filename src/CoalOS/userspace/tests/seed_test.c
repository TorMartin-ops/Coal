/* Prints 16 bytes from /dev/urandom. With coal.seed= on the kernel command line the bytes repeat from boot to boot. */

#include <libc/stdio.h>
#include <libc/unistd.h>
#include <libc/fcntl.h>

int main(void) {
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd < 0 || read(fd, b, sizeof b) != (ssize_t)sizeof b) {
        puts("seed_test: cannot read /dev/urandom");
        return 1;
    }
    printf("random:");
    for (int i = 0; i < 16; i++) printf("%02x", b[i]);
    printf("\n");
    return 0;
}
