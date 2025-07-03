/**
 * @file kernel.c
 * @brief Main kernel entry point for Coal OS
 * 
 * Description:
 * This file contains the main entry point (`main`) for the Coal OS kernel,
 * invoked by the Multiboot2 bootloader. It orchestrates the initialization
 * sequence for all major kernel subsystems: GDT/TSS, memory management (paging,
 * physical and virtual allocators), interrupt handling (IDT/PICs), essential
 * hardware drivers (PIT, Keyboard, ATA), the scheduler, VFS and FAT filesystem,
 * and finally launches initial user-space processes.
 */

// === Standard/Core Headers ===
#include <kernel/arch/multiboot2.h>
#include <kernel/core/types.h>
#include <kernel/core/init.h>
#include <kernel/lib/string.h>       // Kernel's string functions
#include <libc/stdint.h>  // Kernel's fixed-width integers
#include <libc/stddef.h>  // Kernel's NULL, offsetof

// === Kernel Subsystems ===
#include <kernel/drivers/display/terminal.h>
#include <kernel/cpu/gdt.h>
#include <kernel/cpu/tss.h>
#include <kernel/cpu/idt.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/frame.h>
#include <kernel/memory/buddy.h>
#include <kernel/memory/slab.h>
#include <kernel/memory/percpu_alloc.h> 
#include <kernel/memory/kmalloc.h>
#include <kernel/process/process.h>
#include <kernel/process/scheduler.h>
#include <kernel/cpu/syscall.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/fs/vfs/mount.h>
#include <kernel/fs/vfs/fs_init.h>
#include <kernel/fs/vfs/fs_errno.h>

// === Drivers ===
#include <kernel/drivers/timer/pit.h>
#include <kernel/drivers/input/keyboard.h>      // For keyboard_init()
#include <kernel/drivers/input/keymap.h>        // For keymap_load()
#include <kernel/drivers/input/keyboard_hw.h>   // For KBC_CMD_*, KBC_SR_* (KBC hardware definitions)
#include <kernel/lib/port_io.h>       // For inb/outb used in KBC re-check

// === Utilities ===
#include <kernel/drivers/display/serial.h>         // Essential for early/debug logging
#include <kernel/lib/assert.h>         // KERNEL_ASSERT, KERNEL_PANIC_HALT

// === Test Functions ===
extern pcb_t *create_simple_user_process(void);    // Hardcoded user process without filesystem

// === Constants ===
#define KERNEL_VERSION_STRING "0.1.0" // Coal OS Version
#define MULTIBOOT2_BOOTLOADER_MAGIC_EXPECTED 0x36d76289
#define MIN_USABLE_HEAP_SIZE (1 * 1024 * 1024) // 1MB
#define MAX_CLAMPED_INITIAL_HEAP_SIZE (256 * 1024 * 1024) // 256MB

#define INITIAL_TEST_PROGRAM_PATH "/hello.elf"
#define SYSTEM_SHELL_PATH         "/bin/shell.elf"

// Define KBC_MAX_FLUSH locally if not accessible via included headers for kernel.c
// Ideally, this would be in a shared KBC header or keyboard_hw.h
#ifndef KBC_MAX_FLUSH
#define KBC_MAX_FLUSH 100       // Max reads during a flush
#endif


// === Linker Symbols (Physical Addresses) ===
extern uint8_t _kernel_start_phys;
extern uint8_t _kernel_end_phys;

// === Global Variables ===
uint32_t  g_multiboot_info_phys_addr_global = 0;
uintptr_t g_multiboot_info_virt_addr_global = 0;

// === Static Function Prototypes ===
static struct multiboot_tag *find_multiboot_tag_phys(uint32_t mb_info_phys, uint16_t type);
static struct multiboot_tag *find_multiboot_tag_virt(uintptr_t mb_info_virt, uint16_t type);
static bool parse_memory_map_for_heap(struct multiboot_tag_mmap *mmap_tag,
                                      uintptr_t *out_total_mem_span,
                                      uintptr_t *out_heap_base, size_t *out_heap_size);
static bool initialize_memory_management(uint32_t mb_info_phys);
static void launch_program(const char *path_on_disk, const char *program_description);


