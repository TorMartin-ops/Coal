/**
 * @file procfs.c
 * @brief Read-only /proc. Each open renders a small text snapshot.
 */
#include <kernel/fs/procfs.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/drivers/timer/pit.h>
#include <kernel/memory/buddy.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/process/process.h>
#include <kernel/lib/string.h>
#include <libc/stdarg.h>

#define MAX_PIDS 64
#define SNAPSHOT_MAX 640


enum kind { K_ROOT, K_PIDDIR, K_MEMINFO, K_UPTIME, K_VERSION, K_MOUNTS, K_STATUS, K_CMDLINE };

struct node {
    enum kind kind;
    uint32_t  pid;
    char     *text;                 /* rendered file contents */
    size_t    len;
};

static int pf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int pf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* ---- Process accessors (weak fallbacks: only the calling process) ---------- */

__attribute__((weak)) int proc_info_get(uint32_t pid, proc_info_t *out) {
    pcb_t *p = get_current_process();
    if (!p || p->pid != pid) return -ESRCH;
    memset(out, 0, sizeof(*out));
    out->pid = p->pid;
    out->ppid = p->ppid;
    out->state = 'R';
    strcpy(out->name, "?");
    return 0;
}

__attribute__((weak)) int proc_info_list(uint32_t *pids, int max) {
    pcb_t *p = get_current_process();
    if (!p || max < 1) return 0;
    pids[0] = p->pid;
    return 1;
}

/* ---- Rendering ------------------------------------------------------------ */

static size_t render(const struct node *n, char *out) {
    proc_info_t pi;
    uint32_t ms = (uint32_t)timer_uptime_ms();
    switch (n->kind) {
    case K_MEMINFO: {
        uint32_t lt = (uint32_t)buddy_total_pages(ZONE_NORMAL), lf = (uint32_t)buddy_free_pages_count(ZONE_NORMAL);
        uint32_t ht = (uint32_t)buddy_total_pages(ZONE_HIGH), hf = (uint32_t)buddy_free_pages_count(ZONE_HIGH);
        const uint32_t kb = PAGE_SIZE / 1024;
        return (size_t)pf(out, SNAPSHOT_MAX,
                          "MemTotal:    %8u kB\nMemFree:     %8u kB\nMemAvailable:%8u kB\n"
                          "HighTotal:   %8u kB\nHighFree:    %8u kB\n",
                          (lt + ht) * kb, (lf + hf) * kb, (lf + hf) * kb, ht * kb, hf * kb);
    }
    case K_UPTIME:
        return (size_t)pf(out, SNAPSHOT_MAX, "%u.%u%u %u.00\n", ms / 1000, ms / 100 % 10, ms / 10 % 10, ms / 1000);
    case K_VERSION:
        return (size_t)pf(out, SNAPSHOT_MAX, "Coal OS version %s (i386)\n", COAL_VERSION);
    case K_MOUNTS: {                                /* Linux format: device mountpoint type options dump pass */
        const char *path, *dev, *fs;
        size_t len = 0;
        for (unsigned cur = 0; len < SNAPSHOT_MAX && vfs_mount_next(&cur, &path, &dev, &fs);) {
            const char *type = !strcmp(fs, "fat") ? "vfat" : !strcmp(fs, "procfs") ? "proc" : fs;
            char devname[16];
            pf(devname, sizeof(devname), dev[0] ? "/dev/%s" : "%s", dev[0] ? dev : type);
            len += (size_t)pf(out + len, SNAPSHOT_MAX - len, "%s %s %s rw 0 0\n", devname, path, type);
        }
        return len < SNAPSHOT_MAX ? len : SNAPSHOT_MAX - 1;
    }
    case K_STATUS:
        if (proc_info_get(n->pid, &pi) < 0) return 0;
        return (size_t)pf(out, SNAPSHOT_MAX, "Name:\t%s\nState:\t%c\nPid:\t%u\nPPid:\t%u\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\n",
                          pi.name, pi.state, pi.pid, pi.ppid);
    case K_CMDLINE:
        if (proc_info_get(n->pid, &pi) < 0) return 0;
        memcpy(out, pi.cmdline, pi.cmdline_len);
        return pi.cmdline_len;
    default:
        return 0;
    }
}

/* ---- Driver --------------------------------------------------------------- */

static void *procfs_mount(const char *device) { return (void *)procfs_mount; }
static int procfs_unmount(void *fs) { return 0; }

static bool parse_pid(const char *s, size_t len, uint32_t *pid) {
    uint32_t v = 0;
    if (len == 0 || len > 9) return false;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (uint32_t)(s[i] - '0');
    }
    *pid = v;
    return true;
}

