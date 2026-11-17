/**
 * @file pipe.c
 * @brief Anonymous pipes and socketpair(): descriptors on kernel ring buffers.
 *
 * Each end is an ordinary vnode of an unmounted "pipefs" driver, so read,
 * write, dup, fork and close work through the usual descriptor layer. A pipe
 * end reads from one ring and writes to another (never both); the two ends of
 * a socketpair use two rings, one per direction. A ring holds 64 KiB (the
 * Linux default) in 4 KiB pages that are allocated when first written and
 * given back when the ring drains. Writes of up to PIPE_BUF bytes are atomic.
 * Readers and writers sleep on the ring's wait queue; the kernel runs with
 * interrupts off inside each operation, which is all the locking there is.
 */
#include <kernel/fs/vfs/sys_file.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/paging.h>
#include <kernel/process/process.h>
#include <kernel/process/scheduler.h>
#include <kernel/process/signal.h>
#include <kernel/lib/string.h>
#include <kernel/drivers/wait.h>

#define PIPE_PAGES  16
#define PIPE_SIZE   (PIPE_PAGES * PAGE_SIZE)
#define PIPE_BUF    4096u                   /* POSIX: writes up to this size are never interleaved */

struct pipe {
    uint8_t *page[PIPE_PAGES];              /* the ring, one page per slot; NULL while unused */
    uint32_t head, count;                   /* read position, bytes stored */
    int      readers, writers;              /* open ends */
    drv_waitq_t wait;
};

/* rd/wr are the rings this end reads from and writes to; NULL for the direction it lacks. */
struct pipe_end {
    struct pipe *rd, *wr;
    bool nonblock;
};

static uint32_t room(const struct pipe *p) { return PIPE_SIZE - p->count; }

/* The ring is empty: restart at slot 0 and give the other pages back. */
static void drained(struct pipe *p) {
    p->head = 0;
    for (int i = 1; i < PIPE_PAGES; i++) {
        kfree(p->page[i]);
        p->page[i] = NULL;
    }
}

static ssize_t pipe_read(vnode_t *v, off_t pos, void *out, size_t len) {
    struct pipe_end *e = v->data;
    struct pipe *p = e->rd;
    size_t done = 0;
    int rc = 0;
    (void)pos;

    if (!p) return v->type == S_IFSOCK ? 0 : -EBADF;    /* a socket end shut down for reading sees end of file */
    uintptr_t fl = local_irq_save();
    while (!p->count && !rc) {
        if (!p->writers) break;
        if (e->nonblock) rc = -EAGAIN;
        else rc = drv_wait(&p->wait);
    }
    if (rc && p->count) rc = 0;                         /* data arrived together with the signal: take it */
    while (done < len && p->count) {
        uint32_t off = p->head % PAGE_SIZE;
        size_t n = MIN(MIN(len - done, p->count), PAGE_SIZE - off);
        memcpy((uint8_t *)out + done, p->page[p->head / PAGE_SIZE] + off, n);
        p->head = (p->head + n) % PIPE_SIZE;
        p->count -= n;
        done += n;
    }
    if (done) {
        if (!p->count) drained(p);
        drv_wake_all(&p->wait);
    }
    local_irq_restore(fl);
    return done ? (ssize_t)done : rc;
}

static ssize_t pipe_write(vnode_t *v, off_t pos, const void *in, size_t len) {
    struct pipe_end *e = v->data;
    struct pipe *p = e->wr;
    size_t done = 0;
    int rc = 0;
    (void)pos;

    if (!p) return v->type == S_IFSOCK ? -EPIPE : -EBADF;
    uintptr_t fl = local_irq_save();
    while (done < len && !rc) {
        if (!p->readers) { rc = -EPIPE; break; }
        size_t need = len <= PIPE_BUF ? len : 1;        /* a small write waits for room for all of it */
        if (room(p) < need) {
            if (e->nonblock) { rc = -EAGAIN; break; }
            drv_wake_all(&p->wait);                     /* let readers drain what is already in */
            rc = drv_wait(&p->wait);
            continue;
        }
        uint32_t tail = (p->head + p->count) % PIPE_SIZE, off = tail % PAGE_SIZE;
        size_t n = MIN(MIN(len - done, room(p)), PAGE_SIZE - off);
        uint8_t **pg = &p->page[tail / PAGE_SIZE];
        if (!*pg && !(*pg = kmalloc(PAGE_SIZE))) { rc = -ENOMEM; break; }
        memcpy(*pg + off, (const uint8_t *)in + done, n);
        p->count += n;
        done += n;
    }
    if (done) drv_wake_all(&p->wait);
    local_irq_restore(fl);
    if (rc == -EPIPE) process_kill(get_current_process()->pid, SIGPIPE);
    return done ? (ssize_t)done : rc;
}

