/**
 * kernel.c - Main kernel entry point for UiAOS
 *
 * Author: Group 14 (UiA) & Gemini Assistance
 * Version: 3.3 (Added temp VA init, improved error handling)
 *
 * Description:
 * This file contains the main entry point (`main`) for the UiAOS kernel,
 * invoked by the Multiboot2 bootloader. It orchestrates the initialization
 * sequence for all major kernel subsystems, including memory management (Paging,
 * Buddy, Frame, Slab, Kmalloc), core structures (GDT, TSS, IDT), hardware drivers
 * (PIT, Keyboard, Block Device), filesystem (VFS, FAT), scheduling, and system
 * calls. After initialization, it attempts to launch an initial userspace process
 * and then enters an idle state.
 */

// === Standard/Core Headers ===
#include <multiboot2.h>      // Multiboot2 specification header
#include "types.h"          // Core type definitions (uintptr_t, size_t, bool, etc.)
#include <string.h>         // Kernel's string functions (memcpy, memset, strcmp)
#include <libc/stdint.h>    // Fixed-width integers (SIZE_MAX, uint32_t)

// === Kernel Subsystems ===
#include "terminal.h"       // Early console output
#include "gdt.h"            // Global Descriptor Table setup
#include "tss.h"            // Task State Segment setup
#include "idt.h"            // Interrupt Descriptor Table setup
#include "paging.h"         // Paging functions and constants (staged init)
#include "frame.h"          // Physical frame allocator (ref counting)
#include "buddy.h"          // Physical page allocator (buddy system)
#include "slab.h"           // Slab allocator (object caching)
#include "percpu_alloc.h"   // Per-CPU slab allocator support (if used)
#include "kmalloc.h"        // Kernel dynamic memory allocator facade
#include "process.h"        // Process Control Block management
#include "scheduler.h"      // Task scheduling
#include "syscall.h"        // System call interface definitions
// #include "elf_loader.h"     // ELF binary loading (Consider using process.c's internal loader)
#include "vfs.h"            // Virtual File System interface
#include "mount.h"          // Filesystem mounting
#include "fs_init.h"        // Filesystem layer initialization
#include "fs_errno.h"       // Filesystem error codes
#include "read_file.h"      // Helper to read entire files

// === Drivers ===
#include "pit.h"            // Programmable Interval Timer driver
#include "keyboard.h"       // Keyboard driver
#include "keymap.h"         // Keyboard layout mapping
#include "pc_speaker.h"     // PC Speaker driver
#include "song.h"           // Song definitions for speaker
#include "song_player.h"    // Song playback logic
#include "my_songs.h"       // Specific song data


// === Utilities ===
#include "get_cpu_id.h"     // Function to get current CPU ID
#include "cpuid.h"          // CPUID instruction helper
#include "kmalloc_internal.h" // For ALIGN_UP, potentially others
#include "serial.h"
#include "assert.h"         // For KERNEL_ASSERT, KERNEL_PANIC_HALT

// === Constants ===
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
#define MIN_HEAP_SIZE (1 * 1024 * 1024) // Minimum acceptable heap size (1MB)

// Macro for halting the system on critical failure (defined in assert.h)

#ifndef ARRAY_SIZE // Avoid redefinition if defined elsewhere
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif


// === Linker Symbols ===
// Define these in your linker script (linker.ld)
// Use void* or uint8_t* for byte-level addresses
extern uint8_t _kernel_start_phys;    // Physical start address of kernel code/data
extern uint8_t _kernel_end_phys;      // Physical end address of kernel code/data
extern uint8_t _kernel_text_start_phys;
extern uint8_t _kernel_text_end_phys;
extern uint8_t _kernel_rodata_start_phys;
extern uint8_t _kernel_rodata_end_phys;
extern uint8_t _kernel_data_start_phys;
extern uint8_t _kernel_data_end_phys;
extern uint8_t _kernel_virtual_base;  // Kernel's virtual base address (e.g., 0xC0000000)

// === Global Variables ===
// Define the global variable to store the Multiboot info address
uint32_t g_multiboot_info_phys_addr_global = 0;
// Define the global variable to store the Multiboot info VIRTUAL address (set after mapping)
uintptr_t g_multiboot_info_virt_addr_global = 0;


// === Static Function Prototypes ===
static struct multiboot_tag *find_multiboot_tag(uint32_t mb_info_phys_addr, uint16_t type);
static bool parse_memory_map(struct multiboot_tag_mmap *mmap_tag,
                             uintptr_t *out_total_memory,
                             uintptr_t *out_heap_base_addr, size_t *out_heap_size);
