/**
 * kernel.c - Main kernel entry point for UiAOS
 *
 * Initializes core subsystems, parses Multiboot information, sets up memory,
 * drivers, filesystem, scheduler, and starts the initial user process before
 * entering the idle loop.
 */

// === Standard/Core Headers ===
#include <multiboot2.h>      // Multiboot2 specification
#include "types.h"          // Core type definitions
#include <string.h>         // Kernel's string functions (memcpy, memset, etc.)
#include <libc/stdint.h>    // Fixed-width integers (SIZE_MAX)

// === Kernel Subsystems ===
#include "terminal.h"       // Early console output
#include "gdt.h"            // Global Descriptor Table
#include "tss.h"            // Task State Segment
#include "idt.h"            // Interrupt Descriptor Table
#include "paging.h"         // Paging functions and constants
#include "frame.h"          // Physical frame allocator
#include "buddy.h"          // Physical page allocator (buddy system)
#include "slab.h"           // Slab allocator (used by kmalloc)
#include "percpu_alloc.h"   // Per-CPU slab allocator support
#include "kmalloc.h"        // Kernel dynamic memory allocator facade
#include "kmalloc_internal.h" // Internal kmalloc helpers (ALIGN_UP)
#include "process.h"        // Process Control Block management
#include "scheduler.h"      // Task scheduling
#include "syscall.h"        // System call interface definitions (not handlers)
#include "elf_loader.h"     // ELF binary loading
#include "vfs.h"            // Virtual File System interface
#include "mount.h"          // Filesystem mounting
#include "fs_init.h"        // Filesystem layer initialization
#include "fs_errno.h"       // Filesystem error codes
#include "read_file.h"      // Helper to read entire files

// === Drivers ===
#include "pit.h"            // Programmable Interval Timer
#include "keyboard.h"       // Keyboard driver
#include "keymap.h"         // Keyboard layout mapping
#include "pc_speaker.h"     // PC Speaker driver
#include "song.h"           // Song definitions for speaker
#include "song_player.h"    // Song playback logic
#include "my_songs.h"       // Specific song data

// === Utilities ===
#include "get_cpu_id.h"     // Function to get current CPU ID
#include "cpuid.h"          // CPUID instruction helper

// === Constants ===
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
#define KERNEL_PANIC_HALT() do { terminal_write("\n[KERNEL PANIC] System Halted.\n"); while(1) { asm volatile("cli; hlt"); } } while(0)
#define MIN_HEAP_SIZE (1 * 1024 * 1024) // Minimum acceptable heap size

// === Linker Symbols ===
// Provided by linker script (e.g., linker.ld)
extern uint32_t _kernel_start_phys; // Physical start address of kernel code/data
extern uint32_t _kernel_end_phys;   // Physical end address of kernel code/data

// === Static Function Prototypes ===
static struct multiboot_tag *find_multiboot_tag(uint32_t mb_info_phys_addr, uint16_t type);
static bool find_largest_memory_area(struct multiboot_tag_mmap *mmap_tag, uintptr_t *out_base_addr, size_t *out_size);
static bool init_memory(uint32_t mb_info_phys_addr);
void kernel_idle_task(void); // Keep non-static for potential future assembly calls

// === Multiboot Tag Finding Helper ===
static struct multiboot_tag *find_multiboot_tag(uint32_t mb_info_phys_addr, uint16_t type) {
    // Check if the address is valid (basic check, might need mapping later)
    if (mb_info_phys_addr == 0) {
        terminal_write("[Boot] Error: Multiboot info address is NULL.\n");
        return NULL;
    }

    // Read total size from the structure start
    // Ensure address is accessible (usually identity mapped by bootloader < 1MB)
    uint32_t total_size = *(uint32_t*)mb_info_phys_addr;
    if (total_size < 8) { // Minimum size includes total_size and reserved
        terminal_printf("[Boot] Error: Multiboot total_size (%u) is too small.\n", total_size);
        return NULL;
    }

    // Tags start after 'total_size' and 'reserved' fields (8 bytes total)
    struct multiboot_tag *tag = (struct multiboot_tag *)(mb_info_phys_addr + 8);
    uintptr_t info_end = mb_info_phys_addr + total_size; // Calculate end boundary

    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        uintptr_t current_tag_addr = (uintptr_t)tag;
        // Bounds check: Ensure current tag starts within the info structure
        if (current_tag_addr >= info_end) {
            terminal_printf("[Boot] Error: Multiboot tag parsing went out of bounds (Tag Addr=0x%x, Info End=0x%x).\n", current_tag_addr, info_end);
            return NULL;
        }
        // Check tag size is reasonable (at least size of base struct fields: type + size)
        if (tag->size < 8) { // Use 8 for minimum size (type+size)
            terminal_printf("[Boot] Error: Multiboot tag has invalid size %u at Addr=0x%x.\n", tag->size, current_tag_addr);
            return NULL;
        }

