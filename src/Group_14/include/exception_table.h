#ifndef EXCEPTION_TABLE_H
#define EXCEPTION_TABLE_H

#include <libc/stdint.h> // Make sure this defines uint32_t

/**
 * @brief Defines an entry in the kernel's exception table.
 * This table maps kernel instruction addresses that are allowed to fault
 * when accessing user memory to corresponding "fixup" handler addresses.
 */
typedef struct {
    uint32_t fault_addr;  // Address of the instruction *allowed* to fault (EIP)
    uint32_t fixup_addr;  // Address to jump to *after* the fault occurs
} exception_entry_t;

/**
 * @brief Finds the fixup address for a given faulting instruction address.
 * This function iterates through the exception table defined by the linker.
 *
 * @param fault_eip The EIP where the kernel fault occurred.
 * @return The corresponding fixup address if found, 0 otherwise.
 */
uint32_t find_exception_fixup(uint32_t fault_eip);

/**
 * @brief Linker-defined symbols marking the start and end of the exception table.
 * The linker script must define these symbols around the .ex_table section.
 */
extern exception_entry_t __start_ex_table[];
extern exception_entry_t __stop_ex_table[];

#endif // EXCEPTION_TABLE_H