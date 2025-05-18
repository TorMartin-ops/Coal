// Patched Group_14/src/kernel.c

/**
 * @file kernel.c
 * @brief Main kernel entry point for UiAOS
 *
 * Author: Tor Martin Kohle
 * Version: 4.3.1 - Applied idle loop diagnostic patch
 *
 * Description:
 * This file contains the main entry point (`main`) for the UiAOS kernel,
 * invoked by the Multiboot2 bootloader. It orchestrates the initialization
 * sequence for all major kernel subsystems: GDT/TSS, memory management (paging,
 * physical and virtual allocators), interrupt handling (IDT/PICs), essential
 * hardware drivers (PIT, Keyboard, ATA), the scheduler, VFS and FAT filesystem,
 * and finally launches initial user-space processes including a test suite and
 * a simple shell.
 */

// === Standard/Core Headers ===
#include <multiboot2.h>
#include "types.h"
#include <string.h>       // Kernel's string functions
#include <libc/stdint.h>  // Kernel's fixed-width integers
#include <libc/stddef.h>  // Kernel's NULL, offsetof

// === Kernel Subsystems ===
#include "terminal.h"
#include "gdt.h"
#include "tss.h"
#include "idt.h"
#include "paging.h"
#include "frame.h"
#include "buddy.h"
#include "slab.h"
#include "percpu_alloc.h" // Conditional on USE_PERCPU_ALLOC in kmalloc
#include "kmalloc.h"
#include "process.h"
#include "scheduler.h"
#include "syscall.h"
#include "vfs.h"
#include "mount.h"
#include "fs_init.h"
#include "fs_errno.h"

// === Drivers ===
#include "pit.h"
#include "keyboard.h"
#include "keymap.h"
#include "keyboard_hw.h"   // Hardware definitions for keyboard
// #include "pc_speaker.h"  // Uncomment if PC speaker features are actively used
// #include "song.h"
// #include "song_player.h"
// #include "my_songs.h"

// === Utilities ===
#include "serial.h"         // Essential for early/debug logging
#include "assert.h"         // KERNEL_ASSERT, KERNEL_PANIC_HALT
// Other utilities like cpuid, kmalloc_internal, port_io are included by higher-level headers

// === Constants ===
#define KERNEL_VERSION_STRING "4.3.1" // Updated version
#define MULTIBOOT2_BOOTLOADER_MAGIC_EXPECTED 0x36d76289
#define MIN_USABLE_HEAP_SIZE (1 * 1024 * 1024) // 1MB
#define MAX_CLAMPED_INITIAL_HEAP_SIZE (256 * 1024 * 1024) // 256MB

// Path for the initial test program and the system shell
#define INITIAL_TEST_PROGRAM_PATH "/hello.elf"
#define SYSTEM_SHELL_PATH         "/bin/shell.elf"

