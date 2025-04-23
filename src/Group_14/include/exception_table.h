/**
 * @file exception_table.h
 * @brief Kernel Exception Table Definitions
 *
 * Defines the structure for entries in the kernel's exception table and
 * declares the linker symbols that mark the table's boundaries. This table
 * allows the kernel to gracefully handle expected page faults when accessing
 * user memory via functions like copy_from_user/copy_to_user.
 */

 #ifndef EXCEPTION_TABLE_H
 #define EXCEPTION_TABLE_H
 
 #include <libc/stdint.h> // For uint32_t definition
 
 /**
  * @brief Defines a single entry in the kernel's exception table.
  * Each entry maps a specific kernel instruction address, which is known
  * to potentially fault when accessing user memory (e.g., a MOV instruction
  * in copy_from_user), to a "fixup" address within the same function.
  * If a page fault occurs at `fault_addr`, the page fault handler should
  * look up this table. If found, it modifies the EIP on the fault stack frame
  * to point to `fixup_addr` and returns via `iret`, allowing the function
  * to handle the fault (e.g., by returning an error code) instead of crashing.
  */
 typedef struct {
     uint32_t fault_addr;  /**< Address of the kernel instruction *allowed* to fault (EIP). */
     uint32_t fixup_addr;  /**< Address to jump to (via modified IRET) if a fault occurs at fault_addr. */
 } exception_entry_t;
 
 /**
  * @brief Linker-defined symbol marking the start of the exception table.
  * The table consists of an array of `exception_entry_t` structures.
  * Declared as an array for type safety and easier iteration.
  */
 extern exception_entry_t __start_ex_table[];
 
 /**
  * @brief Linker-defined symbol marking the end (one past the last element) of the exception table.
  */
 extern exception_entry_t __stop_ex_table[];
 
 
 /**
  * @brief Finds the fixup address corresponding to a faulting kernel instruction address.
  *
  * This function iterates through the exception table (between `__start_ex_table`
  * and `__stop_ex_table`) searching for an entry whose `fault_addr` matches the
  * provided `fault_eip`.
  *
  * @param fault_eip The EIP (instruction pointer) where the kernel page fault occurred.
  * @return The corresponding `fixup_addr` if an entry is found.
  * @return 0 if no matching entry is found in the exception table, indicating
  * the fault was unexpected or occurred outside a designated safe region.
  */
 uint32_t find_exception_fixup(uint32_t fault_eip);
 
 
 #endif // EXCEPTION_TABLE_H