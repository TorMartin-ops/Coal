/**
 * @file fpu.c
 * @brief Lazy FPU switching. The registers belong to fpu_owner; CR0.TS is set whenever the running
 *        task is not the owner, so its first FPU or SSE instruction raises #NM and swaps the state in.
 *
 * Invariant: while a process runs, CR0.TS is clear exactly when it is fpu_owner. The kernel itself
 * never uses the FPU.
 */
#include <kernel/process/fpu.h>
#include <kernel/cpu/idt.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>

#define CR0_MP   (1u << 1)
#define CR0_EM   (1u << 2)
#define CR0_TS   (1u << 3)
#define CR4_OSFXSR     (1u << 9)
#define CR4_OSXMMEXCPT (1u << 10)
#define NM_VECTOR 7

static pcb_t *fpu_owner;
static bool have_fxsr;

static inline uint32_t read_cr0(void) { uint32_t v; asm volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline void write_cr0(uint32_t v) { asm volatile("mov %0, %%cr0" : : "r"(v)); }
static inline void set_ts(void) { write_cr0(read_cr0() | CR0_TS); }

static uint8_t *area_of(pcb_t *p) {
    if (!p->fpu) {
        p->fpu_raw = kmalloc(FPU_AREA_SIZE + 16);
        if (!p->fpu_raw) return NULL;
        p->fpu = (uint8_t *)(((uintptr_t)p->fpu_raw + 15) & ~(uintptr_t)15);
    }
    return p->fpu;
}

static void store(uint8_t *area) {
    if (have_fxsr) asm volatile("fxsave (%0)" : : "r"(area) : "memory");
    else asm volatile("fnsave (%0)\n\tfrstor (%0)" : : "r"(area) : "memory");      /* fnsave resets the FPU */
}

static void load(const uint8_t *area) {
    if (have_fxsr) asm volatile("fxrstor (%0)" : : "r"(area) : "memory");
    else asm volatile("frstor (%0)" : : "r"(area) : "memory");
}

/* #NM: the running process wants the FPU. */
static void device_not_available(isr_frame_t *frame) {
    pcb_t *cur = get_current_process();

    (void)frame;
    asm volatile("clts");
    if (!cur || fpu_owner == cur) return;
    if (fpu_owner) {
        uint8_t *a = area_of(fpu_owner);
        if (a) { store(a); fpu_owner->fpu_valid = true; }
    }
    fpu_owner = cur;
    if (cur->fpu_valid) load(cur->fpu);
    else asm volatile("fninit");
}

void fpu_init(void) {
    uint32_t edx;
    uint32_t cr4;

    asm volatile("cpuid" : "=d"(edx) : "a"(1) : "ebx", "ecx");
    have_fxsr = (edx >> 24) & 1;
    write_cr0((read_cr0() & ~CR0_EM) | CR0_MP);
    if (have_fxsr) {
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= CR4_OSFXSR | (((edx >> 25) & 1) ? CR4_OSXMMEXCPT : 0);
        asm volatile("mov %0, %%cr4" : : "r"(cr4));
    }
    asm volatile("fninit");
    register_int_handler(NM_VECTOR, device_not_available, NULL);
}

void fpu_task_switch(const pcb_t *next) {
    if (next == fpu_owner) write_cr0(read_cr0() & ~CR0_TS);
    else set_ts();
}

bool fpu_save_current(void) {
    pcb_t *cur = get_current_process();

    if (fpu_owner == cur) {
        uint8_t *a = area_of(cur);
        if (!a) return false;
        store(a);
        cur->fpu_valid = true;
    }
    return cur->fpu_valid;
}

void fpu_restore_current(const uint8_t *area) {
    pcb_t *cur = get_current_process();
    uint8_t *a = area_of(cur);

    if (!a) return;
    memcpy(a, area, FPU_AREA_SIZE);
    cur->fpu_valid = true;
    if (fpu_owner == cur) load(a);
}

const uint8_t *fpu_area(const pcb_t *p) { return p->fpu; }

void fpu_fork(pcb_t *child, pcb_t *parent) {
    if (!fpu_save_current()) return;                /* the parent never used the FPU: the child starts clean */
    uint8_t *a = area_of(child);
    if (!a) return;
    memcpy(a, parent->fpu, FPU_AREA_SIZE);
    child->fpu_valid = true;
}

void fpu_exec_reset(pcb_t *p) {
    p->fpu_valid = false;
    if (fpu_owner == p) {
        fpu_owner = NULL;
        set_ts();                                   /* the next FPU instruction starts from fninit */
    }
}

void fpu_release(pcb_t *p) {
    if (fpu_owner == p) fpu_owner = NULL;
    kfree(p->fpu_raw);
    p->fpu_raw = p->fpu = NULL;
}
