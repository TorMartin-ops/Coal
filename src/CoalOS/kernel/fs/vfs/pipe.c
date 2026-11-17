/**
 * @file pipe.c
 * @brief Anonymous pipes: sys_pipe() creates two descriptors on one ring buffer.
 *
 * Each end is an ordinary vnode of an unmounted "pipefs" driver, so read,
 * write, dup, fork and close work through the usual descriptor layer.
 * Blocking is a yield loop; there is no wait queue.
 */
#include <kernel/fs/vfs/sys_file.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/process/process.h>
#include <kernel/process/scheduler.h>
#include <kernel/process/signal.h>
#include <kernel/lib/string.h>

#define PIPE_SIZE 4096
#ifndef S_IFIFO
#define S_IFIFO 0010000
#endif

struct pipe {
    uint8_t  buf[PIPE_SIZE];
    uint32_t head, count;           /* read position, bytes stored */
    int      readers, writers;      /* open ends */
};

struct pipe_end {
    struct pipe *pipe;
    bool writer, nonblock;
};

static ssize_t pipe_read(vnode_t *v, off_t pos, void *out, size_t len) {
    struct pipe_end *e = v->data;
    struct pipe *p = e->pipe;
    (void)pos;

    while (p->count == 0) {
        if (p->writers == 0) return 0;
        if (e->nonblock) return -EAGAIN;
        yield();
    }
    size_t n = len < p->count ? len : p->count;
    for (size_t i = 0; i < n; i++)
        ((uint8_t *)out)[i] = p->buf[(p->head + i) % PIPE_SIZE];
    p->head = (p->head + n) % PIPE_SIZE;
    p->count -= n;
    return n;
}

static ssize_t pipe_write(vnode_t *v, off_t pos, const void *in, size_t len) {
    struct pipe_end *e = v->data;
    struct pipe *p = e->pipe;
    size_t done = 0;
    (void)pos;

    while (done < len) {
        if (p->readers == 0) {
            process_kill(get_current_process()->pid, SIGPIPE);
            return done ? (ssize_t)done : -EPIPE;
        }
        size_t room = PIPE_SIZE - p->count;
        if (room == 0) {
            if (e->nonblock) return done ? (ssize_t)done : -EAGAIN;
            yield();
            continue;
        }
        size_t n = len - done < room ? len - done : room;
        for (size_t i = 0; i < n; i++)
            p->buf[(p->head + p->count + i) % PIPE_SIZE] = ((const uint8_t *)in)[done + i];
        p->count += n;
        done += n;
    }
    return done;
}

static int pipe_open(void *fs, const char *path, int flags, mode_t mode, vnode_t *v) {
    (void)fs; (void)path; (void)flags; (void)mode; (void)v;
    return -ENOENT;                 /* only sys_pipe() creates pipes */
}

static int pipe_close(vnode_t *v) {
    struct pipe_end *e = v->data;
    struct pipe *p = e->pipe;

    if (e->writer) p->writers--; else p->readers--;
    if (p->readers == 0 && p->writers == 0) kfree(p);
    kfree(e);
    return 0;
}

static int pipe_getattr(vnode_t *v, struct kstat64 *st) {
    (void)v;
    st->st_mode = S_IFIFO | 0600;
    st->st_nlink = 1;
    st->st_blksize = PIPE_SIZE;
    return 0;
}

static vfs_driver_t pipe_driver = {
    .fs_name = "pipefs",
    .open = pipe_open,
    .close = pipe_close,
    .read = pipe_read,
    .write = pipe_write,
    .getattr = pipe_getattr,
};

static file_t *new_end(struct pipe *p, bool writer, bool nonblock) {
    file_t *f = kmalloc(sizeof(*f));
    vnode_t *v = kmalloc(sizeof(*v));
    struct pipe_end *e = kmalloc(sizeof(*e));

    if (!f || !v || !e) { kfree(f); kfree(v); kfree(e); return NULL; }
    *e = (struct pipe_end){ p, writer, nonblock };
    memset(v, 0, sizeof(*v));
    v->data = e;
    v->fs_driver = &pipe_driver;
    v->type = S_IFIFO;
    f->vnode = v;
    f->flags = (writer ? O_WRONLY : O_RDONLY) | (nonblock ? O_NONBLOCK : 0);
    f->offset = 0;
    if (writer) p->writers++; else p->readers++;
    return f;
}

/* Install sf in the lowest free slot of the current process; -EMFILE if full. */
static int install(sys_file_t *sf) {
    pcb_t *p = get_current_process();
    uintptr_t fl = spinlock_acquire_irqsave(&p->fd_table_lock);

    for (int fd = 0; fd < MAX_FD; fd++) {
        if (!p->fd_table[fd]) {
            p->fd_table[fd] = sf;
            spinlock_release_irqrestore(&p->fd_table_lock, fl);
            return fd;
        }
    }
    spinlock_release_irqrestore(&p->fd_table_lock, fl);
    return -EMFILE;
}

int sys_pipe(int fds[2], int flags) {
    struct pipe *p = kmalloc(sizeof(*p));
    if (!p) return -ENOMEM;
    memset(p, 0, sizeof(*p));

    bool nb = (flags & O_NONBLOCK) != 0;
    file_t *ends[2] = { new_end(p, false, nb), new_end(p, true, nb) };
    sys_file_t *sf[2] = { NULL, NULL };
    int rc = -ENOMEM;

    for (int i = 0; i < 2; i++) {
        sf[i] = ends[i] ? kmalloc(sizeof(**sf)) : NULL;
        if (sf[i]) { sf[i]->vfs_file = ends[i]; sf[i]->flags = ends[i]->flags; sf[i]->dups = 0; }
    }
    if (sf[0] && sf[1] && (fds[0] = install(sf[0])) >= 0) {
        if ((fds[1] = install(sf[1])) >= 0) return 0;
        get_current_process()->fd_table[fds[0]] = NULL;
        rc = -EMFILE;
    }
    for (int i = 0; i < 2; i++)
        if (ends[i]) vfs_close(ends[i]);        /* the last close frees the pipe */
    if (!ends[0] && !ends[1]) kfree(p);
    kfree(sf[0]);
    kfree(sf[1]);
    return rc;
}
