/**
 * @file percpu.h
 * @brief Per-CPU data, reached through the GS segment base in kernel mode.
 *
 * Entries from user mode execute swapgs first (user GS base <-> IA32_KERNEL_GS_BASE), so %gs:0 always
 * holds a pointer to this structure while in the kernel. Only the boot CPU exists today; everything a
 * second CPU needs of its own (stack pointers, TSS, GDT, current task) already lives here.
 */
#ifndef PERCPU_H
#define PERCPU_H

#include <kernel/core/types.h>
#include <kernel/cpu/tss.h>

struct tcb;

#define IST_STACK_SIZE 4096

struct percpu {
    struct percpu *self;            /* %gs:0 */
    uint64_t kernel_rsp;            /* top of the running task's kernel stack: SYSCALL loads it */
    uint64_t user_rsp;              /* scratch: the user stack pointer during the SYSCALL entry */
    struct tcb *current;            /* the task on this CPU */
    uint32_t id;
    uint64_t user_fs, user_gs;      /* the user FS base and GS base the CPU holds (GS: in KERNEL_GS_BASE) */
    uint64_t gdt[8];
    struct tss tss;
    uint8_t ist_stacks[3][IST_STACK_SIZE] __attribute__((aligned(16)));
};

_Static_assert(offsetof(struct percpu, kernel_rsp) == 8 && offsetof(struct percpu, user_rsp) == 16 &&
               offsetof(struct percpu, current) == 24, "update PC_* in cpu_defs.inc and cpu_current()");

/** Sets up the boot CPU: GDT, TSS, GS base, CR4 features, and the SYSCALL MSRs. Call once, first. */
void cpu_init_boot(void);

static inline struct percpu *this_cpu(void) {
    struct percpu *cpu;
    asm("movq %%gs:0, %0" : "=r"(cpu));
    return cpu;
}

static inline struct tcb *cpu_current(void) {
    struct tcb *t;
    asm volatile("movq %%gs:24, %0" : "=r"(t));
    return t;
}

static inline void cpu_set_current(struct tcb *t) {
    asm volatile("movq %0, %%gs:24" : : "r"(t) : "memory");
}

/** The kernel stack the next entry from user mode uses (TSS.rsp0 and the SYSCALL stack). */
void cpu_set_kernel_stack(uint64_t top);

/** Makes fs and gs the user FS and GS bases (writes the registers that differ). */
void cpu_set_user_bases(uint64_t fs, uint64_t gs);

/** The user FS and GS bases right now; user code with FSGSBASE may have changed them. */
void cpu_get_user_bases(uint64_t *fs, uint64_t *gs);

/** CPU features the kernel turned on (CR4 bits and friends). */
extern bool g_cpu_smap, g_cpu_fsgsbase;

#endif
