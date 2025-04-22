/**
 * kernel.c - Main kernel entry point for UiAOS
 *
 * Author: Group 14 (UiA) & Gemini Assistance (Refined)
 * Version: 4.2 (Linker fixes for main and global variables) // Version remains 4.2 conceptually
 *
 * Description:
 * This file contains the main entry point (`main`) for the UiAOS kernel,
 * invoked by the Multiboot2 bootloader via assembly (`_start`). It orchestrates
 * the initialization sequence for all major kernel subsystems... (rest of description unchanged)
 */

// === Standard/Core Headers ===
#include <multiboot2.h>      // Multiboot2 specification header
#include "types.h"          // Core type definitions (uintptr_t, size_t, bool, etc.)
#include <string.h>         // Kernel's string functions (memcpy, memset, strcmp)
#include <libc/stdint.h>    // Fixed-width integers (SIZE_MAX, uint32_t)
#include <libc/stddef.h>    // NULL, offsetof

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
#include "vfs.h"            // Virtual File System interface
#include "mount.h"          // Filesystem mounting
#include "fs_init.h"        // Filesystem layer initialization
#include "fs_errno.h"       // Filesystem error codes
#include "read_file.h"      // Helper to read entire files
#include "sys_file.h"       // System file interface
#include "syscall.h"        // System call interface

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
#include "serial.h"         // Serial port driver/logging
#include "assert.h"         // For KERNEL_ASSERT, KERNEL_PANIC_HALT
#include "port_io.h"        // For outb (needed if PIC masking is used)

// === Constants ===
#define KERNEL_VERSION "4.2" // Define kernel version
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
#define MIN_HEAP_SIZE (1 * 1024 * 1024) // Minimum acceptable heap size (1MB)
#define MAX_INITIAL_HEAP_SIZE (256 * 1024 * 1024) // Optional: Limit initial heap size (e.g., 256MB)

// === Utility Macros ===
#ifndef ARRAY_SIZE // Avoid redefinition if defined elsewhere
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

// Define MIN/MAX if not available from a standard header
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif


// === Linker Symbols ===
// These symbols are defined in the linker script (linker.ld) and mark
// the physical start and end addresses of various kernel sections.
// Using uint8_t* treats them as byte addresses.
extern uint8_t _kernel_start_phys;      // Physical start address of kernel code/data
extern uint8_t _kernel_end_phys;        // Physical end address of kernel code/data
extern uint8_t _kernel_text_start_phys; // Start of .text section (physical)
extern uint8_t _kernel_text_end_phys;   // End of .text section (physical)
extern uint8_t _kernel_rodata_start_phys; // Start of .rodata section (physical)
extern uint8_t _kernel_rodata_end_phys; // End of .rodata section (physical)
extern uint8_t _kernel_data_start_phys; // Start of .data/.bss section (physical)
extern uint8_t _kernel_data_end_phys;   // End of .data/.bss section (physical)
extern uint8_t _kernel_virtual_base;    // Kernel's virtual base address (higher half)


// === Global Variables ===
// *** FIX: Remove 'static' to give these global linkage ***
// Global variable to store the Multiboot info PHYSICAL address (passed by bootloader)
/* static */ uint32_t g_multiboot_info_phys_addr_global = 0;
// Global variable to store the Multiboot info VIRTUAL address (set after paging)
/* static */ uintptr_t g_multiboot_info_virt_addr_global = 0;


// === Static Function Prototypes ===
// These functions are only used within this file, so 'static' is appropriate.
static struct multiboot_tag *find_multiboot_tag_phys(uint32_t mb_info_phys_addr, uint16_t type);
static struct multiboot_tag *find_multiboot_tag_virt(uintptr_t mb_info_virt_addr, uint16_t type);
static bool parse_memory_map(struct multiboot_tag_mmap *mmap_tag,
                             uintptr_t *out_total_memory,
                             uintptr_t *out_heap_base_addr, size_t *out_heap_size);
static bool init_memory(uint32_t mb_info_phys_addr);
static void launch_initial_process(void);

// *** Implementation of static functions (find_multiboot_tag_phys, find_multiboot_tag_virt, parse_memory_map, init_memory, launch_initial_process) remains unchanged ***
// ... (Scroll down for the main function) ...
// === Multiboot Tag Finding Helper (Physical Address Version) ===
/**
 * @brief Finds a Multiboot 2 tag by type using physical addresses.
 * @ingroup boot
 *
 * Iterates through the Multiboot 2 information structure (at its physical address)
 * to find a tag of the specified type. Includes bounds checking for robustness.
 * This version is safe to call BEFORE paging is fully enabled.
 *
 * @param mb_info_phys_addr Physical address of the Multiboot 2 info structure.
 * @param type The type of tag to search for (MULTIBOOT_TAG_TYPE_*).
 * @return Pointer (physical address) to the found tag, or NULL if not found or an error occurs.
 */
static struct multiboot_tag *find_multiboot_tag_phys(uint32_t mb_info_phys_addr, uint16_t type) {
    // Basic validation: Ensure info address is within a reasonable range (e.g., below 1MB)
    // This memory area is expected to be identity-mapped by the bootloader.
    if (mb_info_phys_addr == 0 || mb_info_phys_addr >= 0x100000) {
        terminal_write("[Boot Error] Multiboot info physical address invalid or inaccessible early.\n");
        return NULL;
    }

    // Access physical memory directly using volatile pointers to prevent optimization issues.
    volatile uint32_t* mb_info_ptr = (volatile uint32_t*)((uintptr_t)mb_info_phys_addr);
    uint32_t total_size = mb_info_ptr[0]; // Read total size (first field)

