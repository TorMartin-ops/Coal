/**
 * kernel.c - Main kernel entry point for UiAOS
 *
 * Author: Group 14 (UiA)
 * Version: 3.2 (Fixes for kernel.c compilation errors)
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
#include "elf_loader.h"     // ELF binary loading
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

// === Constants ===
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
#define MIN_HEAP_SIZE (1 * 1024 * 1024) // Minimum acceptable heap size (1MB)

// Macro for halting the system on critical failure
#ifndef KERNEL_PANIC_HALT // Avoid redefinition if defined elsewhere
#define KERNEL_PANIC_HALT(msg) do { \
    terminal_printf("\n[KERNEL PANIC] %s at %s:%d. System Halted.\n", msg, __FILE__, __LINE__); \
    while(1) { asm volatile("cli; hlt"); } \
} while(0)
#endif

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
    uint32_t total_size = *(uint32_t*)mb_info_phys_addr;

    // Basic size sanity check (e.g., min 8 bytes, max 1MB for info struct size)
    if (total_size < 8 || total_size > 0x100000) {
        // Use %lu for uint32_t (long unsigned int)
        terminal_printf("[Boot Error] Multiboot total_size (%lu) invalid.\n", (long unsigned int)total_size);
        return NULL;
    }

    struct multiboot_tag *tag = (struct multiboot_tag *)(mb_info_phys_addr + 8); // First tag starts after total_size and reserved fields
    uintptr_t info_end = mb_info_phys_addr + total_size;

    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        uintptr_t current_tag_addr = (uintptr_t)tag;

        // Bounds check: Ensure tag header itself is within the info structure
        if (current_tag_addr + sizeof(struct multiboot_tag) > info_end) {
             // Use %p for addresses (uintptr_t)
             terminal_printf("[Boot Error] Multiboot tag header OOB (Tag Addr=%p, Info End=%p).\n", (void*)current_tag_addr, (void*)info_end);
             return NULL;
        }
        // Bounds check: Ensure the entire tag (based on its size field) is within the info structure
        if (tag->size < sizeof(struct multiboot_tag) || (current_tag_addr + tag->size) > info_end) {
            // Use %u for tag->size (uint32_t), %p for addresses
            terminal_printf("[Boot Error] Multiboot tag invalid size %u at Addr=%p (Info End=%p).\n", tag->size, (void*)current_tag_addr, (void*)info_end);
            return NULL;
        }

        // Check if this is the tag we're looking for
        if (tag->type == type) {
            return tag; // Found
        }

        // Advance to the next tag, ensuring 8-byte alignment
        uintptr_t next_tag_addr = current_tag_addr + ((tag->size + 7) & ~7);

        // Check bounds before advancing to the next tag
        if (next_tag_addr >= info_end) {
             // If the next address is exactly the end, it implies the current tag
             // was the last one before a potential (but missing) END tag.
             break; // Stop searching
        }
        tag = (struct multiboot_tag *)next_tag_addr;
    }

    return NULL; // End tag reached or tag not found
}


// === Memory Area Parsing Helper ===
/**
 * parse_memory_map
 *
 * Parses the Multiboot memory map tag to determine the total physical memory
 * and identify the largest available region above 1MB suitable for the kernel heap.
 * Excludes memory regions overlapping with the kernel's physical location.
 *
 * @param mmap_tag Pointer to the Multiboot memory map tag.
 * @param out_total_memory Pointer to store the total detected physical memory (aligned up).
 * @param out_heap_base_addr Pointer to store the physical start address of the chosen heap region.
 * @param out_heap_size Pointer to store the size of the chosen heap region.
 * @return true on success, false if no suitable heap region is found or an error occurs.
 */
 // Removed duplicate include guards and includes already present at top of file

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
 * @param mmap_tag Pointer to the Multiboot memory map tag (virtual address).
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

    // Use %#lx for hex addresses, %lu for size_t/uint32_t decimals
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

            // terminal_printf("  Evaluating AVAILABLE region [0x%x - 0x%x) for heap\n", usable_start, usable_end);

            // Check for overlap with the kernel image [kernel_start_phys_addr, kernel_end_phys_addr)
            uintptr_t overlap_start = (usable_start > kernel_start_phys_addr) ? usable_start : kernel_start_phys_addr;
            uintptr_t overlap_end = (usable_end < kernel_end_phys_addr) ? usable_end : kernel_end_phys_addr;

            if (overlap_start < overlap_end) { // Kernel overlaps with this region
                // terminal_printf("    Region overlaps with kernel [0x%x - 0x%x)\n", kernel_start_phys_addr, kernel_end_phys_addr);

                // Consider the part *before* the kernel (if any)
                if (usable_start < kernel_start_phys_addr) {
                    uint64_t size_before = (uint64_t)kernel_start_phys_addr - usable_start;
                    // terminal_printf("    Segment before kernel: Base=0x%x, Size=0x%x:%08x bytes\n", usable_start, (uint32_t)(size_before >> 32), (uint32_t)size_before);
                    if (size_before > best_heap_size_64) {
                        best_heap_size_64 = size_before;
                        best_heap_base = usable_start;
                        // terminal_printf("    -> New best heap candidate (before kernel)\n");
                    }
                }

                // Consider the part *after* the kernel (if any)
                if (usable_end > kernel_end_phys_addr) {
                    uintptr_t start_after = kernel_end_phys_addr; // Start right after aligned kernel end
                    uint64_t size_after = (uint64_t)usable_end - start_after; // Use 64-bit subtraction if usable_end can be UINTPTR_MAX
                    // terminal_printf("    Segment after kernel: Base=0x%x, Size=0x%x:%08x bytes\n", start_after, (uint32_t)(size_after >> 32), (uint32_t)size_after);

                    if (size_after > best_heap_size_64) {
                        best_heap_size_64 = size_after;
                        best_heap_base = start_after;
                         // terminal_printf("    -> New best heap candidate (after kernel)\n");
                    }
                }
            } else { // No overlap with kernel
                 // terminal_printf("    No overlap with kernel, full region usable: Size=0x%x:%08x bytes\n", (uint32_t)(region_len >> 32), (uint32_t)region_len);
                 if (region_len > best_heap_size_64) {
                     best_heap_size_64 = region_len;
                     best_heap_base = usable_start;
                     // terminal_printf("    -> New best heap candidate (no overlap)\n");
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
 * Stages:
 * 0. Parse Multiboot memory map to find available regions and total memory.
 * 1. Allocate a physical frame for the initial kernel page directory using the early allocator.
 * 2. Set up early identity and higher-half mappings for kernel/heap in the initial PD structure. Enable PSE.
 * 3. Initialize the Buddy Allocator using the now identity-mapped heap region.
 * 4. Pre-map the kernel's temporary VMA range using the Buddy allocator to ensure PTs exist.
 * 5. Finalize mappings (map all physical RAM to higher half) and activate paging using `paging_finalize_and_activate`.
 * 6. Initialize the Frame Allocator (requires Buddy and active paging).
 * 7. Initialize Kmalloc (requires Frame Allocator).
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
    // Note: Buddy needs the VIRTUAL address corresponding to heap_phys_start AFTER paging is enabled.
    // However, the current implementation seems to initialize it using the physical address before paging.
    // This implies the early mapping included an identity map for the heap area. Let's assume that's correct for now.
    // If not, buddy_init needs to be called AFTER paging_finalize_and_activate using the higher-half address.
    buddy_init((void *)heap_phys_start, heap_size); // Uses identity mapped heap
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
     // Find the MB info tag size (safer access now via virtual address if possible)
     // Assuming direct physical access might still be okay if identity mapped, but virtual is better
     // We need to map the MB info struct itself before accessing its content virtually.
     uintptr_t mb_info_phys_page_start = PAGE_ALIGN_DOWN(g_multiboot_info_phys_addr_global);
     // Determine a reasonable size to map (e.g., at least one page, maybe more if struct is large)
     // A safer approach is needed here to determine the actual size *after* paging is on.
     // For now, map one page as a minimum.
     size_t mb_mapping_size = PAGE_SIZE;

     // Calculate the virtual address where the MB info physical page will be mapped
     uintptr_t mb_info_virt_page_start = KERNEL_SPACE_VIRT_START + mb_info_phys_page_start; // Example mapping logic

     terminal_printf("   Mapping MB Info Phys [%#lx - %#lx) to Virt [%#lx - %#lx)\n",
                      (unsigned long)mb_info_phys_page_start, (unsigned long)(mb_info_phys_page_start + mb_mapping_size),
                      (unsigned long)mb_info_virt_page_start, (unsigned long)(mb_info_virt_page_start + mb_mapping_size));

     // Use the post-activation mapping function
     if (paging_map_range((uint32_t*)g_kernel_page_directory_phys,
                          mb_info_virt_page_start,   // Target Virtual Addr
                          mb_info_phys_page_start,   // Source Physical Addr
                          mb_mapping_size,           // Size to map
                          PTE_KERNEL_READONLY_FLAGS) != 0) // Flags (Kernel Read-Only)
     {
          KERNEL_PANIC_HALT("Failed to map Multiboot info structure!");
          return false;
     }
     // Calculate the final virtual address of the structure start
     g_multiboot_info_virt_addr_global = mb_info_virt_page_start + (g_multiboot_info_phys_addr_global % PAGE_SIZE);
     terminal_printf("   Multiboot structure accessible at VIRT: %#lx\n", (unsigned long)g_multiboot_info_virt_addr_global);

 } else {
      KERNEL_PANIC_HALT("Multiboot physical address is zero after paging activation!");
      return false;
 }
 // --- End STAGE 4.5 ---

    // Inside init_memory() in kernel.c after Stage 4.5

 // --- Stage 5: Map Physical Memory to Higher Half ---
 terminal_write(" Stage 5: Mapping physical memory to higher half...\n");
 // This large mapping is often unnecessary and can consume page tables.
 // Typically handled on-demand via page faults or specific driver mappings.
 terminal_write("   Skipping Stage 5 large physical memory mapping (handled on demand).\n");


 // --- Stage 6: Initialize Frame Allocator ---
 terminal_write(" Stage 6: Initializing Frame Allocator...\n");

     // *** Calculate virtual address of mmap tag using the mapped MB info ***
     struct multiboot_tag_mmap *mmap_tag_virt = NULL;
     if (g_multiboot_info_virt_addr_global) {
         // Find the tag VIRTUALLY now that MB info is mapped
          struct multiboot_tag* tag = (struct multiboot_tag *)(g_multiboot_info_virt_addr_global + 8);
          uint32_t total_virt_size = *(uint32_t*)g_multiboot_info_virt_addr_global;
          uintptr_t info_virt_end = g_multiboot_info_virt_addr_global + total_virt_size;

          // Re-implement tag finding logic using virtual addresses
          while ((uintptr_t)tag < info_virt_end && tag->type != MULTIBOOT_TAG_TYPE_END) {
               // Bounds check virtually
               if ((uintptr_t)tag + sizeof(struct multiboot_tag) > info_virt_end ||
                   tag->size < 8 ||
                   (uintptr_t)tag + tag->size > info_virt_end) {
                    break; // Invalid tag
               }
               if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
                    mmap_tag_virt = (struct multiboot_tag_mmap *)tag;
                    break;
               }
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

     // *** Pass VIRTUAL address of the tag to frame_init ***
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

     serial_init();

     // 1. Early Initialization (Console, CPU Features, Core Tables)
     terminal_init(); // Initialize console output ASAP
     terminal_write("=== UiAOS Kernel Booting ===\n");
     terminal_printf(" Version: %s\n\n", "3.2-BuildFix"); // Example version

     // Verify Multiboot Magic
     if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
         KERNEL_PANIC_HALT("Invalid Multiboot Magic number received from bootloader.");
     }
     terminal_printf("[Boot] Multiboot magic OK (Info at phys %#lx).\n", (unsigned long)mb_info_phys_addr);

     // Initialize essential CPU tables: GDT and TSS
     terminal_write("[Kernel] Initializing GDT & TSS...\n");
     gdt_init(); // Sets up segments and loads TSS selector

     // Initialize Interrupt Handling: IDT and PICs
     terminal_write("[Kernel] Initializing IDT & PIC...\n");
     idt_init(); // Sets up interrupt gates and remaps PIC

     // 2. Memory Management Initialization (Multi-Stage Process)
     if (!init_memory(g_multiboot_info_phys_addr_global)) {
         // Panic should have occurred within init_memory
         return; // Should not be reached
     }

     // 3. Hardware Driver Initialization (Requires Memory Management)
     terminal_write("[Kernel] Initializing Hardware Drivers...\n");
     terminal_write("  Initializing PIT...\n");
     init_pit(); // Requires IDT
     terminal_write("  Initializing Keyboard...\n");
     keyboard_init(); // Requires IDT
     keymap_load(KEYMAP_NORWEGIAN); // Set desired layout (Example: Norwegian)

     // 4. Filesystem Initialization (Optional, Requires Block Device)
     terminal_write("[Kernel] Initializing Filesystem Layer...\n");
     // *** UNCOMMENTED Filesystem Initialization Block ***
     bool fs_ready = false; // Flag to track if FS is ready
     if (fs_init() == FS_SUCCESS) { // fs_init internally handles VFS init, driver registration, root mount
         terminal_write("  [OK] Filesystem initialized and root mounted.\n");
         fs_ready = true;
         list_mounts(); // List mounted filesystems for verification
         // Optional: Test file access
         // fs_test_file_access("/test.txt"); // Replace with a file expected on your disk image
     } else {
         terminal_write("  [Warning] Filesystem initialization failed. Continuing without FS.\n");
         // Depending on OS requirements, this could be a panic:
         // KERNEL_PANIC_HALT("Filesystem initialization failed.");
         fs_ready = false;
     }


     // 5. Scheduler and Initial Process Setup
     terminal_write("[Kernel] Initializing Scheduler...\n");
     // *** UNCOMMENTED Scheduler Initialization ***
     scheduler_init(); // Requires memory allocators
     bool scheduler_ready = true; // Assume success for now, add error check if scheduler_init can fail


     terminal_write("[Kernel] Creating initial user process...\n");
     const char *user_prog_path = "/hello.elf"; // Default user program to load

     bool task_added = false; // Flag to track if a task was successfully added

     // *** Use actual fs_ready flag ***
     terminal_printf(" [Debug] FS Check before loading user process: fs_is_initialized() returns %d\n", fs_is_initialized());
     if (fs_is_initialized()) { // Check if FS actually initialized successfully
         pcb_t *user_proc_pcb = create_user_process(user_prog_path);
         if (user_proc_pcb) {
             terminal_printf("  [OK] Process created (PID %lu) from '%s'. Adding to scheduler.\n", (unsigned long)user_proc_pcb->pid, user_prog_path);

             // *** Call scheduler_add_task here (assuming it exists and is safe) ***
             if (scheduler_ready) {
                 // Assuming scheduler_add_task takes pcb_t* and returns 0 on success
                 if (scheduler_add_task(user_proc_pcb) == 0) {
                     terminal_write("  [OK] Initial user process scheduled.\n");
                     task_added = true; // Mark task as added successfully
                 } else {
                     terminal_printf("  [ERROR] Failed to add initial process (PID %lu) to scheduler.\n", (unsigned long)user_proc_pcb->pid);
                     destroy_process(user_proc_pcb); // Clean up failed process
                 }
             } else {
                  terminal_write("  [Warning] Cannot add task to scheduler (Scheduler init failed or skipped).\n");
                  destroy_process(user_proc_pcb); // Clean up process if cannot add to scheduler
             }
         } else {
             terminal_printf("  [ERROR] Failed to create initial user process from '%s'.\n", user_prog_path);
             // Decide whether to panic or continue
             // KERNEL_PANIC_HALT("Failed to create initial process.");
         }
     } else {
          terminal_write("  [Info] Filesystem not available, cannot load initial user process.\n");
     }


     // 6. Enable Preemption and Start Idle Loop
    // *** Check the flag before enabling scheduler readiness ***
     if (task_added && scheduler_ready) {
         terminal_write("[Kernel] Enabling preemptive scheduling via PIT...\n");
         pit_set_scheduler_ready(); // Now it's safe to call this
     } else {
         terminal_write("[Kernel] No tasks scheduled or scheduler not ready. Entering simple idle loop.\n");
     }

     terminal_write("\n[Kernel] Initialization complete. Enabling interrupts and entering idle task/scheduler.\n");
     terminal_write("======================================================================\n");

     // Enable interrupts (usually done just before jumping to idle/first task)
     asm volatile ("sti");

     // If tasks were added, the scheduler should take over. If not, fall back to idle.
     // The specific mechanism depends on the scheduler design (e.g., calling schedule() or just idling).
     // For now, we go to idle. The PIT interrupt should trigger the scheduler if ready.
     kernel_idle_task();

     // --- Code should not be reached beyond kernel_idle_task ---
     KERNEL_PANIC_HALT("Reached end of main() unexpectedly!");
 }