/**
 * @file fs_errno.h
 * @brief Linux errno values (positive); kernel code returns their negatives.
 */
#ifndef FS_ERRNO_H
#define FS_ERRNO_H

#define EPERM         1
#define ENOENT        2
#define ESRCH         3
#define EINTR         4
#define EIO           5
#define ENXIO         6
#define E2BIG         7
#define ENOEXEC       8
#define EBADF         9
#define ECHILD       10
#define EAGAIN       11
#define ENOMEM       12
#define EACCES       13
#define EFAULT       14
#define ENOTBLK      15
#define EBUSY        16
#define EEXIST       17
#define EXDEV        18
#define ENODEV       19
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define ENFILE       23
#define EMFILE       24
#define ENOTTY       25
#define ETXTBSY      26
#define EFBIG        27
#define ENOSPC       28
#define ESPIPE       29
#define EROFS        30
#define EMLINK       31
#define EPIPE        32
#define EDOM         33
#define ERANGE       34
#define EDEADLK      35
#define ENAMETOOLONG 36
#define ENOLCK       37
#define ENOSYS       38
#define ENOTEMPTY    39
#define ELOOP        40
#define EOVERFLOW    75

/* Legacy codes still used by the block layer (disk, buffer cache). */
enum {
    FS_SUCCESS               = 0,
    FS_ERR_INVALID_PARAM     = -2,
    FS_ERR_OUT_OF_MEMORY     = -3,
    FS_ERR_IO                = -4,
    FS_ERR_NOT_FOUND         = -5,
    FS_ERR_PERMISSION_DENIED = -6,
    FS_ERR_FILE_EXISTS       = -7,
    FS_ERR_NOT_A_DIRECTORY   = -8,
    FS_ERR_IS_A_DIRECTORY    = -9,
    FS_ERR_NO_SPACE          = -10,
    FS_ERR_NOT_SUPPORTED     = -12,
    FS_ERR_INVALID_FORMAT    = -13,
    FS_ERR_BUSY              = -17,
    FS_ERR_INTERNAL          = -18,
    FS_ERR_NO_RESOURCES      = -22,
    FS_ERR_OUT_OF_BOUNDS     = -23,
};

#endif /* FS_ERRNO_H */
