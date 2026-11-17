/**
 * @file div64.h
 * @brief 64-bit by 32-bit arithmetic without libgcc: one or two DIV/MUL instructions each.
 */
#ifndef KERNEL_LIB_DIV64_H
#define KERNEL_LIB_DIV64_H

#include <kernel/core/types.h>

/** n / d for any 64-bit n (the quotient may exceed 32 bits). */
static inline uint64_t div64_32(uint64_t n, uint32_t d) {
    uint32_t hi = (uint32_t)(n >> 32), lo = (uint32_t)n;
    uint32_t q_hi = hi / d, rem = hi % d, q_lo;

    asm("divl %2" : "=a"(q_lo), "=d"(rem) : "r"(d), "0"(lo), "1"(rem));
    return ((uint64_t)q_hi << 32) | q_lo;
}

/** floor(n / d). */
static inline int64_t div64_s32_floor(int64_t n, uint32_t d) {
    if (n >= 0) return (int64_t)div64_32((uint64_t)n, d);
    return -(int64_t)div64_32((uint64_t)-n + d - 1, d);
}

/** (a * b) >> 32 without overflowing 64 bits for a < 2^64. */
static inline uint64_t mul64_32_shr32(uint64_t a, uint32_t b) {
    return (uint64_t)(uint32_t)(a >> 32) * b + (((uint64_t)(uint32_t)a * b) >> 32);
}

#endif /* KERNEL_LIB_DIV64_H */
