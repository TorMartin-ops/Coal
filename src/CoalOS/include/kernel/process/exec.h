/**
 * @file exec.h
 * @brief Program image construction shared by create_user_process and execve
 *        (internal to kernel/process)
 */
#ifndef PROCESS_EXEC_H
#define PROCESS_EXEC_H

#include <kernel/process/process.h>
#include <kernel/memory/mm.h>

typedef struct {
    mm_struct_t *mm;     /* new address space, fully populated */
    uint32_t entry;
    uint32_t sp;         /* initial user stack pointer (points at argc) */
} exec_image_t;

/** Reads path, builds an address space with the segments, heap, and Linux i386 stack. */
int exec_build_image(const char *path, const char *const *argv, const char *const *envp,
                     exec_image_t *img);

/** Frees every user page, the VMAs and the page directory of mm. mm must not be active. */
void exec_free_mm(mm_struct_t *mm);

/** Releases p's address space; switches to the kernel page directory first. */
void exec_release_mm(pcb_t *p);

/** Records the process name (basename of path) and argv for /proc. */
void exec_set_name(pcb_t *p, const char *path, const char *const *argv);

/* process.c helpers */
void process_kstack_init(void);
isr_frame_t *process_user_frame(pcb_t *p);
void process_set_entry_frame(pcb_t *p);
void process_init_user_frame(isr_frame_t *f, uint32_t entry, uint32_t sp);

#endif
