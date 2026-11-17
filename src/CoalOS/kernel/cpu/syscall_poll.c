/**
 * @file syscall_poll.c
 * @brief poll, select and pselect6 on one wait loop.
 *
 * Readiness comes from the drivers' poll() hooks; the caller sleeps on
 * drv_poll_queue, which every wake-up of any wait queue also wakes. None of
 * these calls restart after a signal handler (signal(7)).
 */

#include "syscall_internal.h"
#include <kernel/drivers/wait.h>
#include <kernel/drivers/timer/pit.h>
#include <kernel/lib/string.h>

#define POLL_MAX    128
#define FD_SETSIZE  1024
#define SET_WORDS   (FD_SETSIZE / 32)

/* Scans the descriptors once with IRQs off, so no wake-up can slip in before the task blocks.
 * Returns the number of ready descriptors, or -errno. */
typedef long (*scan_fn)(void *ctx);

/* timeout_ms: < 0 waits for ever, 0 only scans. A signal ends the wait with -EINTR. */
static long poll_wait(scan_fn scan, void *ctx, int32_t timeout_ms) {
    uint64_t deadline = timeout_ms > 0 ? timer_uptime_ms() + (uint32_t)timeout_ms : 0;

    for (;;) {
        uintptr_t flags = local_irq_save();
        long ready = scan(ctx);
        if (!ready && timeout_ms != 0) {
            uint64_t now = timer_uptime_ms();
            if (timeout_ms < 0 || now < deadline) {
                int rc = timeout_ms < 0 ? drv_wait(&drv_poll_queue)
                                        : drv_wait_timeout(&drv_poll_queue, (uint32_t)(deadline - now));
                local_irq_restore(flags);
                if (rc) return rc;
                continue;
            }
        }
        local_irq_restore(flags);
        return ready;
    }
}

//============================================================================
// poll
//============================================================================

struct poll_ctx { struct linux_pollfd *fds; uint32_t nfds; };

static int16_t poll_one(const struct linux_pollfd *p) {
    if (p->fd < 0) return 0;
    sys_file_t *sf = sys_file_get(p->fd);
    if (!sf) return POLLNVAL;
    uint32_t r = vfs_poll(sf->vfs_file);
    sys_file_put(sf);
    return (int16_t)(r & (p->events | POLLERR | POLLHUP));
}

static long poll_scan(void *arg) {
    struct poll_ctx *c = arg;
    long ready = 0;

    for (uint32_t i = 0; i < c->nfds; i++) {
        c->fds[i].revents = poll_one(&c->fds[i]);
        ready += c->fds[i].revents != 0;
    }
    return ready;
}

long sc_poll(uintptr_t ufds, unsigned long nfds, int32_t timeout_ms) {
    struct linux_pollfd fds[POLL_MAX];
    struct poll_ctx c = { fds, (uint32_t)nfds };

    if (nfds > POLL_MAX) return -EINVAL;
    if (copy_from_user(fds, (const void *)ufds, nfds * sizeof(fds[0])) != 0) return -EFAULT;
    long ready = poll_wait(poll_scan, &c, timeout_ms);
    if (ready < 0) return ready;
    for (uint32_t i = 0; i < c.nfds; i++)
        if (copy_to_user(&((struct linux_pollfd *)ufds)[i].revents, &fds[i].revents, sizeof(int16_t)) != 0)
            return -EFAULT;
    return ready;
}

//============================================================================
// select
//============================================================================

enum { SEL_READ, SEL_WRITE, SEL_EXCEPT };

struct select_ctx {
    int n;
    uint32_t in[3][SET_WORDS], out[3][SET_WORDS];
};

static long select_scan(void *arg) {
    struct select_ctx *c = arg;
    long ready = 0;

    memset(c->out, 0, sizeof(c->out));
    for (int fd = 0; fd < c->n; fd++) {
        uint32_t bit = 1u << (fd % 32);
        int w = fd / 32;
        if (!((c->in[SEL_READ][w] | c->in[SEL_WRITE][w] | c->in[SEL_EXCEPT][w]) & bit)) continue;

        sys_file_t *sf = fd < MAX_FD ? sys_file_get(fd) : NULL;
        if (!sf) return -EBADF;
        uint32_t ev = vfs_poll(sf->vfs_file);
        sys_file_put(sf);
        if ((c->in[SEL_READ][w] & bit) && (ev & (POLLIN | POLLHUP | POLLERR))) {
            c->out[SEL_READ][w] |= bit;
            ready++;
        }
        if ((c->in[SEL_WRITE][w] & bit) && (ev & (POLLOUT | POLLERR))) {
            c->out[SEL_WRITE][w] |= bit;
            ready++;
        }                                       /* no descriptor has exceptional conditions (no urgent data) */
    }
    return ready;
}

