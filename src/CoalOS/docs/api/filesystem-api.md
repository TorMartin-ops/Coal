# File System API

This document describes the file system APIs in Coal OS, including VFS operations, file I/O, and directory management.

## Overview

Coal OS implements a Virtual File System (VFS) layer that provides:
- **Unified Interface**: Common API for different file systems
- **Mount Points**: Multiple file systems in single namespace
- **Page Cache**: Efficient caching of file data
- **Buffer Cache**: Block-level caching

## File Operations

### Opening and Closing Files

```c
/**
 * @brief Open a file
 * @param path File path
 * @param flags Open flags (O_RDONLY, O_WRONLY, etc.)
 * @param mode Creation mode (if O_CREAT)
 * @return File descriptor or negative error code
 */
int vfs_open(const char *path, int flags, mode_t mode);

/**
 * @brief Close a file
 * @param fd File descriptor
 * @return 0 on success, negative error code on failure
 */
int vfs_close(int fd);

/**
 * @brief Duplicate file descriptor
 * @param oldfd Original file descriptor
 * @return New file descriptor or negative error
 */
int vfs_dup(int oldfd);

/**
 * @brief Duplicate file descriptor to specific number
 * @param oldfd Original file descriptor
 * @param newfd Desired file descriptor
 * @return New file descriptor or negative error
 */
int vfs_dup2(int oldfd, int newfd);
```

### Open Flags

```c
#define O_RDONLY    0x0000  // Open for reading only
#define O_WRONLY    0x0001  // Open for writing only
#define O_RDWR      0x0002  // Open for reading and writing
#define O_CREAT     0x0040  // Create file if it doesn't exist
#define O_EXCL      0x0080  // Fail if file exists (with O_CREAT)
#define O_NOCTTY    0x0100  // Don't make terminal controlling tty
#define O_TRUNC     0x0200  // Truncate file to zero length
#define O_APPEND    0x0400  // Append mode
#define O_NONBLOCK  0x0800  // Non-blocking I/O
#define O_DIRECTORY 0x10000 // Must be a directory
#define O_CLOEXEC   0x80000 // Close on exec
```

### Reading and Writing

```c
/**
 * @brief Read from file
 * @param fd File descriptor
 * @param buf Buffer to read into
 * @param count Number of bytes to read
 * @return Bytes read or negative error code
 */
ssize_t vfs_read(int fd, void *buf, size_t count);

/**
 * @brief Write to file
 * @param fd File descriptor
 * @param buf Buffer to write from
 * @param count Number of bytes to write
 * @return Bytes written or negative error code
 */
ssize_t vfs_write(int fd, const void *buf, size_t count);

/**
 * @brief Read from file at offset
 * @param fd File descriptor
 * @param buf Buffer to read into
 * @param count Number of bytes to read
 * @param offset File offset
 * @return Bytes read or negative error code
 */
ssize_t vfs_pread(int fd, void *buf, size_t count, off_t offset);

/**
 * @brief Write to file at offset
 * @param fd File descriptor
 * @param buf Buffer to write from
 * @param count Number of bytes to write
 * @param offset File offset
 * @return Bytes written or negative error code
 */
ssize_t vfs_pwrite(int fd, const void *buf, size_t count, off_t offset);
```

### File Positioning

```c
/**
 * @brief Seek to position in file
 * @param fd File descriptor
 * @param offset Offset to seek
 * @param whence SEEK_SET, SEEK_CUR, or SEEK_END
 * @return New file position or negative error
 */
off_t vfs_lseek(int fd, off_t offset, int whence);

#define SEEK_SET 0  // Seek from beginning
#define SEEK_CUR 1  // Seek from current position
#define SEEK_END 2  // Seek from end
```

### File Information

```c
/**
 * @brief Get file status
 * @param path File path
 * @param statbuf Buffer to fill
 * @return 0 on success, negative error code on failure
 */
int vfs_stat(const char *path, struct stat *statbuf);

/**
 * @brief Get file status by descriptor
 * @param fd File descriptor
 * @param statbuf Buffer to fill
 * @return 0 on success, negative error code on failure
 */
int vfs_fstat(int fd, struct stat *statbuf);

/**
 * @brief Get file status (don't follow symlinks)
 * @param path File path
 * @param statbuf Buffer to fill
 * @return 0 on success, negative error code on failure
 */
int vfs_lstat(const char *path, struct stat *statbuf);

// File status structure
struct stat {
    dev_t     st_dev;     // Device ID
    ino_t     st_ino;     // Inode number
    mode_t    st_mode;    // File mode
    nlink_t   st_nlink;   // Number of hard links
    uid_t     st_uid;     // User ID
    gid_t     st_gid;     // Group ID
    dev_t     st_rdev;    // Device ID (if special file)
    off_t     st_size;    // File size
    blksize_t st_blksize; // Block size
    blkcnt_t  st_blocks;  // Number of blocks
    time_t    st_atime;   // Access time
    time_t    st_mtime;   // Modification time
    time_t    st_ctime;   // Status change time
};
```

