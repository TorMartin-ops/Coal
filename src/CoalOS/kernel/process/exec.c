/**
 * Program images: builds an address space from an ELF file plus the initial
 * user stack laid out like Linux i386 (argc, argv, envp, auxv, strings), and
 * implements execve on top of it.
 */
#include <kernel/process/exec.h>
#include <kernel/process/oom.h>
#include <kernel/process/elf_loader.h>
#include <kernel/process/fpu.h>
#include <kernel/process/vdso.h>
#include <kernel/cpu/vdso_abi.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/fs/vfs/read_file.h>
#include <kernel/lib/string.h>
#include <kernel/memory/frame.h>
#include <kernel/memory/kmap.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/paging_process.h>
#include <kernel/lib/random.h>

/* Linux auxiliary vector types */
#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_EXECFN 31
#define AT_SYSINFO 32
#define AT_SYSINFO_EHDR 33

#define NAUX        20          /* pairs, including AT_NULL */
#define MAX_ARGS    4096

static uint32_t nx_flag(void) {
    return g_nx_supported ? PAGE_NX_BIT : 0;
}

static int new_address_space(mm_struct_t **out) {
    uintptr_t pd = paging_new_directory();
    mm_struct_t *mm;

    if (!pd) return -ENOMEM;
    mm = create_mm((uint32_t *)pd);
    if (!mm) { put_frame(pd); return -ENOMEM; }
    *out = mm;
    return 0;
}

static void free_mm(mm_struct_t *mm) {
    uint32_t *pd = mm->pgd_phys;

    destroy_mm(mm);                      /* releases the user pages and page tables */
    put_frame((uintptr_t)pd);
}

void exec_free_mm(mm_struct_t *mm) {
    oom_teardown_begin();
    free_mm(mm);
    oom_teardown_end();
}

void exec_release_mm(pcb_t *p) {
    mm_struct_t *mm = p->mm;

    if (!mm) return;
    asm volatile("mov %0, %%cr3" : : "r"(g_kernel_page_directory_phys) : "memory");
    oom_teardown_begin();                /* before p->mm goes: the OOM killer must not think the memory is back */
    p->mm = NULL;
    p->page_directory_phys = NULL;
    free_mm(mm);
    oom_teardown_end();
}

/**
 * Builds the Linux i386 initial stack (low to high): argc, argv[], NULL,
 * envp[], NULL, auxv[], padding, AT_PLATFORM/AT_RANDOM data, strings, and a
 * zero word at the very top. The image is assembled in a kernel buffer and
 * then copied into freshly mapped user pages.
 */
