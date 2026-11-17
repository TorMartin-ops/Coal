/**
 * @file console_dev.c
 * @brief Console device: stdin/stdout/stderr files backed by the tty.
 */
#include <kernel/drivers/display/console_dev.h>
#include <kernel/drivers/display/tty.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>

static vfs_driver_t console_driver;

static void *console_mount(const char *device) {
    (void)device;
    return &console_driver; /* any non-NULL context */
}

static int console_unmount(void *ctx) {
    (void)ctx;
    return 0;
}

static vnode_t *console_open(void *ctx, const char *path, int flags) {
    (void)ctx; (void)path; (void)flags;
    vnode_t *vn = kmalloc(sizeof(*vn));
    if (vn) {
        vn->data = NULL;
        vn->fs_driver = &console_driver;
    }
    return vn;
}

static int console_read(file_t *file, void *buf, size_t len) {
    if ((file->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    return (int)tty_read(buf, len);
}

static int console_write(file_t *file, const void *buf, size_t len) {
    if ((file->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    return (int)tty_write(buf, len);
}

static int console_close(file_t *file) {
    kfree(file->vnode);
    file->vnode = NULL;
    return 0;
}

static off_t console_lseek(file_t *file, off_t offset, int whence) {
    (void)file; (void)offset; (void)whence;
    return -ESPIPE;
}

static vfs_driver_t console_driver = {
    .fs_name = "console",
    .mount   = console_mount,
    .unmount = console_unmount,
    .open    = console_open,
    .read    = console_read,
    .write   = console_write,
    .close   = console_close,
    .lseek   = console_lseek,
};

file_t *create_console_file(int flags) {
    vnode_t *vn = console_open(NULL, NULL, flags);
    if (!vn) return NULL;

    file_t *f = kmalloc(sizeof(*f));
    if (!f) {
        kfree(vn);
        return NULL;
    }
    memset(f, 0, sizeof(*f));
    f->vnode = vn;
    f->flags = (uint32_t)flags;
    spinlock_init(&f->lock);
    return f;
}

void console_dev_init(void) {
    tty_init();
    vfs_register_driver(&console_driver);
}
