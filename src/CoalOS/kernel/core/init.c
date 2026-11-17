/**
 * @file init.c
 * @brief Boot sequence: runs once from main() after the Multiboot2 stub.
 */
#define LOG_MODULE "boot"
#include <kernel/core/init.h>
#include <kernel/core/log.h>
#include <kernel/core/multiboot.h>
#include <kernel/cpu/gdt.h>
#include <kernel/cpu/idt.h>
#include <kernel/process/fpu.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/frame.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/drivers/display/console_dev.h>
#include <kernel/drivers/timer/pit.h>
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

extern uint8_t _kernel_start_phys;
extern uint8_t _kernel_end_phys;

/* Physical address of the Multiboot2 info; the low 4 MiB stay identity-mapped,
 * so it is readable at the same address after paging is enabled. */
uint32_t g_multiboot_info_phys_addr_global = 0;

#define INIT_PATH "/sbin/init"

#define MIN_USABLE_HEAP_SIZE          (1 * 1024 * 1024)

/* ---- Memory map ---- */

/* Removes [res_start, res_end) from [*start, *end), keeping the larger remainder. */
static void subtract_reserved(uintptr_t *start, uintptr_t *end, uintptr_t res_start, uintptr_t res_end) {
    if (*start >= *end || res_start >= *end || res_end <= *start) return;
    uintptr_t below = res_start > *start ? res_start - *start : 0;
    uintptr_t above = res_end < *end ? *end - res_end : 0;
    if (below >= above) *end = *start + below;
    else                *start = res_end;
}

/* The largest available region, minus the kernel image and the boot info, becomes the heap. */
static void initialize_memory_management(uint32_t mb_info) {
    struct multiboot_tag_mmap *mmap = (void *)multiboot_find_tag(mb_info, MULTIBOOT_TAG_TYPE_MMAP);
    uintptr_t kstart = (uintptr_t)&_kernel_start_phys;
    uintptr_t kend = ALIGN_UP((uintptr_t)&_kernel_end_phys, PAGE_SIZE);
    uintptr_t iend = ALIGN_UP(mb_info + multiboot_info_size(mb_info), PAGE_SIZE);
    uintptr_t best_base = 0, best_len = 0;

    if (!mmap || mmap->entry_size < sizeof(multiboot_memory_map_t)) KERNEL_PANIC_HALT("no usable Multiboot memory map");
    if (mb_info >= IDENTITY_MAP_SIZE) KERNEL_PANIC_HALT("Multiboot info outside the identity-mapped region");

    for (multiboot_memory_map_t *e = mmap->entries;
         (uintptr_t)e + mmap->entry_size <= (uintptr_t)mmap + mmap->size;
         e = (void *)((uintptr_t)e + mmap->entry_size)) {
        if (e->type != MULTIBOOT_MEMORY_AVAILABLE || e->addr >= 0xFFFFFFFFull) continue;

        uint64_t end64 = e->addr + e->len;
        uintptr_t start = (uintptr_t)e->addr, end = end64 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uintptr_t)end64;
        subtract_reserved(&start, &end, kstart, kend);
        subtract_reserved(&start, &end, mb_info, iend);
        if (end > start && end - start > best_len) { best_base = start; best_len = end - start; }
    }
    if (best_len < MIN_USABLE_HEAP_SIZE) KERNEL_PANIC_HALT("no memory region large enough for the heap");
    memory_init(best_base, best_len);
}

/* Keyboard controller: interrupts on and scancode translation on (config 0x41);
 * every other bit stays as the controller reports it. */
static void configure_keyboard_controller(void) {
    uint8_t config = 0;

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

void kernel_main_init(uint32_t magic, uint32_t mb_info_phys_addr) {
    timer_early_init();
    serial_init(SERIAL_COM1);
    serial_init(SERIAL_COM2);
    terminal_init();
    console_dev_init();
    kprintf("Coal OS %s", COAL_VERSION);

    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) KERNEL_PANIC_HALT("not booted by a Multiboot2 loader");
    /* Spec: 8-byte aligned and non-null; GRUB puts it above 1 MiB. */
    if (mb_info_phys_addr == 0 || (mb_info_phys_addr & 7)) KERNEL_PANIC_HALT("bad Multiboot2 info address");
    g_multiboot_info_phys_addr_global = mb_info_phys_addr;

    gdt_init();
    initialize_memory_management(mb_info_phys_addr);
    idt_init();
    init_pit();
    fpu_init();
    scheduler_init();
    keyboard_init();
    keymap_load(KEYMAP_NORWEGIAN);
    net_init();

    if (fs_init() != FS_SUCCESS) KERNEL_PANIC_HALT("cannot mount the root file system");
    pcb_t *init = create_user_process(INIT_PATH);
    if (!init) KERNEL_PANIC_HALT("cannot start " INIT_PATH);
    scheduler_add_task(init);
    fs_start_flusher();

    configure_keyboard_controller();
    LOG_INFO("starting %s", INIT_PATH);
    klog_console_quiet();
    timer_start();
    asm volatile("sti");
    scheduler_start();
    KERNEL_PANIC_HALT("scheduler_start() returned");
}