//-----------------------------------------------------------------------------
// Multiboot Information Parsing (Physical & Virtual)
//-----------------------------------------------------------------------------
static struct multiboot_tag *find_multiboot_tag_phys(uint32_t mb_info_phys, uint16_t type) {
    if (mb_info_phys == 0 || mb_info_phys >= 0x100000) return NULL;
    volatile uint32_t* header = (volatile uint32_t*)((uintptr_t)mb_info_phys);
    uint32_t total_size = header[0];
    if (total_size < 8 || total_size > 0x100000) return NULL; 

    volatile struct multiboot_tag *tag = (volatile struct multiboot_tag *)(mb_info_phys + 8);
    uintptr_t info_end = mb_info_phys + total_size;

    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        uintptr_t current_tag_addr = (uintptr_t)tag;
        if (current_tag_addr + sizeof(struct multiboot_tag) > info_end || tag->size < 8 || (current_tag_addr + tag->size) > info_end) return NULL;
        if (tag->type == type) return (struct multiboot_tag *)tag; 
        uintptr_t next_tag_addr = current_tag_addr + ((tag->size + 7) & ~7); 
        if (next_tag_addr <= current_tag_addr || next_tag_addr >= info_end) break; 
        tag = (volatile struct multiboot_tag *)next_tag_addr;
    }
    return NULL;
}

static struct multiboot_tag *find_multiboot_tag_virt(uintptr_t mb_info_virt, uint16_t type) {
    if (mb_info_virt == 0) return NULL;
    uint32_t* header = (uint32_t*)mb_info_virt;
    uint32_t total_size = header[0];
    if (total_size < 8 || total_size > 0x100000) return NULL;

    struct multiboot_tag *tag = (struct multiboot_tag *)(mb_info_virt + 8);
    uintptr_t info_end = mb_info_virt + total_size;

    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        uintptr_t current_tag_addr = (uintptr_t)tag;
        if (current_tag_addr + sizeof(struct multiboot_tag) > info_end || tag->size < 8 || (current_tag_addr + tag->size) > info_end) return NULL;
        if (tag->type == type) return tag;
        uintptr_t next_tag_addr = current_tag_addr + ((tag->size + 7) & ~7);
        if (next_tag_addr <= current_tag_addr || next_tag_addr >= info_end) break;
        tag = (struct multiboot_tag *)next_tag_addr;
    }
    return NULL;
}

//-----------------------------------------------------------------------------
// Memory Initialization
//-----------------------------------------------------------------------------
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

static inline uintptr_t safe_add_base_len(uintptr_t base, uint64_t len) {
    if (len > ((uint64_t)UINTPTR_MAX - base)) {
        return UINTPTR_MAX;
    }
    return base + (uintptr_t)len;
}