static int build_stack(mm_struct_t *mm, const char *path, const char *const *argv,
                       const char *const *envp, const elf_info_t *ei, uint32_t *sp_out) {
    static const char platform[] = "i686";
    size_t argc = 0, envc = 0, strsz, vecsz, size, off, vo;
    uint32_t base, *vec, hwcap, a = 0, execfn, plat, rnd;
    uint8_t *buf;
    int ret = 0;

    while (argv && argv[argc]) if (++argc > MAX_ARGS) return -E2BIG;
    while (envp && envp[envc]) if (++envc > MAX_ARGS) return -E2BIG;
    strsz = 4 + strlen(path) + 1 + sizeof(platform) + 16;
    for (size_t i = 0; i < argc; i++) strsz += strlen(argv[i]) + 1;
    for (size_t i = 0; i < envc; i++) strsz += strlen(envp[i]) + 1;
    if (strsz > USER_ARG_MAX) return -E2BIG;

    vecsz = (1 + argc + 1 + envc + 1 + 2 * NAUX) * sizeof(uint32_t);
    size = PAGE_ALIGN_UP(strsz + vecsz + 32);
    base = USER_STACK_TOP_VIRT_ADDR - size;
    buf = kmalloc(size);
    vec = kmalloc(vecsz);
    if (!buf || !vec) { ret = -ENOMEM; goto out; }
    memset(buf, 0, size);
    memset(vec, 0, vecsz);

    off = size - 4;                                       /* zero word at the top */
#define PUT(s) ({ size_t l_ = strlen(s) + 1; off -= l_; memcpy(buf + off, (s), l_); base + off; })
    execfn = PUT(path);
    for (size_t i = envc; i-- > 0; ) vec[2 + argc + i] = PUT(envp[i]);
    for (size_t i = argc; i-- > 0; ) vec[1 + i] = PUT(argv[i]);
    off -= sizeof(platform); memcpy(buf + off, platform, sizeof(platform)); plat = base + off;
    off -= 16; random_bytes(buf + off, 16); rnd = base + off;
#undef PUT
    off &= ~(size_t)15;

    vec[0] = argc;
    asm volatile("cpuid" : "=d"(hwcap) : "a"(1) : "ebx", "ecx");
    uint32_t *aux = &vec[1 + argc + 1 + envc + 1];
#define AUX(t, v) do { aux[a++] = (t); aux[a++] = (v); } while (0)
    AUX(AT_PHDR, ei->phdr); AUX(AT_PHENT, ei->phent); AUX(AT_PHNUM, ei->phnum);
    AUX(AT_PAGESZ, PAGE_SIZE); AUX(AT_BASE, 0); AUX(AT_FLAGS, 0); AUX(AT_ENTRY, ei->entry);
    AUX(AT_UID, 0); AUX(AT_EUID, 0); AUX(AT_GID, 0); AUX(AT_EGID, 0);
    AUX(AT_HWCAP, hwcap); AUX(AT_CLKTCK, 100); AUX(AT_SECURE, 0);
    AUX(AT_RANDOM, rnd); AUX(AT_PLATFORM, plat); AUX(AT_EXECFN, execfn);
    AUX(AT_SYSINFO, VDSO_VSYSCALL); AUX(AT_SYSINFO_EHDR, VDSO_BASE); AUX(AT_NULL, 0);
#undef AUX

    vo = (off - vecsz) & ~(size_t)15;                     /* argc sits on a 16-byte boundary */
    memcpy(buf + vo, vec, vecsz);
    *sp_out = base + vo;

    for (size_t pg = 0; pg < size; pg += PAGE_SIZE) {
        uintptr_t phys = frame_alloc_user();
        void *v;
        if (!phys) { ret = -ENOMEM; goto out; }
        v = kmap(phys);
        memcpy(v, buf + pg, PAGE_SIZE);
        kunmap(v);
        if (paging_map_single_4k(mm->pgd_phys, base + pg, phys,
                                 PAGE_PRESENT | PAGE_RW | PAGE_USER | nx_flag())) {
            put_frame(phys);
            ret = -ENOMEM;
            goto out;
        }
    }
out:
    kfree(buf);
    kfree(vec);
    return ret;
}

#define SHEBANG_MAX   255       /* longest "#!" line, like Linux BINPRM_BUF_SIZE - 1 */
#define SHEBANG_DEPTH 4

static int build_image(const char *path, const char *const *argv, const char *const *envp,
                       exec_image_t *img, int depth);

/* "#!interp [arg]": runs interp with the optional single argument, the script path and the old arguments. */
static int exec_script(const char *path, const void *file, size_t fsize, const char *const *argv,
                       const char *const *envp, exec_image_t *img, int depth) {
    char line[SHEBANG_MAX + 1], *interp, *arg = NULL, *end;
    const char **nargv;
    size_t n = fsize < SHEBANG_MAX ? fsize : SHEBANG_MAX, argc = 0, k = 0;
    int ret;

    if (depth >= SHEBANG_DEPTH) return -ELOOP;
    memcpy(line, file, n);
    line[n] = 0;
    for (end = line; *end && *end != '\n'; end++) { }
    if (!*end && fsize > SHEBANG_MAX) return -ENOEXEC;       /* line too long */
    while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    *end = 0;
    for (interp = line + 2; *interp == ' ' || *interp == '\t'; interp++) { }
    if (!*interp) return -ENOEXEC;
    for (char *c = interp; *c; c++) {
        if (*c != ' ' && *c != '\t') continue;
        *c++ = 0;
        while (*c == ' ' || *c == '\t') c++;
        if (*c) arg = c;                                    /* the rest of the line is one argument */
        break;
    }
    while (argv && argv[argc]) argc++;
    nargv = kmalloc((argc + 4) * sizeof(*nargv));
    if (!nargv) return -ENOMEM;
    nargv[k++] = interp;
    if (arg) nargv[k++] = arg;
    nargv[k++] = path;
    for (size_t i = 1; i < argc; i++) nargv[k++] = argv[i];
    nargv[k] = NULL;
    ret = build_image(interp, nargv, envp, img, depth + 1);
    kfree(nargv);
    return ret;
}

