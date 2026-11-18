/**
 * @file init.c
 * @brief Boot sequence: runs once from main() after the Multiboot2 stub.
 */
#define LOG_MODULE "boot"
#include <kernel/core/init.h>
#include <kernel/core/log.h>
#include <kernel/core/boot_info.h>
#include <kernel/core/cmdline.h>
#include <kernel/lib/random.h>
#include <kernel/drivers/virtio/virtio.h>
#include <kernel/cpu/percpu.h>
#include <kernel/cpu/idt.h>
#include <kernel/cpu/syscall.h>
#include <kernel/process/vdso.h>
#include <kernel/drivers/timer/clock.h>
#include <kernel/process/fpu.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/frame.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/drivers/display/console_dev.h>
#include <kernel/drivers/timer/pit.h>
#include <kernel/drivers/timer/timer.h>
#include <kernel/drivers/input/keyboard.h>
#include <kernel/drivers/input/keyboard_hw.h>
#include <kernel/drivers/input/keymap.h>
#include <kernel/process/scheduler.h>
#include <kernel/process/process.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/net/net.h>
#include <kernel/fs/vfs/fs_init.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/lib/port_io.h>
#include <kernel/lib/assert.h>
#include <kernel/debug/ksan.h>
#include <kernel/memory/kfence.h>

#define INIT_PATH "/sbin/init"

/* Keyboard controller: interrupts on and scancode translation on (config 0x41);
 * every other bit stays as the controller reports it. */
static void configure_keyboard_controller(void) {
    uint8_t config = 0;

    if (inb(KBC_STATUS_PORT) == KBC_ABSENT_STATUS) return;
    outb(KBC_CMD_PORT, KBC_CMD_READ_CONFIG);
    for (volatile int i = 0; i < 15000; i++) asm volatile("pause");
    if (inb(KBC_STATUS_PORT) & KBC_SR_OBF) config = inb(KBC_DATA_PORT);
    config = (config | KBC_CFG_INT_KB | KBC_CFG_TRANSLATION) & ~KBC_CFG_DISABLE_KB;

    for (volatile int i = 0; (inb(KBC_STATUS_PORT) & KBC_SR_IBF) && i < KBC_WAIT_TIMEOUT_CYCLES; i++)
        asm volatile("pause");
    outb(KBC_CMD_PORT, KBC_CMD_WRITE_CONFIG);
    for (volatile int i = 0; (inb(KBC_STATUS_PORT) & KBC_SR_IBF) && i < KBC_WAIT_TIMEOUT_CYCLES; i++)
        asm volatile("pause");
    outb(KBC_DATA_PORT, config);

    for (volatile int i = 0; i < 25000; i++) asm volatile("pause");   /* let it apply */
    for (int n = 0; (inb(KBC_STATUS_PORT) & KBC_SR_OBF) && n < KBC_FLUSH_MAX_ATTEMPTS; n++) {
        (void)inb(KBC_DATA_PORT);                                    /* drop stale bytes */
        for (volatile int i = 0; i < 5000; i++) asm volatile("pause");
    }
}

/* coal.seed=N makes /dev/urandom, getrandom and AT_RANDOM repeatable, for tests. */
static void seed_random_from_cmdline(void) {
    char word[24];
    uint64_t seed;

    if (cmdline_get("coal.seed", 0, word, sizeof(word)) && cmdline_number(word, NULL, &seed)) random_seed(seed);
}

void kernel_main_init(uint32_t magic, uintptr_t boot_info) {
    struct boot_memmap map;

    ksan_early_init();
    serial_init(SERIAL_COM1);
    serial_init(SERIAL_COM2);
    terminal_init();
    console_dev_init();
    kprintf("Coal OS %s", COAL_VERSION);

    cpu_init_boot();
    cmdline_init(boot_info_cmdline(magic, boot_info));
    seed_random_from_cmdline();
    boot_info_memmap(magic, boot_info, &map);
    memory_init(&map);
    ksan_init();
#ifdef COAL_DEBUG_KERNEL
    kfence_init();
#endif
    idt_init();
    timer_early_init();
    clock_init();
    vdso_init();
    timer_init();
    LOG_INFO("timer: %s", timer_device_name());
    fpu_init();
    scheduler_init();
    keyboard_init();
    keymap_load(KEYMAP_NORWEGIAN);
    net_init();
    virtio_init();

    if (fs_init() != FS_SUCCESS) KERNEL_PANIC_HALT("cannot mount the root file system");
    pcb_t *init = create_user_process(INIT_PATH);
    if (!init) KERNEL_PANIC_HALT("cannot start " INIT_PATH);
    scheduler_add_task(init);
    fs_start_flusher();

    configure_keyboard_controller();
    LOG_INFO("starting %s", INIT_PATH);
    klog_console_quiet();
    asm volatile("sti");
    scheduler_start();
    KERNEL_PANIC_HALT("scheduler_start() returned");
}