// === Linker Symbols (Physical Addresses) ===
extern uint8_t _kernel_start_phys;
extern uint8_t _kernel_end_phys;
// Add other section symbols if specifically needed for calculations beyond kernel_end_phys

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
                if (current_candidate_start < kernel_phys_start_addr && kernel_phys_end_addr < region_p_end) {
                    uint64_t size_before = kernel_phys_start_addr - current_candidate_start;
                    uint64_t size_after = region_p_end - kernel_phys_end_addr;
                    if (size_after > size_before && size_after > best_candidate_size64) {
                        best_candidate_base = kernel_phys_end_addr;
                        best_candidate_size64 = size_after;
                    } else if (size_before > best_candidate_size64) {
                        best_candidate_base = current_candidate_start;
                        best_candidate_size64 = size_before;
                    }
                } else if (region_p_end > kernel_phys_end_addr) {
                    current_candidate_start = kernel_phys_end_addr;
                    current_candidate_len64 = region_p_end - current_candidate_start;
                    if (current_candidate_len64 > best_candidate_size64) {
                        best_candidate_base = current_candidate_start;
                        best_candidate_size64 = current_candidate_len64;
                    }
                }
            } else if (current_candidate_len64 > best_candidate_size64) {
                best_candidate_base = current_candidate_start;
                best_candidate_size64 = current_candidate_len64;
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

    terminal_write("  Stage 4.5: Mapping Multiboot Info to Kernel VAS...\n");
    uintptr_t mb_info_phys_page = PAGE_ALIGN_DOWN(g_multiboot_info_phys_addr_global);
    uintptr_t mb_info_virt_page = KERNEL_SPACE_VIRT_START + mb_info_phys_page;
    size_t mb_total_struct_size = *(volatile uint32_t*)g_multiboot_info_phys_addr_global;
    size_t mb_pages_to_map = ALIGN_UP(mb_total_struct_size, PAGE_SIZE) / PAGE_SIZE;
    if (mb_pages_to_map == 0 && mb_total_struct_size > 0) mb_pages_to_map = 1;

    if (paging_map_range((uint32_t*)g_kernel_page_directory_phys, mb_info_virt_page, mb_info_phys_page,
                         mb_pages_to_map * PAGE_SIZE, PTE_KERNEL_READONLY_FLAGS) != 0) {
        KERNEL_PANIC_HALT("Failed to map Multiboot info structure!");
    }
    g_multiboot_info_virt_addr_global = mb_info_virt_page + (g_multiboot_info_phys_addr_global % PAGE_SIZE);
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
    g_multiboot_info_phys_addr_global = mb_info_phys_addr;

    serial_init();
    terminal_init();

    terminal_printf("\n=== UiAOS Kernel Booting (Version: %s) ===\n", KERNEL_VERSION_STRING);
    terminal_printf("[Boot] Author: Tor Martin Kohle\n");

    terminal_write("[Boot] Verifying Multiboot environment...\n");
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC_EXPECTED) {
        KERNEL_PANIC_HALT("Invalid Multiboot Magic number.");
    }
    if (mb_info_phys_addr == 0 || mb_info_phys_addr >= 0x100000) {
        KERNEL_PANIC_HALT("Invalid Multiboot info physical address.");
    }
    terminal_printf("  Multiboot magic OK (Info at phys %#lx).\n", (unsigned long)mb_info_phys_addr);

    terminal_write("[Kernel] Initializing core systems (pre-interrupts)...\n");
    gdt_init();
    initialize_memory_management(g_multiboot_info_phys_addr_global);
    idt_init();
    init_pit();
    keyboard_init();
    keymap_load(KEYMAP_NORWEGIAN);
    scheduler_init();

    terminal_write("[Kernel] Initializing Filesystem Layer...\n");
    bool fs_ready = (fs_init() == FS_SUCCESS);
    if (fs_ready) {
        terminal_write("  [OK] Filesystem initialized and root mounted.\n");
    } else {
        terminal_write("  [CRITICAL] Filesystem initialization FAILED. User programs cannot be loaded.\n");
    }
    terminal_write("[Kernel Debug] KBC Status after fs_init(): 0x");
    serial_print_hex(inb(KBC_STATUS_PORT));
    serial_write("\n");

    if (fs_ready) {
        launch_program(INITIAL_TEST_PROGRAM_PATH, "Test Suite");
        terminal_write("[Kernel Debug] KBC Status after hello.elf launch: 0x");
        serial_print_hex(inb(KBC_STATUS_PORT));
        serial_write("\n");
        launch_program(SYSTEM_SHELL_PATH, "System Shell");
        terminal_write("[Kernel Debug] KBC Status before final sti: 0x");
        serial_print_hex(inb(KBC_STATUS_PORT));
        serial_write("\n");

    } else {
        terminal_write("  [Kernel] Skipping user process launch due to FS init failure.\n");
    }

    terminal_write("[Kernel] Re-checking and forcing KBC configuration before interrupts...\n");

    while (inb(KBC_STATUS_PORT) & KBC_SR_IBF);
    outb(KBC_CMD_PORT, KBC_CMD_READ_CONFIG);
    while (!(inb(KBC_STATUS_PORT) & KBC_SR_OBF));
    uint8_t current_config = inb(KBC_DATA_PORT);
    terminal_printf("   Read KBC Config Byte (before final write): 0x%x\n", current_config);

    uint8_t desired_final_config = 0x61; // Target config: KB Int ON, Mouse Int OFF, KB Clock ON, Mouse Clock OFF, Translate ON

    if (current_config != desired_final_config) {
        terminal_printf("   Modifying KBC Config Byte from 0x%x to 0x%x...\n", current_config, desired_final_config);
        while (inb(KBC_STATUS_PORT) & KBC_SR_IBF);
        outb(KBC_CMD_PORT, KBC_CMD_WRITE_CONFIG);
        while (inb(KBC_STATUS_PORT) & KBC_SR_IBF);
        outb(KBC_DATA_PORT, desired_final_config);
        for(volatile int d=0; d<10000; ++d);
        // It's good practice to flush after writing config too, though main issue is earlier.
        // kbc_flush_output_buffer("Kernel Final Config Write Flush"); // If function is made available/replicated
    } else {
        terminal_printf("   KBC Config Byte 0x%x already has desired settings.\n", current_config);
    }
    uint8_t final_kbc_status_check = inb(KBC_STATUS_PORT);
    terminal_printf("   KBC Status register *after* explicit config write attempt: 0x%x\n", final_kbc_status_check);


    terminal_write("[Kernel] Finalizing setup and enabling interrupts...\n");
    syscall_init();
    scheduler_start();
    terminal_printf("\n[Kernel] Initialization complete. UiAOS %s operational. Enabling interrupts.\n", KERNEL_VERSION_STRING);
    terminal_write("================================================================================\n\n");


    terminal_write("[Kernel Debug] KBC Status before final sti: 0x");
    serial_print_hex(inb(KBC_STATUS_PORT));
    serial_write("\n");

    asm volatile ("sti");

    serial_write("[Kernel DEBUG] Interrupts Enabled. Entering main HLT loop.\n");
    while (1) {
        asm volatile ("hlt");
    }
}