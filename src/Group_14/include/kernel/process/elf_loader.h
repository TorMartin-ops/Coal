#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <kernel/process/elf.h>   // so we know what Elf32_Ehdr, Elf32_Phdr are
#include <kernel/memory/mm.h>    // for mm_struct_t, if needed
#include <kernel/core/types.h> // for uint32_t, uintptr_t if not in mm.h

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Loads a 32-bit ELF file into the given process address space.
 *
 * @param path          Path to the ELF file in your filesystem
 * @param mm            Pointer to the process memory manager (page directory, etc.)
 * @param entry_point   [out] Receives the ELF's entry point
 * @param initial_brk   [out] Receives the initial brk (heap start)
 * @return 0 on success, negative on failure
 */
int load_elf_and_init_memory(const char *path,
                             mm_struct_t *mm,
                             uint32_t *entry_point,
                             uintptr_t *initial_brk);

#ifdef __cplusplus
}
#endif

#endif // ELF_LOADER_H
