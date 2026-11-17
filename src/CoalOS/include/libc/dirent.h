#ifndef LIBC_DIRENT_H
#define LIBC_DIRENT_H

#include <libc/stdint.h>

#define DT_UNKNOWN 0
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8

/* Layout of Linux linux_dirent64. */
struct dirent {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
};

typedef struct DIR DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);

#endif