    // Basic size sanity check (e.g., min 8 bytes for header, max reasonable size like 1MB)
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
        if (current_tag_addr + offsetof(struct multiboot_tag, size) + sizeof(tag->size) > info_end) {
             terminal_printf("[Boot Error] Multiboot tag header (type/size) OOB (Tag Addr=%p, Info End=%p).\n", (void*)current_tag_addr, (void*)info_end);
             return NULL;
        }
        // Bounds check 2: Ensure the tag size field is reasonable and the full tag fits
        // Size must be at least the size of the basic tag structure.
        if (tag->size < 8 || (current_tag_addr + tag->size) > info_end) { // Minimum tag size is 8 bytes (type+size)
            terminal_printf("[Boot Error] Multiboot tag invalid size %u at Addr=%p (Info End=%p).\n", tag->size, (void*)current_tag_addr, (void*)info_end);
            return NULL;
        }

        // Check if this is the tag we're looking for
        if (tag->type == type) {
            return (struct multiboot_tag *)tag; // Found (cast away volatile)
        }

        // Advance to the next tag, ensuring 8-byte alignment
        uintptr_t next_tag_addr = current_tag_addr + ((tag->size + 7) & ~7);

        // Bounds check 3: Ensure next tag address is valid (progresses) and within limits
        if (next_tag_addr <= current_tag_addr || next_tag_addr >= info_end) {
             terminal_printf("[Boot Error] Multiboot next tag address invalid/OOB (Next Addr=%p).\n", (void*)next_tag_addr);
             break; // Invalid progression
        }
        tag = (volatile struct multiboot_tag *)next_tag_addr;
    }

    return NULL; // End tag reached or tag not found
}

// === Multiboot Tag Finding Helper (Virtual Address Version) ===
/**
 * @brief Finds a Multiboot 2 tag by type using virtual addresses.
 * @ingroup boot
 *
 * Iterates through the Multiboot 2 information structure (at its mapped virtual address)
 * to find a tag of the specified type. Includes bounds checking.
 * This version MUST only be called AFTER the Multiboot structure has been mapped
 * into the kernel's virtual address space.
 *
 * @param mb_info_virt_addr Virtual address of the mapped Multiboot 2 info structure.
 * @param type The type of tag to search for (MULTIBOOT_TAG_TYPE_*).
 * @return Pointer (virtual address) to the found tag, or NULL if not found or an error occurs.
 */
static struct multiboot_tag *find_multiboot_tag_virt(uintptr_t mb_info_virt_addr, uint16_t type) {
    if (mb_info_virt_addr == 0) {
        terminal_write("[Kernel Error] Cannot find Multiboot tag with NULL virtual address.\n");
        return NULL;
    }

    // Access via standard (non-volatile) pointers now that MMU handles caching etc.
    uint32_t* mb_info_ptr = (uint32_t*)mb_info_virt_addr;
    uint32_t total_size = mb_info_ptr[0]; // Read size via virtual address

    // Basic size sanity check (same as physical version)
    if (total_size < 8 || total_size > 0x100000) {
        terminal_printf("[Kernel Error] Multiboot (virtual) total_size (%lu) invalid.\n", (unsigned long)total_size);
        return NULL;
    }

    // Tags start at offset 8 from the virtual base
    struct multiboot_tag *tag = (struct multiboot_tag *)(mb_info_virt_addr + 8);
    uintptr_t info_end = mb_info_virt_addr + total_size;

    // Iterate through tags
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        uintptr_t current_tag_addr = (uintptr_t)tag;

        // Bounds checks (similar to physical version, using virtual addresses)
         if (current_tag_addr + offsetof(struct multiboot_tag, size) + sizeof(tag->size) > info_end) {
             terminal_printf("[Kernel Error] Multiboot tag (virtual) header OOB (Tag Addr=%p, Info End=%p).\n", (void*)current_tag_addr, (void*)info_end);
             return NULL;
        }
        if (tag->size < 8 || (current_tag_addr + tag->size) > info_end) {
            terminal_printf("[Kernel Error] Multiboot tag (virtual) invalid size %u at Addr=%p (Info End=%p).\n", tag->size, (void*)current_tag_addr, (void*)info_end);
            return NULL;
        }

        if (tag->type == type) {
            return tag; // Found
        }

        // Advance to the next tag (8-byte aligned)
        uintptr_t next_tag_addr = current_tag_addr + ((tag->size + 7) & ~7);

        // Bounds check for next tag address
        if (next_tag_addr <= current_tag_addr || next_tag_addr >= info_end) {
            terminal_printf("[Kernel Error] Multiboot next tag (virtual) address invalid/OOB (Next Addr=%p).\n", (void*)next_tag_addr);
            break;
        }
        tag = (struct multiboot_tag *)next_tag_addr;
    }

    return NULL; // Not found
}


// === Memory Area Parsing Helper ===
// Helper to safely add a 64-bit length to a 32-bit base address, handling overflow.
static inline uintptr_t safe_add_base_len(uintptr_t base, uint64_t len) {
    // Check if adding length would exceed the maximum value for uintptr_t (SIZE_MAX)
    if (len > ((uint64_t)UINTPTR_MAX - base)) {
        return UINTPTR_MAX; // Overflow, clamp to max uintptr_t
    }
    return base + (uintptr_t)len; // Safe to cast len (within 32-bit range) and add
}


/**
 * @brief Parses the Multiboot memory map to find total memory and a suitable heap region.
 * @ingroup mm
 *
 * Iterates through the memory map entries provided by Multiboot. It calculates the
 * total physical memory span and identifies the largest available memory region
 * above 1MB (and not overlapping the kernel) to be used for the initial kernel heap
 * (managed by the buddy allocator initially).
 * This function expects the MMAP tag to be accessible (either physically or virtually).
 *
 * @param mmap_tag Pointer to the Multiboot memory map tag.
 * @param out_total_memory Pointer to store the total detected physical memory span (aligned up).
 * @param out_heap_base_addr Pointer to store the physical start address of the chosen heap region.
 * @param out_heap_size Pointer to store the size of the chosen heap region.
 * @return true on success, false if no suitable heap region is found or MMAP tag is invalid.
 */
