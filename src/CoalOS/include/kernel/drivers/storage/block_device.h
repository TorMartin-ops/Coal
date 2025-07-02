#pragma once
#ifndef BLOCK_DEVICE_H
#define BLOCK_DEVICE_H

#include <kernel/core/types.h>
#include <kernel/sync/spinlock.h> // Include for spinlock_t
#include <kernel/cpu/isr_frame.h>

// --- Error Codes ---
#define BLOCK_ERR_OK          0
#define BLOCK_ERR_PARAMS     -1 // Invalid parameters
#define BLOCK_ERR_TIMEOUT    -2 // Operation timed out
#define BLOCK_ERR_DEV_ERR    -3 // Device reported an error (ERR bit)
#define BLOCK_ERR_DEV_FAULT  -4 // Device fault (DF bit)
#define BLOCK_ERR_NO_DEV     -5 // Device not present or failed IDENTIFY/setup
#define BLOCK_ERR_BOUNDS     -6 // LBA out of bounds
#define BLOCK_ERR_UNSUPPORTED -7 // Feature/command not supported by drive
#define BLOCK_ERR_LOCKED     -8 // Could not acquire lock
#define BLOCK_ERR_INTERNAL   -9 // Internal driver error
#define BLOCK_ERR_IO         -10 // Generic I/O Error (e.g., DRQ not set when expected)

// --- Device Structure ---
typedef struct {
    const char *device_name;   // e.g., "hda", "hdb"
    uint16_t io_base;          // e.g., 0x1F0
    uint16_t control_base;     // e.g., 0x3F6
    bool is_slave;             // Master (false) or Slave (true)
    // --- REORDERED FIELDS ---
    uint64_t total_sectors;    // Moved total_sectors up
    uint32_t sector_size;      // <<< MOVED UP: Now uint32_t before uint16_t
    uint16_t multiple_sector_count; // Max sectors per READ/WRITE MULTIPLE cmd (0 if unsupported)
    // --- END REORDER ---
    bool initialized;
    bool lba48_supported;
    spinlock_t *channel_lock;  // Pointer to the channel's lock (primary/secondary)
} block_device_t;

// --- Public API ---

// Initializes ATA channels (locks) - Call once at boot
void ata_channels_init(void);

// Initializes a specific block device structure (hda, hdb, etc.)
int block_device_init(const char *device, block_device_t *dev);

// Reads sectors using best available method (MULTIPLE or single PIO)
// LBA is now uint64_t
int block_device_read(block_device_t *dev, uint64_t lba, void *buffer, size_t count);

// Writes sectors using best available method (MULTIPLE or single PIO)
// LBA is now uint64_t
int block_device_write(block_device_t *dev, uint64_t lba, const void *buffer, size_t count);

void ata_primary_irq_handler(isr_frame_t* frame); // <<< ADDED DECLARATION

#endif /* BLOCK_DEVICE_H */