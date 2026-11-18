#include <kernel/lib/assert.h>
#include <kernel/core/log.h>
#ifdef COAL_DEBUG_KERNEL
#include <kernel/debug/ksan.h>
#endif

void kernel_panic(const char *msg, const char *file, int line) {
    asm volatile("cli");
    kprintf("KERNEL PANIC: %s at %s:%d", msg, file, line);
#ifdef COAL_DEBUG_KERNEL
    ksan_backtrace();
#endif
    for (;;) asm volatile("hlt");
}
