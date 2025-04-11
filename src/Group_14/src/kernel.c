/**
 * kernel.c - Main kernel entry point for UiAOS
 *
 * REVISED: Uses staged paging initialization and memory map parsing
 * to find the initial PD frame, resolving early boot memory issues.
 * Implements a robust initialization sequence suitable for OS development.
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

// === Constants ===
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
// Macro for halting the system on critical failure
#define KERNEL_PANIC_HALT(msg) do { \
    terminal_printf("\n[KERNEL PANIC] %s System Halted.\n", msg); \
    while(1) { asm volatile("cli; hlt"); } \
} while(0)

#define MIN_HEAP_SIZE (1 * 1024 * 1024) // Minimum acceptable heap size (1MB)

// === Linker Symbols ===
// Define these in your linker script (linker.ld)
// Use void* or uint8_t* for byte-level addresses
extern uint8_t _kernel_start_phys;    // Physical start address of kernel code/data
extern uint8_t _kernel_end_phys;      // Physical end address of kernel code/data
extern uint8_t _kernel_virtual_base;  // Kernel's virtual base address (e.g., 0xC0000000)

// === Static Function Prototypes ===
static struct multiboot_tag *find_multiboot_tag(uint32_t mb_info_phys_addr, uint16_t type);
static bool parse_memory_map(struct multiboot_tag_mmap *mmap_tag,
                             uintptr_t *out_total_memory,
                             uintptr_t *out_heap_base_addr, size_t *out_heap_size);
static uintptr_t find_early_free_frame(struct multiboot_tag_mmap *mmap_tag); // Finds frame for initial PD
static bool init_memory(uint32_t mb_info_phys_addr); // Main memory initialization sequence
void kernel_idle_task(void); // Idle task loop

// === Multiboot Tag Finding Helper (Improved Validation) ===
static struct multiboot_tag *find_multiboot_tag(uint32_t mb_info_phys_addr, uint16_t type) {
    if (mb_info_phys_addr == 0 || mb_info_phys_addr >= 0x100000) { // Assume info struct is below 1MB
        terminal_write("[Boot Error] Multiboot info address invalid or inaccessible early.\n");
        return NULL;
    }
    uint32_t total_size = *(uint32_t*)mb_info_phys_addr;
    if (total_size < 8 || total_size > 0x100000) { // Basic size sanity check
        terminal_printf("[Boot Error] Multiboot total_size (%u) invalid.\n", total_size);
        return NULL;
    }

    struct multiboot_tag *tag = (struct multiboot_tag *)(mb_info_phys_addr + 8);
    uintptr_t info_end = mb_info_phys_addr + total_size;

    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        uintptr_t current_tag_addr = (uintptr_t)tag;
        // Bounds check tag header
        if (current_tag_addr + 8 > info_end) {
            terminal_printf("[Boot Error] Multiboot tag header OOB (Tag Addr=0x%x, Info End=0x%x).\n", current_tag_addr, info_end);
            return NULL;
        }
        // Bounds check tag content
        if (tag->size < 8 || (current_tag_addr + tag->size) > info_end) {
            terminal_printf("[Boot Error] Multiboot tag invalid size %u at Addr=0x%x (Info End=0x%x).\n", tag->size, current_tag_addr, info_end);
            return NULL;
        }

        if (tag->type == type) return tag; // Found

        // Advance to the next tag
        uintptr_t next_tag_addr = current_tag_addr + ((tag->size + 7) & ~7); // 8-byte alignment
        if (next_tag_addr >= info_end) {
            // Allow only if the END tag starts exactly at info_end
            if (next_tag_addr == info_end && (current_tag_addr + tag->size <= info_end) && tag->type == MULTIBOOT_TAG_TYPE_END) break;
            terminal_printf("[Boot Error] Next Multiboot tag calculation OOB (Next Addr=0x%x, Info End=0x%x).\n", next_tag_addr, info_end);
            return NULL;
        }
        tag = (struct multiboot_tag *)next_tag_addr;
    }
    return NULL; // End tag reached or error
}


// === Memory Area Parsing Helper === (Improved Validation)
static bool parse_memory_map(struct multiboot_tag_mmap *mmap_tag,
                             uintptr_t *out_total_memory,
                             uintptr_t *out_heap_base_addr, size_t *out_heap_size)
{
    uintptr_t current_total_memory = 0;
    uintptr_t best_heap_base = 0;
    uint64_t best_heap_size_64 = 0;
    multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
    uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;
    uintptr_t kernel_start_phys_addr = (uintptr_t)&_kernel_start_phys;
    uintptr_t kernel_end_phys_addr = (uintptr_t)&_kernel_end_phys;

    terminal_write("Memory Map (from Multiboot):\n");
    while ((uintptr_t)mmap_entry < mmap_end) {
        if (mmap_tag->entry_size == 0 || mmap_tag->entry_size < sizeof(multiboot_memory_map_t)) {
            terminal_printf("  [ERR] MMAP entry size (%u) invalid!\n", mmap_tag->entry_size);
            return false;
        }

        uintptr_t region_start = (uintptr_t)mmap_entry->addr;
        uint64_t region_len = mmap_entry->len;
        uintptr_t region_end = region_start + region_len;

        terminal_printf("  Addr: 0x%08x Len: 0x%08x Type: %d\n",
                        region_start, (uint32_t)region_len, mmap_entry->type);

        // Update total memory detected
        if (region_end > current_total_memory) {
            current_total_memory = region_end;
        }

        // Consider AVAILABLE memory >= 1MB for the heap
        if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE && region_start >= 0x100000) {
            uintptr_t usable_start = region_start;
            uint64_t usable_len = region_len;

            // Check for overlap with the kernel physical region
            uintptr_t max_start = (usable_start > kernel_start_phys_addr) ? usable_start : kernel_start_phys_addr;
            uintptr_t min_end = (region_end < kernel_end_phys_addr) ? region_end : kernel_end_phys_addr;

            if (max_start < min_end) { // Overlap detected
                // Check portion before kernel
                if (usable_start < kernel_start_phys_addr) {
                    if (kernel_start_phys_addr - usable_start > best_heap_size_64) {
                        best_heap_size_64 = kernel_start_phys_addr - usable_start;
                        best_heap_base = usable_start;
                    }
                }
                // Check portion after kernel
                if (region_end > kernel_end_phys_addr) {
                    usable_start = kernel_end_phys_addr;
                    usable_len = region_end - kernel_end_phys_addr;
                } else {
                    usable_len = 0; // Fully contained, no usable part here
                }
            }

            // Update best heap region if this (potentially adjusted) part is larger
            if (usable_len > best_heap_size_64) {
                best_heap_size_64 = usable_len;
                best_heap_base = usable_start;
            }
        }

        // Advance to the next entry
        uintptr_t next_entry_addr = (uintptr_t)mmap_entry + mmap_tag->entry_size;
        if (next_entry_addr > mmap_end) { // Check before dereferencing next entry
             terminal_write("  [Warning] MMAP entry advancement out of bounds.\n");
             break;
        }
        mmap_entry = (multiboot_memory_map_t *)next_entry_addr;
    }

    // Final checks and output assignment
    if (best_heap_size_64 > 0) {
        *out_heap_base_addr = best_heap_base;
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

    *out_total_memory = ALIGN_UP(current_total_memory, PAGE_SIZE);
    terminal_printf("  Total Physical Memory Detected: %u MB\n", *out_total_memory / (1024*1024));
    terminal_printf("  Selected Heap Region: Phys Addr=0x%x, Size=%u bytes\n",
                    *out_heap_base_addr, *out_heap_size);
    return true;
}


/**
 * @brief Finds the first available physical page frame >= 1MB from the memory map.
 * Checks for overlaps with kernel image.
 *
 * @param mmap_tag Pointer to the Multiboot memory map tag.
 * @return Physical address of a free frame, or 0 if none found/error.
 */