        // Found the requested tag type
        if (tag->type == type) {
            // Additional check: ensure the found tag doesn't extend beyond info_end
            if (current_tag_addr + tag->size > info_end) {
                 terminal_printf("[Boot] Error: Found tag type %u but its size %u extends beyond info end (Addr=0x%x, Info End=0x%x).\n", tag->type, tag->size, current_tag_addr, info_end);
                 return NULL;
            }
            return tag;
        }

        // Advance to the next tag, ensuring 8-byte alignment
        uintptr_t next_tag_addr = current_tag_addr + ((tag->size + 7) & ~7);
        // Check if the *start* of the next tag would be out of bounds.
        if (next_tag_addr >= info_end) {
            // Allow if the *next* tag is the END tag and starts exactly at the boundary
            if (next_tag_addr == info_end && ((struct multiboot_tag *)next_tag_addr)->type == MULTIBOOT_TAG_TYPE_END) {
                // Reached end tag perfectly, search failed
                break;
            }
             terminal_printf("[Boot] Error: Next Multiboot tag calculation out of bounds (Next Addr=0x%x, Info End=0x%x).\n", next_tag_addr, info_end);
             return NULL; // Avoid reading past end
        }
        tag = (struct multiboot_tag *)next_tag_addr;
    }

    // Reached the end tag without finding the requested type
    return NULL;
}

// === Memory Area Finding Helper ===
static bool find_largest_memory_area(struct multiboot_tag_mmap *mmap_tag, uintptr_t *out_base_addr, size_t *out_size) {
    uintptr_t best_base = 0;
    uint64_t best_size = 0; // Use 64-bit to handle large regions before converting to size_t
    multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
    uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;
    uintptr_t kernel_end_aligned = PAGE_ALIGN_UP((uintptr_t)&_kernel_end_phys); // Align kernel end UP

    terminal_write("Memory Map (from Multiboot):\n");
    while ((uintptr_t)mmap_entry < mmap_end) {
        // Basic validation of entry size from header
        if (mmap_tag->entry_size < sizeof(multiboot_memory_map_t)) { // Ensure size is at least minimum expected
             terminal_printf("  [FATAL] MMAP entry size (%u) in header is too small!\n", mmap_tag->entry_size);
             return false;
        }

        terminal_printf("  Addr: 0x%x%x Len: 0x%x%x Type: %d\n",
                        (uint32_t)(mmap_entry->addr >> 32), (uint32_t)mmap_entry->addr,
                        (uint32_t)(mmap_entry->len >> 32), (uint32_t)mmap_entry->len,
                        mmap_entry->type);

        // We need AVAILABLE memory located at or above 1MB (0x100000) for the main heap
        if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE && mmap_entry->addr >= 0x100000) {
            uintptr_t region_start = (uintptr_t)mmap_entry->addr;
            uint64_t region_len = mmap_entry->len;
            uintptr_t usable_start = region_start;

            // Adjust start if the region overlaps with the kernel's physical memory
            if (usable_start < kernel_end_aligned) {
                if (region_start + region_len > kernel_end_aligned) {
                    // Region starts below kernel end but ends after it; adjust start and length
                    uint64_t overlap = kernel_end_aligned - usable_start;
                    usable_start = kernel_end_aligned;
                    region_len = (region_len > overlap) ? region_len - overlap : 0;
                } else {
                    // Region is entirely below kernel end; unusable for our heap
                    region_len = 0;
                }
            }

            // Update best region if this one is larger and usable
            if (region_len > best_size) {
                best_size = region_len;
                best_base = usable_start;
            }
        }
        // Advance to the next entry using the entry_size from the tag header
        // Add bounds check for advancement
        uintptr_t next_entry_addr = (uintptr_t)mmap_entry + mmap_tag->entry_size;
        if (next_entry_addr > mmap_end) {
            terminal_printf("  [Warning] MMAP entry advancement goes beyond tag size.\n");
            break; // Stop processing to avoid reading OOB
        }
        mmap_entry = (multiboot_memory_map_t *)next_entry_addr;
    }

    // Check if a suitable region was found
    if (best_size > 0) {
        *out_base_addr = best_base;
        // Safely convert best_size (uint64_t) to size_t
        if (best_size > (uint64_t)SIZE_MAX) {
            terminal_write("  [Warning] Largest memory region exceeds size_t capacity! Clamping.\n");
            *out_size = SIZE_MAX;
        } else {
            *out_size = (size_t)best_size;
        }
        terminal_printf("  Selected Heap Region: Phys Addr=0x%x, Size=%u bytes (%u MB)\n",
                        *out_base_addr, *out_size, *out_size / (1024 * 1024));
        return true;
    }

    terminal_write("  [FATAL] No suitable memory region found above 1MB for heap!\n");
    return false;
}

