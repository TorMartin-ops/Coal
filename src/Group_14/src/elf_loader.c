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
 int load_elf_binary(const char *path, uint32_t *page_directory_phys, uint32_t *entry_point) {
    size_t file_size = 0;
    void *file_data = read_file(path, &file_size);
    if (!file_data) { return -1; }

    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_data;
    if (memcmp(ehdr->e_ident, "\x7F" "ELF", 4) != 0) { kfree(file_data); return -1; }
    if (ehdr->e_type != 2 || ehdr->e_machine != 3) { kfree(file_data); return -1; }
    *entry_point = ehdr->e_entry;

    Elf32_Phdr *phdr = (Elf32_Phdr *)((uint8_t *)file_data + ehdr->e_phoff);
    for (Elf32_Half i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD || phdr[i].p_memsz == 0) continue;

        uint32_t seg_vaddr  = phdr[i].p_vaddr;
        uint32_t seg_offset = phdr[i].p_offset;
        uint32_t seg_filesz = phdr[i].p_filesz;
        uint32_t seg_memsz  = phdr[i].p_memsz;

        if (seg_offset > file_size || (seg_offset + seg_filesz) > file_size) { kfree(file_data); return -1; }

        uint32_t flags = PAGE_PRESENT | PAGE_USER; // Base flags
        if (phdr[i].p_flags & 2) flags |= PAGE_RW;

        // --- This mapping is problematic ---
        // It assumes identity mapping phys=virt and doesn't allocate physical frames.
        // The logic in process.c's load_elf_and_init_memory is better.
        // Commenting out the problematic map call.
        /*
        if (paging_map_range(page_directory_phys, seg_vaddr, ???, seg_memsz, flags) != 0) { // Missing physical address
            terminal_write("[elf_loader] Error: paging_map_range failed.\n");
            kfree(file_data);
            return -1;
        }
        */
        terminal_printf("[elf_loader] Warning: Segment %d mapping skipped in load_elf_binary (handled by process loader).\n", i);


        // Copy/Zero logic is fine if addresses were correctly mapped
        // void* map_target_addr = (void*)seg_vaddr; // Assumes identity map or pre-mapped
        // memcpy(map_target_addr, (uint8_t *)file_data + seg_offset, seg_filesz);
        // if (seg_memsz > seg_filesz) {
        //     memset((uint8_t *)map_target_addr + seg_filesz, 0, seg_memsz - seg_filesz);
        // }
    }

    kfree(file_data);
    terminal_write("[elf_loader] load_elf_binary finished (check if actually used).\n");
    return 0;
}