static bool parse_memory_map(struct multiboot_tag_mmap *mmap_tag,
                             uintptr_t *out_total_memory,
                             uintptr_t *out_heap_base_addr, size_t *out_heap_size)
{
    // Use KERNEL_ASSERT with expression and message string
    KERNEL_ASSERT(mmap_tag != NULL, "MMAP tag pointer cannot be NULL");
    KERNEL_ASSERT(out_total_memory != NULL, "Output total memory pointer cannot be NULL");
    KERNEL_ASSERT(out_heap_base_addr != NULL, "Output heap base pointer cannot be NULL");
    KERNEL_ASSERT(out_heap_size != NULL, "Output heap size pointer cannot be NULL");

    uintptr_t current_total_memory = 0;
    uintptr_t best_heap_base = 0;
    uint64_t best_heap_size_64 = 0; // Use 64-bit for potentially large region sizes
    multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
    // Calculate end boundary based on the tag header's size field
    uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;

    // Get kernel physical boundaries using linker symbols
    uintptr_t kernel_start_phys_addr = (uintptr_t)&_kernel_start_phys;
    // Align kernel end UP to the nearest page boundary to ensure the whole kernel is excluded
    uintptr_t kernel_end_phys_addr = ALIGN_UP((uintptr_t)&_kernel_end_phys, PAGE_SIZE);
     // Handle potential overflow during alignment if _kernel_end_phys is very high
    if (kernel_end_phys_addr == 0 && (uintptr_t)&_kernel_end_phys > 0) {
        kernel_end_phys_addr = UINTPTR_MAX;
    }

    terminal_printf("  Kernel physical region: [%#lx - %#lx)\n",
                    (unsigned long)kernel_start_phys_addr, (unsigned long)kernel_end_phys_addr);
    terminal_write("  Memory Map (from Multiboot):\n");

    // --- First Pass: Calculate total memory span and log regions ---
    int region_count = 0;
    uint64_t total_available_memory = 0; // Track sum of available regions
    multiboot_memory_map_t *temp_entry = mmap_entry;

    while ((uintptr_t)temp_entry < mmap_end) {
        // Validate entry size before use (should be constant within a tag)
        // Ensure mmap_tag itself is valid before dereferencing entry_size
        if ( (uintptr_t)mmap_tag + offsetof(struct multiboot_tag_mmap, entry_size) + sizeof(mmap_tag->entry_size) > mmap_end ) {
             terminal_printf("  [ERR] MMAP tag structure invalid (cannot read entry_size).\n");
             return false;
        }
        if (mmap_tag->entry_size == 0 || mmap_tag->entry_size < sizeof(multiboot_memory_map_t)) {
            terminal_printf("  [ERR] MMAP entry size (%u) invalid!\n", mmap_tag->entry_size);
            return false; // Cannot proceed
        }
        // Ensure the full entry fits within the tag bounds before accessing its members
        if ((uintptr_t)temp_entry + mmap_tag->entry_size > mmap_end) {
             terminal_printf("  [ERR] MMAP entry structure exceeds tag boundary.\n");
             break; // Stop parsing this potentially corrupt tag
        }

        // Extract region details (address is 64-bit in struct, but we use 32-bit OS)
        uintptr_t region_start = (uintptr_t)temp_entry->addr; // Truncate address to 32-bit uintptr_t
        uint64_t region_len = temp_entry->len;                // Keep length 64-bit
        uintptr_t region_end = safe_add_base_len(region_start, region_len); // Use safe add for end calculation

        // Log entry details (using correct formats for 32/64 bit values)
        // %lu for unsigned long (commonly 32-bit on i386), print 64-bit hex pair for length
        terminal_printf("    Entry %d: Addr=%#010lx, Len=0x%08lx%08lx (%s)\n",
                        region_count,
                        (unsigned long)region_start,
                        (unsigned long)(region_len >> 32), (unsigned long)region_len, // Print 64-bit length
                        (temp_entry->type == MULTIBOOT_MEMORY_AVAILABLE) ? "Available" : "Reserved/Other");

        // Update total physical memory span based on the highest end address found
        if (region_end > current_total_memory) {
            current_total_memory = region_end;
        }

        // Accumulate total *available* memory size
        if (temp_entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
            // Use safe 64-bit addition
            if (UINT64_MAX - total_available_memory < region_len) {
                total_available_memory = UINT64_MAX; // Prevent overflow
            } else {
                total_available_memory += region_len;
            }
        }

        region_count++;

        // Advance to the next entry safely, using the entry size provided in the tag header
        uintptr_t next_entry_addr = (uintptr_t)temp_entry + mmap_tag->entry_size;
        // Check if next address would still be within the tag bounds
        // Use >= because the loop condition is < mmap_end
        if (next_entry_addr >= mmap_end && (uintptr_t)temp_entry + mmap_tag->entry_size != mmap_end) {
             // If next addr is exactly mmap_end, it's okay for the last entry, otherwise error
             if (next_entry_addr != mmap_end) {
                  terminal_printf("  [ERR] MMAP next entry address calculation exceeds tag boundary.\n");
             }
             break; // Reached or exceeded end
        }
         // Check for infinite loop if size is 0 or advancement doesn't happen
         if(next_entry_addr <= (uintptr_t)temp_entry) {
              terminal_printf("  [ERR] MMAP entry size invalid, cannot advance.\n");
              return false;
         }
        temp_entry = (multiboot_memory_map_t *)next_entry_addr;
    }

    terminal_printf("  Found %d memory regions. Total physical span ends at: %#lx\n",
                    region_count, (unsigned long)current_total_memory);
    terminal_printf("  Total AVAILABLE memory: 0x%08lx%08lx bytes (~%lu MB)\n",
                    (unsigned long)(total_available_memory >> 32), (unsigned long)total_available_memory, // Print 64-bit
                    (unsigned long)(total_available_memory / (1024*1024))); // Approx MB

    // Safety check: If MMAP parsing failed to find *any* memory, panic.
    if (current_total_memory == 0) {
        terminal_write("  [FATAL] No valid memory regions found in MMAP!\n");
        return false; // Cannot proceed without memory info
    }

    // --- Second Pass: Find the best heap region ---
    // Iterate again to find the largest suitable *available* region for the initial heap.
    mmap_entry = mmap_tag->entries; // Reset entry pointer
    while ((uintptr_t)mmap_entry < mmap_end) {
         // Basic validation (as above) - Check tag validity first
         if ( (uintptr_t)mmap_tag + offsetof(struct multiboot_tag_mmap, entry_size) + sizeof(mmap_tag->entry_size) > mmap_end ) return false;
         if (mmap_tag->entry_size == 0 || mmap_tag->entry_size < sizeof(multiboot_memory_map_t)) return false;
         if ((uintptr_t)mmap_entry + mmap_tag->entry_size > mmap_end) break;

        uintptr_t region_start = (uintptr_t)mmap_entry->addr;
        uint64_t region_len = mmap_entry->len;
        uintptr_t region_end = safe_add_base_len(region_start, region_len);

        // Consider only AVAILABLE memory regions that start at or after 1MB (0x100000)
        if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE && region_start >= 0x100000) {
            uintptr_t usable_start = region_start;
            uintptr_t usable_end = region_end;
            uint64_t usable_len = region_len;

            // Check for overlap with the kernel physical region [kernel_start_phys_addr, kernel_end_phys_addr)
            // Find the intersection range [overlap_start, overlap_end)
            uintptr_t overlap_start = MAX(usable_start, kernel_start_phys_addr);
            uintptr_t overlap_end = MIN(usable_end, kernel_end_phys_addr);

            if (overlap_start < overlap_end) { // Kernel overlaps with this region
                // Kernel is within this available region. We might be able to use parts
                // of this region *before* or *after* the kernel.

                // Part 1: Check space *before* the kernel
                if (usable_start < kernel_start_phys_addr) {
                    uint64_t size_before = (uint64_t)kernel_start_phys_addr - usable_start;
                    if (size_before > best_heap_size_64) {
                        best_heap_size_64 = size_before;
                        best_heap_base = usable_start;
                    }
                }
                // Part 2: Check space *after* the kernel
                if (usable_end > kernel_end_phys_addr) {
                    uintptr_t start_after = kernel_end_phys_addr; // Start right after aligned kernel end
                    // Calculate size carefully using 64-bit arithmetic before casting
                    uint64_t size_after = 0;
                    if (usable_end >= start_after) { // Ensure no underflow possibility
                         size_after = (uint64_t)usable_end - start_after;
                    }

                    if (size_after > best_heap_size_64) {
                        best_heap_size_64 = size_after;
                        best_heap_base = start_after;
                    }
                }
            } else { // No overlap with kernel
                // The entire available region is usable. Check if it's the best one found so far.
                if (usable_len > best_heap_size_64) {
                     best_heap_size_64 = usable_len;
                     best_heap_base = usable_start;
                }
            }
        } // End if AVAILABLE and >= 1MB

        // Advance to the next entry
        uintptr_t next_entry_addr = (uintptr_t)mmap_entry + mmap_tag->entry_size;
        // Adjust check for loop condition
         if (next_entry_addr >= mmap_end && (uintptr_t)mmap_entry + mmap_tag->entry_size != mmap_end) {
             if (next_entry_addr != mmap_end) {
                  terminal_printf("  [ERR] MMAP next entry address calculation exceeds tag boundary (pass 2).\n");
             }
             break;
        }
         // Check for infinite loop
         if(next_entry_addr <= (uintptr_t)mmap_entry) {
              terminal_printf("  [ERR] MMAP entry size invalid, cannot advance (pass 2).\n");
              return false;
         }
        mmap_entry = (multiboot_memory_map_t *)next_entry_addr;
    } // End while loop through mmap entries for heap finding

    terminal_printf("  MMAP Parsing Result: Best Heap Candidate PhysBase=%#lx, Size64=0x%08lx%08lx\n",
                    (unsigned long)best_heap_base,
                    (unsigned long)(best_heap_size_64 >> 32),
                    (unsigned long)best_heap_size_64);

    // --- Final checks and output assignment ---
    if (best_heap_size_64 >= MIN_HEAP_SIZE && best_heap_base != 0) {
        *out_heap_base_addr = best_heap_base;

        // Clamp 64-bit size to size_t (typically 32-bit on i386) for output
        size_t final_heap_size;
        if (best_heap_size_64 > (uint64_t)SIZE_MAX) {
            terminal_write("  [Warning] Largest heap region exceeds 32-bit size_t! Clamping to SIZE_MAX.\n");
            final_heap_size = SIZE_MAX;
        } else {
            final_heap_size = (size_t)best_heap_size_64;
        }

        // Optional: Further clamp heap size to a maximum limit if defined
        if (final_heap_size > MAX_INITIAL_HEAP_SIZE) {
            terminal_printf("  [Info] Clamping initial heap size from %lu MB to %lu MB (MAX_INITIAL_HEAP_SIZE).\n",
                            (unsigned long)(final_heap_size / (1024 * 1024)),
                            (unsigned long)(MAX_INITIAL_HEAP_SIZE / (1024 * 1024)));
            final_heap_size = MAX_INITIAL_HEAP_SIZE;
        }

        // Ensure the final (potentially clamped) size still meets the minimum requirement
        if (final_heap_size < MIN_HEAP_SIZE) {
            terminal_printf("  [FATAL] Final heap size (%lu bytes) is less than minimum required (%u bytes)!\n",
                            (unsigned long)final_heap_size, MIN_HEAP_SIZE);
            return false;
        }
        *out_heap_size = final_heap_size;

    } else {
        terminal_printf("  [FATAL] No suitable memory region found >= %u bytes for kernel heap!\n", MIN_HEAP_SIZE);
        return false;
    }

    // Set total physical memory span (aligned up to page size)
    // Use the `current_total_memory` calculated in the first pass.
    *out_total_memory = current_total_memory;
    if (*out_total_memory == 0) {
        terminal_write("  [FATAL] Total memory span calculated as zero!\n");
        return false;
    } else if (*out_total_memory < UINTPTR_MAX) {
        // Align final total memory UP to page size, checking for overflow
        uintptr_t aligned_total = PAGE_ALIGN_UP(*out_total_memory);
        if (aligned_total == 0 && *out_total_memory > 0) { // Check overflow from alignment
            terminal_printf("  [Warning] PAGE_ALIGN_UP overflowed for total memory %#lx. Using UINTPTR_MAX.\n", (unsigned long)*out_total_memory);
            *out_total_memory = UINTPTR_MAX;
        } else {
            *out_total_memory = aligned_total;
        }
    }
    // If already UINTPTR_MAX, leave it.

    terminal_printf("  Total Physical Memory Span (Aligned Up): %#lx bytes (~%lu MB)\n",
                    (unsigned long)*out_total_memory, (unsigned long)(*out_total_memory / (1024 * 1024)));
    terminal_printf("  Selected Initial Heap Region (Final): Phys Addr=%#lx, Size=%lu bytes (%lu KB)\n",
                    (unsigned long)*out_heap_base_addr, (unsigned long)*out_heap_size, (unsigned long)(*out_heap_size / 1024));
    return true;
} // End of parse_memory_map


