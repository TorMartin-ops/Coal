/**
 * @file syscall_state.c
 * @brief Per-process state kept by the system call layer: cwd, close-on-exec
 *        bits, signal handlers and mask. Looked up by pid; entries of
 *        processes that no longer exist are recycled when a new one is made.
 */

#include "syscall_internal.h"
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>

static struct pstate *states;

struct pstate *pstate_find(uint32_t pid) {
    for (struct pstate *s = states; s; s = s->next)
        if (s->pid == pid) return s;
    return NULL;
}

/* Fresh entry for pid: reuse a slot whose process is gone, else allocate. */
static struct pstate *pstate_new(uint32_t pid) {
    struct pstate *s = pstate_find(pid);

    for (struct pstate *t = states; !s && t; t = t->next)
        if (!process_find(t->pid)) s = t;
    if (!s) {
        s = kmalloc(sizeof(*s));
        if (!s) return NULL;
        s->next = states;
        states = s;
    }
    struct pstate *next = s->next;
    memset(s, 0, sizeof(*s));
    s->next = next;
    s->pid = pid;
    strcpy(s->cwd, "/");
    return s;
}

struct pstate *pstate_current(void) {
    pcb_t *p = get_current_process();
    struct pstate *s = pstate_find(p->pid);
    return s ? s : pstate_new(p->pid);
}

void pstate_fork(uint32_t child_pid) {
    struct pstate *parent = pstate_current();
    struct pstate *child = pstate_new(child_pid);

    if (!child || !parent) return;
    strcpy(child->cwd, parent->cwd);
    child->cloexec = parent->cloexec;
    child->blocked = parent->blocked;
    memcpy(child->act, parent->act, sizeof(child->act));
}

void pstate_exec(struct pstate *ps) {
    memset(ps->act, 0, sizeof(ps->act));            /* handlers do not survive execve */
    for (int fd = 0; fd < MAX_FD; fd++)
        if (ps->cloexec & (1u << fd)) sc_close(fd);
    ps->cloexec = 0;
}
