/**
 * @file cpu.c
 * @brief Boot CPU bring-up: per-CPU data, segment tables and the CPU features the kernel relies on.
 */
#define LOG_MODULE "cpu"
#include <kernel/cpu/percpu.h>
#include <kernel/cpu/gdt.h>
#include <kernel/cpu/msr.h>
#include <kernel/cpu/syscall.h>
#include <kernel/core/log.h>

#define CPUID7_EBX_FSGSBASE (1u << 0)
#define CPUID7_EBX_SMEP     (1u << 7)
#define CPUID7_EBX_SMAP     (1u << 20)

bool g_cpu_smap, g_cpu_fsgsbase;

static struct percpu boot_cpu;

static void cpuid_count(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    asm volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "0"(leaf), "2"(sub));
}

void cpu_init_boot(void) {
    struct percpu *cpu = &boot_cpu;
    uint32_t a, b = 0, c, d, max_leaf;
    uint64_t cr4 = read_cr4();

    cpu->self = cpu;
    cpu->id = 0;
    gdt_init(cpu);                                  /* loads null into gs: set its base afterwards */
    wrmsr(MSR_GS_BASE, (uint64_t)cpu);
    wrmsr(MSR_KERNEL_GS_BASE, 0);

    cpuid_count(0, 0, &max_leaf, &b, &c, &d);
    b = 0;
    if (max_leaf >= 7) cpuid_count(7, 0, &a, &b, &c, &d);
    if (b & CPUID7_EBX_SMEP) cr4 |= CR4_SMEP;
    if (b & CPUID7_EBX_SMAP) { cr4 |= CR4_SMAP; g_cpu_smap = true; }
    if (b & CPUID7_EBX_FSGSBASE) { cr4 |= CR4_FSGSBASE; g_cpu_fsgsbase = true; }
    write_cr4(cr4);

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    syscall_init();
    LOG_INFO("smep %s, smap %s, fsgsbase %s", (b & CPUID7_EBX_SMEP) ? "on" : "off", g_cpu_smap ? "on" : "off",
             g_cpu_fsgsbase ? "on" : "off");
}

void cpu_set_kernel_stack(uint64_t top) {
    struct percpu *cpu = this_cpu();

    cpu->tss.rsp0 = top;
    cpu->kernel_rsp = top;
}

void cpu_set_user_bases(uint64_t fs, uint64_t gs) {
    struct percpu *cpu = this_cpu();

    if (fs != cpu->user_fs) { wrmsr(MSR_FS_BASE, fs); cpu->user_fs = fs; }
    if (gs != cpu->user_gs) { wrmsr(MSR_KERNEL_GS_BASE, gs); cpu->user_gs = gs; }
}

void cpu_get_user_bases(uint64_t *fs, uint64_t *gs) {
    struct percpu *cpu = this_cpu();

    if (g_cpu_fsgsbase) {
        asm volatile("rdfsbase %0" : "=r"(cpu->user_fs));
        asm volatile("swapgs; rdgsbase %0; swapgs" : "=r"(cpu->user_gs));
    }
    *fs = cpu->user_fs;
    *gs = cpu->user_gs;
}
