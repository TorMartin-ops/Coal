/**
 * @file elf_loader.h
 * @brief Strict ELF32 (i386, ET_EXEC) loader
 */
#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <kernel/core/types.h>
#include <kernel/memory/mm.h>

typedef struct {
    uint32_t entry;
    uint32_t brk;      /* first page above the highest segment */
    uint32_t phdr;     /* user address of the program headers (AT_PHDR), 0 if unknown */
    uint32_t phent;
    uint32_t phnum;
} elf_info_t;

/**
 * Validates the image in file[0..size) and maps its PT_LOAD segments into mm
 * (frames, page tables and VMAs). Nothing outside the user range
 * [USER_SPACE_MIN, USER_STACK_BOTTOM_VIRT) is ever mapped.
 * On failure the partly built mm is left for the caller to destroy.
 * @return 0, -ENOEXEC for a malformed or unsupported file, -ENOMEM
 */
int elf_load(mm_struct_t *mm, const void *file, size_t size, elf_info_t *info);

#endif
