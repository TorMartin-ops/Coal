/**
 * @file paging_fault.h
 * @brief Page fault handling
 */

#ifndef COAL_MEMORY_PAGING_FAULT_H
#define COAL_MEMORY_PAGING_FAULT_H

#include <kernel/core/types.h>
#include <kernel/memory/paging.h>

/**
 * @brief Page fault error code bits
 */
#define PAGE_FAULT_PRESENT  (1 << 0)  // 0=Not Present, 1=Protection Violation
#define PAGE_FAULT_WRITE    (1 << 1)  // 0=Read, 1=Write
#define PAGE_FAULT_USER     (1 << 2)  // 0=Supervisor, 1=User
#define PAGE_FAULT_RESERVED (1 << 3)  // Reserved bit violation
#define PAGE_FAULT_FETCH    (1 << 4)  // Instruction fetch

/**
 * @brief Page fault handler
 * @param regs Register state at time of fault
 */
void page_fault_handler(registers_t* regs);

/**
 * @brief Get page fault statistics
 * @param total_faults Output total number of page faults
 * @param handled_faults Output number of successfully handled faults
 * @param fatal_faults Output number of fatal page faults
 */
void page_fault_get_stats(uint64_t* total_faults, 
                         uint64_t* handled_faults,
                         uint64_t* fatal_faults);

#endif // COAL_MEMORY_PAGING_FAULT_H