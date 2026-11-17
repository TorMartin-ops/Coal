/**
 * @file syscall_user.c
 * @brief Moving paths and argument vectors across the user boundary, and
 *        the FS_ERR_* to errno mapping.
 */

#include "syscall_internal.h"
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/paging.h>
#include <kernel/lib/string.h>

int copy_path_from_user(const char *user, char *kbuf, size_t size) {
    size_t len = 0;

    while (len < size) {
        /* Never read past the page holding the next byte: the string may end just before an unmapped page. */
        size_t chunk = PAGE_SIZE - (((uintptr_t)user + len) & (PAGE_SIZE - 1));
        if (chunk > size - len) chunk = size - len;

        if (copy_from_user(kbuf + len, user + len, chunk) != 0)
            return -EFAULT;
        const char *nul = memchr(kbuf + len, 0, chunk);
        if (nul)
            return nul == kbuf ? -ENOENT : 0;
        len += chunk;
    }
    return -ENAMETOOLONG;
}

int fs_err_to_errno(int fs_err) {
    static const uint8_t map[] = {
        [0]                              = 0,
        [-FS_ERR_UNKNOWN]                = EIO,
        [-FS_ERR_INVALID_PARAM]          = EINVAL,
        [-FS_ERR_OUT_OF_MEMORY]          = ENOMEM,
        [-FS_ERR_IO]                     = EIO,
        [-FS_ERR_NOT_FOUND]              = ENOENT,
        [-FS_ERR_PERMISSION_DENIED]      = EACCES,
        [-FS_ERR_FILE_EXISTS]            = EEXIST,
        [-FS_ERR_NOT_A_DIRECTORY]        = ENOTDIR,
        [-FS_ERR_IS_A_DIRECTORY]         = EISDIR,
        [-FS_ERR_NO_SPACE]               = ENOSPC,
        [-FS_ERR_READ_ONLY]              = EROFS,
        [-FS_ERR_NOT_SUPPORTED]          = ENOSYS,
        [-FS_ERR_INVALID_FORMAT]         = EINVAL,
        [-FS_ERR_CORRUPT]                = EIO,
        [-FS_ERR_MOUNT]                  = EIO,
        [-FS_ERR_NOT_INIT]               = ENODEV,
        [-FS_ERR_BUSY]                   = EBUSY,
        [-FS_ERR_INTERNAL]               = EIO,
        [-FS_ERR_NAMETOOLONG]            = ENAMETOOLONG,
        [-FS_ERR_OVERFLOW]               = EOVERFLOW,
        [-FS_ERR_EOF]                    = 0,
        [-FS_ERR_NO_RESOURCES]           = ENOMEM,
        [-FS_ERR_OUT_OF_BOUNDS]          = EINVAL,
        [-FS_ERR_BAD_F]                  = EBADF,
        [-FS_ERR_BOUNDS_VIOLATION]       = EFAULT,
    };
    if (fs_err > 0) return fs_err;                  /* already an errno */
    if (-fs_err >= (int)sizeof(map)) return EIO;
    return map[-fs_err];
}

/*
 * Copy a NULL-terminated user pointer array and its strings into one block:
 * [ pointer vector | strings ]. Returns the number of strings or -errno.
 */
static long copy_strv(uint32_t user_vec, char **vec, size_t *nvec, char *strings, size_t *used) {
    long n = 0;

    if (!user_vec) { vec[(*nvec)++] = NULL; return 0; }
    for (;;) {
        uint32_t uptr;
        if (copy_from_user(&uptr, (const void *)(user_vec + 4u * n), sizeof(uptr)) != 0)
            return -EFAULT;
        if (*nvec >= ARG_MAX_VECS) return -E2BIG;
        if (!uptr) { vec[(*nvec)++] = NULL; return n; }

        long len = copy_path_from_user((const char *)uptr, strings + *used, ARG_MAX_BYTES - *used);
        if (len == -ENOENT) {                       /* empty string is a valid argument */
            strings[*used] = '\0';
        } else if (len == -ENAMETOOLONG) {
            return -E2BIG;
        } else if (len < 0) {
            return len;
        }
        vec[(*nvec)++] = strings + *used;
        *used += strlen(strings + *used) + 1;
        n++;
    }
}

long exec_args_copy(uint32_t user_argv, uint32_t user_envp, struct exec_args *out) {
    size_t vec_bytes = ARG_MAX_VECS * sizeof(char *);
    char *block = kmalloc(vec_bytes + ARG_MAX_BYTES);
    if (!block) return -ENOMEM;

    char **vec = (char **)block;
    char *strings = block + vec_bytes;
    size_t nvec = 0, used = 0;

    long argc = copy_strv(user_argv, vec, &nvec, strings, &used);
    size_t envp_at = nvec;
    long envc = argc < 0 ? argc : copy_strv(user_envp, vec, &nvec, strings, &used);
    if (argc < 0 || envc < 0) {
        kfree(block);
        return argc < 0 ? argc : envc;
    }
    out->block = block;
    out->argv  = vec;
    out->envp  = vec + envp_at;
    return 0;
}

void exec_args_free(struct exec_args *a) {
    kfree(a->block);
}
