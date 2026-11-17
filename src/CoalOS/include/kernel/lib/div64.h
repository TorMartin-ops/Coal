/**
 * @file div64.h
 * @brief 64-bit by 32-bit arithmetic helpers.
 */
#ifndef KERNEL_LIB_DIV64_H
#define KERNEL_LIB_DIV64_H

#include <kernel/core/types.h>

/** n / d. */
static inline uint64_t div64_32(uint64_t n, uint32_t d) { return n / d; }

/** floor(n / d). */
static inline int64_t div64_s32_floor(int64_t n, uint32_t d) {
    int64_t q = n / (int64_t)d;
    return n % (int64_t)d < 0 ? q - 1 : q;
}

/** (a * b) >> 32 without overflowing 64 bits for a < 2^64. */
static inline uint64_t mul64_32_shr32(uint64_t a, uint32_t b) { return (uint64_t)(((unsigned __int128)a * b) >> 32); }

#endif /* KERNEL_LIB_DIV64_H */
