/** @file fs_init.h - bring up the file system layer */
#ifndef FS_INIT_H
#define FS_INIT_H

/**
 * Initialise the VFS, register the FAT, devfs and procfs drivers, mount the
 * root disk on / and devfs and procfs on /dev and /proc.
 * Returns 0 or a negative errno (only a failed root mount is fatal).
 */
int fs_init(void);

/** Starts the thread that flushes cached file system changes to disk periodically.
 *  Call after init exists: init must keep pid 1. */
void fs_start_flusher(void);

/**
 * Makes an additional ATA disk (hdb, hdc, hdd) reachable for mounting; the root
 * disk is always attached. Returns 0 or -ENODEV when there is no such drive.
 */
int fs_attach_disk(const char *name);

#endif