static bool parse_memory_map_for_heap(struct multiboot_tag_mmap *mmap_tag,
                                      uintptr_t *out_total_mem_span,
                                      uintptr_t *out_heap_base, size_t *out_heap_size)
{
    KERNEL_ASSERT(mmap_tag && out_total_mem_span && out_heap_base && out_heap_size, "parse_memory_map: Invalid args");
    uintptr_t total_span = 0;
    uintptr_t best_candidate_base = 0;
    uint64_t best_candidate_size64 = 0;
    multiboot_memory_map_t *entry = mmap_tag->entries;
    uintptr_t mmap_end_addr = (uintptr_t)mmap_tag + mmap_tag->size;
    uintptr_t kernel_phys_start_addr = (uintptr_t)&_kernel_start_phys;
    uintptr_t kernel_phys_end_addr = ALIGN_UP((uintptr_t)&_kernel_end_phys, PAGE_SIZE); 

    terminal_printf("  Kernel Physical Range: [%#010lx - %#010lx)\n", kernel_phys_start_addr, kernel_phys_end_addr);

    while ((uintptr_t)entry < mmap_end_addr && (uintptr_t)entry + mmap_tag->entry_size <= mmap_end_addr) {
        uintptr_t region_p_start = (uintptr_t)entry->addr;
        uint64_t region_p_len64 = entry->len;
        uintptr_t region_p_end = safe_add_base_len(region_p_start, region_p_len64);
        if (region_p_end < region_p_start) region_p_end = UINTPTR_MAX; 

        if (region_p_end > total_span) total_span = region_p_end;

        if (entry->type == MULTIBOOT_MEMORY_AVAILABLE && region_p_len64 >= MIN_USABLE_HEAP_SIZE) {
            uintptr_t current_candidate_start = region_p_start;
            uint64_t current_candidate_len64 = region_p_len64;

            if (current_candidate_start < kernel_phys_end_addr && region_p_end > kernel_phys_start_addr) {
                if (region_p_end > kernel_phys_end_addr) { 
                    current_candidate_start = kernel_phys_end_addr;
                    current_candidate_len64 = region_p_end - current_candidate_start;
                } else { 
                    current_candidate_len64 = 0;
                }
            }
            if (current_candidate_len64 >= MIN_USABLE_HEAP_SIZE) {
                if (current_candidate_len64 > best_candidate_size64) {
                    best_candidate_base = current_candidate_start;
                    best_candidate_size64 = current_candidate_len64;
                }
            }
        }
        entry = (multiboot_memory_map_t*)((uintptr_t)entry + mmap_tag->entry_size);
    }

    *out_total_mem_span = ALIGN_UP(total_span, PAGE_SIZE);
    if (*out_total_mem_span < total_span) *out_total_mem_span = UINTPTR_MAX; 

    if (best_candidate_base < 0x100000 && best_candidate_base != 0) {
        terminal_printf("  Warning: Best heap candidate below 1MB (%#lx).\n", best_candidate_base);
    }

    if (best_candidate_size64 >= MIN_USABLE_HEAP_SIZE && best_candidate_base != 0) {
        *out_heap_base = best_candidate_base;
        *out_heap_size = (best_candidate_size64 > (uint64_t)MAX_CLAMPED_INITIAL_HEAP_SIZE) ?
                         MAX_CLAMPED_INITIAL_HEAP_SIZE : (size_t)best_candidate_size64;
        if (*out_heap_size < MIN_USABLE_HEAP_SIZE) {
            terminal_printf("  [FATAL] Heap candidate (%#lx, size %zu) too small after clamping.\n", *out_heap_base, *out_heap_size);
            return false;
        }
        terminal_printf("  Best Heap Found: PhysBase=%#lx, Size=%zu (may be clamped)\n", *out_heap_base, *out_heap_size);
        return true;
    }
    terminal_printf("  [FATAL] No suitable heap region found (>= %u bytes).\n", MIN_USABLE_HEAP_SIZE);
    return false;
}