static bool init_memory(uint32_t mb_info_phys_addr); // Main memory initialization sequence
static void kernel_idle_task(void); // Idle task loop


// === Multiboot Tag Finding Helper (Improved Validation) ===
/**
 * find_multiboot_tag
 *
 * Iterates through the Multiboot 2 information structure to find a tag
 * of the specified type. Includes bounds checking for robustness.
 *
 * @param mb_info_phys_addr Physical address of the Multiboot 2 info structure.
 * @param type The type of tag to search for (MULTIBOOT_TAG_TYPE_*).
 * @return Pointer to the found tag, or NULL if not found or an error occurs.
 */
static struct multiboot_tag *find_multiboot_tag(uint32_t mb_info_phys_addr, uint16_t type) {
    // Basic validation: Ensure info address is within a reasonable range (e.g., below 1MB)
    if (mb_info_phys_addr == 0 || mb_info_phys_addr >= 0x100000) {
        terminal_write("[Boot Error] Multiboot info address invalid or inaccessible early.\n");
        return NULL;
    }

    // Access physical memory directly (safe before paging is fully active)
    // Use volatile to prevent compiler optimization issues before MMU setup.
    volatile uint32_t* mb_info_ptr = (volatile uint32_t*)mb_info_phys_addr;
    uint32_t total_size = mb_info_ptr[0]; // Read total size

    // Basic size sanity check (e.g., min 8 bytes, max 1MB for info struct size)
    if (total_size < 8 || total_size > 0x100000) {
        terminal_printf("[Boot Error] Multiboot total_size (%lu) invalid.\n", (unsigned long)total_size);
        return NULL;
    }

    // Tags start after total_size and reserved fields (offset 8)
    volatile struct multiboot_tag *tag = (volatile struct multiboot_tag *)(mb_info_phys_addr + 8);
    uintptr_t info_end = mb_info_phys_addr + total_size;

    // Iterate through tags until the end tag is found or bounds are exceeded
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        uintptr_t current_tag_addr = (uintptr_t)tag;

        // Bounds check 1: Ensure tag header itself fits within the info structure size
        if (current_tag_addr + sizeof(struct multiboot_tag) > info_end) {
             terminal_printf("[Boot Error] Multiboot tag header OOB (Tag Addr=%p, Info End=%p).\n", (void*)current_tag_addr, (void*)info_end);
             return NULL;
        }
        // Bounds check 2: Ensure the tag size field is reasonable and the full tag fits
        if (tag->size < sizeof(struct multiboot_tag) || (current_tag_addr + tag->size) > info_end) {
            terminal_printf("[Boot Error] Multiboot tag invalid size %u at Addr=%p (Info End=%p).\n", tag->size, (void*)current_tag_addr, (void*)info_end);
            return NULL;
        }

        // Check if this is the tag we're looking for
        if (tag->type == type) {
            return (struct multiboot_tag *)tag; // Found (cast away volatile)
        }

        // Advance to the next tag, ensuring 8-byte alignment
        uintptr_t next_tag_addr = current_tag_addr + ((tag->size + 7) & ~7);

        // Bounds check 3: Ensure next tag address is valid and within limits
        if (next_tag_addr <= current_tag_addr || next_tag_addr >= info_end) {
             terminal_printf("[Boot Error] Multiboot next tag address invalid/OOB (Next Addr=%p).\n", (void*)next_tag_addr);
             break; // Invalid progression
        }
        tag = (volatile struct multiboot_tag *)next_tag_addr;
    }

    return NULL; // End tag reached or tag not found
}


// === Memory Area Parsing Helper ===
// Helper to safely add a 64-bit length to a 32-bit base address
static inline uintptr_t safe_add_base_len(uintptr_t base, uint64_t len) {
    if (len > (UINTPTR_MAX - base)) {
        return UINTPTR_MAX; // Overflow, clamp to max uintptr_t
    }
    return base + (uintptr_t)len; // Safe to cast len and add
}


/**
 * parse_memory_map (Corrected Version)
 *
 * Parses the Multiboot memory map tag to determine the total physical memory
 * and identify the largest available region above 1MB suitable for the kernel heap.
 * Excludes memory regions overlapping with the kernel's physical location.
 * Uses corrected printf formats and overflow handling.
 *
 * @param mmap_tag Pointer to the Multiboot memory map tag (virtual address AFTER mapping).
 * @param out_total_memory Pointer to store the total detected physical memory (aligned up).
 * @param out_heap_base_addr Pointer to store the physical start address of the chosen heap region.
 * @param out_heap_size Pointer to store the size of the chosen heap region.
 * @return true on success, false if no suitable heap region is found or an error occurs.
 */
