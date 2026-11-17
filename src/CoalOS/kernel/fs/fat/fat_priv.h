/**
 * @file fat_priv.h
 * @brief Internal structures and helpers shared by the FAT driver files.
 *
 * Locking: one sleeping mutex per mounted volume, taken around every driver
 * entry point. Waiters sleep, so a holder blocked in disk I/O can be preempted.
 */
#ifndef FAT_PRIV_H
#define FAT_PRIV_H

#include <kernel/core/types.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/drivers/storage/buffer_cache.h>
#include <kernel/sync/mutex.h>

#define FAT_ATTR_RO     0x01
#define FAT_ATTR_VOLID  0x08
#define FAT_ATTR_DIR    0x10
#define FAT_ATTR_ARCH   0x20
#define FAT_ATTR_LFN    0x0F

#define FAT_ENT_SIZE    32u
#define FAT_NODE_ROOT_LBA 0u    /* ent_lba of the root pseudo entry */

/** 8.3 directory entry. */
typedef struct __attribute__((packed)) {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  nt;                /* bit 3: lower-case base, bit 4: lower-case extension */
    uint8_t  ctime_tenth;
    uint16_t ctime, cdate, adate;
    uint16_t cluster_hi;
    uint16_t mtime, mdate;
    uint16_t cluster_lo;
    uint32_t size;
} fat_dirent_t;

struct fat_node;

typedef struct fat_fs {
    const char *dev;            /* buffer cache device name */
    uint32_t bps;               /* bytes per sector (== device sector size) */
    uint32_t spc;               /* sectors per cluster */
    uint32_t cluster_bytes;
    uint8_t  type;              /* 16 or 32 */
    uint8_t  nfats;
    uint32_t fat_lba, fat_sectors;
    uint32_t root_lba, root_sectors;    /* fixed root directory (FAT16) */
    uint32_t root_cluster;              /* FAT32 */
    uint32_t data_lba;
    uint32_t nclusters;         /* data clusters are numbered 2 .. nclusters+1 */
    uint32_t next_free;         /* allocation hint */
    uint8_t *fat;               /* in-memory copy of FAT 1 */
    uint32_t dirty_lo, dirty_hi;/* dirty FAT sector range, lo > hi when clean */
    struct fat_node *nodes;     /* open files and directories */
    mutex_t lock;
    bool unsynced;              /* changes sit in the buffer cache, not yet on the disk */
} fat_fs_t;

/** In-memory state of an open file or directory, shared by all openers. */
typedef struct fat_node {
    struct fat_node *next;
    fat_fs_t *fs;
    uint32_t ref;
    uint32_t first;             /* first cluster; 0 = empty file or FAT16 root */
    uint32_t size;
    uint32_t ent_lba;           /* location of the 8.3 entry, 0 for the root */
    uint32_t ent_off;
    uint32_t mtime, atime, ctime;
    uint32_t hint_idx, hint_cl; /* cluster index cache for sequential access */
    uint8_t  attr;
    bool     dirty;
} fat_node_t;

/** A directory entry with its (possibly long) name resolved. */
typedef struct {
    char     name[VFS_NAME_MAX + 1];
    uint8_t  sfn[11];
    uint8_t  attr;
    uint32_t cluster, size;
    uint32_t mtime, atime, ctime;
    uint32_t idx;               /* slot index of the 8.3 entry within its directory */
    uint32_t first_idx;         /* slot index of its first long-name slot */
    uint32_t lba, off;          /* where the 8.3 entry lives */
} fat_ent_t;

/** Sequential access to the slots of one directory. */
typedef struct {
    fat_fs_t *fs;
    uint32_t first;             /* first cluster, 0 = fixed FAT16 root */
    uint32_t ci, cc;            /* cached cluster index and number */
    buffer_t *buf;
    bool grow;                  /* extend the chain when running off the end */
} fat_dir_t;

static inline void fat_lock(fat_fs_t *fs) { mutex_lock(&fs->lock); }
static inline void fat_unlock(fat_fs_t *fs) { mutex_unlock(&fs->lock); }
static inline uint32_t fat_root(const fat_fs_t *fs) { return fs->type == 32 ? fs->root_cluster : 0; }

/* fat.c: volume, FAT table, cluster allocation */
void    *fat_mount(const char *dev);
int      fat_unmount(void *ctx);
int      fat_sync(void *ctx);
buffer_t *fat_buf(fat_fs_t *fs, uint32_t lba);
uint32_t fat_cluster_lba(const fat_fs_t *fs, uint32_t cluster);
uint32_t fat_next(const fat_fs_t *fs, uint32_t cluster);
uint32_t fat_nth(const fat_fs_t *fs, uint32_t first, uint32_t n);
int      fat_alloc(fat_fs_t *fs, uint32_t prev);
void     fat_set_eoc(fat_fs_t *fs, uint32_t cluster);
void     fat_free_chain(fat_fs_t *fs, uint32_t first);
int      fat_commit(fat_fs_t *fs);
int      fat_flush(fat_fs_t *fs);
void     fat_unix_to_dos(uint32_t t, uint16_t *date, uint16_t *time);
uint32_t fat_dos_to_unix(uint16_t date, uint16_t time);

/* fat_dir.c: directory slots, names, long file names */
void fat_dir_open(fat_dir_t *d, fat_fs_t *fs, uint32_t first);
void fat_dir_close(fat_dir_t *d);
int  fat_dir_next(fat_dir_t *d, uint32_t *pos, fat_ent_t *out);
int  fat_dir_find(fat_fs_t *fs, uint32_t dir, const char *name, fat_ent_t *out);
int  fat_dir_add(fat_fs_t *fs, uint32_t dir, const char *name, uint8_t attr,
                 uint32_t cluster, uint32_t size, uint32_t mtime, fat_ent_t *out);
int  fat_dir_remove(fat_fs_t *fs, uint32_t dir, const fat_ent_t *e);
int  fat_dir_is_empty(fat_fs_t *fs, uint32_t dir);
int  fat_ent_update(fat_fs_t *fs, uint32_t lba, uint32_t off, uint32_t cluster,
                    uint32_t size, uint32_t mtime);

#endif /* FAT_PRIV_H */