// === Memory Subsystem Initialization ===
/**
 * @brief Initializes all core memory management subsystems.
 * @ingroup mm
 *
 * Orchestrates the complex initialization sequence for Paging, Buddy Allocator,
 * Frame Allocator, and Kmalloc. This involves multiple stages to correctly
 * handle dependencies (e.g., needing a physical allocator before virtual mapping,
 * but needing virtual mapping for the frame allocator's metadata).
 *
 * @param mb_info_phys_addr Physical address of the Multiboot 2 info structure.
 * @return true on success. Panics internally on critical failure.
 */
static bool init_memory(uint32_t mb_info_phys_addr) {
    terminal_write("[Kernel] Initializing Memory Subsystems...\n");

    // --- Stage 0: Parse Multiboot Memory Map (using physical addresses) ---
    // Before paging is enabled, we must access the Multiboot structure via its physical address.
    terminal_write(" Stage 0: Parsing Multiboot Memory Map (Physical Access)...\n");
    struct multiboot_tag_mmap *mmap_tag_phys = (struct multiboot_tag_mmap *)find_multiboot_tag_phys(
        mb_info_phys_addr, MULTIBOOT_TAG_TYPE_MMAP);
    if (!mmap_tag_phys) {
        KERNEL_PANIC_HALT("Multiboot memory map tag not found!");
        // return false; // Unreachable due to PANIC
    }

    uintptr_t total_memory = 0;
    uintptr_t heap_phys_start = 0;
    size_t heap_size = 0;
    // parse_memory_map works with physical addresses derived from the tag.
    if (!parse_memory_map(mmap_tag_phys, &total_memory, &heap_phys_start, &heap_size)) {
        // parse_memory_map prints specific error messages.
        KERNEL_PANIC_HALT("Failed to parse memory map or find suitable heap region!");
        // return false; // Unreachable
    }
    // Redundant check, but ensures clarity
    if (heap_size < MIN_HEAP_SIZE || heap_phys_start == 0 || total_memory == 0) {
        KERNEL_PANIC_HALT("Invalid memory parameters after MMAP parse!");
       // return false; // Unreachable
    }

    uintptr_t kernel_phys_start = (uintptr_t)&_kernel_start_phys;
    uintptr_t kernel_phys_end = (uintptr_t)&_kernel_end_phys; // Use non-aligned end for buddy_init exclusion
    terminal_printf("   Kernel Phys Region Used by Buddy Init: [%#lx - %#lx)\n", (unsigned long)kernel_phys_start, (unsigned long)kernel_phys_end);
    // Corrected printf format string (removed extra '<')
    terminal_printf("   Heap Phys Region For Buddy Init:     [%#lx - %#lx) Size: %lu KB\n", (unsigned long)heap_phys_start, (unsigned long)(heap_phys_start + heap_size), (unsigned long)(heap_size / 1024));
    terminal_printf("   Total Phys Memory Span Detected:     %lu MB\n", (unsigned long)(total_memory / (1024 * 1024)));

    // --- Stage 1: Initialize Paging Structures (No Allocation Yet) ---
    // Prepare the initial page directory structure. This might involve internal
    // static allocation or very early bootstrap allocation if designed that way.
    terminal_write(" Stage 1: Initializing Page Directory structure...\n");
    uintptr_t initial_pd_phys;
    if (paging_initialize_directory(&initial_pd_phys) != 0) {
        KERNEL_PANIC_HALT("Failed to initialize initial Page Directory!");
       // return false; // Unreachable
    }
    // Add assert with message
    KERNEL_ASSERT(initial_pd_phys != 0, "Initial Page Directory physical address is NULL");
    terminal_printf("   Initial Page Directory at Phys Addr: %#lx\n", (unsigned long)initial_pd_phys);

    // --- Stage 2: Setup Early Mappings (Kernel Higher-Half, Heap Identity) ---
    // Map the kernel code/data to the higher half and identity map the physical heap region.
    // This requires allocating page tables, which paging_setup_early_maps must handle
    // using a primitive physical allocator (or static allocation) since buddy isn't ready.
    terminal_write(" Stage 2: Setting up early kernel and heap mappings...\n");
    if (paging_setup_early_maps(initial_pd_phys, kernel_phys_start, (uintptr_t)&_kernel_end_phys, heap_phys_start, heap_size) != 0) {
        KERNEL_PANIC_HALT("Failed to setup early paging maps!");
        // return false; // Unreachable
    }

    // --- Stage 3: Initialize Buddy Allocator ---
    // Now that the physical heap region [heap_phys_start, heap_phys_start + heap_size)
    // is identity mapped, we can initialize the buddy allocator to manage it.
    terminal_write(" Stage 3: Initializing Buddy Allocator (Physical Page Allocator)...\n");
    // Buddy init takes the *virtual* address where the physical heap is mapped.
    // Since we identity mapped it, the virtual address == physical address.
    buddy_init((void *)heap_phys_start, heap_size);
    // Optional check if buddy initialization consumed all space unexpectedly
    if (buddy_free_space() == 0 && heap_size >= MIN_BLOCK_SIZE) { // MIN_BLOCK_SIZE from buddy.c
        terminal_write("   [Warning] Buddy Allocator reports zero free space immediately after init.\n");
    }
    terminal_printf("   Buddy Initial Free Space: %lu bytes\n", (unsigned long)buddy_free_space());

    // --- Stage 4: Finalize & Activate Paging ---
    // Perform any final setup (e.g., recursive mapping) and load CR3.
    terminal_write(" Stage 4: Finalizing and activating paging...\n");
    if (paging_finalize_and_activate(initial_pd_phys, total_memory) != 0) {
        KERNEL_PANIC_HALT("Failed to finalize and activate paging!");
       // return false; // Unreachable
    }
    // --- PAGING IS NOW ACTIVE ---
    // We can now access memory via virtual addresses. Physical addresses
    // should only be used when interacting with hardware registers or
    // page table entries directly. Global variables g_kernel_page_directory_phys/virt
    // (from paging.c) should now be valid.
    terminal_write("   Paging Active. Accessing memory via virtual addresses.\n");

    // --- Stage 4.5: Map Multiboot Info Structure into Virtual Memory ---
    // Now that paging is on, map the physical Multiboot info structure into the
    // kernel's virtual address space so it can be accessed safely later.
    terminal_write(" Stage 4.5: Mapping Multiboot Info Structure into kernel VAS...\n");
    // Add assert with message
    KERNEL_ASSERT(g_multiboot_info_phys_addr_global != 0, "Multiboot physical address global is zero before mapping");

    uintptr_t mb_info_phys_page_start = PAGE_ALIGN_DOWN(g_multiboot_info_phys_addr_global);
    // Map at least one page initially to read the total size safely.
    // Map into the higher-half kernel space (KERNEL_SPACE_VIRT_START defined in paging.h)
    // Use an offset based on physical address to avoid collision, assuming sufficient space.
    uintptr_t mb_info_virt_page_start = KERNEL_SPACE_VIRT_START + mb_info_phys_page_start;
    size_t mb_mapping_size = PAGE_SIZE; // Initial mapping size

    terminal_printf("   Mapping MB Info Phys Page [%#lx] to Virt Page [%#lx]\n",
                    (unsigned long)mb_info_phys_page_start,
                    (unsigned long)mb_info_virt_page_start);

    // Use the now-active paging system to map the physical page.
    // Assumes g_kernel_page_directory_phys is valid after stage 4.
    if (paging_map_range((uint32_t*)g_kernel_page_directory_phys, // Use physical address of PD for mapping
                         mb_info_virt_page_start,
                         mb_info_phys_page_start,
                         mb_mapping_size,
                         PTE_KERNEL_READONLY_FLAGS) != 0) // Map read-only
    {
        KERNEL_PANIC_HALT("Failed to map Multiboot info structure!");
        // return false; // Unreachable
    }

    // Calculate the final virtual address of the *start* of the structure
    g_multiboot_info_virt_addr_global = mb_info_virt_page_start + (g_multiboot_info_phys_addr_global % PAGE_SIZE);
    terminal_printf("   Multiboot structure accessible at VIRT: %#lx\n", (unsigned long)g_multiboot_info_virt_addr_global);

    // Check the actual size and map additional pages if needed.
    uint32_t total_mb_size = *(volatile uint32_t*)g_multiboot_info_virt_addr_global; // Read size via virtual mapping
    if (total_mb_size > PAGE_SIZE) {
        size_t total_pages_needed = ALIGN_UP(total_mb_size, PAGE_SIZE) / PAGE_SIZE;
        size_t additional_pages_to_map = total_pages_needed - 1;
        uintptr_t next_phys_page = mb_info_phys_page_start + PAGE_SIZE;
        uintptr_t next_virt_page = mb_info_virt_page_start + PAGE_SIZE;

        terminal_printf("   MB Info > 1 page (%lu bytes). Mapping %lu additional pages...\n",
                        (unsigned long)total_mb_size, (unsigned long)additional_pages_to_map);

        if (paging_map_range((uint32_t*)g_kernel_page_directory_phys,
                             next_virt_page, next_phys_page,
                             additional_pages_to_map * PAGE_SIZE,
                             PTE_KERNEL_READONLY_FLAGS) != 0)
        {
             // This might not be fatal if only basic tags are needed, but log a warning.
             terminal_printf("   [Warning] Failed to map additional Multiboot info pages.\n");
        }
    }
    // --- End Stage 4.5 ---

    // --- Stage 5: Map Physical Memory (Deferred/On-Demand) ---
    // Mapping the *entire* physical memory range can be deferred and done on-demand
    // by the frame allocator when physical pages need to be accessed by the kernel.
    // This saves setup time and avoids allocating potentially many page tables upfront.
    terminal_write(" Stage 5: Physical Memory Mapping (Deferred to Frame Allocator).\n");

    // --- Stage 6: Initialize Frame Allocator ---
    // The frame allocator manages physical frames using metadata (e.g., ref counts).
    // This metadata needs to be stored somewhere, usually allocated from the kernel heap
    // (which relies on the buddy allocator). It also needs the memory map to know
    // which physical frames are actually usable.
    terminal_write(" Stage 6: Initializing Frame Allocator (Physical Frame Management)...\n");
    // Find the MMAP tag again, but this time using the VIRTUAL address mapping.
    struct multiboot_tag_mmap *mmap_tag_virt = (struct multiboot_tag_mmap *)find_multiboot_tag_virt(
        g_multiboot_info_virt_addr_global, MULTIBOOT_TAG_TYPE_MMAP);
    if (!mmap_tag_virt) {
        KERNEL_PANIC_HALT("Cannot find MMAP tag via virtual address!");
       // return false; // Unreachable
    }

    terminal_printf("   Passing MMAP tag (Virt Addr %p) to frame_init.\n", mmap_tag_virt);

    // Initialize the frame allocator, providing the memory map and kernel/heap regions to avoid.
    // frame_init will likely use kmalloc (indirectly via buddy) to allocate its metadata arrays.
    if (frame_init(mmap_tag_virt,
                   kernel_phys_start, kernel_phys_end, // Physical region occupied by kernel
                   heap_phys_start, heap_phys_start + heap_size) != 0) // Physical region used by buddy
    {
        KERNEL_PANIC_HALT("Frame Allocator initialization failed!");
        // return false; // Unreachable
    }
    terminal_write("   Frame Allocator Initialized.\n");

    // --- Stage 7: Initialize Kmalloc (Slab Allocator) ---
    // Kmalloc provides the general-purpose kernel dynamic memory allocation interface.
    // It typically sits on top of the slab allocator, which in turn gets larger memory
    // chunks (slabs) from the page-level allocator (frame allocator or buddy system).
    terminal_write(" Stage 7: Initializing Kmalloc (Slab Allocator Facade)...\n");
    kmalloc_init(); // Initializes slab caches, relies on frame_alloc or buddy_alloc.
    terminal_write("   Kmalloc Initialized.\n");

    // --- Stage 8: Initialize Temporary VA Mapper ---
    // Initialize the utility for temporary virtual address mappings (useful for accessing
    // arbitrary physical memory for short durations). Needs kmalloc.
    terminal_write(" Stage 8: Initializing Temporary VA Mapper...\n");
    if (paging_temp_map_init() != 0) {
        KERNEL_PANIC_HALT("Failed to initialize temporary VA mapper!");
        // return false; // Unreachable
    }
    terminal_write("   Temporary VA Mapper Initialized.\n");


    terminal_write("[OK] Memory Subsystems Initialized Successfully.\n");
    return true;
}

