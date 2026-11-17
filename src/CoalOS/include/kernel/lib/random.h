/**
 * @file random.h
 * @brief Random bytes for AT_RANDOM and getrandom(): RDRAND when the CPU has it, else a TSC-seeded generator.
 */
#ifndef KERNEL_LIB_RANDOM_H
#define KERNEL_LIB_RANDOM_H

#include <kernel/core/types.h>

void random_bytes(void *out, size_t n);

#endif
