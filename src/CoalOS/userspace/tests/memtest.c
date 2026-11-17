/* memtest MB [PROCS]: PROCS processes each map MB of anonymous memory, fill it
 * with a per-page pattern, fork a child that dirties part of it (copy on
 * write), then verify that every page still holds the right pattern. */

#include <libc/stdint.h>
#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/unistd.h>
#include <libc/syscall.h>
#include <libc/sys/wait.h>

#define PAGE 4096u

static uint32_t pattern(unsigned seed, unsigned page, unsigned slot) {
    return (seed * 2654435761u) ^ (page * 40503u) ^ slot;
}

static unsigned check(volatile uint32_t *mem, unsigned pages, unsigned seed) {
    unsigned bad = 0;
    for (unsigned p = 0; p < pages; p++) {
        volatile uint32_t *w = mem + (size_t)p * (PAGE / 4);
        bad += w[0] != pattern(seed, p, 0) || w[512] != pattern(seed, p, 1) || w[1023] != pattern(seed, p, 2);
    }
    return bad;
}

static int worker(unsigned mb, unsigned seed) {
    unsigned pages = mb * (1024 * 1024 / PAGE);
    long r = __sc6(SYS_MMAP, 0, (long)mb << 20, 3, 0x22, -1, 0)   /* PROT_RW, MAP_PRIVATE|MAP_ANON */;
    if (r < 0 && r > -4096) { printf("memtest: mmap failed (%ld)\n", r); return 2; }
    volatile uint32_t *mem = (volatile uint32_t *)r;

    for (unsigned p = 0; p < pages; p++) {
        volatile uint32_t *w = mem + (size_t)p * (PAGE / 4);
        w[0] = pattern(seed, p, 0); w[512] = pattern(seed, p, 1); w[1023] = pattern(seed, p, 2);
    }
    sleep(2);                                   /* let the other workers fill theirs */

    pid_t c = fork();
    if (c == 0) {                               /* dirty every 7th page: COW copies */
        for (unsigned p = 0; p < pages; p += 7) mem[(size_t)p * (PAGE / 4) + 1] = 0xDEADBEEF;
        _exit(check(mem, pages, seed) != 0);    /* untouched words must still match */
    }
    int st = 0;
    waitpid(c, &st, 0);
    unsigned bad = check(mem, pages, seed) + (unsigned)(WIFEXITED(st) ? WEXITSTATUS(st) : 1);
    for (unsigned p = 0; p < pages; p += 7)     /* the parent's copy must not see the child's writes */
        bad += mem[(size_t)p * (PAGE / 4) + 1] == 0xDEADBEEF;
    printf("memtest: worker %u: %u MiB, %u errors\n", seed, mb, bad);
    return bad != 0;
}

int main(int argc, char **argv) {
    unsigned mb = argc > 1 ? (unsigned)atoi(argv[1]) : 64;
    unsigned procs = argc > 2 ? (unsigned)atoi(argv[2]) : 1;
    int failed = 0;

    for (unsigned i = 0; i < procs; i++)
        if (fork() == 0)
            _exit(worker(mb, i + 1));
    for (unsigned i = 0; i < procs; i++) {
        int st = 0;
        waitpid(-1, &st, 0);
        failed += !WIFEXITED(st) || WEXITSTATUS(st);
    }
    printf("memtest: %s (%u x %u MiB)\n", failed ? "FAILED" : "PASSED", procs, mb);
    return failed != 0;
}