### File Mode Macros

```c
// File type macros
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)  // Regular file
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)  // Directory
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)  // Character device
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)  // Block device
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)  // FIFO
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)  // Symbolic link
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK) // Socket

// Permission bits
#define S_IRUSR 0400  // User read
#define S_IWUSR 0200  // User write
#define S_IXUSR 0100  // User execute
#define S_IRGRP 0040  // Group read
#define S_IWGRP 0020  // Group write
#define S_IXGRP 0010  // Group execute
#define S_IROTH 0004  // Others read
#define S_IWOTH 0002  // Others write
#define S_IXOTH 0001  // Others execute
```

## Directory Operations

### Creating and Removing

```c
/**
 * @brief Create a directory
 * @param path Directory path
 * @param mode Creation mode
 * @return 0 on success, negative error code on failure
 */
int vfs_mkdir(const char *path, mode_t mode);

/**
 * @brief Remove a directory
 * @param path Directory path
 * @return 0 on success, negative error code on failure
 */
int vfs_rmdir(const char *path);

/**
 * @brief Remove a file
 * @param path File path
 * @return 0 on success, negative error code on failure
 */
int vfs_unlink(const char *path);

/**
 * @brief Rename file or directory
 * @param oldpath Current path
 * @param newpath New path
 * @return 0 on success, negative error code on failure
 */
int vfs_rename(const char *oldpath, const char *newpath);
```

### Directory Navigation

```c
/**
 * @brief Change current directory
 * @param path New directory path
 * @return 0 on success, negative error code on failure
 */
int vfs_chdir(const char *path);

/**
 * @brief Get current directory
 * @param buf Buffer to fill
 * @param size Buffer size
 * @return Buffer pointer or NULL on error
 */
char *vfs_getcwd(char *buf, size_t size);

/**
 * @brief Change root directory
 * @param path New root path
 * @return 0 on success, negative error code on failure
 */
int vfs_chroot(const char *path);
```

### Directory Reading

```c
/**
 * @brief Open directory for reading
 * @param path Directory path
 * @return Directory handle or NULL on error
 */
DIR *vfs_opendir(const char *path);

/**
 * @brief Read directory entry
 * @param dir Directory handle
 * @return Directory entry or NULL on end/error
 */
struct dirent *vfs_readdir(DIR *dir);

/**
 * @brief Close directory
 * @param dir Directory handle
 * @return 0 on success, negative error code on failure
 */
int vfs_closedir(DIR *dir);

// Directory entry structure
struct dirent {
    ino_t  d_ino;              // Inode number
    off_t  d_off;              // Offset to next entry
    unsigned short d_reclen;    // Length of this record
    unsigned char  d_type;      // File type
    char   d_name[256];        // Null-terminated filename
};

// File types for d_type
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK      10
#define DT_SOCK     12
```

## Mount Operations

### Mounting File Systems

```c
/**
 * @brief Mount a file system
 * @param source Device or file to mount
 * @param target Mount point
 * @param fstype File system type
 * @param flags Mount flags
 * @param data File system specific data
 * @return 0 on success, negative error code on failure
 */
int vfs_mount(const char *source, const char *target,
              const char *fstype, unsigned long flags,
              const void *data);

/**
 * @brief Unmount a file system
 * @param target Mount point
 * @param flags Unmount flags
 * @return 0 on success, negative error code on failure
 */
int vfs_umount(const char *target, int flags);

// Mount flags
#define MS_RDONLY      0x01  // Read-only mount
#define MS_NOSUID      0x02  // Ignore suid bits
#define MS_NODEV       0x04  // Ignore device files
#define MS_NOEXEC      0x08  // No execution
#define MS_SYNCHRONOUS 0x10  // Synchronous writes
#define MS_REMOUNT     0x20  // Remount existing mount
#define MS_NOATIME     0x400 // Don't update access times
```

### Mount Information

```c
/**
 * @brief Get mount information
 * @param path Path to check
 * @param info Mount info structure to fill
 * @return 0 on success, negative error code on failure
 */
int vfs_statfs(const char *path, struct statfs *info);

struct statfs {
    uint32_t f_type;     // File system type
    uint32_t f_bsize;    // Block size
    uint64_t f_blocks;   // Total blocks
    uint64_t f_bfree;    // Free blocks
    uint64_t f_bavail;   // Available blocks
    uint64_t f_files;    // Total inodes
    uint64_t f_ffree;    // Free inodes
    fsid_t   f_fsid;     // File system ID
    uint32_t f_namelen;  // Maximum filename length
};
```

## File Locking

