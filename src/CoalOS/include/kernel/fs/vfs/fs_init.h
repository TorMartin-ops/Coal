/** @file fs_init.h - bring up the file system layer */
#ifndef FS_INIT_H
#define FS_INIT_H

/**
 * Initialise the VFS, register the FAT, devfs and procfs drivers, mount the
 * root disk on / and devfs and procfs on /dev and /proc.
 * Returns 0 or a negative errno (only a failed root mount is fatal).
 */
int fs_init(void);

#endif
