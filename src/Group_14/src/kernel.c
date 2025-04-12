/**
 * kernel.c - Main kernel entry point for UiAOS
 *
 * Author: Group 14 (UiA)
 * Version: 3.1 (Staged Init, Fixes, Enhanced Comments)
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
// Note: block_device.h and disk.h are included via fs_init.h -> fat.h -> disk.h

// === Utilities ===
#include "get_cpu_id.h"     // Function to get current CPU ID
#include "cpuid.h"          // CPUID instruction helper
#include "kmalloc_internal.h" // For ALIGN_UP, potentially others

// === Constants ===
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
#define MIN_HEAP_SIZE (1 * 1024 * 1024) // Minimum acceptable heap size (1MB)

// Macro for halting the system on critical failure
#define KERNEL_PANIC_HALT(msg) do { \
    terminal_printf("\n[KERNEL PANIC] %s System Halted.\n", msg); \
    while(1) { asm volatile("cli; hlt"); } \
} while(0)

// === Linker Symbols ===
// Define these in your linker script (linker.ld)
// Use void* or uint8_t* for byte-level addresses
extern uint8_t _kernel_start_phys;    // Physical start address of kernel code/data
extern uint8_t _kernel_end_phys;      // Physical end address of kernel code/data
extern uint8_t _kernel_virtual_base;  // Kernel's virtual base address (e.g., 0xC0000000)

// === Global Variables ===
// Define the global variable to store the Multiboot info address
uint32_t g_multiboot_info_phys_addr_global = 0;


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
        terminal_printf("[Boot Error] Multiboot total_size (%u) invalid.\n", total_size);
        return NULL;
    }

    struct multiboot_tag *tag = (struct multiboot_tag *)(mb_info_phys_addr + 8); // First tag starts after total_size and reserved fields
    uintptr_t info_end = mb_info_phys_addr + total_size;

    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        uintptr_t current_tag_addr = (uintptr_t)tag;

        // Bounds check: Ensure tag header itself is within the info structure
        if (current_tag_addr + sizeof(struct multiboot_tag) > info_end) {
             terminal_printf("[Boot Error] Multiboot tag header OOB (Tag Addr=0x%x, Info End=0x%x).\n", current_tag_addr, info_end);
             return NULL;
        }
        // Bounds check: Ensure the entire tag (based on its size field) is within the info structure
        if (tag->size < sizeof(struct multiboot_tag) || (current_tag_addr + tag->size) > info_end) {
            terminal_printf("[Boot Error] Multiboot tag invalid size %u at Addr=0x%x (Info End=0x%x).\n", tag->size, current_tag_addr, info_end);
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
static bool parse_memory_map(struct multiboot_tag_mmap *mmap_tag,
                             uintptr_t *out_total_memory,
                             uintptr_t *out_heap_base_addr, size_t *out_heap_size)
{
    uintptr_t current_total_memory = 0;
    uintptr_t best_heap_base = 0;
    uint64_t best_heap_size_64 = 0; // Use 64-bit for size calculations to avoid overflow
    multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
    uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;

    // Get kernel physical boundaries
    uintptr_t kernel_start_phys_addr = (uintptr_t)&_kernel_start_phys;
    uintptr_t kernel_end_phys_addr = (uintptr_t)&_kernel_end_phys;

    terminal_write(" Memory Map (from Multiboot):\n");
    while ((uintptr_t)mmap_entry < mmap_end) {
        // Validate entry size before use
        if (mmap_tag->entry_size == 0 || mmap_tag->entry_size < sizeof(multiboot_memory_map_t)) {
            terminal_printf("  [ERR] MMAP entry size (%u) invalid!\n", mmap_tag->entry_size);
            return false;
        }

        uintptr_t region_start = (uintptr_t)mmap_entry->addr;
        uint64_t region_len = mmap_entry->len;
        uintptr_t region_end = region_start + (uintptr_t)region_len; // Careful with potential overflow if region_len is huge

        // Check for overflow in region_end calculation
        if (region_end < region_start && region_len != 0) {
            terminal_printf("  [Warning] Memory region [0x%llx - 0x%llx) ignored due to potential overflow.\n", (unsigned long long)region_start, (unsigned long long)(region_start + region_len));
            region_end = UINTPTR_MAX; // Assume it extends to max address for total memory calc
        }

        terminal_printf("  Addr: 0x%08x Len: 0x%08llx Type: %d\n",
                        region_start, (unsigned long long)region_len, mmap_entry->type);

        // Update total physical memory detected (highest address seen)
        if (region_end > current_total_memory) {
            current_total_memory = region_end;
        }

        // Find the largest AVAILABLE memory region >= 1MB for the initial heap
        if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE && region_start >= 0x100000) {
            uintptr_t usable_start = region_start;
            uint64_t usable_len = region_len;

            // Adjust usable range to avoid overlap with the kernel image
            uintptr_t overlap_start = (usable_start > kernel_start_phys_addr) ? usable_start : kernel_start_phys_addr;
            uintptr_t overlap_end = (region_end < kernel_end_phys_addr) ? region_end : kernel_end_phys_addr;

            if (overlap_start < overlap_end) { // Kernel overlaps with this region
                // Option 1: Consider the part *before* the kernel
                if (usable_start < kernel_start_phys_addr) {
                    uint64_t size_before = kernel_start_phys_addr - usable_start;
                    if (size_before > best_heap_size_64) {
                        best_heap_size_64 = size_before;
                        best_heap_base = usable_start;
                    }
                }
                // Option 2: Consider the part *after* the kernel
                if (region_end > kernel_end_phys_addr) {
                    usable_start = kernel_end_phys_addr; // Start after kernel
                    usable_len = region_end - kernel_end_phys_addr;
                     // Check if this remaining part is the new best candidate
                    if (usable_len > best_heap_size_64) {
                        best_heap_size_64 = usable_len;
                        best_heap_base = usable_start;
                    }
                }
                // If kernel fully contains region, usable_len would be 0 from the second check.
            } else { // No overlap with kernel
                // Update best heap region if this available region is larger
                if (usable_len > best_heap_size_64) {
                    best_heap_size_64 = usable_len;
                    best_heap_base = usable_start;
                }
            }
        } // End if AVAILABLE and >= 1MB

        // Advance to the next entry safely
        uintptr_t next_entry_addr = (uintptr_t)mmap_entry + mmap_tag->entry_size;
        if (next_entry_addr > mmap_end) { // Check before dereferencing next entry
             terminal_write("  [Warning] MMAP entry advancement out of bounds.\n");
             break;
        }
        mmap_entry = (multiboot_memory_map_t *)next_entry_addr;
    } // End while loop through mmap entries

    // Final checks and output assignment
    if (best_heap_size_64 > 0 && best_heap_base != 0) {
        *out_heap_base_addr = best_heap_base;
        // Clamp heap size to SIZE_MAX if it exceeds the representable range
        if (best_heap_size_64 > (uint64_t)SIZE_MAX) {
            terminal_write("  [Warning] Largest heap region exceeds size_t! Clamping.\n");
            *out_heap_size = SIZE_MAX;
        } else {
            *out_heap_size = (size_t)best_heap_size_64;
        }
    } else {
        terminal_write("  [FATAL] No suitable memory region found >= 1MB for heap!\n");
        return false;
    }

    // Align total memory up to the nearest page boundary
    *out_total_memory = ALIGN_UP(current_total_memory, PAGE_SIZE);

    terminal_printf("  Total Physical Memory Detected: %u MB\n", *out_total_memory / (1024*1024));
    terminal_printf("  Selected Heap Region: Phys Addr=0x%x, Size=%u bytes\n",
                    *out_heap_base_addr, *out_heap_size);
    return true;
}


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
    terminal_write(" Stage 0: Parsing Multiboot Memory Map...\n");
    struct multiboot_tag_mmap *mmap_tag = (struct multiboot_tag_mmap *)find_multiboot_tag(
        mb_info_phys_addr, MULTIBOOT_TAG_TYPE_MMAP);
    if (!mmap_tag) KERNEL_PANIC_HALT("Multiboot memory map tag not found!");

    uintptr_t total_memory = 0;
    uintptr_t heap_phys_start = 0;
    size_t heap_size = 0;
    if (!parse_memory_map(mmap_tag, &total_memory, &heap_phys_start, &heap_size)) {
        KERNEL_PANIC_HALT("Failed to parse memory map or find suitable heap region!");
    }
    if (heap_size < MIN_HEAP_SIZE) KERNEL_PANIC_HALT("Heap region too small!");

    uintptr_t kernel_phys_start = (uintptr_t)&_kernel_start_phys;
    uintptr_t kernel_phys_end = (uintptr_t)&_kernel_end_phys;
    terminal_printf("   Kernel Phys Region: [0x%x - 0x%x)\n", kernel_phys_start, kernel_phys_end);
    terminal_printf("   Heap Phys Region:   [0x%x - 0x%x) Size: %u KB\n", heap_phys_start, heap_phys_start + heap_size, heap_size / 1024);
    terminal_printf("   Total Phys Memory:  %u MB\n", total_memory / (1024 * 1024));

    // --- Stage 1: Allocate Initial Page Directory Frame ---
    terminal_write(" Stage 1: Allocating initial Page Directory frame...\n");
    // Uses the early mechanism built into paging.c
    uintptr_t initial_pd_phys = paging_alloc_early_pt_frame_physical();
    if (!initial_pd_phys) {
        KERNEL_PANIC_HALT("Failed to allocate physical frame for the initial PD!");
    }
    terminal_printf("   Initial PD allocated at Phys: 0x%x\n", initial_pd_phys);
    // Zero the frame using its physical address (must be accessible before paging)
    memset((void*)initial_pd_phys, 0, PAGE_SIZE);

    // --- Stage 2: Setup Early Mappings in the Initial PD ---
    terminal_write(" Stage 2: Setting up early physical maps...\n");
    uint32_t *pd_phys_ptr = (uint32_t*)initial_pd_phys;
    check_and_enable_pse(); // Enable 4MB pages if supported

    // Map Kernel Region (Identity & Higher Half)
    terminal_printf("   Mapping Kernel ID + Higher Half...\n");
    if (paging_map_physical(pd_phys_ptr, kernel_phys_start, kernel_phys_end - kernel_phys_start, PTE_KERNEL_DATA, false) != 0)
        KERNEL_PANIC_HALT("Failed to identity map kernel region!");
    if (paging_map_physical(pd_phys_ptr, kernel_phys_start, kernel_phys_end - kernel_phys_start, PTE_KERNEL_DATA, true) != 0)
        KERNEL_PANIC_HALT("Failed to map kernel to higher half!");

    // Map Buddy Heap Region (Identity)
    terminal_printf("   Mapping Buddy Heap ID...\n");
    if (paging_map_physical(pd_phys_ptr, heap_phys_start, heap_size, PTE_KERNEL_DATA, false) != 0)
        KERNEL_PANIC_HALT("Failed to identity map buddy heap region!");

    // Map VGA Memory (Higher Half)
    terminal_printf("   Mapping VGA Memory Higher Half...\n");
    if (paging_map_physical(pd_phys_ptr, 0xB8000, PAGE_SIZE, PTE_KERNEL_DATA, true) != 0)
        KERNEL_PANIC_HALT("Failed to map VGA memory!");

    // Map the Page Directory itself into the higher half (Recursive Mapping Setup)
    terminal_printf("   Mapping Page Directory self Higher Half...\n");
    if (paging_map_physical(pd_phys_ptr, initial_pd_phys, PAGE_SIZE, PTE_KERNEL_DATA, true) != 0)
        KERNEL_PANIC_HALT("Failed to map Page Directory to higher half!");

    // --- Stage 3: Initialize Buddy Allocator ---
    terminal_write(" Stage 3: Initializing Buddy Allocator...\n");
    // Pass the identity-mapped physical address and size.
    buddy_init((void *)heap_phys_start, heap_size);
    if (buddy_free_space() == 0 && heap_size >= MIN_BLOCK_SIZE) {
        // A zero free space might be valid if heap_size was exactly MIN_BLOCK_SIZE,
        // but generally indicates a problem if the heap was larger.
        terminal_write("  [Warning] Buddy Allocator reports zero free space after init.\n");
        // KERNEL_PANIC_HALT("Buddy Allocator initialization failed (no free space reported)!");
    }
    terminal_printf("   Buddy Initial Free Space: %u bytes\n", buddy_free_space());

    // --- Stage 4: Pre-map Kernel Temporary VMA Range ---
    terminal_write(" Stage 4: Pre-mapping Kernel temporary VMA range...\n");
    // Use revised definitions from paging.h
    uintptr_t temp_vma_start = PAGE_ALIGN_DOWN(TEMP_MAP_ADDR_COW_DST); // Lowest temp address used
    // Calculate end address carefully - it's the start of the PD mapping + PAGE_SIZE
    uintptr_t temp_vma_end = TEMP_MAP_ADDR_PD + PAGE_SIZE; // End address is exclusive
    terminal_printf("   Temp Range Virt: [0x%x - 0x%x)\n", temp_vma_start, temp_vma_end);

    // Basic sanity check for the revised temporary range definition
    if (temp_vma_start >= temp_vma_end || temp_vma_start < KERNEL_SPACE_VIRT_START) {
        KERNEL_PANIC_HALT("Invalid temporary VMA range definition after revision!");
    }

    // --- REMOVED the panic check that was here ---
    // // Check if start address is below kernel base <-- REMOVED THIS CHECK
    // if (temp_vma_start < KERNEL_SPACE_VIRT_START) {
    //     KERNEL_PANIC_HALT("Temporary VMA range starts below kernel base!");
    // }

    // Iterate through pages in the temp range and ensure PTs are allocated
    for (uintptr_t v_addr = temp_vma_start; v_addr < temp_vma_end; v_addr += PAGE_SIZE) {
        uint32_t pde_index = PDE_INDEX(v_addr);
        // uint32_t pte_index = PTE_INDEX(v_addr); // For logging if needed

        // Check if a PDE for this virtual address range already exists in our initial PD
        if (!(pd_phys_ptr[pde_index] & PAGE_PRESENT)) {
            terminal_printf("   Allocating PT for Temp VMA PDE[%d] (Virt 0x%x)...\n", pde_index, v_addr);

            // Allocate a physical frame for the Page Table using the Buddy Allocator
            uintptr_t temp_pt_phys = (uintptr_t)BUDDY_ALLOC(PAGE_SIZE);
            if (!temp_pt_phys) {
                KERNEL_PANIC_HALT("Failed to allocate PT frame for temporary kernel mappings!");
            }

            // Zero the allocated Page Table frame via its identity mapping
            memset((void*)temp_pt_phys, 0, PAGE_SIZE);

            // Add the PDE entry pointing to the new PT. Kernel R/W, Not User accessible.
            uint32_t pde_flags = PAGE_PRESENT | PAGE_RW;
            pd_phys_ptr[pde_index] = (temp_pt_phys & ~0xFFF) | pde_flags;
            terminal_printf("     Added PDE[%d] -> PT Phys 0x%x\n", pde_index, temp_pt_phys);
        }
        // If PDE was already present, assume the PT is valid and proceed.
    } // End loop for pre-mapping temp VMA PTs

    // --- Stage 5: Finalize Higher-Half Mappings & Activate Paging ---
    terminal_write(" Stage 5: Finalizing mappings and activating paging...\n");
    // This function will map all physical memory to the higher half and enable paging.
    // It relies on the Buddy allocator (for new PTs) and the pre-mapped temp range.
    if (paging_finalize_and_activate(initial_pd_phys, total_memory) != 0) {
        KERNEL_PANIC_HALT("Failed to finalize mappings or activate paging!");
    }
    // Paging is ON. g_kernel_page_directory_phys/virt are set.

    // --- Stage 6: Initialize Frame Allocator ---
    terminal_write(" Stage 6: Initializing Frame Allocator...\n");
    // Requires active paging and the Buddy allocator.
    if (frame_init(mmap_tag, kernel_phys_start, kernel_phys_end, heap_phys_start, heap_phys_start + heap_size) != 0) {
        KERNEL_PANIC_HALT("Frame Allocator initialization failed!");
    }

    // --- Stage 7: Initialize Kmalloc ---
    terminal_write(" Stage 7: Initializing Kmalloc...\n");
    // Depends on Frame Allocator -> Buddy.
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

    // 1. Early Initialization (Console, CPU Features, Core Tables)
    terminal_init(); // Initialize console output ASAP
    terminal_write("=== UiAOS Kernel Booting ===\n");
    terminal_printf(" Version: %s\n\n", "3.1-StagedMem"); // Example version

    // Verify Multiboot Magic
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        KERNEL_PANIC_HALT("Invalid Multiboot Magic number received from bootloader.");
    }
    terminal_printf("[Boot] Multiboot magic OK (Info at phys 0x%x).\n", mb_info_phys_addr);

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
    if (fs_init() == FS_SUCCESS) { // fs_init internally handles VFS init, driver registration, root mount
        terminal_write("  [OK] Filesystem initialized and root mounted.\n");
        list_mounts(); // List mounted filesystems for verification
        // Optional: Test file access
        // fs_test_file_access("/test.txt"); // Replace with a file expected on your disk image
    } else {
        terminal_write("  [Warning] Filesystem initialization failed. Continuing without FS.\n");
        // Depending on OS requirements, this could be a panic:
        // KERNEL_PANIC_HALT("Filesystem initialization failed.");
    }

    // 5. Scheduler and Initial Process Setup
    terminal_write("[Kernel] Initializing Scheduler...\n");
    scheduler_init(); // Requires memory allocators

    terminal_write("[Kernel] Creating initial user process...\n");
    const char *user_prog_path = "/hello.elf"; // Default user program to load

    if (!fs_is_initialized()) {
         terminal_write("  [Info] Filesystem not available, cannot load initial user process.\n");
         // Without an initial process, the system will just idle.
    } else {
        // Attempt to create and schedule the initial process
        pcb_t *user_proc_pcb = create_user_process(user_prog_path);
        if (user_proc_pcb) {
            terminal_printf("  [OK] Process created (PID %d) from '%s'. Adding to scheduler.\n", user_proc_pcb->pid, user_prog_path);
            if (scheduler_add_task(user_proc_pcb) != 0) {
                 terminal_printf("  [ERROR] Failed to add initial process (PID %d) to scheduler.\n", user_proc_pcb->pid);
                 destroy_process(user_proc_pcb); // Clean up failed process
                 KERNEL_PANIC_HALT("Cannot schedule initial process.");
            } else {
                 terminal_write("  [OK] Initial user process scheduled.\n");
            }
        } else {
            terminal_printf("  [ERROR] Failed to create initial user process from '%s'.\n", user_prog_path);
            // Depending on requirements, this might be a panic:
            KERNEL_PANIC_HALT("Failed to create initial process.");
        }
    }

    // 6. Enable Preemption and Start Idle Loop
    if (get_current_task() != NULL) { // Check if any task was actually added
        terminal_write("[Kernel] Enabling preemptive scheduling via PIT...\n");
        pit_set_scheduler_ready(); // Allow PIT handler to call schedule()
    } else {
        terminal_write("[Kernel] No tasks scheduled. Entering simple idle loop.\n");
    }

    terminal_write("\n[Kernel] Initialization complete. Enabling interrupts and entering idle task.\n");
    terminal_write("======================================================================\n");

    // The idle task will enable interrupts and halt, waiting for the PIT
    // or other interrupts to trigger scheduling or handling.
    kernel_idle_task();

    // --- Code should not be reached beyond kernel_idle_task ---
    KERNEL_PANIC_HALT("Reached end of main() unexpectedly!");
}