static bool parse_memory_map(struct multiboot_tag_mmap *mmap_tag,
                             uintptr_t *out_total_memory,
                             uintptr_t *out_heap_base_addr, size_t *out_heap_size)
{
    uintptr_t current_total_memory = 0;
    uintptr_t best_heap_base = 0;
    uint64_t best_heap_size_64 = 0; // Use 64-bit for size calculations
    multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
    uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;

    // Get kernel physical boundaries
    uintptr_t kernel_start_phys_addr = (uintptr_t)&_kernel_start_phys;
    // Align kernel end UP to ensure the whole kernel is excluded
    uintptr_t kernel_end_phys_addr = ALIGN_UP((uintptr_t)&_kernel_end_phys, PAGE_SIZE);
     if (kernel_end_phys_addr == 0 && (uintptr_t)&_kernel_end_phys > 0) kernel_end_phys_addr = UINTPTR_MAX; // Handle alignment overflow

    terminal_printf("  Kernel phys memory boundaries: Start=%#lx, End=%#lx\n",
                    (unsigned long)kernel_start_phys_addr, (unsigned long)kernel_end_phys_addr);
    terminal_write("  Memory Map (from Multiboot):\n");

    // First pass: Calculate total physical memory span
    int region_count = 0;
    uint64_t total_available_memory = 0;
    multiboot_memory_map_t *temp_entry = mmap_entry;
    while ((uintptr_t)temp_entry < mmap_end) {
        // Validate entry size before use
        if (mmap_tag->entry_size == 0 || mmap_tag->entry_size < sizeof(multiboot_memory_map_t)) {
            terminal_printf("  [ERR] MMAP entry size (%u) invalid!\n", mmap_tag->entry_size);
            return false;
        }
        // Ensure the full entry fits within the tag bounds
        if ((uintptr_t)temp_entry + mmap_tag->entry_size > mmap_end) {
             terminal_printf("  [ERR] MMAP entry structure exceeds tag boundary.\n");
             break; // Stop parsing
        }

        uintptr_t region_start = (uintptr_t)temp_entry->addr; // Use 32-bit addr
        uint64_t region_len = temp_entry->len; // Keep length 64-bit
        uintptr_t region_end = safe_add_base_len(region_start, region_len); // Use safe add

        // Debug print for each entry (using corrected formats)
        terminal_printf("    Entry %d: Addr=%#lx, Len=0x%lx:%08lx (%s)\n",
                        region_count,
                        (unsigned long)region_start,
                        (unsigned long)(region_len >> 32), (unsigned long)region_len, // Print 64-bit length as hex pair
                        (temp_entry->type == MULTIBOOT_MEMORY_AVAILABLE) ? "Available" : "Reserved/Other");


        // Always update total memory span based on the highest end address found
        if (region_end > current_total_memory) {
            current_total_memory = region_end;
        }

        // Track total *available* memory separately
        if (temp_entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
            total_available_memory += region_len;
        }

        region_count++;

        // Advance to the next entry safely
        uintptr_t next_entry_addr = (uintptr_t)temp_entry + mmap_tag->entry_size;
        // Check if next address would exceed bounds before casting
        if (next_entry_addr > mmap_end) {
            break;
        }
        temp_entry = (multiboot_memory_map_t *)next_entry_addr;
    }

    terminal_printf("  Found %d memory regions, total physical span ends at: %#lx\n",
                    region_count, (unsigned long)current_total_memory);
    terminal_printf("  Total AVAILABLE memory: 0x%lx:%08lx bytes (~%lu MB)\n",
                    (unsigned long)(total_available_memory >> 32), (unsigned long)total_available_memory, // Print 64-bit as hex pair
                    (unsigned long)(total_available_memory / (1024*1024))); // Approx MB

    // Safety check: if we found no valid memory, set a minimum
    if (current_total_memory == 0) {
        terminal_write("  [ERROR] No valid memory regions found! Assuming 16 MB minimum.\n");
        current_total_memory = 16 * 1024 * 1024; // Assume at least 16 MB
    }

    // Second pass: Find best heap region
    mmap_entry = mmap_tag->entries; // Reset entry pointer
    while ((uintptr_t)mmap_entry < mmap_end) {
         // Validation (as above)
         if (mmap_tag->entry_size == 0 || mmap_tag->entry_size < sizeof(multiboot_memory_map_t)) { return false; }
         if ((uintptr_t)mmap_entry + mmap_tag->entry_size > mmap_end) { break; }

        uintptr_t region_start = (uintptr_t)mmap_entry->addr;
        uint64_t region_len = mmap_entry->len;
        uintptr_t region_end = safe_add_base_len(region_start, region_len);

        // Find the largest AVAILABLE memory region >= 1MB for the initial heap
        if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE && region_start >= 0x100000) { // Skip first MB
            uintptr_t usable_start = region_start;
            uintptr_t usable_end = region_end;

            // Check for overlap with the kernel image [kernel_start_phys_addr, kernel_end_phys_addr)
            uintptr_t overlap_start = (usable_start > kernel_start_phys_addr) ? usable_start : kernel_start_phys_addr;
            uintptr_t overlap_end = (usable_end < kernel_end_phys_addr) ? usable_end : kernel_end_phys_addr;

            if (overlap_start < overlap_end) { // Kernel overlaps with this region
                // Consider the part *before* the kernel (if any)
                if (usable_start < kernel_start_phys_addr) {
                    uint64_t size_before = (uint64_t)kernel_start_phys_addr - usable_start;
                    if (size_before > best_heap_size_64) {
                        best_heap_size_64 = size_before;
                        best_heap_base = usable_start;
                    }
                }
                // Consider the part *after* the kernel (if any)
                if (usable_end > kernel_end_phys_addr) {
                    uintptr_t start_after = kernel_end_phys_addr; // Start right after aligned kernel end
                    uint64_t size_after = (uint64_t)usable_end - start_after; // Use 64-bit subtraction
                    if (size_after > best_heap_size_64) {
                        best_heap_size_64 = size_after;
                        best_heap_base = start_after;
                    }
                }
            } else { // No overlap with kernel
                 if (region_len > best_heap_size_64) {
                     best_heap_size_64 = region_len;
                     best_heap_base = usable_start;
                 }
            }
        } // End if AVAILABLE and >= 1MB

        // Advance to the next entry safely
        uintptr_t next_entry_addr = (uintptr_t)mmap_entry + mmap_tag->entry_size;
        if (next_entry_addr > mmap_end) { break; }
        mmap_entry = (multiboot_memory_map_t *)next_entry_addr;
    } // End while loop through mmap entries

    terminal_printf("  Parse MMAP Result: HeapBase=%#lx, HeapSize64=0x%lx:%08lx\n",
                    (unsigned long)best_heap_base,
                    (unsigned long)(best_heap_size_64 >> 32),
                    (unsigned long)best_heap_size_64);

    // Final checks and output assignment
    if (best_heap_size_64 > 0 && best_heap_base != 0) {
        *out_heap_base_addr = best_heap_base;
        // Clamp 64-bit size to size_t for output
        if (best_heap_size_64 > (uint64_t)SIZE_MAX) {
            terminal_write("  [Warning] Largest heap region exceeds 32-bit size_t! Clamping to SIZE_MAX.\n");
            *out_heap_size = SIZE_MAX;
        } else {
            *out_heap_size = (size_t)best_heap_size_64;
        }

        // Optional: Clamp heap size to a reasonable maximum if desired
        size_t max_initial_heap = 256 * 1024 * 1024; // Limit to 256MB for example
        if (*out_heap_size > max_initial_heap) {
            terminal_printf("  [Info] Clamping initial heap size from %lu MB to %lu MB.\n",
                           (unsigned long)((*out_heap_size) / (1024 * 1024)),
                           (unsigned long)(max_initial_heap / (1024 * 1024)));
            *out_heap_size = max_initial_heap;
        }

        // Ensure clamped size still meets minimum requirement
        if (*out_heap_size < MIN_HEAP_SIZE) {
            terminal_printf("  [FATAL] Heap size (%lu bytes) is less than minimum required (%u bytes)!\n",
                            (unsigned long)*out_heap_size, MIN_HEAP_SIZE);
            return false;
        }
    } else {
        terminal_write("  [FATAL] No suitable memory region found >= 1MB for heap!\n");
        return false;
    }

    // IMPORTANT: Set total physical memory span (aligned)
    *out_total_memory = current_total_memory;
    if (*out_total_memory == 0) {
        terminal_write("  [WARNING] Total memory calculation resulted in zero! Setting 16MB minimum.\n");
        *out_total_memory = 16 * 1024 * 1024;  // 16MB minimum
    } else if (*out_total_memory < UINTPTR_MAX) {
        // Align final total memory UP to page size (careful with overflow)
        uintptr_t aligned_total = PAGE_ALIGN_UP(*out_total_memory);
        if (aligned_total == 0 && *out_total_memory > 0) { // Check overflow from alignment
             terminal_printf("  [Warning] PAGE_ALIGN_UP overflowed for total memory %#lx. Setting to UINTPTR_MAX.\n", (unsigned long)*out_total_memory);
            *out_total_memory = UINTPTR_MAX;
        } else {
            *out_total_memory = aligned_total;
        }
    }
    // else: If already UINTPTR_MAX, leave it.

    terminal_printf("  Total Physical Memory Span (Aligned): %#lx bytes (~%lu MB)\n",
                    (unsigned long)*out_total_memory, (unsigned long)(*out_total_memory / (1024 * 1024)));
    terminal_printf("  Selected Heap Region (Final): Phys Addr=%#lx, Size=%lu bytes (%lu KB)\n",
                    (unsigned long)*out_heap_base_addr, (unsigned long)*out_heap_size, (unsigned long)(*out_heap_size / 1024));
    return true;
} // End of parse_memory_map