static int build_image(const char *path, const char *const *argv, const char *const *envp,
                       exec_image_t *img, int depth) {
    mm_struct_t *mm = NULL;
    elf_info_t ei;
    size_t fsize = 0;
    void *file = read_file(path, &fsize);
    int ret;

    if (!file) return -ENOENT;
    if (fsize >= 2 && ((char *)file)[0] == '#' && ((char *)file)[1] == '!') {
        ret = exec_script(path, file, fsize, argv, envp, img, depth);
        kfree(file);
        return ret;
    }
    ret = new_address_space(&mm);
    if (ret) goto out;
    ret = elf_load(mm, file, fsize, &ei);
    if (ret) goto fail;

    mm->start_brk = mm->end_brk = ei.brk;
    mm->start_stack = USER_STACK_TOP_VIRT_ADDR;
    if (!insert_vma(mm, USER_STACK_BOTTOM_VIRT, USER_STACK_TOP_VIRT_ADDR,
                    VM_READ | VM_WRITE | VM_USER | VM_GROWS_DOWN | VM_ANONYMOUS)) {
        ret = -ENOMEM;
        goto fail;
    }
    ret = build_stack(mm, path, argv, envp, &ei, &img->sp);
    if (!ret) ret = vdso_map(mm);
    if (ret) goto fail;
    img->mm = mm;
    img->entry = ei.entry;
    goto out;
fail:
    exec_free_mm(mm);
out:
    kfree(file);
    return ret;
}

int exec_build_image(const char *path, const char *const *argv, const char *const *envp,
                     exec_image_t *img) {
    return build_image(path, argv, envp, img, 0);
}

void exec_set_name(pcb_t *p, const char *path, const char *const *argv) {
    const char *base = path;
    size_t n = 0;

    for (const char *c = path; *c; c++) if (*c == '/') base = c + 1;
    strncpy(p->name, base, sizeof(p->name) - 1);
    p->name[sizeof(p->name) - 1] = 0;
    for (size_t i = 0; argv && argv[i]; i++) {
        size_t l = strlen(argv[i]) + 1;
        if (n + l > sizeof(p->cmdline)) break;
        memcpy(p->cmdline + n, argv[i], l);
        n += l;
    }
    p->cmdline_len = n;
}

int process_execve(const char *path, const char *const *argv, const char *const *envp,
                   isr_frame_t *regs) {
    pcb_t *p = get_current_process();
    exec_image_t img;
    mm_struct_t *old;
    uintptr_t flags;
    int ret;

    if (!p) return -ESRCH;
    ret = exec_build_image(path, argv, envp, &img);
    if (ret) return ret;

    flags = local_irq_save();
    old = p->mm;
    p->mm = img.mm;
    p->page_directory_phys = img.mm->pgd_phys;
    asm volatile("mov %0, %%cr3" : : "r"(img.mm->pgd_phys) : "memory");
    local_irq_restore(flags);

    if (old) exec_free_mm(old);
    fpu_exec_reset(p);
    exec_set_name(p, path, argv);
    process_init_user_frame(regs, img.entry, img.sp);
    return 0;
}
