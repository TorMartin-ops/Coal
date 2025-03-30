#include "elf_loader.h"
#include "terminal.h"
#include "buddy.h"
#include "paging.h"
#include "libc/string.h"
#include "read_file.h"

int load_elf_binary(const char *path, uint32_t *page_directory, uint32_t *entry_point) {
    size_t file_size = 0;
    void *file_data = read_file(path, &file_size);
    if (!file_data) {
        terminal_write("ELF loader: Failed to read file.\n");
        return -1;
    }
    
    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_data;
    if (ehdr->e_ident[0] != 0x7F ||
        ehdr->e_ident[1] != 'E'  ||
        ehdr->e_ident[2] != 'L'  ||
        ehdr->e_ident[3] != 'F') {
        terminal_write("ELF loader: Invalid ELF magic.\n");
        return -1;
    }
    
    if (ehdr->e_type != 2 || ehdr->e_machine != 3) {
        terminal_write("ELF loader: Unsupported ELF type or machine.\n");
        return -1;
    }
    
    *entry_point = ehdr->e_entry;
    
    Elf32_Phdr *phdr = (Elf32_Phdr *)((uint8_t *)file_data + ehdr->e_phoff);
    for (Elf32_Half i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;
        
        uint32_t seg_vaddr = phdr[i].p_vaddr;
        uint32_t seg_offset = phdr[i].p_offset;
        uint32_t seg_filesz = phdr[i].p_filesz;
        uint32_t seg_memsz = phdr[i].p_memsz;
        
        if (paging_map_range(page_directory, seg_vaddr, seg_memsz, phdr[i].p_flags) != 0) {
            terminal_write("ELF loader: Failed to map segment.\n");
            return -1;
        }
        
        memcpy((void *)seg_vaddr, (uint8_t *)file_data + seg_offset, seg_filesz);
        if (seg_memsz > seg_filesz) {
            memset((uint8_t *)seg_vaddr + seg_filesz, 0, seg_memsz - seg_filesz);
        }
    }
    
    terminal_write("ELF loader: ELF binary loaded successfully.\n");
    return 0;
}
