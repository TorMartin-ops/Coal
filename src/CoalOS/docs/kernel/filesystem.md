# File System

Coal OS implements a layered file system architecture with VFS abstraction, FAT support, and advanced caching mechanisms.

## Architecture Overview

```
┌─────────────────────────────────────────────────────┐
│              User Applications                      │
├─────────────────────────────────────────────────────┤
│          System Call Interface                      │
│  (open, read, write, close, mkdir, etc.)          │
├─────────────────────────────────────────────────────┤
│         Virtual File System (VFS)                   │
│  ┌─────────────┐  ┌─────────────┐  ┌────────────┐ │
│  │   Inode     │  │    File     │  │   Mount    │ │
│  │   Cache     │  │ Descriptors │  │   Table    │ │
│  └─────────────┘  └─────────────┘  └────────────┘ │
├─────────────────────────────────────────────────────┤
│              Page Cache Layer                       │
│         (4KB page-level caching)                   │
├─────────────────────────────────────────────────────┤
│           File System Drivers                       │
│  ┌─────────────┐  ┌─────────────┐  ┌────────────┐ │
│  │    FAT16    │  │    FAT32    │  │   Future   │ │
│  │   Driver    │  │   Driver    │  │     FS     │ │
│  └─────────────┘  └─────────────┘  └────────────┘ │
├─────────────────────────────────────────────────────┤
│            Buffer Cache Layer                       │
│         (512B block-level caching)                 │
├─────────────────────────────────────────────────────┤
│            Block Device Layer                       │
│               (ATA/IDE Driver)                      │
└─────────────────────────────────────────────────────┘
```

## Virtual File System (VFS)

The VFS provides a uniform interface to different file systems:

### VFS Operations
```c
typedef struct vfs_driver {
    const char *fs_name;
    
    // Mount operations
    void *(*mount)(const char *device);
    int (*unmount)(void *fs_context);
    
    // File operations
    vnode_t *(*open)(void *fs_context, const char *path, int flags);
    int (*read)(file_t *file, void *buf, size_t len);
    int (*write)(file_t *file, const void *buf, size_t len);
    int (*close)(file_t *file);
    off_t (*lseek)(file_t *file, off_t offset, int whence);
    
    // Directory operations
    int (*mkdir)(void *fs_context, const char *path, mode_t mode);
    int (*rmdir)(void *fs_context, const char *path);
    int (*readdir)(file_t *dir, struct dirent *entry, size_t index);
    
    // Metadata operations
    int (*stat)(void *fs_context, const char *path, struct stat *st);
    int (*unlink)(void *fs_context, const char *path);
    
    // Page cache support
    ssize_t (*read_inode)(void *fs_context, uint32_t inode, 
                          uint64_t offset, void *buffer, size_t size);
    ssize_t (*write_inode)(void *fs_context, uint32_t inode,
                           uint64_t offset, const void *buffer, size_t size);
} vfs_driver_t;
```

### File Descriptors
```c
typedef struct file {
    vnode_t    *vnode;      // Underlying vnode
    uint32_t    flags;      // Open flags (O_RDONLY, etc.)
    off_t       offset;     // Current position
    spinlock_t  lock;       // Per-file lock
    uint32_t    ref_count;  // Reference count
} file_t;
```

### Mount System
```c
typedef struct mount {
    const char *mount_point;    // Where it's mounted
    const char *device;         // Device name
    const char *fs_type;        // File system type
    void *fs_context;           // FS-specific data
    uint32_t device_id;         // Unique device ID
    struct mount *next;         // Mount list
} mount_t;
```

## FAT File System Driver

Coal OS includes a comprehensive FAT16/32 implementation:

### Features
- Long filename support (VFAT)
- Directory operations
- File creation/deletion
- Efficient cluster allocation
- FAT caching

### FAT Structure
```c
typedef struct fat_fs {
    // Boot sector info
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint32_t sectors_per_fat;
    
    // Calculated values
    uint32_t first_data_sector;
    uint32_t total_clusters;
    uint32_t fat_start_sector;
    uint32_t root_dir_sector;
    
    // Runtime data
    uint32_t *fat_cache;        // Cached FAT
    disk_t   *disk;             // Underlying device
    spinlock_t lock;            // FS lock
} fat_fs_t;
```

### Directory Entry
```c
typedef struct {
    char     name[11];          // 8.3 format
    uint8_t  attributes;        // File attributes
    uint8_t  reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_high;      // FAT32 only
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t cluster_low;
    uint32_t file_size;
} __attribute__((packed)) fat_dir_entry_t;
```

## Page Cache

High-performance page-level caching system:

### Features
- 4KB page granularity
- LRU replacement policy
- Write-back caching
- Read-ahead support
- Per-file synchronization

### Page Cache Entry
```c
struct page_cache_entry {
    // Identification
    uint32_t device_id;
    uint32_t inode_number;
    uint32_t page_index;
    
    // Page data
    void *data;             // 4KB buffer
    uint32_t flags;         // VALID, DIRTY, LOCKED
    
    // Reference counting
    uint32_t ref_count;
    uint32_t map_count;
    
    // LRU management
    struct page_cache_entry *lru_next;
    struct page_cache_entry *lru_prev;
    
    // Hash chain
    struct page_cache_entry *hash_next;
};
```

