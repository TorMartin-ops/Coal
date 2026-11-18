/* ksan_test OP [ARG]: provoke one bug in the debug kernel (system call 0x1000, see kernel/debug/ksan_test.c). The
 * reports go to the kernel log; tools/sanitizers/selftest.py checks them. */
#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/syscall.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: ksan_test OP [ARG]\n");
        return 2;
    }
    long r = __sc(0x1000, atoi(argv[1]), argc > 2 ? atoi(argv[2]) : 0, 0, 0, 0);
    printf("ksan_test %s: %ld\n", argv[1], r);
    return 0;
}