static bool initialize_memory_management(uint32_t mb_info_phys) {
    terminal_write("[Kernel] Initializing Memory Subsystems...\n");

    uintptr_t total_phys_memory_span;
    uintptr_t initial_heap_phys_base;
    size_t    initial_heap_size;
    uintptr_t kernel_phys_start = (uintptr_t)&_kernel_start_phys;
    uintptr_t kernel_phys_end_aligned = ALIGN_UP((uintptr_t)&_kernel_end_phys, PAGE_SIZE);

    terminal_write("  Stage 0: Parsing Multiboot memory map...\n");
    struct multiboot_tag_mmap *mmap_tag_phys = (struct multiboot_tag_mmap *)find_multiboot_tag_phys(mb_info_phys, MULTIBOOT_TAG_TYPE_MMAP);
    if (!mmap_tag_phys) KERNEL_PANIC_HALT("Multiboot MMAP tag not found!");
    if (!parse_memory_map_for_heap(mmap_tag_phys, &total_phys_memory_span, &initial_heap_phys_base, &initial_heap_size)) {
        KERNEL_PANIC_HALT("Failed to parse memory map or find suitable heap for buddy allocator!");
    }

    terminal_write("  Stage 1+2: Initializing Page Directory & Early Maps...\n");
    uintptr_t pd_phys;
    if (paging_initialize_directory(&pd_phys) != 0) KERNEL_PANIC_HALT("Failed to initialize Page Directory!");
    if (paging_setup_early_maps(pd_phys, kernel_phys_start, (uintptr_t)&_kernel_end_phys, initial_heap_phys_base, initial_heap_size) != 0) {
        KERNEL_PANIC_HALT("Failed to setup early paging maps!");
    }

    terminal_write("  Stage 3: Initializing Buddy Allocator...\n");
    buddy_init((void *)initial_heap_phys_base, initial_heap_size); 
    terminal_printf("    Buddy Allocator: Initial Free Space: %zu KB\n", buddy_free_space() / 1024);

    terminal_write("  Stage 4: Finalizing and Activating Paging...\n");
    if (paging_finalize_and_activate(pd_phys, total_phys_memory_span) != 0) KERNEL_PANIC_HALT("Failed to activate paging!");

    terminal_write("  Stage 4.5: Using Multiboot Info from Identity-Mapped Region...\n");
    
    // The multiboot info should be in the identity-mapped region (first 4MB)
    // So we can use it directly at its physical address
    if (g_multiboot_info_phys_addr_global >= 0x400000) {
        KERNEL_PANIC_HALT("Multiboot info outside identity-mapped region!");
    }
    
    // For now, just use the physical address directly since it's identity mapped
    g_multiboot_info_virt_addr_global = g_multiboot_info_phys_addr_global;
    
    // Read the size from the multiboot header
    size_t mb_total_struct_size = *(volatile uint32_t*)g_multiboot_info_virt_addr_global;
    
    // Sanity check the size
    if (mb_total_struct_size < 8 || mb_total_struct_size > 0x100000) {
        terminal_printf("    WARNING: Invalid multiboot size: %u bytes. Using default.\n", (unsigned)mb_total_struct_size);
        mb_total_struct_size = 8192; // Default to 8KB
    }
    
    terminal_printf("    Multiboot info at VIRT: %#lx (Size: %u bytes)\n", g_multiboot_info_virt_addr_global, (unsigned)mb_total_struct_size);


    terminal_write("  Stage 6: Initializing Frame Allocator...\n");
    struct multiboot_tag_mmap *mmap_tag_virt = (struct multiboot_tag_mmap *)find_multiboot_tag_virt(g_multiboot_info_virt_addr_global, MULTIBOOT_TAG_TYPE_MMAP);
    if (!mmap_tag_virt) KERNEL_PANIC_HALT("Cannot find MMAP tag via virtual address for Frame Allocator!");
    if (frame_init(mmap_tag_virt, kernel_phys_start, kernel_phys_end_aligned, initial_heap_phys_base, initial_heap_phys_base + initial_heap_size) != 0) {
        KERNEL_PANIC_HALT("Frame Allocator initialization failed!");
    }

    terminal_write("  Stage 7: Initializing Kmalloc...\n");
    kmalloc_init(); 

    terminal_write("  Stage 8: Initializing Temporary VA Mapper...\n");
    if (paging_temp_map_init() != 0) KERNEL_PANIC_HALT("Failed to initialize temporary VA mapper!");


    terminal_write("[OK] Memory Subsystems Initialized Successfully.\n");
    return true;
}


//-----------------------------------------------------------------------------
// Initial Process Launch Helper
//-----------------------------------------------------------------------------
static void launch_program(const char *path_on_disk, const char *program_description) {
    terminal_printf("[Kernel] Attempting to launch %s from '%s'...\n", program_description, path_on_disk);
    pcb_t *proc_pcb = create_user_process(path_on_disk);

    if (proc_pcb) {
        if (scheduler_add_task(proc_pcb) == 0) {
            terminal_printf("  [OK] %s (PID %lu) scheduled successfully.\n", program_description, (unsigned long)proc_pcb->pid);
        } else {
            terminal_printf("  [ERROR] Failed to add %s (PID %lu) to scheduler!\n", program_description, (unsigned long)proc_pcb->pid);
            destroy_process(proc_pcb); 
        }
    } else {
        terminal_printf("  [ERROR] Failed to create process for %s from '%s'.\n", program_description, path_on_disk);
    }
}


//-----------------------------------------------------------------------------
// Kernel Main Entry Point
//-----------------------------------------------------------------------------
void main(uint32_t magic, uint32_t mb_info_phys_addr) {
    // Use modular initialization system following Single Responsibility Principle
    kernel_main_init(magic, mb_info_phys_addr);
    
    // Should never reach here - kernel_main_init() should not return
    KERNEL_PANIC_HALT("kernel_main_init() returned unexpectedly!");
}