// === Memory Subsystem Initialization (Revised Order & Structure) ===
/**
 * init_memory
 *
 * Orchestrates the initialization of the core memory management subsystems:
 * Paging, Buddy Allocator, Frame Allocator, and Kmalloc.
 * This involves several stages to correctly handle dependencies and bootstrap
 * the virtual memory system.
 *
 * @param mb_info_phys_addr Physical address of the Multiboot 2 info structure.
 * @return true on success, false on critical failure (panics internally).
 */
 static bool init_memory(uint32_t mb_info_phys_addr) {
    terminal_write("[Kernel] Initializing Memory Subsystems...\n");

    // --- Stage 0: Parse Multiboot Memory Map ---
    terminal_write(" Stage 0: Parsing Multiboot Memory Map (using physical address)...\n");
    // Use the physically addressed find_multiboot_tag helper here
    struct multiboot_tag_mmap *mmap_tag_phys = (struct multiboot_tag_mmap *)find_multiboot_tag(
        mb_info_phys_addr, MULTIBOOT_TAG_TYPE_MMAP);
    if (!mmap_tag_phys) {
        KERNEL_PANIC_HALT("Multiboot memory map tag not found!");
        return false; // Unreachable
    }

    uintptr_t total_memory = 0;
    uintptr_t heap_phys_start = 0;
    size_t heap_size = 0;
    // parse_memory_map also uses physical addresses at this stage
    if (!parse_memory_map(mmap_tag_phys, &total_memory, &heap_phys_start, &heap_size)) {
        KERNEL_PANIC_HALT("Failed to parse memory map or find suitable heap region!");
        return false; // Unreachable
    }
     if (heap_size < MIN_HEAP_SIZE) {
        KERNEL_PANIC_HALT("Heap region too small!");
        return false; // Unreachable
     }
     if (total_memory == 0) {
        KERNEL_PANIC_HALT("Total physical memory reported as zero!");
        return false; // Unreachable
     }

    uintptr_t kernel_phys_start = (uintptr_t)&_kernel_start_phys;
    uintptr_t kernel_phys_end = (uintptr_t)&_kernel_end_phys;
    terminal_printf("   Kernel Phys Region: [%#lx - %#lx)\n", (unsigned long)kernel_phys_start, (unsigned long)kernel_phys_end);
    terminal_printf("   Heap Phys Region:   [%#lx - %#lx) Size: %lu KB\n", (unsigned long)heap_phys_start, (unsigned long)(heap_phys_start + heap_size), (unsigned long)(heap_size / 1024));
    terminal_printf("   Total Phys Memory:  %lu MB\n", (unsigned long)(total_memory / (1024 * 1024)));

    // --- Stage 1: Allocate Initial Page Directory Frame ---
    terminal_write(" Stage 1: Allocating initial Page Directory frame...\n");
    uintptr_t initial_pd_phys;
    if (paging_initialize_directory(&initial_pd_phys) != 0) {
        KERNEL_PANIC_HALT("Failed to allocate/initialize initial Page Directory!");
        return false; // Unreachable
    }
    terminal_printf("   Initial PD allocated at Phys: %#lx\n", (unsigned long)initial_pd_phys);

    // --- Stage 2: Setup Early Mappings (Kernel Higher-Half, Heap Identity) ---
    terminal_write(" Stage 2: Setting up early physical maps...\n");
    if (paging_setup_early_maps(initial_pd_phys, kernel_phys_start, kernel_phys_end, heap_phys_start, heap_size) != 0) {
        KERNEL_PANIC_HALT("Failed to setup early mappings!");
        return false; // Unreachable
    }

    // --- Stage 3: Initialize Buddy Allocator ---
    terminal_write(" Stage 3: Initializing Buddy Allocator...\n");
    // Buddy is initialized using the *physical* address range identified before paging activation.
    // The early maps ensure this physical range is accessible via identity mapping.
    buddy_init((void *)heap_phys_start, heap_size);
    if (buddy_free_space() == 0 && heap_size >= MIN_BLOCK_SIZE) {
        terminal_write("  [Warning] Buddy Allocator reports zero free space after init.\n");
    }
    terminal_printf("   Buddy Initial Free Space: %lu bytes\n", (unsigned long)buddy_free_space());

    // --- Stage 4: Finalize & Activate Paging ---
    terminal_write(" Stage 4: Finalizing and activating paging...\n");
    if (paging_finalize_and_activate(initial_pd_phys, total_memory) != 0) {
        KERNEL_PANIC_HALT("Failed to finalize and activate paging!");
        return false; // Unreachable
    }
    // Paging is now ON. g_kernel_page_directory_phys/virt should be valid.

    // --- STAGE 4.5: Map Multiboot Info Structure ---
    terminal_write(" Stage 4.5: Mapping Multiboot Info Structure...\n");
    if (g_multiboot_info_phys_addr_global != 0) {
        uintptr_t mb_info_phys_page_start = PAGE_ALIGN_DOWN(g_multiboot_info_phys_addr_global);
        // Map at least one page to access the size field safely.
        size_t mb_mapping_size = PAGE_SIZE;
        uintptr_t mb_info_virt_page_start = KERNEL_SPACE_VIRT_START + mb_info_phys_page_start;

        terminal_printf("   Mapping MB Info Phys [%#lx - %#lx) to Virt [%#lx - %#lx)\n",
                         (unsigned long)mb_info_phys_page_start, (unsigned long)(mb_info_phys_page_start + mb_mapping_size),
                         (unsigned long)mb_info_virt_page_start, (unsigned long)(mb_info_virt_page_start + mb_mapping_size));

        if (paging_map_range((uint32_t*)g_kernel_page_directory_phys,
                             mb_info_virt_page_start,
                             mb_info_phys_page_start,
                             mb_mapping_size,
                             PTE_KERNEL_READONLY_FLAGS) != 0)
        {
             KERNEL_PANIC_HALT("Failed to map Multiboot info structure!");
             return false;
        }
        // Calculate the final virtual address of the structure start
        g_multiboot_info_virt_addr_global = mb_info_virt_page_start + (g_multiboot_info_phys_addr_global % PAGE_SIZE);
        terminal_printf("   Multiboot structure accessible at VIRT: %#lx\n", (unsigned long)g_multiboot_info_virt_addr_global);

        // Optional: Map more pages if the structure is larger than one page
        uint32_t total_mb_size = *(volatile uint32_t*)g_multiboot_info_virt_addr_global; // Read size via virtual mapping
        if (total_mb_size > PAGE_SIZE) {
            size_t additional_pages_needed = (ALIGN_UP(total_mb_size, PAGE_SIZE) / PAGE_SIZE) - 1;
            uintptr_t next_phys_page = mb_info_phys_page_start + PAGE_SIZE;
            uintptr_t next_virt_page = mb_info_virt_page_start + PAGE_SIZE;
            terminal_printf("   MB Info > 1 page (%u bytes). Mapping %u additional pages...\n", total_mb_size, additional_pages_needed);
            if (paging_map_range((uint32_t*)g_kernel_page_directory_phys,
                                 next_virt_page, next_phys_page,
                                 additional_pages_needed * PAGE_SIZE,
                                 PTE_KERNEL_READONLY_FLAGS) != 0)
            {
                 terminal_printf("   [Warning] Failed to map additional Multiboot info pages.\n");
                 // Non-fatal, but access beyond first page might fail
            }
        }
    } else {
         KERNEL_PANIC_HALT("Multiboot physical address is zero after paging activation!");
         return false;
    }
    // --- End STAGE 4.5 ---

    // --- Stage 5: Map Physical Memory to Higher Half ---
    terminal_write(" Stage 5: Mapping physical memory to higher half...\n");
    terminal_write("   Skipping Stage 5 large physical memory mapping (handled on demand).\n");

    // --- Stage 6: Initialize Frame Allocator ---
    terminal_write(" Stage 6: Initializing Frame Allocator...\n");
    // Find the MMAP tag VIRTUALLY using the mapped Multiboot structure
    struct multiboot_tag_mmap *mmap_tag_virt = NULL;
    if (g_multiboot_info_virt_addr_global) {
         // Re-implement tag finding logic using virtual addresses
          struct multiboot_tag* tag = (struct multiboot_tag *)(g_multiboot_info_virt_addr_global + 8);
          uint32_t total_virt_size = *(uint32_t*)g_multiboot_info_virt_addr_global;
          uintptr_t info_virt_end = g_multiboot_info_virt_addr_global + total_virt_size;
          while ((uintptr_t)tag < info_virt_end && tag->type != MULTIBOOT_TAG_TYPE_END) {
               if ((uintptr_t)tag + sizeof(struct multiboot_tag) > info_virt_end || tag->size < 8 || (uintptr_t)tag + tag->size > info_virt_end) break;
               if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) { mmap_tag_virt = (struct multiboot_tag_mmap *)tag; break; }
               uintptr_t next_tag_virt = (uintptr_t)tag + ((tag->size + 7) & ~7);
               if (next_tag_virt <= (uintptr_t)tag || next_tag_virt >= info_virt_end) break;
               tag = (struct multiboot_tag *)next_tag_virt;
          }
    }
    if (!mmap_tag_virt) {
         KERNEL_PANIC_HALT("Cannot find MMAP tag via virtual address!");
         return false;
    }

    terminal_printf("   Passing MMAP tag virtual address %p to frame_init.\n", mmap_tag_virt);

    // Pass VIRTUAL address of the tag to frame_init
    if (frame_init(mmap_tag_virt,
                   kernel_phys_start, kernel_phys_end,
                   heap_phys_start, heap_phys_start + heap_size) != 0) {
        KERNEL_PANIC_HALT("Frame Allocator initialization failed!");
        return false; // Unreachable
    }

    // --- Stage 7: Initialize Kmalloc ---
    terminal_write(" Stage 7: Initializing Kmalloc...\n");
    kmalloc_init();

    terminal_write("[OK] Memory Subsystems Initialized Successfully.\n");
    return true;
 }


