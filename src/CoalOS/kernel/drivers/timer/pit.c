/**
 * @file pit.c
 * @brief PIT driver: 1000 Hz tick that also drives the scheduler, and the
 *        TSC clock that gives log lines real times before interrupts run.
 */
#include <kernel/drivers/timer/pit.h>
#include <kernel/drivers/pic.h>
#include <kernel/cpu/idt.h>
#include <kernel/cpu/isr_frame.h>
#include <kernel/lib/port_io.h>
#include <kernel/process/scheduler.h>

#define PIT_BASE_HZ  1193182u
#define PIT_CH0      0x40
#define PIT_CMD      0x43
#define PIT_CMD_CH0_RATE_GEN 0x34 /* channel 0, lobyte/hibyte, mode 2 */
#define PIT_CH2      0x42
#define PIT_CMD_CH2_ONESHOT 0xB0  /* channel 2, lobyte/hibyte, mode 0 */
#define PORT_PC_CTRL 0x61         /* bit 0 gates channel 2, bit 1 is the speaker, bit 5 its output */
#define CALIBRATE_MS 4u

/* The counter is split so that readers on i386 can take a consistent
 * snapshot without disabling interrupts. */
static volatile uint32_t s_ticks_lo;
static volatile uint32_t s_ticks_hi;
static void (*s_hook)(void);

/* Until timer_start() the clock is the TSC; afterwards it is the tick count plus the boot time so far. */
static bool     s_running;
static uint32_t s_base_ms;
static uint32_t s_tsc_khz;              /* 0: no TSC, early time stays at 0 */
static uint64_t s_tsc0;

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint32_t tsc_ms(void) {
    uint64_t d = rdtsc() - s_tsc0;
    uint32_t q, r;

    if (!s_tsc_khz) return 0;
    asm("divl %4" : "=a"(q), "=d"(r) : "a"((uint32_t)d), "d"((uint32_t)(d >> 32)), "r"(s_tsc_khz));
    return q;
}

void timer_early_init(void) {
    uint32_t edx, count = PIT_BASE_HZ / 1000 * CALIBRATE_MS;

    asm volatile("cpuid" : "=d"(edx) : "a"(1) : "ebx", "ecx");
    if (!(edx & (1u << 4))) return;                         /* CPUID.1:EDX bit 4: TSC */

    outb(PORT_PC_CTRL, (inb(PORT_PC_CTRL) & ~2u) | 1u);     /* gate channel 2 open, speaker off */
    outb(PIT_CMD, PIT_CMD_CH2_ONESHOT);
    outb(PIT_CH2, (uint8_t)count);
    outb(PIT_CH2, (uint8_t)(count >> 8));
    s_tsc0 = rdtsc();
    while (!(inb(PORT_PC_CTRL) & 0x20)) { }
    /* ticks / (count / PIT_BASE_HZ) in kHz: the counter holds count * (1000 / PIT_BASE_HZ) ms, not exactly CALIBRATE_MS */
    uint64_t num = (rdtsc() - s_tsc0) * PIT_BASE_HZ;
    uint32_t rem;
    asm("divl %4" : "=a"(s_tsc_khz), "=d"(rem) : "a"((uint32_t)num), "d"((uint32_t)(num >> 32)), "r"(count * 1000u));
}

uint32_t timer_tsc_khz(uint64_t *tsc0) {
    *tsc0 = s_tsc0;
    return s_tsc_khz;
}

void timer_start(void) {
    s_base_ms = tsc_ms();
    s_running = true;
}

static void pit_irq_handler(isr_frame_t *frame) {
    (void)frame;
    if (++s_ticks_lo == 0) {
        s_ticks_hi++;
    }
    if (s_hook) {
        s_hook();
    }
    /* EOI first: scheduler_tick() may switch tasks and not return here. */
    pic_send_eoi(0);
    scheduler_tick();
}

void timer_set_hook(void (*fn)(void)) {
    s_hook = fn;
}

uint64_t timer_ticks(void) {
    uint32_t hi, lo;

    if (!s_running) return tsc_ms();
    do {
        hi = s_ticks_hi;
        lo = s_ticks_lo;
    } while (hi != s_ticks_hi);
    return (((uint64_t)hi << 32) | lo) + s_base_ms;
}

_Static_assert(TIMER_HZ == 1000, "timer_uptime_ms() assumes a 1 ms tick");

uint64_t timer_uptime_ms(void) {
    return timer_ticks(); /* no 64-bit division in the kernel (no libgcc) */
}

void init_pit(void) {
    const uint16_t divisor = (uint16_t)(PIT_BASE_HZ / TIMER_HZ);
    register_int_handler(IRQ0_VECTOR, pit_irq_handler, NULL);
    outb(PIT_CMD, PIT_CMD_CH0_RATE_GEN);
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)(divisor >> 8));
}
