/**
 * @file kernel.c
 * @brief Kernel entry point called by the boot stub (boot/boot.asm)
 */
#include <kernel/core/init.h>
#include <kernel/lib/assert.h>

void main(uint32_t magic, uintptr_t boot_info) {
    kernel_main_init(magic, boot_info);

    // kernel_main_init() hands over to the scheduler and never returns
    KERNEL_PANIC_HALT("kernel_main_init() returned unexpectedly!");
}