static int procfs_open(void *fs, const char *path, int flags, mode_t mode, vnode_t *v) {
    const char *c[2] = { NULL, NULL };
    size_t cl[2] = { 0, 0 };
    int nc = 0;
    for (const char *p = path; *p;) {
        while (*p == '/') p++;
        if (!*p) break;
        if (nc == 2) return -ENOENT;
        c[nc] = p;
        while (*p && *p != '/') p++;
        cl[nc] = (size_t)(p - c[nc]);
        nc++;
    }
    enum kind kind = K_ROOT;
    uint32_t pid = 0;
    proc_info_t pi;

    if (nc > 0) {
        if (nc == 1 && cl[0] == 7 && !strncmp(c[0], "meminfo", 7)) kind = K_MEMINFO;
        else if (nc == 1 && cl[0] == 6 && !strncmp(c[0], "uptime", 6)) kind = K_UPTIME;
        else if (nc == 1 && cl[0] == 7 && !strncmp(c[0], "version", 7)) kind = K_VERSION;
        else if (nc == 1 && cl[0] == 6 && !strncmp(c[0], "mounts", 6)) kind = K_MOUNTS;
        else {
            if (cl[0] == 4 && !strncmp(c[0], "self", 4)) {
                pcb_t *cur = get_current_process();
                if (!cur) return -ENOENT;
                pid = cur->pid;
            } else if (!parse_pid(c[0], cl[0], &pid)) {
                return -ENOENT;
            }
            if (proc_info_get(pid, &pi) < 0) return -ENOENT;
            kind = K_PIDDIR;
            if (nc == 2) {
                if (cl[1] == 6 && !strncmp(c[1], "status", 6)) kind = K_STATUS;
                else if (cl[1] == 7 && !strncmp(c[1], "cmdline", 7)) kind = K_CMDLINE;
                else return -ENOENT;
            }
        }
    }
    if (flags & O_CREAT) return (flags & O_EXCL) ? -EEXIST : -EACCES;
    if ((flags & O_ACCMODE) != O_RDONLY) return -EACCES;

    struct node *n = kmalloc(sizeof(*n));
    if (!n) return -ENOMEM;
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    n->pid = pid;
    if (kind >= K_MEMINFO) {
        n->text = kmalloc(SNAPSHOT_MAX);
        if (!n->text) { kfree(n); return -ENOMEM; }
        n->len = render(n, n->text);
    }
    v->data = n;
    v->type = kind >= K_MEMINFO ? S_IFREG : S_IFDIR;
    return 0;
}

static int procfs_close(vnode_t *v) {
    struct node *n = v->data;
    kfree(n->text);
    kfree(n);
    return 0;
}

static ssize_t procfs_read(vnode_t *v, off_t pos, void *buf, size_t len) {
    struct node *n = v->data;
    if ((size_t)pos >= n->len) return 0;
    if (len > n->len - (size_t)pos) len = n->len - (size_t)pos;
    memcpy(buf, n->text + pos, len);
    return (ssize_t)len;
}

static int procfs_readdir(vnode_t *v, off_t *pos, struct vfs_dirent *out) {
    static const char *const root_names[] = { "meminfo", "uptime", "version", "mounts", "self" };
    static const char *const pid_names[] = { "status", "cmdline" };
    struct node *n = v->data;
    uint32_t i = (uint32_t)*pos;
    out->type = DT_DIR;
    out->ino = 1;

    if (i < 2) {
        strcpy(out->name, i == 0 ? "." : "..");
    } else if (n->kind == K_PIDDIR) {
        if (i - 2 >= 2) return 0;
        strcpy(out->name, pid_names[i - 2]);
        out->type = DT_REG;
        out->ino = 100 + i;
    } else if (i - 2 < 5) {
        strcpy(out->name, root_names[i - 2]);
        out->type = i - 2 == 4 ? DT_DIR : DT_REG;
        out->ino = 10 + i;
    } else {
        uint32_t pids[MAX_PIDS];
        uint32_t k = i - 7;
        int cnt = proc_info_list(pids, MAX_PIDS);
        if ((int)k >= cnt) return 0;
        pf(out->name, sizeof(out->name), "%u", pids[k]);
        out->ino = 1000 + pids[k];
    }
    (*pos)++;
    return 1;
}

static int procfs_getattr(vnode_t *v, struct kstat64 *st) {
    struct node *n = v->data;
    bool dir = n->kind < K_MEMINFO;
    st->st_mode = dir ? S_IFDIR | 0555 : S_IFREG | 0444;
    st->st_nlink = dir ? 2 : 1;
    st->st_size = (int64_t)n->len;
    st->st_blksize = 1024;
    st->st_ino = n->kind == K_ROOT ? 1 : 10 + n->kind + ((uint64_t)n->pid << 8);
    st->st_atime = st->st_mtime = st->st_ctime = vfs_time();
    return 0;
}

static vfs_driver_t procfs_driver = {
    .fs_name = "procfs",
    .magic = 0x9fa0,                                /* PROC_SUPER_MAGIC */
    .mount = procfs_mount,
    .unmount = procfs_unmount,
    .open = procfs_open,
    .close = procfs_close,
    .read = procfs_read,
    .readdir = procfs_readdir,
    .getattr = procfs_getattr,
};

int procfs_init(void) { return vfs_register_driver(&procfs_driver); }
