/** @file fat.h - FAT16/FAT32 driver (with long file names) */
#ifndef FAT_H
#define FAT_H

/** Register the "fat" driver with the VFS. Returns 0 or a negative errno. */
int fat_register_driver(void);

#endif
