/*
 * Time calls through the vDSO: the kernel maps an ELF image at AT_SYSINFO_EHDR with
 * __vdso_clock_gettime, __vdso_gettimeofday and __vdso_time, which read the time page without a
 * system call. The symbols are found the way vdso(7) describes: through the dynamic section.
 * Without a vDSO, or without the symbol, the calls fall back to the system call.
 */
#include <libc/syscall.h>
#include <libc/string.h>
#include <libc/time.h>
#include <libc/sys/time.h>
#include <libc/stdint.h>

extern void *__vdso_base;

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define DT_HASH    4
#define DT_STRTAB  5
#define DT_SYMTAB  6

struct phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; };
struct sym  { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; };

static void *vdso_sym(const char *name, void *fallback) {
    const uint8_t *eh = __vdso_base;
    const struct phdr *ph;
    const uint64_t *dyn = NULL;
    const uint32_t *hash = NULL;
    const struct sym *syms = NULL;
    const char *strs = NULL;
    uintptr_t off = 0;

    if (!eh || memcmp(eh, "\177ELF\2", 5)) return fallback;
    ph = (const struct phdr *)(eh + *(const uint64_t *)(eh + 32));          /* e_phoff */
    for (unsigned i = 0; i < *(const uint16_t *)(eh + 56); i++) {           /* e_phnum */
        if (ph[i].type == PT_LOAD) off = (uintptr_t)eh + ph[i].offset - ph[i].vaddr;
        else if (ph[i].type == PT_DYNAMIC) dyn = (const uint64_t *)(eh + ph[i].offset);
    }
    for (; dyn && dyn[0]; dyn += 2) {
        if (dyn[0] == DT_HASH) hash = (const uint32_t *)(dyn[1] + off);
        else if (dyn[0] == DT_STRTAB) strs = (const char *)(dyn[1] + off);
        else if (dyn[0] == DT_SYMTAB) syms = (const struct sym *)(dyn[1] + off);
    }
    if (!hash || !strs || !syms) return fallback;
    for (uint32_t i = 1; i < hash[1]; i++)                                   /* hash[1] = nchain = symbol count */
        if (syms[i].shndx && !strcmp(strs + syms[i].name, name)) return (void *)(syms[i].value + off);
    return fallback;
}

static int sys_clock_gettime(clockid_t clk, struct timespec *ts) { return (int)__SC3(SYS_CLOCK_GETTIME, clk, ts, 0); }
static int sys_gettimeofday(struct timeval *tv, void *tz)        { return (int)__SC3(SYS_GETTIMEOFDAY, tv, tz, 0); }
static time_t sys_time(time_t *t)                                { return __SC1(SYS_TIME, t); }

int clock_gettime(clockid_t clk, struct timespec *ts) {
    static int (*fn)(clockid_t, struct timespec *);

    if (!fn) fn = vdso_sym("__vdso_clock_gettime", sys_clock_gettime);
    return (int)__syscall_ret(fn(clk, ts));
}

int gettimeofday(struct timeval *tv, void *tz) {
    static int (*fn)(struct timeval *, void *);

    if (!fn) fn = vdso_sym("__vdso_gettimeofday", sys_gettimeofday);
    return (int)__syscall_ret(fn(tv, tz));
}

time_t time(time_t *t) {
    static time_t (*fn)(time_t *);

    if (!fn) fn = vdso_sym("__vdso_time", sys_time);
    return (time_t)__syscall_ret(fn(t));
}
