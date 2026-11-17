/* Model-specific registers and control registers. */
#ifndef KERNEL_CPU_MSR_H
#define KERNEL_CPU_MSR_H

#include <kernel/core/types.h>

#define MSR_APIC_BASE       0x1B
#define MSR_TSC_DEADLINE    0x6E0
#define MSR_EFER            0xC0000080
#define MSR_STAR            0xC0000081
#define MSR_LSTAR           0xC0000082
#define MSR_FMASK           0xC0000084
#define MSR_FS_BASE         0xC0000100
#define MSR_GS_BASE         0xC0000101
#define MSR_KERNEL_GS_BASE  0xC0000102

#define EFER_SCE            (1ull << 0)

#define CR0_MP              (1ul << 1)
#define CR0_EM              (1ul << 2)
#define CR0_TS              (1ul << 3)
#define CR4_FSGSBASE        (1ul << 16)
#define CR4_OSFXSR          (1ul << 9)
#define CR4_OSXMMEXCPT      (1ul << 10)
#define CR4_SMEP            (1ul << 20)
#define CR4_SMAP            (1ul << 21)

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v) {
    asm volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}

static inline uint64_t read_cr0(void) { uint64_t v; asm volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline void write_cr0(uint64_t v) { asm volatile("mov %0, %%cr0" : : "r"(v) : "memory"); }
static inline uint64_t read_cr2(void) { uint64_t v; asm volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr3(void) { uint64_t v; asm volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void write_cr3(uint64_t v) { asm volatile("mov %0, %%cr3" : : "r"(v) : "memory"); }
static inline uint64_t read_cr4(void) { uint64_t v; asm volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr4(uint64_t v) { asm volatile("mov %0, %%cr4" : : "r"(v) : "memory"); }

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#endif
