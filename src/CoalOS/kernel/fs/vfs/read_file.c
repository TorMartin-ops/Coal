/**
 * @file read_file.c
 * @brief Load a whole file into kernel memory (ELF loader helper).
 */
#define LOG_MODULE "read_file"
#include <kernel/core/log.h>
#include <kernel/fs/vfs/read_file.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/memory/kmalloc.h>

void *read_file(const char *path, size_t *file_size) {
    file_t *f;
    struct kstat64 st;
    char *buf = NULL;
    size_t total = 0;

    if (!path || !file_size) return NULL;
    *file_size = 0;
    if (vfs_open(path, O_RDONLY, 0, &f) < 0) return NULL;
    if (vfs_fstat(f, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > 0x10000000) goto out;

    size_t size = (size_t)st.st_size;
    buf = kmalloc(size);
    if (!buf) { LOG_ERROR("out of memory loading %s (%u bytes)", path, (unsigned)size); goto out; }
    while (total < size) {
        ssize_t n = vfs_read(f, buf + total, size - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    if (total != size) {
        LOG_ERROR("short read on %s: %u of %u bytes", path, (unsigned)total, (unsigned)size);
        kfree(buf);
        buf = NULL;
        total = 0;
    }
out:
    vfs_close(f);
    *file_size = total;
    return buf;
}