// === Memory Subsystem Initialization ===
static bool init_memory(uint32_t mb_info_phys_addr) {
    terminal_write("[Kernel] Initializing Memory Subsystems...\n");

    // --- Find Memory Map and Heap Region ---
    struct multiboot_tag_mmap *mmap_tag = (struct multiboot_tag_mmap *)find_multiboot_tag(
        mb_info_phys_addr, MULTIBOOT_TAG_TYPE_MMAP);
    if (!mmap_tag) {
        terminal_write("  [FATAL] Multiboot memory map tag not found!\n");
        return false;
    }

    uintptr_t heap_phys_start = 0;
    size_t heap_size = 0;
    uintptr_t total_memory = 0;

    // Determine total physical memory size
    multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
    uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;
    while ((uintptr_t)mmap_entry < mmap_end) {
        if (mmap_tag->entry_size == 0) break;
        uintptr_t region_end = (uintptr_t)mmap_entry->addr + (uintptr_t)mmap_entry->len;
        if (region_end > total_memory) total_memory = region_end;
         // Check entry size before advancing
        if (mmap_tag->entry_size < sizeof(multiboot_memory_map_t)) break;
        uintptr_t next_entry_addr = (uintptr_t)mmap_entry + mmap_tag->entry_size;
        if (next_entry_addr > mmap_end) break;
        mmap_entry = (multiboot_memory_map_t *)next_entry_addr;
    }
    total_memory = ALIGN_UP(total_memory, PAGE_SIZE);
    terminal_printf("  Detected Total Memory: %u MB\n", total_memory / (1024*1024));

    // Find the largest suitable region for the buddy heap
    if (!find_largest_memory_area(mmap_tag, &heap_phys_start, &heap_size)) {
        return false; // Error already printed
    }
    if (heap_size < MIN_HEAP_SIZE) {
        terminal_printf("  [FATAL] Selected heap region size (%u bytes) is less than minimum required (%u bytes).\n", heap_size, MIN_HEAP_SIZE);
        return false;
    }

    // Align heap start UP for the buddy allocator
    size_t required_alignment = (MIN_BLOCK_SIZE > DEFAULT_ALIGNMENT) ? MIN_BLOCK_SIZE : DEFAULT_ALIGNMENT;
    uintptr_t aligned_heap_start = ALIGN_UP(heap_phys_start, required_alignment);
    size_t adjustment = aligned_heap_start - heap_phys_start;
    if (heap_size <= adjustment) {
        terminal_printf("  [FATAL] Heap size (%u bytes) is too small after alignment adjustment (%u bytes).\n", heap_size, adjustment);
        return false;
    }
    heap_phys_start = aligned_heap_start;
    heap_size -= adjustment;
    // --- End Find Memory Map and Heap Region ---


    // --- Initialize Memory Allocators (Order is CRITICAL!) ---

    // 1. Buddy Allocator (Manages physical pages/blocks)
    terminal_write("  Initializing Buddy Allocator...\n");
    buddy_init((void *)heap_phys_start, heap_size);
    if (buddy_free_space() == 0 && heap_size >= MIN_BLOCK_SIZE) {
        terminal_write("  [FATAL] Buddy Allocator initialization failed (no free space reported)!\n");
        return false;
    }
    terminal_printf("  Buddy Initial Free Space: %u bytes\n", buddy_free_space());

    // 2. Paging (Sets up virtual memory, uses buddy_alloc for early frames)
    terminal_write("  Initializing Paging...\n");
    if (paging_init((uintptr_t)&_kernel_start_phys, (uintptr_t)&_kernel_end_phys, total_memory) != 0) {
        terminal_write("  [FATAL] Paging initialization failed!\n");
        return false;
    }

    // 3. Frame Allocator (Reference counting, uses buddy + paging)
    terminal_write("  Initializing Frame Allocator...\n");
    if (frame_init(mmap_tag, (uintptr_t)&_kernel_start_phys, (uintptr_t)&_kernel_end_phys, heap_phys_start, heap_phys_start + heap_size) != 0) {
        terminal_write("  [FATAL] Frame Allocator initialization failed!\n");
        return false;
    }

    // 4. Kmalloc (Kernel heap facade, uses slab/percpu -> buddy)
    terminal_write("  Initializing Kmalloc...\n");
    kmalloc_init();

    terminal_write("[OK] Memory Subsystems Initialized.\n");
    return true;
}

