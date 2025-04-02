#include "elf_loader.h"
#include "terminal.h"
#include "buddy.h"
#include "paging.h"
#include <string.h>  // for memcpy, memset
#include "read_file.h"

/**
 * load_elf_binary
 *
 * Loads an ELF file from disk (using read_file) into memory, mapped
 * into 'page_directory' for each PT_LOAD segment. On success, sets 
 * '*entry_point' to the ELF's entry address.
 *
 * EXPECTATIONS:
 *  - path is a valid string path
 *  - page_directory is a valid PDE (buddy-allocated or so)
 *  - read_file returns a pointer to the entire ELF
 *  - concurrency disclaimers: if in SMP or interrupt environment, lock or disable interrupts
 *
 * @param path            File path to the ELF
 * @param page_directory  The PDE to map segments into
 * @param entry_point     Output param for the ELF's e_entry
 * @return 0 on success, -1 on error (logged to terminal)
 */
int load_elf_binary(const char *path, uint32_t *page_directory, uint32_t *entry_point) {
    // 1) Read file from disk
    size_t file_size = 0;
    void *file_data = read_file(path, &file_size);
    if (!file_data) {
        terminal_write("[elf_loader] Error: read_file failed.\n");
        return -1;
    }

    // 2) Basic ELF validations
    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_data;
    if (ehdr->e_ident[0] != 0x7F ||
        ehdr->e_ident[1] != 'E'  ||
        ehdr->e_ident[2] != 'L'  ||
        ehdr->e_ident[3] != 'F')
    {
        terminal_write("[elf_loader] Invalid ELF magic.\n");
        return -1;
    }
    if (ehdr->e_type != 2 /* ET_EXEC */ || ehdr->e_machine != 3 /* EM_386 */) {
        terminal_write("[elf_loader] Unsupported ELF type or machine (only i386 exec supported).\n");
        return -1;
    }

    // 3) Record entry point
    *entry_point = ehdr->e_entry;

    // 4) Iterate program headers (PHdr)
    Elf32_Phdr *phdr = (Elf32_Phdr *)((uint8_t *)file_data + ehdr->e_phoff);
    for (Elf32_Half i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) {
            // skip non-loadable segment
            continue;
        }
        // gather info
        uint32_t seg_vaddr  = phdr[i].p_vaddr;
        uint32_t seg_offset = phdr[i].p_offset;
        uint32_t seg_filesz = phdr[i].p_filesz;
        uint32_t seg_memsz  = phdr[i].p_memsz;

        // 4a) Validate offset < file_size
        if (seg_offset > file_size) {
            terminal_write("[elf_loader] Invalid segment offset beyond file size.\n");
            return -1;
        }
        // 4b) Validate offset+filesz <= file_size
        if ((seg_offset + seg_filesz) > file_size) {
            terminal_write("[elf_loader] Segment data extends beyond file size.\n");
            return -1;
        }
        // 4c) Possibly align seg_vaddr to page boundary if needed, or rely on p_align

        // 4d) Attempt to map the segment range into page_directory
        // ELF typical flags: PF_X=1, PF_W=2, PF_R=4
        // We pass them to paging_map_range. The code typically expects PAGE_PRESENT=1, PAGE_RW=2, ...
        // Convert ELF flags p_flags to your paging flags if needed.
        // For simplicity, we do:
        uint32_t flags = PAGE_PRESENT;
        if (phdr[i].p_flags & 2) { // PF_W
            flags |= PAGE_RW;
        }
        // if we want user-mode, we do PAGE_USER, etc.

        if (paging_map_range(page_directory, seg_vaddr, seg_memsz, flags) != 0) {
            terminal_write("[elf_loader] Error: paging_map_range failed for segment.\n");
            return -1;
        }

        // 5) Copy segment content from the ELF file into memory
        //    Virtual address seg_vaddr = physical identity in your OS model
        memcpy((void *)seg_vaddr, (uint8_t *)file_data + seg_offset, seg_filesz);

        // 6) Zero out the .bss portion if memsz > filesz
        if (seg_memsz > seg_filesz) {
            memset((uint8_t *)seg_vaddr + seg_filesz, 0, seg_memsz - seg_filesz);
        }

        // (Optional) debugging
        // terminal_write("[elf_loader] Mapped segment #");
        // print_number(i);
        // ...
    }

    // 7) Done
    terminal_write("[elf_loader] ELF binary loaded successfully.\n");
    return 0;
}