```c
/**
 * @brief File locking operations
 * @param fd File descriptor
 * @param cmd Lock command
 * @param lock Lock structure
 * @return 0 on success, negative error code on failure
 */
int vfs_fcntl(int fd, int cmd, struct flock *lock);

// Lock commands
#define F_GETLK  5   // Get lock
#define F_SETLK  6   // Set lock
#define F_SETLKW 7   // Set lock and wait

struct flock {
    short l_type;    // F_RDLCK, F_WRLCK, F_UNLCK
    short l_whence;  // SEEK_SET, SEEK_CUR, SEEK_END
    off_t l_start;   // Starting offset
    off_t l_len;     // Length (0 = to EOF)
    pid_t l_pid;     // Process holding lock
};
```

## Page Cache Operations

```c
/**
 * @brief Read through page cache
 * @param file File structure
 * @param offset File offset
 * @param buffer Destination buffer
 * @param size Number of bytes
 * @return Bytes read or negative error
 */
ssize_t page_cache_read(file_t *file, off_t offset, 
                       void *buffer, size_t size);

/**
 * @brief Write through page cache
 * @param file File structure
 * @param offset File offset
 * @param buffer Source buffer
 * @param size Number of bytes
 * @return Bytes written or negative error
 */
ssize_t page_cache_write(file_t *file, off_t offset,
                        const void *buffer, size_t size);

/**
 * @brief Sync page cache to disk
 * @param file File structure (NULL for all)
 * @return 0 on success, negative error code on failure
 */
int page_cache_sync(file_t *file);

/**
 * @brief Drop page cache pages
 * @param file File structure
 * @param offset Starting offset
 * @param length Number of bytes
 * @return 0 on success, negative error code on failure
 */
int page_cache_invalidate(file_t *file, off_t offset, size_t length);
```

## Usage Examples

### Basic File I/O

```c
// Open file for reading
int fd = vfs_open("/etc/config.txt", O_RDONLY, 0);
if (fd < 0) {
    kprintf("Failed to open file: %d\n", fd);
    return fd;
}

// Read file contents
char buffer[1024];
ssize_t bytes = vfs_read(fd, buffer, sizeof(buffer) - 1);
if (bytes > 0) {
    buffer[bytes] = '\0';
    kprintf("File contents: %s\n", buffer);
}

// Close file
vfs_close(fd);
```

### Creating Files

```c
// Create new file with permissions
int fd = vfs_open("/tmp/output.txt", O_CREAT | O_WRONLY | O_TRUNC, 
                  S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
if (fd < 0) {
    return fd;
}

// Write data
const char *data = "Hello, World!\n";
vfs_write(fd, data, strlen(data));

// Close file
vfs_close(fd);
```

### Directory Operations

```c
// Create directory
if (vfs_mkdir("/tmp/newdir", 0755) < 0) {
    kprintf("Failed to create directory\n");
}

// List directory contents
DIR *dir = vfs_opendir("/tmp");
if (dir) {
    struct dirent *entry;
    while ((entry = vfs_readdir(dir)) != NULL) {
        kprintf("%s (type: %d)\n", entry->d_name, entry->d_type);
    }
    vfs_closedir(dir);
}

// Change to directory
vfs_chdir("/tmp/newdir");

// Get current directory
char cwd[PATH_MAX];
if (vfs_getcwd(cwd, sizeof(cwd))) {
    kprintf("Current directory: %s\n", cwd);
}
```

### File Information

```c
// Get file status
struct stat st;
if (vfs_stat("/bin/shell", &st) == 0) {
    kprintf("File size: %lld bytes\n", st.st_size);
    kprintf("Permissions: %o\n", st.st_mode & 0777);
    kprintf("Owner: %d:%d\n", st.st_uid, st.st_gid);
    
    if (S_ISREG(st.st_mode)) {
        kprintf("Regular file\n");
    } else if (S_ISDIR(st.st_mode)) {
        kprintf("Directory\n");
    }
}
```

### Mounting File Systems

```c
// Mount FAT file system
int result = vfs_mount("/dev/hda1", "/mnt/disk", "fat", 0, NULL);
if (result < 0) {
    kprintf("Mount failed: %d\n", result);
    return result;
}

// Check file system info
struct statfs fs_info;
if (vfs_statfs("/mnt/disk", &fs_info) == 0) {
    kprintf("Block size: %u\n", fs_info.f_bsize);
    kprintf("Total blocks: %llu\n", fs_info.f_blocks);
    kprintf("Free blocks: %llu\n", fs_info.f_bfree);
}

// Unmount when done
vfs_umount("/mnt/disk", 0);
```

## Performance Tips

1. **Use page cache**: Automatic for normal I/O
2. **Buffer I/O**: Read/write in larger chunks
3. **Minimize syscalls**: Use pread/pwrite for random access
4. **Directory caching**: Cache directory listings
5. **Async I/O**: Use non-blocking mode when possible

## Error Handling

Common error codes:
- `-ENOENT`: No such file or directory
- `-EACCES`: Permission denied
- `-EISDIR`: Is a directory
- `-ENOTDIR`: Not a directory
- `-ENOSPC`: No space left on device
- `-EMFILE`: Too many open files
- `-ENAMETOOLONG`: Filename too long
- `-ELOOP`: Too many symbolic links