static uintptr_t find_early_free_frame(struct multiboot_tag_mmap *mmap_tag) {
    if (!mmap_tag) return 0;

    multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
    uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;
    uintptr_t kernel_start_phys_addr = (uintptr_t)&_kernel_start_phys;
    uintptr_t kernel_end_phys_addr = (uintptr_t)&_kernel_end_phys;

    terminal_write("  [Kernel] Searching for early free frame >= 1MB...\n");

    while ((uintptr_t)mmap_entry < mmap_end) {
        // Validate entry size before use
        if (mmap_tag->entry_size == 0 || mmap_tag->entry_size < sizeof(multiboot_memory_map_t)) break;

        if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
            uintptr_t region_start = (uintptr_t)mmap_entry->addr;
            uint64_t region_len = mmap_entry->len;
            uintptr_t region_end = region_start + region_len;

            // Iterate through pages in this available region
            uintptr_t current_page = ALIGN_UP(region_start, PAGE_SIZE);

            while (current_page + PAGE_SIZE <= region_end) {
                // Check if page is >= 1MB
                if (current_page >= 0x100000) {
                    // Check if it overlaps with the kernel image
                    bool overlaps_kernel = (current_page < kernel_end_phys_addr &&
                                           (current_page + PAGE_SIZE) > kernel_start_phys_addr);

                    if (!overlaps_kernel) {
                        // Found a suitable page
                        terminal_printf("  [Kernel] Found suitable early frame: Phys=0x%x\n", current_page);
                        return current_page;
                    }
                }
                // Check for potential overflow before adding PAGE_SIZE
                if (current_page > UINTPTR_MAX - PAGE_SIZE) break;
                current_page += PAGE_SIZE;
            }
        }

        // Advance to the next entry
        uintptr_t next_entry_addr = (uintptr_t)mmap_entry + mmap_tag->entry_size;
        if (next_entry_addr > mmap_end) break; // Prevent reading past end
        mmap_entry = (multiboot_memory_map_t *)next_entry_addr;
    }

    terminal_write("  [Kernel Error] No suitable early free frame found!\n");
    return 0; // Not found
}


