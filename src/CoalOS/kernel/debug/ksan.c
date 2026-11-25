/**
 * @file ksan.c
 * @brief Report path shared by the debug-kernel sanitizers, and the stack-protector canary.
 */
#define LOG_MODULE "ksan"

#include <kernel/debug/ksan.h>
#include <kernel/core/cmdline.h>
#include <kernel/core/log.h>
#include <kernel/lib/random.h>
#include <kernel/lib/string.h>
#include <kernel/drivers/display/terminal.h>
#include <libc/stdarg.h>

uintptr_t __stack_chk_guard = 0x595e9fbd94fda766ul;

static bool g_panic[KSAN_KINDS];
static bool g_in_report;

static const char *const kind_name[KSAN_KINDS] = { "UBSAN", "KSAN", "LOCKDEP" };

/* The canary needs no function with a protector to be live across the change: this runs first, in a function that
 * never returns to a protected frame, and the zero low byte stops string overruns from copying it. */
__attribute__((no_stack_protector)) void ksan_early_init(void) {
    uintptr_t c;

    random_bytes(&c, sizeof(c));
    __stack_chk_guard = c & ~0xFFul;
}

static void read_mode(const char *key, enum ksan_kind kind) {
    char v[8];

    g_panic[kind] = cmdline_get(key, 0, v, sizeof(v)) && !strcmp(v, "panic");
}

void ksan_init(void) {
    read_mode("coal.ubsan", KSAN_UBSAN);
    read_mode("coal.san", KSAN_MEM);
    read_mode("coal.lockdep", KSAN_LOCK);
}

void ksan_halt(const char *why) {
    asm volatile("cli");
    kprintf("KERNEL PANIC: %s", why);
    for (;;)
        asm volatile("hlt");
}

bool ksan_begin(enum ksan_kind kind, const char *fmt, ...) {
    char msg[200];
    va_list ap;

    if (g_in_report)
        return false;                           /* a finding inside the report path would recurse */
    g_in_report = true;
    va_start(ap, fmt);
    _vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    LOG_WARN("%s: %s", kind_name[kind], msg);
    ksan_backtrace();
    return true;
}

void ksan_trace(const char *label, const uintptr_t *bt, unsigned n) {
    unsigned used = 0;

    while (used < n && bt[used])
        used++;
    if (!used)
        return;
    LOG_WARN("%s:", label);
    ksan_backtrace_buf(bt, used);
}

void ksan_end(enum ksan_kind kind) {
    g_in_report = false;
    if (g_panic[kind])
        ksan_halt(kind_name[kind]);
}

void __stack_chk_fail(void) {
    LOG_WARN("stack smashing detected");
    ksan_backtrace();
    ksan_halt("stack smashing detected");
}
