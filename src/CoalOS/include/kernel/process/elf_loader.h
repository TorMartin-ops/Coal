/**
 * @file elf_loader.h
 * @brief Strict ELF64 (x86-64) loader for static executables: ET_EXEC and static-PIE (ET_DYN without PT_INTERP)
 */
#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <kernel/core/types.h>
#include <kernel/memory/mm.h>

typedef struct {
    uintptr_t entry;
    uintptr_t brk;      /* first page above the highest segment */
    uintptr_t phdr;     /* user address of the program headers (AT_PHDR), 0 if unknown */
    uintptr_t phent;
    uintptr_t phnum;
} elf_info_t;

/**
 * Validates the image in file[0..size) and maps its PT_LOAD segments into mm
 * (frames, page tables and VMAs). A static-PIE image is placed at a fixed base. Nothing outside
 * [USER_SPACE_MIN, ELF_LIMIT) is ever mapped.
 * On failure the partly built mm is left for the caller to destroy.
 * @return 0, -ENOEXEC for a malformed or unsupported file, -ENOMEM
 */
int elf_load(mm_struct_t *mm, const void *file, size_t size, elf_info_t *info);

#endif