/**
 * @brief Creates the initial userspace process.
 * @ingroup process
 *
 * Attempts to load and create the first user process (e.g., "/hello.elf" or an init process).
 * This requires the filesystem to be mounted and functional.
 */
static void launch_initial_process(void) {
    terminal_write("[Kernel] Creating initial user process...\n");

    // Define the path to the initial program within the mounted filesystem.
    const char *user_prog_path = "/hello.elf"; // TODO: Consider making this configurable (e.g., via boot args)

    terminal_printf("  Attempting to load '%s'...\n", user_prog_path);

    // create_user_process handles file reading, ELF loading, memory setup, etc.
    pcb_t *user_proc_pcb = create_user_process(user_prog_path);

    if (user_proc_pcb != NULL) {
        terminal_printf("  [OK] Process created (PID %lu). Adding to scheduler.\n", (unsigned long)user_proc_pcb->pid);
        // Add the newly created process to the scheduler's run queue.
        // This depends on your scheduler's API.
        // Example:
        if (scheduler_add_task(user_proc_pcb) == 0) { // Assuming scheduler takes PCB or TCB wrapping PCB
            terminal_write("  [OK] Initial user process scheduled successfully.\n");
        } else {
            // This should ideally not happen if create_user_process succeeded.
            terminal_printf("  [ERROR] Failed to add initial process (PID %lu) to scheduler queue!\n", (unsigned long)user_proc_pcb->pid);
            // Attempt to clean up the partially created process
            destroy_process(user_proc_pcb); // Ensure destroy_process is robust
        }
    } else {
        // create_user_process should log specific errors internally.
        terminal_printf("  [ERROR] Failed to create initial user process from '%s'. Check VFS/ELF loader.\n", user_prog_path);
        // Depending on requirements, this could be a fatal error.
        // KERNEL_PANIC_HALT("Failed to launch initial user process.");
        terminal_write("  [Warning] Continuing without initial user process.\n");
    }
}


