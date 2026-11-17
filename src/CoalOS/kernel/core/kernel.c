/**
 * @file kernel.c
 * @brief Kernel entry point called by the Multiboot2 boot stub
 */
#include <kernel/core/init.h>
#include <kernel/lib/assert.h>

void main(uint32_t magic, uint32_t mb_info_phys_addr) {
    kernel_main_init(magic, mb_info_phys_addr);

    // kernel_main_init() hands over to the scheduler and never returns
    KERNEL_PANIC_HALT("kernel_main_init() returned unexpectedly!");
}
