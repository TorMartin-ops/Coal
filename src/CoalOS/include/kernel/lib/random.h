/**
 * @file random.h
 * @brief Kernel random number generator behind getrandom(2), /dev/urandom and AT_RANDOM.
 *
 * A ChaCha20 generator with fast key erasure. It starts from the cycle counter, which is weak;
 * entropy from a host device (virtio-rng) is mixed in with random_add() and makes it strong.
 */
#ifndef KERNEL_LIB_RANDOM_H
#define KERNEL_LIB_RANDOM_H

#include <kernel/core/types.h>

/** Mixes len bytes of entropy into the generator. Safe from interrupt context. */
void random_add(const void *data, size_t len);

/** Fills out with random bytes. Safe from interrupt context. */
void random_bytes(void *out, size_t len);

#endif