// === Memory Subsystem Initialization (REVISED ORDER) ===
static bool init_memory(uint32_t mb_info_phys_addr) {
    terminal_write("[Kernel] Initializing Memory Subsystems (v2)...\n");

    // --- Get Memory Map and Calculate Ranges ---
    struct multiboot_tag_mmap *mmap_tag = (struct multiboot_tag_mmap *)find_multiboot_tag(
        mb_info_phys_addr, MULTIBOOT_TAG_TYPE_MMAP);
    if (!mmap_tag) KERNEL_PANIC_HALT("Multiboot memory map tag not found!");

    uintptr_t total_memory = 0;
    uintptr_t heap_phys_start = 0;
    size_t heap_size = 0;
    if (!parse_memory_map(mmap_tag, &total_memory, &heap_phys_start, &heap_size)) {
        KERNEL_PANIC_HALT("Failed to parse memory map or find heap region!");
    }
    if (heap_size < MIN_HEAP_SIZE) KERNEL_PANIC_HALT("Heap region too small!");

    uintptr_t kernel_phys_start = (uintptr_t)&_kernel_start_phys;
    uintptr_t kernel_phys_end = (uintptr_t)&_kernel_end_phys;

    // --- Stage 0: Find Frame for Initial Page Directory ---
    terminal_write(" Stage 0: Finding early frame for PD...\n");
    uintptr_t initial_pd_phys = find_early_free_frame(mmap_tag);
    if (!initial_pd_phys) {
        KERNEL_PANIC_HALT("Failed to find a free physical frame for the initial PD!");
    }

    // --- Stage 1: Initialize Page Directory Structure ---
    terminal_write(" Stage 1: Initializing Page Directory Structure...\n");
    if (paging_initialize_directory(initial_pd_phys) == 0) { // Pass the found address
        KERNEL_PANIC_HALT("Failed to initialize page directory structure!");
    }

    // --- Stage 2: Setup Early Physical Mappings ---
    terminal_write(" Stage 2: Setting up early physical maps...\n");
    if (paging_setup_early_maps(initial_pd_phys, kernel_phys_start, kernel_phys_end, heap_phys_start, heap_size) != 0) {
        KERNEL_PANIC_HALT("Failed to set up early physical memory maps!");
    }
    // The initial PD frame is now identity-mapped.

    // --- Stage 2.5: Zero the Initial Page Directory ---
    terminal_printf(" Stage 2.5: Zeroing initial PD frame at phys 0x%x...\n", initial_pd_phys);
    // Access via physical address (which is identity mapped now)
    memset((void*)initial_pd_phys, 0, PAGE_SIZE);
    // Re-apply early mappings because memset cleared them
    terminal_write("   Re-applying early physical maps after zeroing PD...\n");
    if (paging_setup_early_maps(initial_pd_phys, kernel_phys_start, kernel_phys_end, heap_phys_start, heap_size) != 0) {
       KERNEL_PANIC_HALT("Failed to re-apply early physical memory maps!");
    }

    // --- Stage 3: Initialize Buddy Allocator ---
    terminal_write(" Stage 3: Initializing Buddy Allocator...\n");
    // Pass the heap region which is now identity-mapped.
    buddy_init((void *)heap_phys_start, heap_size);
    if (buddy_free_space() == 0 && heap_size >= MIN_BLOCK_SIZE) { // Check if init likely failed
        KERNEL_PANIC_HALT("Buddy Allocator initialization failed (no free space reported)!");
    }
    terminal_printf("  Buddy Initial Free Space: %u bytes\n", buddy_free_space());

    // --- Stage 4: Finalize Virtual Mappings & Activate Paging ---
    terminal_write(" Stage 4: Finalizing mappings and activating paging...\n");
    // Uses buddy allocator internally now to map higher-half physical memory
    if (paging_finalize_and_activate(initial_pd_phys, total_memory) != 0) {
        KERNEL_PANIC_HALT("Failed to finalize mappings or activate paging!");
    }
    // Paging is ON. Globals g_kernel_page_directory_phys/virt are set.

    // --- Stage 5: Initialize Frame Allocator ---
    terminal_write(" Stage 5: Initializing Frame Allocator...\n");
    // Requires buddy and active paging. Reserves the initial_pd_phys frame.
    if (frame_init(mmap_tag, kernel_phys_start, kernel_phys_end, heap_phys_start, heap_phys_start + heap_size) != 0) {
        KERNEL_PANIC_HALT("Frame Allocator initialization failed!");
    }

    // --- Stage 6: Initialize Kmalloc ---
    terminal_write(" Stage 6: Initializing Kmalloc...\n");
    kmalloc_init(); // Depends on frame allocator -> buddy

    terminal_write("[OK] Memory Subsystems Initialized.\n");
    return true;
}


