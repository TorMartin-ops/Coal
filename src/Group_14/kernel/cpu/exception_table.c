/**
 * @file exception_table.c
 * @brief Exception Table Lookup Implementation
 *
 * Provides the function to search the kernel's exception table, which is used
 * by the page fault handler to determine if a fault during kernel execution
 * (specifically when accessing user memory) is expected and has a defined
 * recovery path.
 */

 #include <kernel/cpu/exception_table.h>
 #include <kernel/lib/assert.h> // For KERNEL_ASSERT
 #include <kernel/core/debug.h>  // For DEBUG_PRINTK (optional)
 
 // --- Debug Configuration ---
 #define DEBUG_EX_TABLE 0 // Set to 1 to enable exception table debug messages
 
 #if DEBUG_EX_TABLE
 #define EXTABLE_DEBUG_PRINTK(fmt, ...) DEBUG_PRINTK("[ExTable] " fmt, ##__VA_ARGS__)
 #else
 #define EXTABLE_DEBUG_PRINTK(fmt, ...) ((void)0)
 #endif
 
 
 /**
  * @brief Finds the fixup address corresponding to a faulting kernel instruction address.
  *
  * Iterates through the exception table defined by the linker symbols
  * `__start_ex_table` and `__stop_ex_table`.
  *
  * @param fault_eip The EIP where the kernel fault occurred. Must not be 0.
  * @return The corresponding fixup address if found, 0 otherwise.
  */
 uint32_t find_exception_fixup(uint32_t fault_eip) {
     KERNEL_ASSERT(fault_eip != 0, "find_exception_fixup called with fault_eip=0");
 
     EXTABLE_DEBUG_PRINTK("Searching fixup for fault_eip=0x%x in table [0x%x - 0x%x)\n",
                          fault_eip, __start_ex_table, __stop_ex_table);
 
     // Iterate through the table using the linker-defined symbols.
     // The symbols refer to the start and end addresses of the array of entries.
     for (exception_entry_t *entry = __start_ex_table; entry < __stop_ex_table; ++entry) {
         if (entry->fault_addr == fault_eip) {
             EXTABLE_DEBUG_PRINTK(" -> Found entry: fault=0x%x -> fixup=0x%x\n",
                                  entry->fault_addr, entry->fixup_addr);
             KERNEL_ASSERT(entry->fixup_addr != 0, "Exception table entry has NULL fixup address!");
             return entry->fixup_addr; // Return the handler address
         }
     }
 
     EXTABLE_DEBUG_PRINTK(" -> Fixup not found.\n");
     return 0; // Indicate faulting address not found in the table
 }