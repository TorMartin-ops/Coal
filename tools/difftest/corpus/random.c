/* getrandom and /dev/urandom: lengths, flags, error cases, and that output varies. */
#include "trace.h"

int main(void) {
    init();
    unsigned char a[512], b[512];
    R("getrandom(16)", K(SYS_getrandom, a, 16, 0));
    R("getrandom(0)", K(SYS_getrandom, a, 0, 0));
    R("getrandom(256)", K(SYS_getrandom, a, 256, 0));
    R("getrandom(512)", K(SYS_getrandom, a, 512, 0));
    memset(a, 0, sizeof a); memset(b, 0, sizeof b);
    long n1 = K(SYS_getrandom, a, 64, 0), n2 = K(SYS_getrandom, b, 64, 0);
    NOTE("two draws of 64 bytes differ: %s (%ld,%ld)", yn(memcmp(a, b, 64) != 0), n1, n2);
    int zeros = 0, ones = 0;
    K(SYS_getrandom, a, 512, 0);
    for (int i = 0; i < 512; i++) { zeros += a[i] == 0; ones += __builtin_popcount(a[i]); }
    NOTE("512 random bytes: zero bytes < 12: %s, set bits within 1800..2300: %s", yn(zeros < 12), yn(ones > 1800 && ones < 2300));
    R("getrandom(GRND_NONBLOCK)", K(SYS_getrandom, a, 16, 1));
    R("getrandom(GRND_RANDOM)", K(SYS_getrandom, a, 16, 2));
    R("getrandom(bad flags)", K(SYS_getrandom, a, 16, 0x80));
    R("getrandom(huge len, bad ptr)", K(SYS_getrandom, 8, 16, 0));
    R("getrandom(len 100000)", POS(K(SYS_getrandom, (long)malloc(100000), 100000, 0)));
    long f = R("open(/dev/urandom)", K(SYS_open, "/dev/urandom", O_RDONLY));
    memset(a, 0, 32);
    R("read(urandom 32)", K(SYS_read, f, a, 32));
    NOTE("urandom data not all zero: %s", yn(memcmp(a, b + 100, 32) != 0 || a[0] | a[1] | a[2] | a[3]));
    STATT("stat(/dev/urandom)", "/dev/urandom");
    R("write(urandom)", K(SYS_write, f, "seed", 4));
    K(SYS_close, f);
    f = R("open(/dev/random)", K(SYS_open, "/dev/random", O_RDONLY));
    R("read(random 8)", K(SYS_read, f, a, 8));
    K(SYS_close, f);
    f = R("open(/dev/zero)", K(SYS_open, "/dev/zero", O_RDWR));
    memset(a, 7, 8);
    R("read(zero 8)", K(SYS_read, f, a, 8));
    NOTE("zero bytes returned: %s", yn(!a[0] && !a[7]));
    R("write(zero)", K(SYS_write, f, "abc", 3));
    K(SYS_close, f);
    f = R("open(/dev/null)", K(SYS_open, "/dev/null", O_RDWR));
    R("read(null)", K(SYS_read, f, a, 8));
    R("write(null)", K(SYS_write, f, "abc", 3));
    STATT("stat(/dev/null)", "/dev/null");
    K(SYS_close, f);
    return 0;
}