/* Waits up to timeout_ms (< 0: for ever); *left_ms receives what remains of a finite timeout. */
static long do_select(int32_t n, const uintptr_t uset[3], int32_t timeout_ms, int32_t *left_ms) {
    struct select_ctx c;
    size_t bytes;

    if (n < 0 || n > FD_SETSIZE) return -EINVAL;
    bytes = ((size_t)n + 31) / 32 * 4;
    memset(c.in, 0, sizeof(c.in));
    for (int i = 0; i < 3; i++)
        if (uset[i] && copy_from_user(c.in[i], (const void *)uset[i], bytes) != 0) return -EFAULT;
    c.n = n;

    uint32_t start = (uint32_t)timer_uptime_ms();
    long ready = poll_wait(select_scan, &c, timeout_ms);
    if (ready < 0) return ready;
    for (int i = 0; i < 3; i++)
        if (uset[i] && copy_to_user((void *)uset[i], c.out[i], bytes) != 0) return -EFAULT;
    if (timeout_ms > 0) {
        uint32_t used = (uint32_t)timer_uptime_ms() - start;
        *left_ms = (uint32_t)timeout_ms > used ? timeout_ms - (int32_t)used : 0;
    }
    return ready;
}

/* Milliseconds for a seconds + sub-second pair, rounded up; saturates at "no limit". */
static int32_t to_ms(int64_t sec, int64_t sub, int32_t sub_per_ms) {
    if (sec < 0 || sub < 0 || sub >= 1000 * (int64_t)sub_per_ms) return -2;
    if (sec >= 0x7FFFFFFF / 1000 - 1) return -1;
    return (int32_t)(sec * 1000 + (sub + sub_per_ms - 1) / sub_per_ms);
}

long sc_select(int n, uintptr_t r, uintptr_t w, uintptr_t e, uintptr_t utv) {
    uintptr_t sets[3] = { r, w, e };
    struct linux_timeval tv;
    int32_t timeout = -1, left = 0;

    if (utv) {
        if (copy_from_user(&tv, (const void *)utv, sizeof(tv)) != 0) return -EFAULT;
        if ((timeout = to_ms(tv.tv_sec, tv.tv_usec, 1000)) == -2) return -EINVAL;
    }
    long ready = do_select(n, sets, timeout, &left);
    if (ready >= 0 && timeout > 0) {                        /* Linux reports the time left */
        tv = (struct linux_timeval){ left / 1000, left % 1000 * 1000 };
        if (copy_to_user((void *)utv, &tv, sizeof(tv)) != 0) return -EFAULT;
    }
    return ready;
}

long sc_pselect6(int n, uintptr_t r, uintptr_t w, uintptr_t e, uintptr_t uts, uintptr_t usig) {
    uintptr_t sets[3] = { r, w, e };
    struct linux_timespec ts;
    int32_t timeout = -1, left = 0;

    if (uts) {
        if (copy_from_user(&ts, (const void *)uts, sizeof(ts)) != 0) return -EFAULT;
        if ((timeout = to_ms(ts.tv_sec, ts.tv_nsec, 1000000)) == -2) return -EINVAL;
    }
    if (usig) {                                             /* { const sigset_t *mask; size_t size; } */
        uintptr_t sig[2];
        if (copy_from_user(sig, (const void *)usig, sizeof(sig)) != 0) return -EFAULT;
        if (sig[0]) {
            long rc = signal_mask_swap(sig[0], sig[1]);
            if (rc) return rc;
        }
    }
    long ready = do_select(n, sets, timeout, &left);
    if (ready >= 0 && timeout > 0) {
        ts = (struct linux_timespec){ left / 1000, left % 1000 * 1000000 };
        if (copy_to_user((void *)uts, &ts, sizeof(ts)) != 0) return -EFAULT;
    }
    return ready;
}