// === Kernel Idle Task ===
void kernel_idle_task(void) {
    terminal_write("[Idle] Kernel idle task started. Halting CPU when idle.\n");
    while(1) {
        asm volatile("sti"); // Enable interrupts briefly
        asm volatile("hlt"); // Halt CPU until next interrupt
        // Interrupts are automatically disabled by CPU on interrupt entry.
        // We loop back and re-enable them before halting again.
    }
}

// === Main Kernel Entry Point ===
void main(uint32_t magic, uint32_t mb_info_phys_addr) {
    // 1. Early Initialization (Terminal, GDT, TSS, IDT)
    terminal_init();
    terminal_write("=== UiAOS Kernel Booting (v3 - Map Parse Init) ===\n\n");

    // Check Multiboot Magic
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        KERNEL_PANIC_HALT("Invalid Multiboot Magic.");
    }
    terminal_printf("[Boot] Multiboot magic OK (Info at phys 0x%x).\n", mb_info_phys_addr);

    // Initialize GDT and TSS
    terminal_write("[Kernel] Initializing GDT & TSS...\n");
    gdt_init();

    // Initialize IDT and PICs
    terminal_write("[Kernel] Initializing IDT & PIC...\n");
    idt_init();

    // 2. Memory Management Initialization (Revised Order)
    if (!init_memory(mb_info_phys_addr)) {
        // Panic occurs within init_memory if it returns false
        return; // Should not be reached
    }

    // 3. Hardware Driver Initialization (Post-Memory)
    terminal_write("[Kernel] Initializing PIT...\n");
    init_pit(); // Depends on IDT
    terminal_write("[Kernel] Initializing Keyboard...\n");
    keyboard_init(); // Depends on IDT
    keymap_load(KEYMAP_NORWEGIAN); // Set desired layout

    // 4. Filesystem Initialization (Optional)
    terminal_write("[Kernel] Initializing Filesystem...\n");
    if (fs_init() == FS_SUCCESS) {
        terminal_write("  [OK] Filesystem initialized and root mounted.\n");
        list_mounts();
    } else {
        terminal_write("  [Warning] Filesystem initialization failed. Continuing without FS.\n");
        // KERNEL_PANIC_HALT("Filesystem init failed."); // Uncomment if FS is essential
    }

    // 5. Scheduler and Initial Process Creation
    terminal_write("[Kernel] Initializing Scheduler...\n");
    scheduler_init(); // Depends on memory allocators

    terminal_write("[Kernel] Creating initial user process ('/hello.elf')...\n");
    const char *user_prog_path = "/hello.elf";
    if (!fs_is_initialized()) { // Check if FS is actually up before trying to load
         terminal_write("  [Info] Filesystem not initialized, cannot load initial process.\n");
    } else {
        pcb_t *user_proc_pcb = create_user_process(user_prog_path); // Requires allocators, paging, FS
        if (user_proc_pcb) {
            terminal_printf("  [OK] Process created (PID %d). Adding to scheduler.\n", user_proc_pcb->pid);
            if (scheduler_add_task(user_proc_pcb) != 0) {
                 destroy_process(user_proc_pcb); // Clean up failed process
                 KERNEL_PANIC_HALT("Cannot add initial process to scheduler.");
            } else {
                 terminal_write("  [OK] Initial user process added to scheduler.\n");
                 pit_set_scheduler_ready(); // Allow PIT handler to call schedule()
                 terminal_write("[Kernel] Scheduler marked as ready.\n");
            }
        } else {
            terminal_printf("  [ERROR] Failed to create initial user process from '%s'.\n", user_prog_path);
            KERNEL_PANIC_HALT("Failed to create initial process.");
        }
    }

    // 6. Start Scheduling / Idle Loop
    terminal_write("\n[Kernel] Initialization complete. Enabling interrupts and entering idle task.\n");
    terminal_write("======================================================================\n");
    // If no tasks were added (e.g., FS failed), this will just halt.
    // If tasks were added, the first timer interrupt will trigger the scheduler.
    kernel_idle_task();

    // --- Should not be reached ---
    KERNEL_PANIC_HALT("Reached end of main() unexpectedly!");
}