/* One end stops using a ring; the last user frees it. IRQs off. */
static void ring_put(struct pipe *p, bool reader) {
    if (!p) return;
    if (reader) p->readers--; else p->writers--;
    drv_wake_all(&p->wait);
    if (!p->readers && !p->writers) {
        for (int i = 0; i < PIPE_PAGES; i++) kfree(p->page[i]);
        kfree(p);
    }
}

static int pipe_open(void *fs, const char *path, int flags, mode_t mode, vnode_t *v) {
    (void)fs; (void)path; (void)flags; (void)mode; (void)v;
    return -ENOENT;                 /* only pipe() and socketpair() create these */
}

static int pipe_close(vnode_t *v) {
    struct pipe_end *e = v->data;
    uintptr_t fl = local_irq_save();

    ring_put(e->rd, true);
    ring_put(e->wr, false);
    local_irq_restore(fl);
    kfree(e);
    return 0;
}

static int pipe_getattr(vnode_t *v, struct kstat64 *st) {
    st->st_mode = v->type | 0600;
    st->st_nlink = 1;
    st->st_blksize = PAGE_SIZE;
    return 0;
}

static void pipe_set_nonblock(vnode_t *v, bool on) {
    ((struct pipe_end *)v->data)->nonblock = on;
}

static uint32_t pipe_poll(vnode_t *v) {
    struct pipe_end *e = v->data;
    uint32_t ev = 0;

    if (e->rd) {
        if (e->rd->count) ev |= POLLIN;
        if (!e->rd->writers) ev |= POLLHUP | (v->type == S_IFSOCK ? POLLIN : 0);
    }
    if (e->wr) ev |= !e->wr->readers ? POLLERR : room(e->wr) >= PIPE_BUF ? POLLOUT : 0;
    return ev;
}

static vfs_driver_t pipe_driver = {
    .fs_name = "pipefs",
    .magic = 0x50495045,                            /* PIPEFS_MAGIC */
    .open = pipe_open,
    .close = pipe_close,
    .read = pipe_read,
    .write = pipe_write,
    .getattr = pipe_getattr,
    .set_nonblock = pipe_set_nonblock,
    .poll = pipe_poll,
};

static struct pipe *new_ring(void) {
    struct pipe *p = kmalloc(sizeof(*p));
    if (p) memset(p, 0, sizeof(*p));
    return p;
}

/* A descriptor for an end that reads rd and writes wr. On failure the end's references are dropped again. */
static int open_end(struct pipe *rd, struct pipe *wr, uint32_t type, int oflags) {
    struct pipe_end *e = kmalloc(sizeof(*e));
    int fd = -ENOMEM;

    if (rd) rd->readers++;
    if (wr) wr->writers++;
    if (e) {
        *e = (struct pipe_end){ rd, wr, (oflags & O_NONBLOCK) != 0 };
        fd = sys_install_vnode(&pipe_driver, e, type, oflags);
    }
    if (fd < 0) {
        uintptr_t fl = local_irq_save();
        ring_put(rd, true);
        ring_put(wr, false);
        local_irq_restore(fl);
        kfree(e);
    }
    return fd;
}

int sys_pipe(int fds[2], int flags) {
    struct pipe *p = new_ring();
    int nb = flags & O_NONBLOCK;

    if (!p) return -ENOMEM;
    if ((fds[0] = open_end(p, NULL, S_IFIFO, O_RDONLY | nb)) < 0) return fds[0];   /* released p */
    if ((fds[1] = open_end(NULL, p, S_IFIFO, O_WRONLY | nb)) < 0) {
        int rc = fds[1];
        sys_close(fds[0]);
        return rc;
    }
    return 0;
}

int sys_socketpair(int fds[2], int flags) {
    struct pipe *ab = new_ring(), *ba = new_ring();         /* A to B, B to A */
    int oflags = O_RDWR | (flags & O_NONBLOCK);

    if (!ab || !ba) {
        kfree(ab);
        kfree(ba);
        return -ENOMEM;
    }
    if ((fds[0] = open_end(ba, ab, S_IFSOCK, oflags)) < 0) return fds[0];
    if ((fds[1] = open_end(ab, ba, S_IFSOCK, oflags)) < 0) {
        int rc = fds[1];
        sys_close(fds[0]);
        return rc;
    }
    return 0;
}

bool sys_is_socketpair(sys_file_t *sf) {
    vnode_t *v = sf->vfs_file->vnode;
    return v->fs_driver == &pipe_driver && v->type == S_IFSOCK;
}

int sys_socketpair_shutdown(sys_file_t *sf, int how) {
    struct pipe_end *e = sf->vfs_file->vnode->data;
    uintptr_t fl;

    if (how < 0 || how > 2) return -EINVAL;
    fl = local_irq_save();
    if (how != 1) { ring_put(e->rd, true); e->rd = NULL; }     /* SHUT_RD, SHUT_RDWR */
    if (how != 0) { ring_put(e->wr, false); e->wr = NULL; }    /* SHUT_WR, SHUT_RDWR */
    local_irq_restore(fl);
    return 0;
}
