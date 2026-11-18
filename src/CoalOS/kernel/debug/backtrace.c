/**
 * @file backtrace.c
 * @brief Frame-pointer backtraces and the embedded kernel symbol table.
 *
 * The debug kernel keeps frame pointers, so the chain rbp -> saved rbp / return address is walked. Only
 * addresses on a task's kernel stack or in the kernel's own data (boot and idle stacks) are followed, and the
 * walk stops at a guard page, a pointer that does not move up, or a return address outside the kernel text.
 * Names come from the table scripts/gen_ksyms.py embeds (functions only, offsets from the start of the text).
 */
#define LOG_MODULE "ksan"

#include <kernel/debug/ksan.h>
#include <kernel/cpu/isr_frame.h>
#include <kernel/core/log.h>
#include <kernel/memory/paging.h>
#include <kernel/process/process.h>

extern const uint32_t ksym_count, ksym_addr[], ksym_size[], ksym_name[];
extern const char ksym_names[];
extern char _kernel_text_start[], _kernel_text_end[], _kernel_data_start[], _kernel_data_end[];

#define KSTACK_SLOT_BYTES ((KSTACK_PAGES + 1) * PAGE_SIZE)

const char *ksym_lookup(uintptr_t addr, uintptr_t *offset, uintptr_t *size) {
    uintptr_t base = (uintptr_t)_kernel_text_start;
    uint32_t lo = 0, hi = ksym_count;

    if (addr < base || addr >= (uintptr_t)_kernel_text_end || !ksym_count)
        return NULL;
    addr -= base;
    while (hi - lo > 1) {                       /* last symbol whose address is <= addr */
        uint32_t mid = (lo + hi) / 2;
        if (ksym_addr[mid] <= addr)
            lo = mid;
        else
            hi = mid;
    }
    if (ksym_addr[lo] > addr)
        return NULL;
    *offset = addr - ksym_addr[lo];
    *size = ksym_size[lo];
    return &ksym_names[ksym_name[lo]];
}

static bool in_text(uintptr_t a) {
    return a >= (uintptr_t)_kernel_text_start && a < (uintptr_t)_kernel_text_end;
}

/* A frame pointer worth dereferencing: 8-aligned, on a task kernel stack outside its guard page, or in kernel data. */
static bool fp_ok(uintptr_t fp) {
    if (fp & 7)
        return false;
    if (fp >= KERNEL_STACK_VADDR_START && fp + 16 <= KERNEL_STACK_VADDR_END)
        return (fp - KERNEL_STACK_VADDR_START) % KSTACK_SLOT_BYTES >= PAGE_SIZE;
    return fp >= (uintptr_t)_kernel_data_start && fp + 16 <= (uintptr_t)_kernel_data_end;
}

static unsigned walk(uintptr_t fp, uintptr_t *bt, unsigned max, unsigned skip) {
    unsigned n = 0;

    for (unsigned depth = 0; fp_ok(fp) && n < max && depth < 32; depth++) {
        uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

        if (!in_text(ret))
            break;
        if (skip)
            skip--;
        else
            bt[n++] = ret;
        if (next <= fp || next - fp > 0x4000)
            break;
        fp = next;
    }
    return n;
}

unsigned ksan_capture(uintptr_t *bt, unsigned max) {
    /* Frame 0 is this function, its saved return address leads into the caller, which we skip. */
    return walk((uintptr_t)__builtin_frame_address(0), bt, max, 1);
}

static void print_one(unsigned i, uintptr_t a) {
    uintptr_t off, size;
    const char *name = ksym_lookup(a, &off, &size);

    if (name)
        LOG_WARN("  #%u [<%016lx>] %s+%#lx/%#lx", i, (unsigned long)a, name, (unsigned long)off, (unsigned long)size);
    else
        LOG_WARN("  #%u [<%016lx>] ?", i, (unsigned long)a);
}

void ksan_backtrace_buf(const uintptr_t *bt, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        print_one(i, bt[i]);
}

void ksan_backtrace(void) {
    uintptr_t bt[16];
    unsigned n = walk((uintptr_t)__builtin_frame_address(0), bt, 16, 1);     /* skip the reporter itself */

    ksan_backtrace_buf(bt, n);
}

void ksan_backtrace_frame(const isr_frame_t *f) {
    uintptr_t bt[16];
    unsigned n = 0;

    if (in_text(f->rip))
        bt[n++] = f->rip;
    n += walk(f->rbp, bt + n, 16 - n, 0);
    ksan_backtrace_buf(bt, n);
}