// === Kernel Idle Task ===
/**
 * kernel_idle_task
 *
 * A simple idle task that halts the CPU, waiting for the next interrupt.
 * This is run when no other tasks are ready to be scheduled.
 */
static void kernel_idle_task(void) {
    terminal_write("[Idle] Kernel idle task started. Halting CPU when idle.\n");
    while(1) {
        // Enable interrupts briefly to allow pending interrupts (like PIT) to fire.
        asm volatile("sti");
        // Halt the CPU. It will wake up on the next interrupt.
        // Interrupts are automatically disabled by the CPU upon entering an ISR.
        asm volatile("hlt");
        // Loop back to re-enable interrupts before halting again.
    }
}

// === Main Kernel Entry Point ===
/**
 * main
 *
 * The C entry point for the UiAOS kernel, called from assembly (`_start`).
 * Initializes all kernel subsystems and starts the scheduler.
 *
 * @param magic The Multiboot2 magic number passed by the bootloader.
 * @param mb_info_phys_addr The physical address of the Multiboot 2 info structure.
 */
void main(uint32_t magic, uint32_t mb_info_phys_addr) {
    // Store Multiboot info address globally FIRST - needed by early memory init
    g_multiboot_info_phys_addr_global = mb_info_phys_addr;

    // Initialize early hardware needed for output
    serial_init();           // Serial port for backup/QEMU logging
    terminal_init();         // VGA terminal for primary output

    terminal_write("=== UiAOS Kernel Booting ===\n");
    terminal_printf(" Version: %s\n\n", "3.3-PanicFix"); // Updated version

    // === Pre-Initialization Checks ===
    terminal_write("[Boot] Verifying Multiboot information...\n");
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        terminal_printf("  [FATAL] Invalid Multiboot Magic number: %#lx (Expected: %#lx)\n",
                        (unsigned long)magic, (unsigned long)MULTIBOOT2_BOOTLOADER_MAGIC);
        KERNEL_PANIC_HALT("Invalid Multiboot Magic number.");
    }
    if (mb_info_phys_addr == 0 || mb_info_phys_addr >= 0x100000) { // Sanity check address
         terminal_printf("  [FATAL] Invalid or inaccessible Multiboot info address: %#lx\n",
                         (unsigned long)mb_info_phys_addr);
         KERNEL_PANIC_HALT("Invalid Multiboot info address.");
    }
    terminal_printf("[Boot] Multiboot magic OK (Info at phys %#lx).\n", (unsigned long)mb_info_phys_addr);

    // === Core System Initialization ===
    terminal_write("[Kernel] Initializing GDT & TSS...\n");
    gdt_init();

    terminal_write("[Kernel] Initializing IDT & PIC...\n");
    idt_init();

    // === Memory Management Initialization ===
    if (!init_memory(g_multiboot_info_phys_addr_global)) {
        // Panic occurs within init_memory on failure
        return; // Should be unreachable
    }

    // *** Initialize Temporary VA Mapper *** (FIXED LOCATION)
    terminal_write("[Kernel] Initializing Temporary VA Mapper...\n");
    if (paging_temp_map_init() != 0) {
        KERNEL_PANIC_HALT("Failed to initialize temporary VA mapper!");
    }

    // === Hardware Driver Initialization ===
    terminal_write("[Kernel] Initializing Hardware Drivers...\n");
    terminal_write("  Initializing PIT...\n");
    init_pit();
    terminal_write("  Initializing Keyboard...\n");
    keyboard_init();
    keymap_load(KEYMAP_NORWEGIAN); // Consider making this configurable

    // === Filesystem Initialization ===
    terminal_write("[Kernel] Initializing Filesystem Layer...\n");
    bool fs_ready = false;
    int fs_init_status = fs_init();
    if (fs_init_status == FS_SUCCESS) {
        terminal_write("  [OK] Filesystem initialized and root mounted.\n");
        fs_ready = true;
        list_mounts(); // List mounts for verification
    } else {
        terminal_printf("  [Warning] Filesystem initialization failed (Error: %d). Continuing without FS.\n", fs_init_status);
        // Consider panic based on requirements: KERNEL_PANIC_HALT("Filesystem init failed.");
    }

    // === Scheduler Initialization ===
    terminal_write("[Kernel] Initializing Scheduler...\n");
    scheduler_init(); // Initializes idle task
    // Error check scheduler_init if it can fail

    // === Initial Process Creation ===
    terminal_write("[Kernel] Creating initial user process...\n");
    bool task_added = false;
    if (fs_ready) { // Only attempt if FS is available
        const char *user_prog_path = "/hello.elf"; // Configurable?
        terminal_printf("  Attempting to load '%s'...\n", user_prog_path);
        pcb_t *user_proc_pcb = create_user_process(user_prog_path);
        if (user_proc_pcb) {
            terminal_printf("  [OK] Process created (PID %lu). Adding to scheduler.\n", (unsigned long)user_proc_pcb->pid);
            if (scheduler_add_task(user_proc_pcb) == 0) {
                terminal_write("  [OK] Initial user process scheduled.\n");
                task_added = true;
            } else {
                terminal_printf("  [ERROR] Failed to add initial process (PID %lu) to scheduler.\n", (unsigned long)user_proc_pcb->pid);
                destroy_process(user_proc_pcb); // Clean up failed process
            }
        } else {
            terminal_printf("  [ERROR] Failed to create initial user process from '%s'.\n", user_prog_path);
            // Decide whether to panic or continue: KERNEL_PANIC_HALT("Failed to create initial process.");
        }
    } else {
         terminal_write("  [Info] Filesystem not available, cannot load initial user process.\n");
    }

    // === Start Scheduling ===
    if (task_added) {
        terminal_write("[Kernel] Enabling preemptive scheduling via PIT...\n");
        pit_set_scheduler_ready(); // Allow PIT handler to call schedule()
    } else {
        terminal_write("[Kernel] No user tasks scheduled. Entering idle loop.\n");
    }

    terminal_write("\n[Kernel] Initialization complete. Enabling interrupts and entering idle task/scheduler.\n");
    terminal_write("======================================================================\n");

    // Enable interrupts - NOW the PIT timer will start firing and causing schedule() calls
    asm volatile ("sti");

    // Enter the idle loop. If tasks are scheduled, the PIT IRQ will trigger
    // the scheduler to switch away from this idle loop. If no tasks are ready,
    // it will continue to halt the CPU until an interrupt occurs.
    kernel_idle_task();

    // --- Code should not be reached beyond kernel_idle_task ---
    KERNEL_PANIC_HALT("Reached end of main() unexpectedly!");
}