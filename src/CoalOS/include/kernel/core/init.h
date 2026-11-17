/**
 * @file init.h
 * @brief Boot sequence entry
 */
#ifndef KERNEL_CORE_INIT_H
#define KERNEL_CORE_INIT_H

#include <kernel/core/types.h>

/** Brings up the kernel and starts the first process. Never returns. */
void kernel_main_init(uint32_t magic, uint32_t mb_info_phys_addr);

#endif
