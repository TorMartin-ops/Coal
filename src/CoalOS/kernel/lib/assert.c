#include <kernel/lib/assert.h>
#include <kernel/core/log.h>

void kernel_panic(const char *msg, const char *file, int line) {
    asm volatile("cli");
    kprintf("KERNEL PANIC: %s at %s:%d", msg, file, line);
    for (;;) asm volatile("hlt");
}
