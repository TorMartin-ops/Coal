/**
 * @file cmdline.h
 * @brief Kernel command line from the boot loader ("root=/dev/vda virtio_mmio.device=512@0xfeb00000:5").
 *
 * The line is a list of whitespace-separated words; "key=value" words carry a value,
 * bare words are flags. A key may repeat.
 */
#ifndef KERNEL_CORE_CMDLINE_H
#define KERNEL_CORE_CMDLINE_H

#include <kernel/core/types.h>

/** Copies the command line out of the Multiboot2 information. Call once, early. */
void cmdline_init(uintptr_t mb_info);

/** The whole line (empty when the loader gave none). */
const char *cmdline_raw(void);

/**
 * Value of the nth (from 0) "key=value" word, copied into out (NUL terminated, cut to cap).
 * Returns false when there is no such word.
 */
bool cmdline_get(const char *key, unsigned nth, char *out, size_t cap);

/** True when the bare word `key` or any "key=..." word is present. */
bool cmdline_has(const char *key);

/** Parses an unsigned number in decimal or 0x hexadecimal; returns false on junk or overflow. */
bool cmdline_number(const char *s, const char **end, uint64_t *out);

#endif