// === Kernel Idle Task ===
void kernel_idle_task(void) {
    terminal_write("[Idle] Kernel idle task started. Halting CPU when idle.\n");
    while(1) {
        asm volatile("sti"); // Enable interrupts
        asm volatile("hlt"); // Halt CPU until next interrupt
        // Interrupts are automatically disabled by CPU on interrupt entry.
        // No need for cli here unless specific race conditions exist before hlt.
    }
}

// === Main Kernel Entry Point ===
void main(uint32_t magic, uint32_t mb_info_phys_addr) {
    // 1. Early Initialization (Terminal, GDT, TSS, IDT)
    terminal_init();
    terminal_write("=== UiAOS Kernel Booting ===\n\n");

    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        terminal_write("[FATAL] Invalid Multiboot magic number received from bootloader.\n");
        KERNEL_PANIC_HALT();
    }
    terminal_printf("[Boot] Multiboot magic OK (Info at phys 0x%x).\n", mb_info_phys_addr);

    terminal_write("[Kernel] Initializing GDT & TSS...\n");
    gdt_init();

    terminal_write("[Kernel] Initializing IDT & PIC...\n");
    idt_init();

    // 2. Memory Management Initialization
    if (!init_memory(mb_info_phys_addr)) {
        terminal_write("[FATAL] Core memory system initialization failed.\n");
        KERNEL_PANIC_HALT();
    }

    // 3. Hardware Driver Initialization (Requires Memory Management)
    terminal_write("[Kernel] Initializing PIT...\n");
    init_pit();

    terminal_write("[Kernel] Initializing Keyboard...\n");
    keyboard_init();
    keymap_load(KEYMAP_NORWEGIAN); // Set desired layout

    // 4. Filesystem Initialization (Requires Memory, Drivers potentially)
    terminal_write("[Kernel] Initializing Filesystem...\n");
    if (fs_init() == FS_SUCCESS) {
        terminal_write("  [OK] Filesystem initialized and root mounted.\n");
        list_mounts(); // List mounted filesystems
    } else {
        // Non-fatal? Depends on kernel requirements.
        terminal_write("  [Warning] Filesystem initialization failed. Continuing without FS.\n");
    }

    // 5. Scheduler and Initial Process Creation
    terminal_write("[Kernel] Initializing Scheduler...\n");
    scheduler_init();

    terminal_write("[Kernel] Creating initial user process ('/hello.elf')...\n");
    const char *user_prog_path = "/hello.elf"; // Path within the mounted filesystem
    pcb_t *user_proc_pcb = create_user_process(user_prog_path);

    if (user_proc_pcb) {
        terminal_printf("  [OK] Process created (PID %d). Adding to scheduler.\n", user_proc_pcb->pid);
        if (scheduler_add_task(user_proc_pcb) != 0) {
             terminal_write("  [ERROR] Failed to add initial process to scheduler. Destroying process.\n");
            destroy_process(user_proc_pcb);
            terminal_write("[Kernel] Cannot proceed without an initial process.\n");
            KERNEL_PANIC_HALT(); // If init process is essential
        } else {
            terminal_write("  [OK] Initial user process added to scheduler.\n");
            // IMPORTANT: Mark scheduler as ready ONLY after adding the first task.
            pit_set_scheduler_ready();
            terminal_write("[Kernel] Scheduler marked as ready.\n");
        }
    } else {
        terminal_printf("  [ERROR] Failed to create initial user process from '%s'.\n", user_prog_path);
        terminal_write("[Kernel] Cannot proceed without an initial process.\n");
        KERNEL_PANIC_HALT();
    }

    // 6. Start Scheduling (Enter Idle Task)
    terminal_write("\n[Kernel] Initialization complete. Enabling interrupts and entering idle task.\n");
    terminal_write("======================================================================\n");
    kernel_idle_task(); // Enters the idle loop (sti; hlt)

    // --- Code below should ideally not be reached ---
    terminal_write("\n[KERNEL ERROR] Reached end of main() unexpectedly!\n");
    KERNEL_PANIC_HALT();
}