// === Main Kernel Entry Point ===
/**
 * @brief The C entry point for the UiAOS kernel.
 * @ingroup main
 *
 * Called from assembly (`_start` in multiboot2.asm) after basic CPU setup.
 * Initializes all kernel subsystems in the correct order, enables interrupts,
 * and enters the idle loop.
 *
 * @param magic The Multiboot2 magic number passed by the bootloader (must be 0x36d76289).
 * @param mb_info_phys_addr The physical address of the Multiboot 2 info structure.
 */
// *** FIX: Renamed back to 'main' to match the call from multiboot2.asm ***
void main(uint32_t magic, uint32_t mb_info_phys_addr) {
    // Store Multiboot info address globally FIRST - needed by early init_memory stages
    g_multiboot_info_phys_addr_global = mb_info_phys_addr;

    // Initialize earliest hardware needed for boot diagnostics
    serial_init();        // Serial port for logging (especially useful in QEMU)
    terminal_init();      // VGA terminal for primary console output

    terminal_write("\n=== UiAOS Kernel Booting ===\n");
    terminal_printf(" Version: %s\n\n", KERNEL_VERSION);

    // === Pre-Initialization Checks ===
    terminal_write("[Boot] Verifying Multiboot information...\n");
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        terminal_printf("  [FATAL] Invalid Multiboot Magic number: %#lx (Expected: %#lx)\n",
                        (unsigned long)magic, (unsigned long)MULTIBOOT2_BOOTLOADER_MAGIC);
        KERNEL_PANIC_HALT("Invalid Multiboot Magic number.");
    }
    // Basic sanity check on the physical address provided
    if (mb_info_phys_addr == 0 || mb_info_phys_addr >= 0x100000) { // Must be below 1MB
         terminal_printf("  [FATAL] Invalid or inaccessible Multiboot info physical address: %#lx\n",
                         (unsigned long)mb_info_phys_addr);
         KERNEL_PANIC_HALT("Invalid Multiboot info address.");
    }
    terminal_printf("[Boot] Multiboot magic OK (Info at phys %#lx).\n", (unsigned long)mb_info_phys_addr);

    // === Core System Initialization (Pre-Interrupts) ===

    // 1. GDT & TSS
    terminal_write("[Kernel] Initializing GDT & TSS...\n");
    gdt_init();
    // TSS is initialized but ESP0 (kernel stack pointer for syscalls/interrupts from user)
    // is typically set later, often per-process or per-cpu. scheduler_init handles initial setup.
    terminal_write("  GDT & TSS structures initialized.\n");

    // 2. Memory Management
    // This is a complex multi-stage process that enables paging.
    if (!init_memory(g_multiboot_info_phys_addr_global)) {
        // Panic occurs within init_memory on failure.
        KERNEL_PANIC_HALT("Memory initialization failed (unreachable)");
    }
    // --- PAGING IS NOW ACTIVE --- Kmalloc, Frame Allocator etc. are usable.

    // 3. IDT & Interrupt Handling Base
    // Must be done AFTER paging is active, as interrupt handlers reside in higher-half kernel space.
    terminal_write("[Kernel] Initializing IDT & Interrupt Handlers...\n");
    idt_init(); // Sets up IDT entries to point to ISR/IRQ stubs.
    terminal_write("  IDT initialized.\n");

    // 4. Hardware Drivers (Essential for Scheduling & Interaction)
    terminal_write("[Kernel] Initializing Core Hardware Drivers...\n");
    terminal_write("  Initializing PIT (for scheduling ticks)...\n");
    init_pit(); // Configures PIT to generate timer interrupts at SCHEDULER_HZ.
    terminal_write("  Initializing Keyboard...\n");
    keyboard_init();
    keymap_load(KEYMAP_NORWEGIAN); // Load default keymap
    // Other essential drivers (like block device for FS) might be initialized here or in fs_init.

    // 5. Scheduler Initialization
    // Sets up the initial kernel idle task, prepares scheduler data structures,
    // and crucially, sets the initial TSS ESP0 value for the boot CPU.
    terminal_write("[Kernel] Initializing Scheduler...\n");
    scheduler_init();
    terminal_write("  Scheduler initialized (Idle Task ready, TSS ESP0 set).\n");
    syscall_init();
    terminal_write("  Syscall interface initialized.\n");


    // 6. Filesystem Initialization
    // Mount the root filesystem. Requires block device drivers and memory allocators.
    // Must be done BEFORE attempting to load user processes from disk and BEFORE STI.
    terminal_write("[Kernel] Initializing Filesystem Layer...\n");
    bool fs_ready = false;
    int fs_init_status = fs_init(); // Attempts to init block device and mount root FS
    if (fs_init_status == FS_SUCCESS) {
        terminal_write("  [OK] Filesystem initialized and root mounted.\n");
        fs_ready = true;
        list_mounts(); // Optional: List mounts for verification
    } else {
        // Allow boot to continue without FS, but log clearly. Initial process launch will fail.
        terminal_printf("  [Warning] Filesystem initialization failed (Error: %d). Cannot load user programs.\n", fs_init_status);
        // For a real system, might panic: KERNEL_PANIC_HALT("Filesystem init failed.");
    }

    // 7. Initial Process Creation
    // Load and prepare the first user-space process (e.g., init, shell, or test program).
    // Requires a functional filesystem and scheduler structures. Must be done BEFORE STI.
    if (fs_ready) {
        launch_initial_process(); // Handles creation and adding to scheduler
    } else {
        terminal_write("  [Info] Filesystem not ready, skipping initial user process creation.\n");
    }

    // 8. Mark Scheduler Ready & Enable Interrupts
    // Once all essential pre-interrupt setup is complete, we can allow the scheduler
    // to start preempting tasks by enabling interrupts.
    terminal_write("[Kernel] Finalizing setup before enabling interrupts...\n");


    // Mark the scheduler as fully ready to handle timer ticks.
    scheduler_start(); // Sets g_scheduler_ready = true
    terminal_write("  Scheduler marked as ready.\n");
    // === Enable Interrupts ===
    // This allows hardware interrupts (like the PIT timer tick) to occur,
    // which will trigger the scheduler (`schedule()`) and start multitasking.
    // NO complex initialization should happen after this point without proper locking.
    terminal_write("\n[Kernel] Initialization complete. Enabling interrupts now.\n");
    terminal_write("======================================================================\n\n");
    serial_write("[Kernel DEBUG] STI...\n");

    asm volatile ("sti");

    serial_write("[Kernel DEBUG] Interrupts Enabled.\n");

    // === Enter Kernel Idle Loop ===
    // The `main` function's primary job is done. It now enters an infinite
    // idle loop. The `hlt` instruction pauses the CPU until the next interrupt
    // occurs (e.g., timer tick, keyboard input), saving power. The interrupt
    // handler will do work and potentially switch tasks via the scheduler.
    // Control returns here only when the idle task is scheduled.
    terminal_write("[Kernel] Entering idle loop (hlt).\n");
    while (1) {
        // Halt the CPU until the next interrupt.
        asm volatile ("hlt");
        // When an interrupt occurs and returns, the loop continues.
        // The scheduler might switch tasks, so we might not return here immediately.
    }

    // --- Code should NOT be reached ---
    KERNEL_PANIC_HALT("Reached end of main() unexpectedly!");
}

