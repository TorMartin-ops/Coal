/**
 * @file devfs.h
 * @brief Tiny in-memory device file system, mounted at /dev.
 */
#ifndef DEVFS_H
#define DEVFS_H

#include <kernel/core/types.h>

/** Operations of a character device; any callback may be NULL. */
typedef struct devfs_char_ops {
    ssize_t (*read)(void *priv, void *buf, size_t len);
    ssize_t (*write)(void *priv, const void *buf, size_t len);
    /** `arg` is the raw user-space argument of ioctl(2). */
    int     (*ioctl)(void *priv, uint32_t cmd, void *arg);
    void    *priv;
    uint32_t rdev;                      /* (major << 8) | minor, shown by stat */
    uint32_t (*poll)(void *priv);       /* POLLIN/POLLOUT bits; NULL = always ready */
} devfs_char_ops_t;

/**
 * Add /dev/<name>. `ops` must stay valid for the lifetime of the kernel.
 * Returns 0, -EEXIST for a duplicate name, -ENOSPC when the table is full.
 */
int devfs_register_char(const char *name, const devfs_char_ops_t *ops);

/** Register the driver and the built-in nodes (console, tty, null, zero). */
int devfs_init(void);

#endif
