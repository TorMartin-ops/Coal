/**
 * @file process.h
 * @brief Processes: PCB, fork/execve/exit/wait
 */
#ifndef PROCESS_H
#define PROCESS_H

#include <kernel/core/types.h>
#include <kernel/cpu/isr_frame.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/mm.h>
#include <kernel/cpu/vdso_abi.h>
#include <kernel/process/rlimit.h>
#include <kernel/process/scheduler.h>
#include <kernel/sync/spinlock.h>

struct mm_struct;
struct sys_file;

#define NSIG 64

#define MAX_FD              32
#define KSTACK_PAGES        4
#define PROCESS_KSTACK_SIZE (KSTACK_PAGES * PAGE_SIZE)

/* User address space layout (see paging.h and vdso_abi.h). The stack VMA grows down from the top. */
#define USER_STACK_TOP          VVAR_BASE
#define USER_STACK_BOTTOM       (USER_STACK_TOP - MM_STACK_MAX)
#define USER_ARG_MAX            (128 * 1024)

/* waitpid options */
#define WNOHANG 1

#define CWD_MAX 256

struct sigact { uintptr_t handler; uint64_t flags; uintptr_t restorer; uint64_t mask; };    /* = struct linux_sigaction */

typedef struct pcb {
    tcb_t task;                       /* scheduling state; task.process == this */

    uint32_t pid, ppid;
    uint32_t pgid, sid;
    struct pcb *parent, *children, *sibling;
    struct pcb *all_next;             /* list of every process */
    struct pcb *dead_next;            /* list of exited processes nobody waits for */
    int      exit_status;             /* Linux wait status encoding */
    bool     wait_blocked;            /* parent is blocked in process_waitpid() */
    bool     exiting;                 /* process_exit() has started */
    uint8_t  *fpu_raw, *fpu;          /* FPU save area (fpu is 16-byte aligned inside fpu_raw), see fpu.c */
    bool     fpu_valid;               /* the save area holds state to load on first FPU use */
    uint64_t pending_signals;
    uint64_t sig_blocked;             /* blocked signal mask (bit n-1 = signal n) */
    uint64_t sig_saved;               /* mask to restore when sig_restore is set (pselect6) */
    bool     sig_restore;
    struct sigact sigact[NSIG + 1];   /* indexed by signal number */

    struct rlim rlim[RL_COUNT];       /* resource limits, see rlimit.h */
    uint32_t cpu_ms, cpu_next;        /* CPU time used; the time at which rlimit_charge() has to act */
    uint32_t cpu_ns_rem;              /* nanoseconds below one millisecond of cpu_ms */
    uint32_t rl_hits;                 /* RL_HIT() mask of limits that stopped this process or a child */

    uint64_t fs_base, gs_base;        /* user FS and GS bases (arch_prctl), loaded on every switch */
    uintptr_t sigaltstack_sp;         /* sigaltstack: 0 when none */
    size_t   sigaltstack_size;
    char     cwd[CWD_MAX];
    uint32_t cloexec;                 /* bit n: descriptor n is closed by execve */

    struct mm_struct *mm;
    void    *page_directory_phys;     /* PML4, physical; NULL for kernel tasks: they run on any directory */

    struct sys_file *fd_table[MAX_FD];
    spinlock_t       fd_table_lock;

    char     name[16];
    char     cmdline[128];            /* argv, NUL separated (for /proc) */
    uint32_t cmdline_len;

    void    *kernel_stack_vaddr_top;
} pcb_t;

pcb_t *get_current_process(void);

/** The register frame the current task entered the kernel with (a system call or interrupt from user mode). */
static inline isr_frame_t *user_regs_of(pcb_t *p) { return (isr_frame_t *)p->kernel_stack_vaddr_top - 1; }
pcb_t *process_find(uint32_t pid);
pcb_t *process_all(void);           /* head of the list linked by all_next */

/** Loads path as a new process (argv = {path}). The caller schedules it. */
pcb_t *create_user_process(const char *path);

/** Starts a kernel thread: a process without user space that runs entry() (which must not return). */
pcb_t *process_spawn_kernel(const char *name, void (*entry)(void));

/**
 * Linux fork(): duplicates the address space (copy-on-write through
 * paging_clone_directory) and shares the open files. The child resumes from
 * the same frame with rax = 0.
 * @return child pid, or -errno
 */
int process_fork(const isr_frame_t *regs);

/**
 * Linux execve() for kernel-resident argv/envp (NULL-terminated). On success
 * rewrites *regs to enter the new image; on failure the old image is intact.
 * @return 0 or -errno
 */
int process_execve(const char *path, const char *const *argv, const char *const *envp,
                   isr_frame_t *regs);

/** Terminates the current process. status is a wait status (see PROC_EXITCODE). */
void process_exit(int status) __attribute__((noreturn));
#define PROC_EXITCODE(code) (((code) & 0xff) << 8)

/**
 * Linux wait4() semantics for pid > 0, -1, 0 (own group) and < -1 (group).
 * Blocks unless WNOHANG. Stores the wait status in *status if non-NULL.
 * @return reaped pid, 0 (WNOHANG, nothing ready) or -errno
 */
int process_waitpid(int pid, int *status, int options);

/** Frees the PCB and kernel stack of tasks that were never waited for. */
void process_reap_orphans(void);

int process_setsid(pcb_t *proc);
int process_setpgid(pcb_t *proc, uint32_t pgid);

/** Sends sig (1..31, or 0 to probe) to pid, or a group for pid <= 0. Default actions only. */
int process_kill(int pid, int sig);

#endif
