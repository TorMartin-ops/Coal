/**
 * @file elf_loader.c
 * @brief Standalone ELF binary loader (potentially redundant with process.c).
 */

 #include <kernel/process/elf_loader.h>
 #include <kernel/process/elf.h>          // ELF structures (Elf32_Ehdr, etc.)
 #include <kernel/drivers/display/terminal.h>     // Logging
 #include <kernel/memory/kmalloc.h>      // kmalloc, kfree
 #include <kernel/fs/vfs/read_file.h>    // read_file
 #include <kernel/memory/frame.h>        // frame_alloc, frame_free
 #include <kernel/memory/paging.h>       // Paging functions and constants
 #include <kernel/fs/vfs/fs_errno.h>     // Filesystem error codes (used implicitly by read_file)
 #include <kernel/lib/assert.h>       // KERNEL_ASSERT
 #include <kernel/core/types.h>        // size_t, uintptr_t, etc. (Needs MIN macro added)
 
 #include <kernel/lib/string.h>       // memcpy, memset
 #include <libc/stdbool.h> // bool type
 
 // TODO: Define MIN macro in a suitable header (e.g., include/types.h or include/utils.h)
 // #define MIN(a, b) (((a) < (b)) ? (a) : (b))
 #ifndef MIN
 #warning "MIN macro is not defined. Please define it (e.g., in types.h)."
 #define MIN(a, b) (((a) < (b)) ? (a) : (b)) // Temporary definition
 #endif
 
 // TODO: Define missing ELF identifier indices in include/elf.h
 /* Example definitions to add to elf.h:
 #define EI_CLASS  4 // File class
 #define EI_DATA   5 // Data encoding
 #define ELFCLASS32 1 // 32-bit objects
 #define ELFDATA2LSB 1 // Little-endian
 */
 
 // TODO: Define missing paging constants in include/paging.h
 /* Example definitions to add to paging.h:
 #define PAGE_SIZE 4096u
 #define PAGING_PAGE_MASK   (~(PAGE_SIZE - 1u)) // Align down mask
 #define PAGING_OFFSET_MASK (PAGE_SIZE - 1u)   // Offset mask
 */
 
 
 // Helper structure to track allocated frames for cleanup
 typedef struct {
     uintptr_t vaddr; // Use uintptr_t for addresses
     uintptr_t paddr; // Use uintptr_t for addresses
 } allocated_page_info_t;
 
 /**
  * @brief Loads an ELF binary file into the specified page directory.
  *
  * Reads the ELF file, validates headers, allocates physical frames for LOAD segments,
  * maps these frames into the provided page directory at the specified virtual addresses,
  * and copies/zeroes the segment data.
  *
  * NOTE: This function duplicates logic found in process.c/load_elf_and_init_memory.
  * Its use might be limited or obsolete. Error handling for partial mapping
  * failures is complex and might not be perfectly robust here.
  *
  * @param path Path to the ELF executable file.
  * @param page_directory_phys Physical address of the target page directory (PDE).
  * @param entry_point Output parameter: stores the ELF entry point virtual address.
  * @return 0 on success, a negative error code (e.g., -FS_ERR_*, -1) on failure.
  */
 int load_elf_binary(const char *path, uint32_t *page_directory_phys, uint32_t *entry_point) {
     size_t file_size = 0;
     allocated_page_info_t *phys_frames = NULL; // Array to track allocated frames
     uint32_t total_pages_needed = 0;
     uint32_t allocated_frame_count = 0;
     int ret = -1; // Default error return
 
     terminal_printf("[elf_loader] Loading ELF binary: '%s'\n", path);
 
     // 1. Read the entire ELF file into memory
     void *file_data = read_file(path, &file_size);
     if (!file_data) {
         terminal_printf("[elf_loader] Error: Failed to read file '%s'.\n", path);
         return -1; // Or specific error from read_file if available
     }
     terminal_printf("[elf_loader] Read %u bytes from '%s'.\n", (unsigned int)file_size, path);
 
     // 2. Validate ELF Header
     Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_data;
     // Corrected: Check individual magic bytes
     if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
         ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
         ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
         ehdr->e_ident[EI_MAG3] != ELFMAG3) {
         terminal_printf("[elf_loader] Error: Invalid ELF magic number.\n");
         goto cleanup_file;
     }
     // Note: EI_CLASS, ELFCLASS32, EI_DATA, ELFDATA2LSB need definitions in elf.h
     if (ehdr->e_ident[EI_CLASS] != ELFCLASS32 || ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
          terminal_printf("[elf_loader] Error: Not a 32-bit LSB ELF.\n");
          goto cleanup_file;
     }
     if (ehdr->e_type != ET_EXEC || ehdr->e_machine != EM_386) {
         terminal_printf("[elf_loader] Error: Not an executable for i386 (Type=%d, Machine=%d).\n", ehdr->e_type, ehdr->e_machine);
         goto cleanup_file;
     }
     if (ehdr->e_phentsize != sizeof(Elf32_Phdr) || ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
          terminal_printf("[elf_loader] Error: Invalid program header table.\n");
          goto cleanup_file;
     }
 
     *entry_point = ehdr->e_entry;
     terminal_printf("[elf_loader] ELF Entry Point: 0x%x\n", *entry_point);
 
     Elf32_Phdr *phdrs = (Elf32_Phdr *)((uint8_t *)file_data + ehdr->e_phoff);
 
     // 3. First Pass: Calculate total pages needed for all PT_LOAD segments
     terminal_printf("[elf_loader] Calculating total pages needed...\n");
     for (Elf32_Half i = 0; i < ehdr->e_phnum; i++) {
         Elf32_Phdr *phdr = &phdrs[i];
         if (phdr->p_type == PT_LOAD && phdr->p_memsz > 0) {
             uint32_t vaddr_start = phdr->p_vaddr;
             uint32_t vaddr_end = vaddr_start + phdr->p_memsz;
 
             // Align start down and end up to page boundaries
             // Note: PAGING_PAGE_MASK and PAGE_SIZE need definitions in paging.h
             uintptr_t page_start = vaddr_start & PAGING_PAGE_MASK; // Align down
             uintptr_t page_end = (vaddr_end + PAGE_SIZE - 1) & PAGING_PAGE_MASK; // Align up
 
             if (page_end <= page_start) continue; // Should not happen if memsz > 0
 
             uint32_t pages_needed = (page_end - page_start) / PAGE_SIZE;
             total_pages_needed += pages_needed;
             terminal_printf("[elf_loader] Segment %d (vaddr 0x%lx, memsz %lu) needs %u pages.\n",
                            i, (unsigned long)vaddr_start, (unsigned long)phdr->p_memsz, pages_needed);
         }
     }
 
     if (total_pages_needed == 0) {
         terminal_printf("[elf_loader] Warning: No loadable segments found or all have zero size.\n");
         ret = 0; // No segments to load, technically successful? Or return error?
         goto cleanup_file;
     }
     terminal_printf("[elf_loader] Total pages to allocate: %lu\n", (unsigned long)total_pages_needed);
 
     // 4. Allocate tracking array for physical frames
     phys_frames = kmalloc(total_pages_needed * sizeof(allocated_page_info_t));
     if (!phys_frames) {
         terminal_printf("[elf_loader] Error: Failed to allocate frame tracking array.\n");
         ret = -1; // Consider specific out-of-memory error
         goto cleanup_file;
     }
     memset(phys_frames, 0, total_pages_needed * sizeof(allocated_page_info_t));
 
     // 5. Second Pass: Allocate physical frames and map them
     terminal_printf("[elf_loader] Allocating and mapping pages...\n");
     allocated_frame_count = 0; // Index into phys_frames array
     for (Elf32_Half i = 0; i < ehdr->e_phnum; i++) {
         Elf32_Phdr *phdr = &phdrs[i];
         if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) {
             continue;
         }
 
         uintptr_t vaddr_start = phdr->p_vaddr;
         uintptr_t vaddr_end = vaddr_start + phdr->p_memsz;
         uintptr_t page_start = vaddr_start & PAGING_PAGE_MASK;
         uintptr_t page_end = (vaddr_end + PAGE_SIZE - 1) & PAGING_PAGE_MASK;
 
         // Calculate page flags
         uint32_t flags = PAGE_PRESENT | PAGE_USER;
         if (phdr->p_flags & PF_W) { // Check ELF write flag
             flags |= PAGE_RW;
         }
         // Note: PF_X (execute) flag isn't directly mapped to typical x86 page flags here.
         // Apply NX bit based on write permission (heuristic: RW -> NX, R-X -> No NX)
         if(flags & PAGE_RW) {
             flags |= PAGE_NX_BIT; // If writable, assume not executable
         }
         // If not writable, PAGE_NX_BIT remains unset, allowing execution if EFER.NXE=1
 
 
         // Allocate and map each page for this segment
         for (uintptr_t page_vaddr = page_start; page_vaddr < page_end; page_vaddr += PAGE_SIZE) {
             // Check if we already allocated/mapped this page for a previous overlapping segment?
             // Simple loader: Assume non-overlapping for now, or just map again (last flags win).
             // A more robust loader would handle shared segments.
 
             uintptr_t phys_frame = frame_alloc();
             if (phys_frame == 0) {
                 terminal_printf("[elf_loader] Error: Failed to allocate physical frame for vaddr 0x%lx.\n", (unsigned long)page_vaddr);
                 ret = -1; // Consider out-of-memory error
                 goto cleanup_frames; // Cleanup previously allocated frames
             }
 
             // Track the allocated frame
             // Corrected: Added message to KERNEL_ASSERT
             KERNEL_ASSERT(allocated_frame_count < total_pages_needed, "Allocated frame count exceeds total needed");
             phys_frames[allocated_frame_count].vaddr = page_vaddr;
             phys_frames[allocated_frame_count].paddr = phys_frame;
             allocated_frame_count++;
 
             // Map the page into the target page directory
             // Corrected: Use paging_map_single_4k as found in paging.h
             if (paging_map_single_4k(page_directory_phys, page_vaddr, phys_frame, flags) != 0) {
                 terminal_printf("[elf_loader] Error: Failed to map vaddr 0x%lx to paddr 0x%lx.\n", (unsigned long)page_vaddr, (unsigned long)phys_frame);
                 // frame_free(phys_frame); // Free the just allocated frame? Handled in cleanup_frames
                 // allocated_frame_count--; // Adjust count? No, cleanup loop handles it.
                 ret = -1;
                 goto cleanup_frames; // Cleanup ALL frames allocated so far for this load
             }
             // terminal_printf("[elf_loader] Mapped vaddr 0x%lx -> paddr 0x%lx (flags 0x%x)\n", page_vaddr, phys_frame, flags);
         }
     }
     // Corrected: Added message to KERNEL_ASSERT
     KERNEL_ASSERT(allocated_frame_count == total_pages_needed, "Final allocated frame count mismatch");
     terminal_printf("[elf_loader] Page allocation and mapping complete.\n");
 
     // 6. Third Pass: Copy segment data using temporary kernel mappings
     terminal_printf("[elf_loader] Copying segment data...\n");
     for (Elf32_Half i = 0; i < ehdr->e_phnum; i++) {
         Elf32_Phdr *phdr = &phdrs[i];
         if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) {
             continue;
         }
 
         uintptr_t seg_vaddr = phdr->p_vaddr;
         uint32_t seg_offset = phdr->p_offset;
         uint32_t seg_filesz = phdr->p_filesz;
         uint32_t seg_memsz = phdr->p_memsz;
 
         // Validate file offsets against file size again (belt-and-suspenders)
         if (seg_offset > file_size || (seg_offset + seg_filesz) > file_size) {
              terminal_printf("[elf_loader] Error: Invalid segment file offset/size (Seg %d).\n", i);
              ret = -1;
              goto cleanup_mappings; // Unmap pages, free frames
         }
 
         uint8_t *file_src_base = (uint8_t *)file_data + seg_offset;
         uintptr_t current_vaddr = seg_vaddr;
         uint32_t bytes_processed = 0; // Track bytes copied/zeroed within the segment
 
         while (bytes_processed < seg_memsz) {
             // Note: PAGING_PAGE_MASK, PAGING_OFFSET_MASK, PAGE_SIZE need definitions in paging.h
             uintptr_t page_vaddr = current_vaddr & PAGING_PAGE_MASK;
             uint32_t offset_in_page = current_vaddr & PAGING_OFFSET_MASK;
             uint32_t bytes_left_in_page = PAGE_SIZE - offset_in_page;
             uint32_t bytes_left_in_segment = seg_memsz - bytes_processed;
             // Note: MIN needs definition (e.g., in types.h)
             uint32_t bytes_to_process_this_page = MIN(bytes_left_in_page, bytes_left_in_segment);
 
             // Find the physical address for this virtual page in the target directory
             // Corrected: Use paging_get_physical_address
             uintptr_t phys_addr = 0; // Initialize to 0
             if (paging_get_physical_address(page_directory_phys, page_vaddr, &phys_addr) != 0 || phys_addr == 0) {
                 terminal_printf("[elf_loader] Error: Failed to get physical address for vaddr 0x%lx (Seg %d).\n", (unsigned long)page_vaddr, i);
                 ret = -1;
                 goto cleanup_mappings;
             }
 
             // Temporarily map this physical page into kernel space
             void *temp_mapped_page = paging_temp_map(phys_addr);
             if (!temp_mapped_page) {
                 terminal_printf("[elf_loader] Error: Failed to temporarily map paddr 0x%lx (Seg %d).\n", (unsigned long)phys_addr, i);
                 ret = -1;
                 goto cleanup_mappings;
             }
 
             uint8_t *page_target_ptr = (uint8_t*)temp_mapped_page + offset_in_page;
 
             // Determine how much to copy vs zero for this portion
             uint32_t bytes_to_copy = 0;
             uint32_t bytes_to_zero = 0;
 
             if (bytes_processed < seg_filesz) {
                 // Still within the file data part
                 uint32_t remaining_file_bytes = seg_filesz - bytes_processed;
                 bytes_to_copy = MIN(bytes_to_process_this_page, remaining_file_bytes);
             }
             bytes_to_zero = bytes_to_process_this_page - bytes_to_copy;
 
             // Perform copy
             if (bytes_to_copy > 0) {
                 memcpy(page_target_ptr, file_src_base + bytes_processed, bytes_to_copy);
             }
             // Perform zeroing
             if (bytes_to_zero > 0) {
                 memset(page_target_ptr + bytes_to_copy, 0, bytes_to_zero);
             }
 
             // Unmap the temporary page
             paging_temp_unmap(temp_mapped_page);
 
             bytes_processed += bytes_to_process_this_page;
             current_vaddr += bytes_to_process_this_page;
         }
         // Corrected: Added message to KERNEL_ASSERT
         KERNEL_ASSERT(bytes_processed == seg_memsz, "Bytes processed does not match segment memsz");
     }
     terminal_printf("[elf_loader] Segment data copying complete.\n");
 
     // 7. Success
     ret = 0;
     goto cleanup_frames_array; // Skip other cleanups, just free tracking array and file data
 
 cleanup_mappings:
     terminal_printf("[elf_loader] Cleaning up mappings due to error...\n");
     // Unmap pages using the tracking array
     for(uint32_t k=0; k < allocated_frame_count; ++k) {
         // Corrected: Use paging_unmap_range to unmap a single page
         paging_unmap_range(page_directory_phys, phys_frames[k].vaddr, PAGE_SIZE);
     }
 
 
 cleanup_frames:
     terminal_printf("[elf_loader] Cleaning up allocated frames due to error...\n");
     // Free all frames allocated so far
     for (uint32_t k = 0; k < allocated_frame_count; ++k) {
         if (phys_frames[k].paddr != 0) { // Check if allocated
              frame_free(phys_frames[k].paddr);
         }
     }
     allocated_frame_count = 0; // Reset count
 
 cleanup_frames_array:
     if (phys_frames) {
         kfree(phys_frames);
     }
 
 cleanup_file:
     if (file_data) {
         kfree(file_data);
     }
 
     if (ret == 0) {
         terminal_printf("[elf_loader] load_elf_binary succeeded.\n");
     } else {
          terminal_printf("[elf_loader] load_elf_binary failed (code %d).\n", ret);
     }
     return ret;
 }
 
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
                              uintptr_t *initial_brk) {
     // Validate parameters
     if (!path || !mm || !entry_point || !initial_brk) {
         terminal_printf("[elf_loader] load_elf_and_init_memory: Invalid parameters.\n");
         return -EINVAL;
     }
     
     // Get the page directory from mm_struct
     if (!mm->pgd_phys) {
         terminal_printf("[elf_loader] load_elf_and_init_memory: No page directory in mm.\n");
         return -EINVAL;
     }
     
     terminal_printf("[elf_loader] Loading ELF '%s' into process memory...\n", path);
     
     // 1. Call load_elf_binary to load segments
     int ret = load_elf_binary(path, mm->pgd_phys, entry_point);
     if (ret != 0) {
         terminal_printf("[elf_loader] load_elf_binary failed with error %d\n", ret);
         return ret;
     }
     
     // 2. Calculate initial_brk based on highest loaded address
     // We need to scan the ELF file again to find the highest address used
     size_t file_size = 0;
     void *file_data = read_file(path, &file_size);
     if (!file_data) {
         terminal_printf("[elf_loader] Failed to re-read file for brk calculation.\n");
         return -EIO;
     }
     
     Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_data;
     Elf32_Phdr *phdrs = (Elf32_Phdr *)((uint8_t *)file_data + ehdr->e_phoff);
     
     uintptr_t highest_addr = 0;
     
     // Find the highest virtual address used by any PT_LOAD segment
     for (Elf32_Half i = 0; i < ehdr->e_phnum; i++) {
         Elf32_Phdr *phdr = &phdrs[i];
         if (phdr->p_type == PT_LOAD && phdr->p_memsz > 0) {
             uintptr_t seg_end = phdr->p_vaddr + phdr->p_memsz;
             if (seg_end > highest_addr) {
                 highest_addr = seg_end;
             }
         }
     }
     
     kfree(file_data);
     
     // Align the brk to page boundary
     *initial_brk = (highest_addr + PAGE_SIZE - 1) & PAGING_PAGE_MASK;
     
     terminal_printf("[elf_loader] ELF loaded successfully. Entry: 0x%x, Initial brk: 0x%lx\n", 
                     *entry_point, (unsigned long)*initial_brk);
     
     // 3. VMAs are initialized by the process creation code after this function returns
     
     return 0;
 }