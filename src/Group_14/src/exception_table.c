#include "exception_table.h"
// #include "terminal.h" // Optional for debugging

/**
 * @brief Finds the fixup address for a given faulting instruction address.
 * Iterates through a linker-defined exception table.
 */
uint32_t find_exception_fixup(uint32_t fault_eip) {
    // Iterate through the table provided by the linker
    for (exception_entry_t *entry = __start_ex_table; entry < __stop_ex_table; ++entry) {
        if (entry->fault_addr == fault_eip) {
            // terminal_printf("[EX TABLE] Fixup found for EIP 0x%x -> 0x%x\n", fault_eip, entry->fixup_addr);
            return entry->fixup_addr;
        }
    }
    // terminal_printf("[EX TABLE] Fixup not found for EIP 0x%x\n", fault_eip);
    return 0; // Indicate not found
}