### Cache Operations
```c
// Read through cache
ssize_t page_cache_read(uint32_t dev, uint32_t inode,
                        uint64_t offset, void *buf, size_t size);

// Write through cache
ssize_t page_cache_write(uint32_t dev, uint32_t inode,
                         uint64_t offset, const void *buf, size_t size);

// Sync dirty pages
int page_cache_sync_file(uint32_t dev, uint32_t inode);
```

## Buffer Cache

Block-level caching for disk I/O:

### Features
- 512-byte block size (sector)
- Hash table + LRU
- Write coalescing
- Delayed writes
- Device abstraction

### Buffer Structure
```c
typedef struct buffer {
    disk_t   *disk;         // Device
    uint32_t  block_number; // Block number
    void     *data;         // Block data
    uint32_t  flags;        // VALID, DIRTY
    uint32_t  ref_count;    // References
    
    struct buffer *hash_next;
    struct buffer *lru_next;
    struct buffer *lru_prev;
} buffer_t;
```

## Path Resolution

Efficient path lookup with caching:

```c
vnode_t* path_lookup(const char *path) {
    // Start from root or cwd
    vnode_t *current = (*path == '/') ? root_vnode : cwd_vnode;
    
    char component[256];
    while (get_next_component(&path, component)) {
        // Check permissions
        if (!can_search(current)) {
            return ERR_PTR(-EACCES);
        }
        
        // Lookup in directory
        current = lookup_child(current, component);
        if (!current) {
            return ERR_PTR(-ENOENT);
        }
        
        // Follow symlinks if needed
        if (is_symlink(current)) {
            current = follow_symlink(current);
        }
    }
    
    return current;
}
```

## File Operations

### Open
```c
file_t* vfs_open(const char *path, int flags) {
    // Resolve path
    vnode_t *vnode = path_lookup(path);
    if (IS_ERR(vnode)) {
        return NULL;
    }
    
    // Check permissions
    if (!check_permissions(vnode, flags)) {
        return NULL;
    }
    
    // Allocate file descriptor
    file_t *file = allocate_file();
    file->vnode = vnode;
    file->flags = flags;
    file->offset = 0;
    
    // Call FS-specific open
    if (vnode->ops->open) {
        vnode->ops->open(vnode, flags);
    }
    
    return file;
}
```

### Read/Write
```c
ssize_t vfs_read(file_t *file, void *buf, size_t count) {
    // Check if file is readable
    if (!(file->flags & O_RDONLY)) {
        return -EBADF;
    }
    
    // Use page cache for regular files
    if (S_ISREG(file->vnode->mode)) {
        return page_cache_read(file->vnode->device,
                              file->vnode->inode,
                              file->offset, buf, count);
    }
    
    // Direct read for devices
    return file->vnode->ops->read(file, buf, count);
}
```

## Directory Operations

### Directory Reading
```c
struct dirent {
    uint32_t d_ino;         // Inode number
    uint16_t d_reclen;      // Record length
    uint8_t  d_type;        // File type
    char     d_name[256];   // Filename
};

int readdir(file_t *dir, struct dirent *entry) {
    if (!S_ISDIR(dir->vnode->mode)) {
        return -ENOTDIR;
    }
    
    return dir->vnode->ops->readdir(dir, entry, dir->offset++);
}
```

### Creating Directories
```c
int vfs_mkdir(const char *path, mode_t mode) {
    // Parse parent path
    char parent_path[PATH_MAX];
    char name[NAME_MAX];
    split_path(path, parent_path, name);
    
    // Lookup parent
    vnode_t *parent = path_lookup(parent_path);
    if (IS_ERR(parent)) {
        return PTR_ERR(parent);
    }
    
    // Check if already exists
    if (lookup_child(parent, name)) {
        return -EEXIST;
    }
    
    // Call FS-specific mkdir
    return parent->ops->mkdir(parent, name, mode);
}
```

## Performance Features

### Read-ahead
```c
void trigger_readahead(file_t *file, size_t read_size) {
    // Calculate pages to prefetch
    uint32_t current_page = file->offset / PAGE_SIZE;
    uint32_t pages_to_read = min(read_size / PAGE_SIZE + 2, 8);
    
    // Asynchronously prefetch
    for (int i = 1; i <= pages_to_read; i++) {
        page_cache_prefetch(file->vnode->device,
                           file->vnode->inode,
                           current_page + i, 1);
    }
}
```

### Write Clustering
- Delayed writes for efficiency
- Coalesce adjacent writes
- Periodic sync daemon

## Security

### Permissions
```c
#define S_IRUSR 0400    // Owner read
#define S_IWUSR 0200    // Owner write
#define S_IXUSR 0100    // Owner execute
#define S_IRGRP 0040    // Group read
#define S_IWGRP 0020    // Group write
#define S_IXGRP 0010    // Group execute
#define S_IROTH 0004    // Other read
#define S_IWOTH 0002    // Other write
#define S_IXOTH 0001    // Other execute

bool check_permissions(vnode_t *vnode, int access) {
    mode_t mode = vnode->mode;
    uid_t uid = current_uid();
    
    // Owner permissions
    if (vnode->uid == uid) {
        return (mode & (access << 6)) != 0;
    }
    
    // Group permissions
    if (vnode->gid == current_gid()) {
        return (mode & (access << 3)) != 0;
    }
    
    // Other permissions
    return (mode & access) != 0;
}
```

## Future Enhancements

1. **Journaling**: Add journaling support
2. **Extended Attributes**: xattr support
3. **Quota Management**: User/group quotas
4. **Encryption**: Transparent encryption
5. **Network Filesystems**: NFS/SMB support