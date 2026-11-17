/**
 * @file barrier.h
 * @brief Memory barriers for rings shared with devices.
 *
 * x86 keeps loads and stores in order except for a store followed by a load, so
 * only mb() needs an instruction; a locked add works on every CPU, SSE2 or not.
 */
#ifndef KERNEL_LIB_BARRIER_H
#define KERNEL_LIB_BARRIER_H

#define barrier() asm volatile("" ::: "memory")
#define wmb()     barrier()
#define rmb()     barrier()
#define mb()      asm volatile("lock; addl $0, 0(%%esp)" ::: "memory", "cc")

#endif
