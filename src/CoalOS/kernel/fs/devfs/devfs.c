/**
 * @file devfs.c
 * @brief Device file system: a flat directory of character devices.
 */
#include <kernel/fs/devfs.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/memory/uaccess.h>
#include <kernel/lib/string.h>

#define MAX_DEVICES 16
#define NAME_LEN    16
#define CONSOLE_LINE_MAX 1024

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGWINSZ  0x5413

static struct devfs_node {
    char name[NAME_LEN];
    const devfs_char_ops_t *ops;
} nodes[MAX_DEVICES];
static int node_count;

int devfs_register_char(const char *name, const devfs_char_ops_t *ops) {
    if (!name || !*name || strlen(name) >= NAME_LEN || strchr(name, '/') || !ops) return -EINVAL;
    for (int i = 0; i < node_count; i++)
        if (strcmp(nodes[i].name, name) == 0) return -EEXIST;
    if (node_count == MAX_DEVICES) return -ENOSPC;
    strcpy(nodes[node_count].name, name);
    nodes[node_count].ops = ops;
    node_count++;
    return 0;
}

/* ---- Driver --------------------------------------------------------------- */

static void *devfs_mount(const char *device) { return nodes; }
static int devfs_unmount(void *fs) { return 0; }

static int devfs_open(void *fs, const char *path, int flags, mode_t mode, vnode_t *v) {
    while (*path == '/') path++;
    if (!*path) {
        if ((flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
        v->data = NULL;
        v->type = S_IFDIR;
        return 0;
    }
    for (int i = 0; i < node_count; i++) {
        if (strcmp(nodes[i].name, path) != 0) continue;
        if ((flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
        v->data = &nodes[i];
        v->type = S_IFCHR;
        return 0;
    }
    return (flags & O_CREAT) ? -EACCES : -ENOENT;
}

static int devfs_close(vnode_t *v) { return 0; }

static const devfs_char_ops_t *ops_of(vnode_t *v) {
    return v->data ? ((struct devfs_node *)v->data)->ops : NULL;
}

static ssize_t devfs_read(vnode_t *v, off_t pos, void *buf, size_t len) {
    const devfs_char_ops_t *o = ops_of(v);
    return o && o->read ? o->read(o->priv, buf, len) : -EINVAL;
}

static ssize_t devfs_write(vnode_t *v, off_t pos, const void *buf, size_t len) {
    const devfs_char_ops_t *o = ops_of(v);
    return o && o->write ? o->write(o->priv, buf, len) : -EINVAL;
}

static int devfs_ioctl(vnode_t *v, uint32_t cmd, void *arg) {
    const devfs_char_ops_t *o = ops_of(v);
    return o && o->ioctl ? o->ioctl(o->priv, cmd, arg) : -ENOTTY;
}

static int devfs_readdir(vnode_t *dir, off_t *pos, struct vfs_dirent *out) {
    if (*pos >= 2 + node_count) return 0;
    if (*pos < 2) {
        strcpy(out->name, *pos == 0 ? "." : "..");
        out->type = DT_DIR;
        out->ino = 1;
    } else {
        strcpy(out->name, nodes[*pos - 2].name);
        out->type = DT_CHR;
        out->ino = (uint32_t)*pos;
    }
    (*pos)++;
    return 1;
}

static int devfs_getattr(vnode_t *v, struct kstat64 *st) {
    const devfs_char_ops_t *o = ops_of(v);
    st->st_mode = o ? S_IFCHR | 0666 : S_IFDIR | 0755;
    st->st_nlink = o ? 1 : 2;
    st->st_rdev = o ? o->rdev : 0;
    st->st_ino = o ? (uint64_t)((struct devfs_node *)v->data - nodes) + 2 : 1;
    st->st_blksize = 1024;
    st->st_atime = st->st_mtime = st->st_ctime = vfs_time();
    return 0;
}

static vfs_driver_t devfs_driver = {
    .fs_name = "devfs",
    .mount = devfs_mount,
    .unmount = devfs_unmount,
    .open = devfs_open,
    .close = devfs_close,
    .read = devfs_read,
    .write = devfs_write,
    .ioctl = devfs_ioctl,
    .readdir = devfs_readdir,
    .getattr = devfs_getattr,
};

/* ---- Built-in devices ----------------------------------------------------- */

/* Line-oriented keyboard input; like a tty in canonical mode, the newline is part of the data. */
static ssize_t console_read(void *priv, void *buf, size_t len) {
    if (len == 0) return 0;
    char *p = buf;
    ssize_t n = terminal_read_line_blocking(p, len > CONSOLE_LINE_MAX ? CONSOLE_LINE_MAX : len);
    if (n < 0) return n;
    if ((size_t)n < len) p[n++] = '\n';
    return n;
}

static ssize_t console_write(void *priv, const void *buf, size_t len) {
    terminal_write_len(buf, len);
    return (ssize_t)len;
}

/* Enough tty ioctls for isatty() and window size queries; settings are accepted and ignored. */
static int console_ioctl(void *priv, uint32_t cmd, void *arg) {
    static const struct { uint32_t iflag, oflag, cflag, lflag; uint8_t line, cc[19]; } termios =
        { 0x500, 0x5, 0xBF, 0x8A3B, 0, { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22 } };
    static const uint16_t winsize[4] = { 25, 80, 0, 0 };
    switch (cmd) {
    case TCGETS:     return copy_to_user(arg, &termios, sizeof(termios)) ? -EFAULT : 0;
    case TIOCGWINSZ: return copy_to_user(arg, winsize, sizeof(winsize)) ? -EFAULT : 0;
    case TCSETS: case TCSETSW: case TCSETSF: return 0;
    default:         return -ENOTTY;
    }
}

static ssize_t null_read(void *priv, void *buf, size_t len) { return 0; }
static ssize_t sink_write(void *priv, const void *buf, size_t len) { return (ssize_t)len; }
static ssize_t zero_read(void *priv, void *buf, size_t len) { memset(buf, 0, len); return (ssize_t)len; }

static const devfs_char_ops_t console_ops = { console_read, console_write, console_ioctl, NULL, (5 << 8) | 1 };
static const devfs_char_ops_t null_ops    = { null_read, sink_write, NULL, NULL, (1 << 8) | 3 };
static const devfs_char_ops_t zero_ops    = { zero_read, sink_write, NULL, NULL, (1 << 8) | 5 };

int devfs_init(void) {
    int rc = vfs_register_driver(&devfs_driver);
    if (rc == 0) rc = devfs_register_char("console", &console_ops);
    if (rc == 0) rc = devfs_register_char("tty", &console_ops);
    if (rc == 0) rc = devfs_register_char("null", &null_ops);
    if (rc == 0) rc = devfs_register_char("zero", &zero_ops);
    return rc;
}
