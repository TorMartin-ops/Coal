/**
 * @file exception_table.h
 * @brief Exception table: a faulting kernel instruction that is allowed to
 *        fault (a user memory access) maps to a fixup address in the same function.
 */
#ifndef EXCEPTION_TABLE_H
#define EXCEPTION_TABLE_H

#include <libc/stdint.h>

typedef struct {
    uint32_t fault_addr;    /* EIP of the instruction allowed to fault */
    uint32_t fixup_addr;    /* where the page fault handler resumes */
} exception_entry_t;

/* Bounds of the table, defined by the linker script. */
extern exception_entry_t __start_ex_table[];
extern exception_entry_t __stop_ex_table[];

/** Fixup address for a fault at fault_eip, or 0 if the fault is not expected. */
uint32_t find_exception_fixup(uint32_t fault_eip);

#endif
