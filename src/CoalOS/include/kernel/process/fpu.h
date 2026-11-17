/**
 * @file fpu.h
 * @brief x87 and SSE state per process, switched lazily with CR0.TS and the #NM trap.
 */
#ifndef FPU_H
#define FPU_H

#include <kernel/process/process.h>

#define FPU_AREA_SIZE 512       /* FXSAVE image */

/** Enables SSE for user code and installs the #NM handler. Call once during boot. */
void fpu_init(void);
/** Called on every task switch: the FPU holds the state of another task, so the first use must trap. */
void fpu_task_switch(const pcb_t *next);
/** Copies the live state of the current process into its save area; false if it never used the FPU. */
bool fpu_save_current(void);
/** Loads the current process's FPU state from area (a signal handler returning). */
void fpu_restore_current(const uint8_t *area);
/** The save area of p (FPU_AREA_SIZE bytes), valid after fpu_save_current() returned true. */
const uint8_t *fpu_area(const pcb_t *p);
/** fork: the child starts with a copy of the parent's FPU state. */
void fpu_fork(pcb_t *child, pcb_t *parent);
/** execve: the new program starts with a clean FPU. */
void fpu_exec_reset(pcb_t *p);
/** Frees p's save area and forgets it as FPU owner. */
void fpu_release(pcb_t *p);

#endif
