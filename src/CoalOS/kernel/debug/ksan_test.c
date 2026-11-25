/**
 * @file ksan_test.c
 * @brief Debug-kernel system call 0x1000: provokes one bug of each kind the sanitizers detect.
 *
 * tools/sanitizers/selftest.py runs `ksan_test <op>` for every op and checks the report in the kernel log; it is
 * the regression test of the sanitizers themselves. Never reachable in a release kernel.
 */
#include <kernel/debug/ksan.h>
#include <kernel/debug/lockdep.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/kfence.h>
#include <kernel/memory/frame.h>
#include <kernel/memory/paging.h>
#include <kernel/sync/spinlock.h>
#include <kernel/sync/mutex.h>
#include <kernel/lib/string.h>
#include <kernel/fs/vfs/fs_errno.h>

#define CHURN 300                       /* more than the quarantine holds */

static void churn(size_t size) {
    for (int i = 0; i < CHURN; i++)
        kfree(kmalloc(size));
}

static volatile int g_zero;

__attribute__((noinline)) static long stack_smash(void) {
    char buf[16];

    memset(buf, 'A', 16 + 32 + g_zero * 0);      /* runs over the canary */
    return buf[3];
}

__attribute__((noinline)) static long recurse(long depth) {
    volatile char pad[512];

    pad[0] = (char)depth;
    return depth ? recurse(depth - 1) + pad[0] : pad[1];
}

static spinlock_t lock_a, lock_b;

static long lock_inversion(void) {
    uintptr_t f1, f2;

    spinlock_init(&lock_a);
    spinlock_init(&lock_b);
    f1 = spinlock_acquire_irqsave(&lock_a);
    f2 = spinlock_acquire_irqsave(&lock_b);
    spinlock_release_irqrestore(&lock_b, f2);
    spinlock_release_irqrestore(&lock_a, f1);
    f1 = spinlock_acquire_irqsave(&lock_b);
    f2 = spinlock_acquire_irqsave(&lock_a);             /* B then A after A then B */
    spinlock_release_irqrestore(&lock_a, f2);
    spinlock_release_irqrestore(&lock_b, f1);
    return 0;
}

long sc_ksan_test(long op, long arg) {
    char *p;
    uintptr_t f;
    static mutex_t mu;

    switch (op) {
    case 1:                                             /* write into the unused tail of the object */
        p = kmalloc(20);
        p[24] = 1;
        kfree(p);
        return 0;
    case 2:                                             /* write past the object into the red zone */
        p = kmalloc(32);
        p[32 + 3] = 1;
        kfree(p);
        return 0;
    case 3:                                             /* write before the object */
        p = kmalloc(64);
        p[-2] = 1;
        kfree(p);
        return 0;
    case 4:                                             /* double free */
        p = kmalloc(48);
        kfree(p);
        kfree(p);
        return 0;
    case 5:                                             /* write after free, found when the quarantine lets go */
        p = kmalloc(96);
        kfree(p);
        p[5] = 1;
        churn(96);
        return 0;
    case 6:                                             /* the first byte of fresh memory is 0xa5 */
        p = kmalloc(128);
        arg = (unsigned char)p[0];
        kfree(p);
        return arg;
    case 7:                                             /* free of a pointer inside an object */
        p = kmalloc(64);
        kfree(p + 16);
        kfree(p);
        return 0;
    case 8:                                             /* overflow of a block of pages */
        p = kmalloc(5000);
        p[5000 + 8] = 1;
        kfree(p);
        return 0;
    case 9:                                             /* use after free of a block of pages */
        p = kmalloc(9000);
        kfree(p);
        p[100] = 1;
        for (int i = 0; i < 40; i++)
            kfree(kmalloc(9000));
        return 0;
    case 10:                                            /* UBSan: shift by arg (call with 40) */
        return (long)(1 << (int)arg);
    case 11:                                            /* UBSan: signed overflow (call with 0x7fffffff) */
        return (int)arg + 1;
    case 12:                                            /* UBSan: array index (call with 8) */
        {
            volatile int a[4] = { 1, 2, 3, 4 };
            return a[arg];
        }
    case 13:
        return lock_inversion();
    case 14:                                            /* sleeping with a spinlock held */
        spinlock_init(&lock_a);
        f = spinlock_acquire_irqsave(&lock_a);
        mutex_lock(&mu);
        mutex_unlock(&mu);
        spinlock_release_irqrestore(&lock_a, f);
        return 0;
    case 15:                                            /* recursive acquire: reported, then the machine stops */
        spinlock_init(&lock_a);
        f = spinlock_acquire_irqsave(&lock_a);
        spinlock_acquire_irqsave(&lock_a);
        spinlock_release_irqrestore(&lock_a, f);
        return 0;
    case 16:                                            /* KFENCE: write just past the end of a sampled object */
        p = kfence_alloc(100, NULL, 0);
        if (!p)
            return -ENOMEM;
        p[100 + 8 * 8] = 1;                             /* in the guard page whatever the alignment */
        kfree(p);
        return 0;
    case 17:                                            /* KFENCE: use after free */
        p = kfence_alloc(100, NULL, 0);
        if (!p)
            return -ENOMEM;
        kfree(p);
        p[10] = 1;
        return 0;
    case 18:                                            /* KFENCE: small overflow inside the page, found on free */
        p = kfence_alloc(100, NULL, 0);
        if (!p)
            return -ENOMEM;
        p[100] = 1;
        kfree(p);
        return 0;
    case 19:                                            /* stack protector */
        return stack_smash();
    case 20:                                            /* kernel stack overflow into the guard page */
        return recurse(100000);
    case 21:                                            /* freed frame written through the direct map */
        f = frame_alloc();
        put_frame(f);
        *(volatile char *)phys_to_virt(f) = 1;
        put_frame(frame_alloc());
        for (int i = 0; i < 4; i++)                     /* the frame may sit anywhere in the free lists */
            put_frame(frame_alloc());
        return 0;
    default:
        return -EINVAL;
    }
}
