/**
 * @file ksan.h
 * @brief Debug kernel (COAL_DEBUG_KERNEL): sanitizer reports, backtraces, stack protector.
 *
 * Every sanitizer in the debug kernel (UBSan, heap red zones and poisoning, KFENCE, the lock checker)
 * reports through ksan_report(): one WARN line "<KIND>: <what>" followed by a symbolised backtrace.
 * Command line: coal.ubsan=panic|warn (default warn), coal.san=panic|warn for memory errors (default warn),
 * coal.lockdep=panic|warn (default warn), coal.kfence=N (sample every Nth kmalloc, 0 = off).
 * Release kernels compile all of this to nothing.
 */
#ifndef KERNEL_DEBUG_KSAN_H
#define KERNEL_DEBUG_KSAN_H

#include <kernel/core/types.h>

struct isr_frame;

#ifdef COAL_DEBUG_KERNEL

enum ksan_kind { KSAN_UBSAN, KSAN_MEM, KSAN_LOCK, KSAN_KINDS };

#define KSAN_BT_MAX 8

/** Canary and reporting modes; call once the command line is known. */
void ksan_early_init(void);
void ksan_init(void);

/** Report a finding with the current backtrace; panics when the mode for `kind` says so. A report that wants
 *  to add more traces (where an object was allocated and freed) is ksan_begin(), ksan_trace()..., ksan_end();
 *  ksan_begin() returns false for a finding raised while another is being printed. */
bool ksan_begin(enum ksan_kind kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ksan_trace(const char *label, const uintptr_t *bt, unsigned n);
void ksan_end(enum ksan_kind kind);
#define ksan_report(kind, ...) do { if (ksan_begin((kind), __VA_ARGS__)) ksan_end(kind); } while (0)

/** Print a backtrace: of this context, from a saved capture, or from an exception frame. */
void ksan_backtrace(void);
void ksan_backtrace_buf(const uintptr_t *bt, unsigned n);
void ksan_backtrace_frame(const struct isr_frame *f);

/** Capture up to max return addresses of the callers of the function that calls this one. */
unsigned ksan_capture(uintptr_t *bt, unsigned max);

/** Name of the kernel function containing addr; NULL if none. */
const char *ksym_lookup(uintptr_t addr, uintptr_t *offset, uintptr_t *size);

/** Debug-only stop: print and halt (used by report paths that cannot continue). */
void ksan_halt(const char *why) __attribute__((noreturn));

#else

static inline void ksan_early_init(void) {}
static inline void ksan_init(void) {}

#endif
#endif
