/**
 * @file virtio_blk.h
 * @brief virtio-blk disks, named vda, vdb, ... in probe order.
 */
#ifndef KERNEL_DRIVERS_VIRTIO_BLK_H
#define KERNEL_DRIVERS_VIRTIO_BLK_H

#include <kernel/drivers/storage/block_device.h>

/** Fills dev for the named virtio disk ("vda".."vdd"). BLOCK_ERR_NO_DEV if there is none. */
int virtio_blk_attach(const char *name, block_device_t *dev);

#endif
