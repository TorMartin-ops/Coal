/**
 * @file pit.c
 * @brief The clock (TSC, calibrated against PIT channel 2) and the PIT as a clock event device.
 */
#include <kernel/drivers/timer/pit.h>
#include <kernel/drivers/timer/clock.h>
#include <kernel/drivers/pic.h>
#include <kernel/cpu/idt.h>
#include <kernel/lib/div64.h>
#include <kernel/lib/port_io.h>

#define PIT_BASE_HZ  1193182u
#define PIT_CH0      0x40
#define PIT_CMD      0x43
#define PIT_CMD_CH0_RATE_GEN 0x34 /* channel 0, lobyte/hibyte, mode 2 */
#define PIT_CMD_CH0_ONESHOT  0x30 /* channel 0, lobyte/hibyte, mode 0 */
#define PIT_CH2      0x42
#define PIT_CMD_CH2_ONESHOT 0xB0  /* channel 2, lobyte/hibyte, mode 0 */
#define PORT_PC_CTRL 0x61         /* bit 0 gates channel 2, bit 1 is the speaker, bit 5 its output */
#define CALIBRATE_MS 4u
#define PIT_COUNT_MULT 5003u /* counts = ns * 5003 >> 22, i.e. ns * 1.19318e-3 */
#define PIT_ONESHOT_MAX_NS   54000000u  /* 16-bit counter: 54.9 ms */

static uint32_t s_tsc_khz;              /* 0: no TSC, the clock counts PIT ticks */
static uint64_t s_tsc0;
static volatile uint32_t s_pit_ticks;   /* only counted in periodic mode */

void timer_early_init(void) {
    uint32_t edx, count = PIT_BASE_HZ / 1000 * CALIBRATE_MS;

    asm volatile("cpuid" : "=d"(edx) : "a"(1) : "ebx", "ecx");
    if (!(edx & (1u << 4))) return;                         /* CPUID.1:EDX bit 4: TSC */

    outb(PORT_PC_CTRL, (inb(PORT_PC_CTRL) & ~2u) | 1u);     /* gate channel 2 open, speaker off */
    outb(PIT_CMD, PIT_CMD_CH2_ONESHOT);
    outb(PIT_CH2, (uint8_t)count);
    outb(PIT_CH2, (uint8_t)(count >> 8));
    s_tsc0 = tsc_read();
    while (!(inb(PORT_PC_CTRL) & 0x20)) { }
    /* ticks / (count / PIT_BASE_HZ) in kHz: the counter holds count * (1000 / PIT_BASE_HZ) ms, not exactly CALIBRATE_MS */
    uint64_t num = (tsc_read() - s_tsc0) * PIT_BASE_HZ;
    uint32_t rem;
    asm("divl %4" : "=a"(s_tsc_khz), "=d"(rem) : "a"((uint32_t)num), "d"((uint32_t)(num >> 32)), "r"(count * 1000u));
    clock_calibrate();
}

uint32_t timer_tsc_khz(uint64_t *tsc0) {
    *tsc0 = s_tsc0;
    return s_tsc_khz;
}

uint32_t clock_tsc_khz(void) {
    return s_tsc_khz;
}

uint64_t pit_tick_ns(void) {
    return (uint64_t)s_pit_ticks * 1000000u;
}

/* One clock for the kernel and the vDSO: the TSC scaled in clock.c. */
uint64_t clock_ns(void) {
    return clock_monotonic_ns();
}

uint64_t timer_ticks(void) {
    return div64_32(clock_ns(), 1000000);
}

uint64_t timer_uptime_ms(void) {
    return timer_ticks();
}

/* ---- clock event device -------------------------------------------------------------- */

static void pit_arm(uint32_t delta_ns) {
    uint32_t count = (uint32_t)(((uint64_t)delta_ns * PIT_COUNT_MULT) >> 22) + 1;

    outb(PIT_CMD, PIT_CMD_CH0_ONESHOT);
    outb(PIT_CH0, (uint8_t)count);
    outb(PIT_CH0, (uint8_t)(count >> 8));
}

static void pit_ack(void) {
    if (!s_tsc_khz) s_pit_ticks++;      /* periodic mode: the interrupt is the clock */
    pic_send_eoi(0);
}

static void pit_irq(isr_frame_t *frame) {
    (void)frame;
    pit_ack();
    timer_interrupt();
}

static const struct clockevent pit_oneshot = { "pit", PIT_ONESHOT_MAX_NS, pit_arm };
static const struct clockevent pit_periodic = { "pit-periodic", 0, NULL };

const struct clockevent *pit_clockevent_init(void) {
    const uint16_t divisor = (uint16_t)(PIT_BASE_HZ / 1000);

    register_int_handler(IRQ0_VECTOR, pit_irq, NULL);
    pic_unmask(0);
    if (s_tsc_khz) return &pit_oneshot;
    outb(PIT_CMD, PIT_CMD_CH0_RATE_GEN);
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)(divisor >> 8));
    return &pit_periodic;
}
