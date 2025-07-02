/**
 * @file process_elf_loader.h
 * @brief ELF Loading Interface for Process Creation
 * 
 * Provides functions for loading ELF executables and setting up process memory.
 */

#ifndef KERNEL_PROCESS_ELF_LOADER_H
#define KERNEL_PROCESS_ELF_LOADER_H

#include <kernel/core/types.h>
#include <kernel/memory/mm.h>

/**
 * @brief Loads an ELF executable for a process
 * @param path Path to the ELF file on disk
 * @param mm Memory management structure to populate with VMAs
 * @param entry_point Output parameter for the ELF entry point address
 * @param initial_brk Output parameter for the initial heap break address
 * @return 0 on success, negative error code on failure
 * 
 * This function:
 * - Reads the ELF file from disk
 * - Validates ELF headers and format
 * - Creates VMAs for each loadable segment
 * - Allocates physical frames and maps them to virtual addresses
 * - Copies segment data from file to memory
 * - Sets up initial heap break point
 */
int process_load_elf(const char *path, mm_struct_t *mm, uint32_t *entry_point, uintptr_t *initial_brk);

#endif // KERNEL_PROCESS_ELF_LOADER_H