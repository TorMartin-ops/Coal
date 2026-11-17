/**
 * @file exception_table.c
 * @brief Lookup in the exception table: kernel instructions that may fault on
 *        user memory (copy_from_user and friends) and where to resume.
 */
#include <kernel/cpu/exception_table.h>

uint64_t find_exception_fixup(uint64_t fault_rip) {
    for (exception_entry_t *e = __start_ex_table; e < __stop_ex_table; e++)
        if (e->fault_addr == fault_rip) return e->fixup_addr;
    return 0;
}
