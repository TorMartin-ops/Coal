/**
 * @file assert.h
 * @brief Kernel panic and assertions.
 */
#ifndef ASSERT_H
#define ASSERT_H

/** Prints the message with its location and halts the machine. */
void kernel_panic(const char *msg, const char *file, int line) __attribute__((noreturn));

#define KERNEL_PANIC_HALT(msg) kernel_panic((msg), __FILE__, __LINE__)

#define KERNEL_ASSERT(expr, msg) \
    do { if (!(expr)) kernel_panic("assertion failed: " msg, __FILE__, __LINE__); } while (0)

#endif
