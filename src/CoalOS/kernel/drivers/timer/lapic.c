/**
 * @file lapic.c
 * @brief Local APIC timer. The PIC keeps delivering the ISA interrupts through LINT0 (virtual wire).
 */
#include <kernel/drivers/timer/lapic.h>
#include <kernel/drivers/timer/pit.h>
#include <kernel/drivers/pic.h>
#include <kernel/cpu/idt.h>
#include <kernel/lib/div64.h>
#include <kernel/memory/paging.h>

#define MSR_APIC_BASE     0x1B
#define MSR_TSC_DEADLINE  0x6E0
#define APIC_BASE_ENABLE  (1u << 11)

#define REG_TPR        0x080
#define REG_EOI        0x0B0
#define REG_SVR        0x0F0
#define REG_LVT_TIMER  0x320
#define REG_LVT_LINT0  0x350
#define REG_LVT_LINT1  0x360
#define REG_LVT_ERROR  0x370
#define REG_TIMER_INIT 0x380
#define REG_TIMER_CUR  0x390
#define REG_TIMER_DIV  0x3E0

#define SVR_ENABLE     (1u << 8)
#define LVT_MASKED     (1u << 16)
#define LVT_TSC_DEADLINE (2u << 17)
#define LVT_EXTINT     0x700            /* LINT0: pass the 8259's interrupts through */
#define LVT_NMI        0x400
#define DIV_BY_1       0xB

#define CPUID1_EDX_MSR  (1u << 5)
#define CPUID1_EDX_APIC (1u << 9)
#define CPUID1_ECX_TSC_DEADLINE (1u << 24)

#define LAPIC_VADDR    (KERNEL_MMIO_VADDR_START + 0x200000u)
#define CYC_SHIFT      24               /* cycles = ns * s_cyc_mult >> CYC_SHIFT */
#define CALIBRATE_MS   2u

static volatile uint32_t *regs;
static uint32_t s_cyc_mult;

static inline uint32_t rd(uint32_t reg) { return regs[reg / 4]; }
static inline void wr(uint32_t reg, uint32_t v) { regs[reg / 4] = v; }

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v) {
    asm volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static uint64_t ns_to_cycles(uint32_t ns) {
    return ((uint64_t)ns * s_cyc_mult) >> CYC_SHIFT;
}

static void arm_oneshot(uint32_t delta_ns) {
    uint32_t count = (uint32_t)ns_to_cycles(delta_ns);      /* max_delta_ns keeps it in 32 bits */

    wr(REG_TIMER_INIT, count ? count : 1);
}

static void arm_deadline(uint32_t delta_ns) {
    wrmsr(MSR_TSC_DEADLINE, tsc_read() + ns_to_cycles(delta_ns));
}

static void lapic_timer_irq(isr_frame_t *frame) {
    (void)frame;
    wr(REG_EOI, 0);
    timer_interrupt();
}

static void lapic_spurious_irq(isr_frame_t *frame) {
    (void)frame;                        /* a spurious interrupt is not acknowledged */
}

/* Counts the timer for CALIBRATE_MS of TSC time; returns its frequency in kHz. */
static uint32_t calibrate(void) {
    uint64_t span = (uint64_t)clock_tsc_khz() * CALIBRATE_MS, t0;

    wr(REG_TIMER_DIV, DIV_BY_1);
    wr(REG_LVT_TIMER, LVT_MASKED | LAPIC_TIMER_VECTOR);
    wr(REG_TIMER_INIT, 0xFFFFFFFFu);
    t0 = tsc_read();
    while (tsc_read() - t0 < span) { }
    uint32_t elapsed = 0xFFFFFFFFu - rd(REG_TIMER_CUR);
    wr(REG_TIMER_INIT, 0);
    return elapsed / CALIBRATE_MS;
}

static const struct clockevent *finish(const char *name, void (*arm)(uint32_t), uint32_t max_ns) {
    static struct clockevent ev;

    ev.name = name;
    ev.arm = arm;
    ev.max_delta_ns = max_ns;
    pic_mask(0);                        /* the PIT no longer drives anything */
    return &ev;
}

const struct clockevent *lapic_clockevent_init(void) {
    uint32_t eax, ebx, ecx, edx, khz, base_lo;
    uint64_t max_ns;

    if (!clock_tsc_khz()) return NULL;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    if ((edx & (CPUID1_EDX_MSR | CPUID1_EDX_APIC)) != (CPUID1_EDX_MSR | CPUID1_EDX_APIC)) return NULL;

    base_lo = (uint32_t)rdmsr(MSR_APIC_BASE);
    if (!(base_lo & APIC_BASE_ENABLE)) wrmsr(MSR_APIC_BASE, (uint64_t)(base_lo | APIC_BASE_ENABLE));
    if (paging_map_single_4k((uint32_t *)g_kernel_page_directory_phys, LAPIC_VADDR, base_lo & 0xFFFFF000u,
                             PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT) != 0)
        return NULL;
    regs = (volatile uint32_t *)LAPIC_VADDR;

    wr(REG_TPR, 0);
    wr(REG_SVR, SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
    wr(REG_LVT_LINT0, LVT_EXTINT);
    wr(REG_LVT_LINT1, LVT_NMI);
    wr(REG_LVT_ERROR, LVT_MASKED);
    register_int_handler(LAPIC_TIMER_VECTOR, lapic_timer_irq, NULL);
    register_int_handler(LAPIC_SPURIOUS_VECTOR, lapic_spurious_irq, NULL);

    if (ecx & CPUID1_ECX_TSC_DEADLINE) {
        s_cyc_mult = (uint32_t)div64_32((uint64_t)clock_tsc_khz() << CYC_SHIFT, 1000000);
        wr(REG_LVT_TIMER, LAPIC_TIMER_VECTOR | LVT_TSC_DEADLINE);
        asm volatile("mfence" ::: "memory");        /* the mode switch must land before the first WRMSR */
        return finish("lapic-tsc-deadline", arm_deadline, 0xFFFFFFFFu);
    }

    khz = calibrate();
    if (!khz) return NULL;
    s_cyc_mult = (uint32_t)div64_32((uint64_t)khz << CYC_SHIFT, 1000000);
    wr(REG_LVT_TIMER, LAPIC_TIMER_VECTOR);          /* one-shot */
    max_ns = div64_32(0xFFFFFFFFull << CYC_SHIFT, s_cyc_mult);
    return finish("lapic", arm_oneshot, max_ns > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)max_ns);
}
