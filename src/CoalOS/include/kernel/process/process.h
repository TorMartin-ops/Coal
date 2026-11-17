/**
 * @file process.h
 * @brief Processes: PCB, fork/execve/exit/wait
 */
#ifndef PROCESS_H
#define PROCESS_H

#include <kernel/core/types.h>
#include <kernel/cpu/isr_frame.h>
#include <kernel/memory/paging.h>
#include <kernel/process/scheduler.h>
#include <kernel/sync/spinlock.h>

struct mm_struct;
struct sys_file;

#define MAX_FD              32
#define KSTACK_PAGES        4
#define PROCESS_KSTACK_SIZE (KSTACK_PAGES * PAGE_SIZE)

/* User address space layout. The stack VMA grows down from the top. */
#define USER_SPACE_MIN          0x00400000u   /* PDE 0 is the shared kernel identity map */
#define USER_STACK_TOP_VIRT_ADDR 0xBFFF0000u
#define USER_STACK_BOTTOM_VIRT   0xBF000000u
#define USER_ARG_MAX            (128 * 1024)

/* waitpid options */
#define WNOHANG 1

#define CWD_MAX 256

struct sigact { uint32_t handler, flags, restorer, mask; };

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
    uint32_t pending_signals;
    uint32_t sig_blocked;             /* blocked signal mask (bit n = signal n) */
    uint32_t sig_saved;               /* mask to restore when sig_restore is set (pselect6) */
    bool     sig_restore;
    struct sigact sigact[32];         /* NSIG entries */

    uint32_t tls_base, tls_pages;     /* set_thread_area descriptor, loaded on every switch */
    char     cwd[CWD_MAX];
    uint32_t cloexec;                 /* bit n: descriptor n is closed by execve */

    struct mm_struct *mm;
    uint32_t *page_directory_phys;    /* 0 for the idle task: runs on any directory */

    struct sys_file *fd_table[MAX_FD];
    spinlock_t       fd_table_lock;

    char     name[16];
    char     cmdline[128];            /* argv, NUL separated (for /proc) */
    uint32_t cmdline_len;

    uint32_t *kernel_stack_vaddr_top;
} pcb_t;

pcb_t *get_current_process(void);
pcb_t *process_find(uint32_t pid);
pcb_t *process_all(void);           /* head of the list linked by all_next */

/** Loads path as a new process (argv = {path}). The caller schedules it. */
pcb_t *create_user_process(const char *path);

/** Starts a kernel thread: a process without user space that runs entry() (which must not return). */
pcb_t *process_spawn_kernel(const char *name, void (*entry)(void));

/**
 * Linux fork(): duplicates the address space (copy-on-write through
 * paging_clone_directory) and shares the open files. The child resumes from
 * the same frame with eax = 0.
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
