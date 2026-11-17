/**
 * @file init.c
 * @brief Kernel Initialization Management Implementation
 *
 * Boot sequence: each init_* function handles one phase; kernel_main_init
 * runs them in order.
 */

//============================================================================
// Includes
//============================================================================
#include <kernel/core/init.h>
#include <kernel/cpu/gdt.h>
#include <kernel/cpu/idt.h>
#include <kernel/cpu/syscall.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/frame.h>
#include <kernel/memory/buddy.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/drivers/timer/pit.h>
#include <kernel/drivers/input/keyboard.h>
#include <kernel/drivers/display/console_dev.h>
#include <kernel/drivers/input/keymap.h>
#include <kernel/process/scheduler.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/fs/vfs/fs_init.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/lib/port_io.h>
#include <kernel/drivers/input/keyboard_hw.h>
#include <kernel/lib/assert.h>
#include <kernel/core/multiboot.h>
#include <kernel/process/process.h>
#include <libc/string.h>

//============================================================================
// Linker Symbols
//============================================================================
extern uint8_t _kernel_start_phys;
extern uint8_t _kernel_end_phys;

//============================================================================
// Globals and Constants
//============================================================================
// Physical address of the Multiboot2 info; the low 4 MiB stay identity-mapped,
// so it is readable at the same address after paging is enabled.
uint32_t g_multiboot_info_phys_addr_global = 0;

const char *KERNEL_VERSION_STRING = "0.1.0";
const char *INITIAL_TEST_PROGRAM_PATH = "/hello.elf";
const char *SYSTEM_SHELL_PATH = "/bin/shell.elf";

#define MIN_USABLE_HEAP_SIZE (1 * 1024 * 1024)            // 1MB
#define MAX_CLAMPED_INITIAL_HEAP_SIZE (256 * 1024 * 1024) // 256MB
#define IDENTITY_MAPPED_LIMIT 0x400000u                   // first 4MB
#define MULTIBOOT2_BOOTLOADER_MAGIC_EXPECTED  0x36d76289U
#define SHORT_DELAY_CYCLES                   15000
#define KBC_FORCE_CONFIG_DELAY               25000
#define KBC_STATUS_CLEAR_DELAY               5000

static void launch_program(const char *path_on_disk, const char *program_description);
static bool initialize_memory_management(uint32_t mb_info_phys);

//============================================================================
// Utility Functions Implementation
//============================================================================

init_result_t init_success(const char *phase_name)
{
    init_result_t result = {
        .error_code = E_SUCCESS,
        .phase_name = phase_name,
        .error_message = "Operation completed successfully"
    };
    return result;
}

init_result_t init_failure(const char *phase_name, error_t error_code, const char *error_message)
{
    init_result_t result = {
        .error_code = error_code,
        .phase_name = phase_name,
        .error_message = error_message
    };
    return result;
}

bool init_handle_result(const init_result_t *result, bool continue_on_error)
{
    if (!result) {
        terminal_write("[INIT ERROR] Null result provided\n");
        return false;
    }
    
    if (result->error_code == E_SUCCESS) {
        terminal_printf("  [OK] %s\n", result->phase_name);
        return true;
    } else {
        terminal_printf("  [ERROR] %s: %s\n", result->phase_name, result->error_message);
        
        if (!continue_on_error) {
            terminal_printf("  [CRITICAL] Initialization failed, system halting\n");
            return false;
        } else {
            terminal_printf("  [WARNING] Continuing despite error in %s\n", result->phase_name);
            return true;
        }
    }
}

//============================================================================
// Multiboot Validation Implementation
//============================================================================

init_result_t init_validate_multiboot(uint32_t magic, uint32_t mb_info_phys_addr)
{
    // Validate multiboot magic number
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC_EXPECTED) {
        return init_failure("Multiboot Validation", E_INVAL,
                           "Invalid Multiboot magic number");
    }
    
    // The Multiboot2 spec only guarantees an 8-byte aligned, non-null address.
    // GRUB places the info structure above 1 MiB, so no upper bound is checked.
    if (mb_info_phys_addr == 0 || (mb_info_phys_addr & 7) != 0) {
        return init_failure("Multiboot Validation", E_INVAL,
                           "Invalid Multiboot info physical address");
    }
    
    // Store global reference
    g_multiboot_info_phys_addr_global = mb_info_phys_addr;
    
    terminal_printf("  Multiboot magic OK (Info at phys 0x%x)\n", mb_info_phys_addr);
    
    return init_success("Multiboot Validation");
}

//============================================================================
// Core System Initialization Implementation
//============================================================================

init_result_t init_basic_io(void)
{
    // Initialize serial communication first
    serial_init(SERIAL_COM1);
    serial_init(SERIAL_COM2);
    
    // Initialize terminal for user output
    terminal_init();
    
    // Initialize console device driver
    console_dev_init();
    
    // Display boot banner
    terminal_printf("\n=== Coal OS Kernel Booting (Version: %s) ===\n", KERNEL_VERSION_STRING);
    terminal_printf("[Boot] A hobby operating system project\n");
    
    return init_success("Basic I/O Systems");
}

init_result_t init_core_systems(uint32_t mb_info_phys_addr)
{
    // Initialize Global Descriptor Table
    gdt_init();
    
    // Initialize memory management
    initialize_memory_management(mb_info_phys_addr);
    
    // Initialize Interrupt Descriptor Table
    idt_init();
    
    return init_success("Core Systems (GDT, Memory, IDT)");
}

init_result_t init_interrupt_systems(void)
{
    // Initialize Programmable Interval Timer
    init_pit();
    
    // Initialize process scheduler
    scheduler_init();
    
    return init_success("Interrupt and Timing Systems");
}

init_result_t init_input_systems(void)
{
    // Initialize keyboard driver
    keyboard_init();
    
    // Load Norwegian keymap (could be configurable)
    keymap_load(KEYMAP_NORWEGIAN);
    
    return init_success("Input Systems");
}

//============================================================================
// Filesystem and Process Initialization Implementation
//============================================================================

init_result_t init_filesystem(void)
{
    bool fs_ready = (fs_init() == FS_SUCCESS);
    
    if (fs_ready) {
        return init_success("Filesystem initialized and root mounted");
    } else {
        return init_failure("Filesystem Initialization", E_IO,
                           "Failed to initialize filesystem or mount root");
    }
}

init_result_t init_launch_processes(bool filesystem_ready)
{
    if (!filesystem_ready) {
        return init_failure("Process Launch", E_NOTFOUND,
                           "Cannot launch processes - filesystem not ready");
    }
    
    // Launch test program
    launch_program(INITIAL_TEST_PROGRAM_PATH, "Test Suite");
    
    // Launch system shell
    launch_program(SYSTEM_SHELL_PATH, "System Shell");
    serial_printf("[Init] Shell launched\n");
    
    return init_success("Initial User Processes Launched");
}

//============================================================================
// Hardware Configuration Implementation
//============================================================================

init_result_t init_configure_keyboard_controller(void)
{
    // The keyboard driver expects interrupts on and scancode translation on
    // (config 0x41); keep every other bit as the controller reports it.
    uint8_t current_config = 0;
    outb(KBC_CMD_PORT, KBC_CMD_READ_CONFIG);

    for (volatile int wait = 0; wait < SHORT_DELAY_CYCLES; wait++) {
        asm volatile("pause");
    }

    if (inb(KBC_STATUS_PORT) & KBC_SR_OBF) {
        current_config = inb(KBC_DATA_PORT);
    }

    uint8_t desired_config = (current_config | KBC_CFG_INT_KB | KBC_CFG_TRANSLATION) & ~KBC_CFG_DISABLE_KB;
    serial_printf("[INIT] KBC config byte 0x%x -> 0x%x\n", current_config, desired_config);

    for (volatile int wait = 0; (inb(KBC_STATUS_PORT) & KBC_SR_IBF) && wait < KBC_WAIT_TIMEOUT_CYCLES; wait++) {
        asm volatile("pause");
    }
    outb(KBC_CMD_PORT, KBC_CMD_WRITE_CONFIG);

    for (volatile int wait = 0; (inb(KBC_STATUS_PORT) & KBC_SR_IBF) && wait < KBC_WAIT_TIMEOUT_CYCLES; wait++) {
        asm volatile("pause");
    }
    outb(KBC_DATA_PORT, desired_config);

    // Let the controller apply the new configuration
    for (volatile int wait = 0; wait < KBC_FORCE_CONFIG_DELAY; wait++) {
        asm volatile("pause");
    }

    // Drop bytes queued before the new configuration took effect
    int bytes_cleared = 0;
    while (inb(KBC_STATUS_PORT) & KBC_SR_OBF) {
        (void)inb(KBC_DATA_PORT);
        if (++bytes_cleared >= KBC_FLUSH_MAX_ATTEMPTS) {
            serial_printf("[INIT] KBC buffer clear stopped after %d bytes\n", KBC_FLUSH_MAX_ATTEMPTS);
            break;
        }
        for (volatile int wait = 0; wait < KBC_STATUS_CLEAR_DELAY; wait++) {
            asm volatile("pause");
        }
    }

    return init_success("Keyboard Controller Configuration");
}

init_result_t init_finalize_configuration(void)
{
    // Initialize system call interface
    syscall_init();
    
    // Ensure segment registers are properly configured
    asm volatile (
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        : : : "ax"
    );
    
    return init_success("Final System Configuration");
}

//============================================================================
// Main Initialization Orchestration Implementation
//============================================================================

void kernel_main_init(uint32_t magic, uint32_t mb_info_phys_addr)
{
    init_result_t result;
    
    // Phase 1: Basic I/O initialization
    result = init_basic_io();
    if (!init_handle_result(&result, false)) {
        KERNEL_PANIC_HALT("Basic I/O initialization failed");
    }
    
    // Phase 2: Multiboot validation
    terminal_write("[Boot] Verifying Multiboot environment...\n");
    result = init_validate_multiboot(magic, mb_info_phys_addr);
    if (!init_handle_result(&result, false)) {
        KERNEL_PANIC_HALT("Multiboot validation failed");
    }
    
    // Phase 3: Core system initialization
    terminal_write("[Kernel] Initializing core systems (pre-interrupts)...\n");
    result = init_core_systems(mb_info_phys_addr);
    if (!init_handle_result(&result, false)) {
        KERNEL_PANIC_HALT("Core systems initialization failed");
    }
    
    // Phase 4: Interrupt and timing systems
    result = init_interrupt_systems();
    if (!init_handle_result(&result, false)) {
        KERNEL_PANIC_HALT("Interrupt systems initialization failed");
    }
    
    // Phase 5: Input systems
    result = init_input_systems();
    if (!init_handle_result(&result, false)) {
        KERNEL_PANIC_HALT("Input systems initialization failed");
    }
    
    // Phase 6: Filesystem initialization
    terminal_write("[Kernel] Initializing Filesystem Layer...\n");
    result = init_filesystem();
    bool filesystem_ready = (result.error_code == E_SUCCESS);
    init_handle_result(&result, true); // Continue even if filesystem fails
    
    // Phase 7: Launch initial processes
    result = init_launch_processes(filesystem_ready);
    init_handle_result(&result, true); // Continue even if process launch fails
    
    // Phase 8: Hardware configuration
    terminal_write("[Kernel] Configuring hardware for interrupt operation...\n");
    result = init_configure_keyboard_controller();
    init_handle_result(&result, true); // Continue even if KBC config fails
    
    // Phase 9: Final configuration
    terminal_write("[Kernel] Finalizing setup and enabling interrupts...\n");
    result = init_finalize_configuration();
    if (!init_handle_result(&result, false)) {
        KERNEL_PANIC_HALT("Final configuration failed");
    }
    
    // Display completion banner
    terminal_printf("\n[Kernel] Initialization complete. Coal OS %s operational. Enabling interrupts.\n", 
                   KERNEL_VERSION_STRING);
    terminal_write("================================================================================\n\n");
    
    // Enable interrupts
    asm volatile ("sti");
    
    // Transfer control to scheduler
    scheduler_start();
    
    // Should never reach here
    KERNEL_PANIC_HALT("scheduler_start() returned unexpectedly!");
}

//============================================================================
// Memory Management Support Functions
//============================================================================

static inline uintptr_t safe_add_base_len(uintptr_t base, uint64_t len) {
    if (len > ((uint64_t)UINTPTR_MAX - base)) {
        return UINTPTR_MAX;
    }
    return base + (uintptr_t)len;
}

// Removes [res_start, res_end) from [*start, *end), keeping the larger remainder.
static void subtract_reserved(uintptr_t *start, uintptr_t *end, uintptr_t res_start, uintptr_t res_end) {
    if (*start >= *end || res_start >= *end || res_end <= *start) return;
    uintptr_t below = (res_start > *start) ? res_start - *start : 0;
    uintptr_t above = (res_end < *end) ? *end - res_end : 0;
    if (below >= above) {
        *end = *start + below;
    } else {
        *start = res_end;
    }
}

static bool parse_memory_map_for_heap(struct multiboot_tag_mmap *mmap_tag,
                                      uintptr_t *out_total_mem_span,
                                      uintptr_t *out_heap_base, size_t *out_heap_size)
{
    KERNEL_ASSERT(mmap_tag && out_total_mem_span && out_heap_base && out_heap_size, "parse_memory_map: Invalid args");
    if (mmap_tag->entry_size < sizeof(multiboot_memory_map_t)) {
        terminal_printf("  [FATAL] Invalid MMAP entry size %u.\n", mmap_tag->entry_size);
        return false;
    }

    uintptr_t total_span = 0;
    uintptr_t best_candidate_base = 0;
    uint64_t best_candidate_size64 = 0;
    multiboot_memory_map_t *entry = mmap_tag->entries;
    uintptr_t mmap_end_addr = (uintptr_t)mmap_tag + mmap_tag->size;
    uintptr_t kernel_phys_start_addr = (uintptr_t)&_kernel_start_phys;
    uintptr_t kernel_phys_end_addr = ALIGN_UP((uintptr_t)&_kernel_end_phys, PAGE_SIZE);
    uintptr_t mb_info_start = g_multiboot_info_phys_addr_global;
    uintptr_t mb_info_end = ALIGN_UP(mb_info_start + multiboot_info_size(mb_info_start), PAGE_SIZE);

    terminal_printf("  Kernel Physical Range: [%#010x - %#010x)\n", kernel_phys_start_addr, kernel_phys_end_addr);

    while ((uintptr_t)entry + mmap_tag->entry_size <= mmap_end_addr) {
        uintptr_t region_p_start = (uintptr_t)entry->addr;
        uint64_t region_p_len64 = entry->len;
        uintptr_t region_p_end = safe_add_base_len(region_p_start, region_p_len64);

        if (region_p_end > total_span) total_span = region_p_end;

        if (entry->type == MULTIBOOT_MEMORY_AVAILABLE && region_p_len64 >= MIN_USABLE_HEAP_SIZE) {
            // The heap must not overlap the kernel image or the boot info
            uintptr_t cand_start = region_p_start;
            uintptr_t cand_end = region_p_end;
            subtract_reserved(&cand_start, &cand_end, kernel_phys_start_addr, kernel_phys_end_addr);
            subtract_reserved(&cand_start, &cand_end, mb_info_start, mb_info_end);
            uint64_t cand_len64 = cand_end - cand_start;

            if (cand_len64 >= MIN_USABLE_HEAP_SIZE && cand_len64 > best_candidate_size64) {
                best_candidate_base = cand_start;
                best_candidate_size64 = cand_len64;
            }
        }
        entry = (multiboot_memory_map_t*)((uintptr_t)entry + mmap_tag->entry_size);
    }

    *out_total_mem_span = ALIGN_UP(total_span, PAGE_SIZE);
    if (*out_total_mem_span < total_span) *out_total_mem_span = UINTPTR_MAX;

    if (best_candidate_base < 0x100000 && best_candidate_base != 0) {
        terminal_printf("  Warning: Best heap candidate below 1MB (%#x).\n", best_candidate_base);
    }

    if (best_candidate_size64 >= MIN_USABLE_HEAP_SIZE && best_candidate_base != 0) {
        *out_heap_base = best_candidate_base;
        *out_heap_size = (best_candidate_size64 > (uint64_t)MAX_CLAMPED_INITIAL_HEAP_SIZE) ?
                         MAX_CLAMPED_INITIAL_HEAP_SIZE : (size_t)best_candidate_size64;
        terminal_printf("  Best Heap Found: PhysBase=%#x, Size=%zu (may be clamped)\n", *out_heap_base, *out_heap_size);
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
    struct multiboot_tag_mmap *mmap_tag = (struct multiboot_tag_mmap *)multiboot_find_tag(mb_info_phys, MULTIBOOT_TAG_TYPE_MMAP);
    if (!mmap_tag) KERNEL_PANIC_HALT("Multiboot MMAP tag not found!");
    if (!parse_memory_map_for_heap(mmap_tag, &total_phys_memory_span, &initial_heap_phys_base, &initial_heap_size)) {
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

    // The boot info must stay reachable at its physical address after paging
    // is on, which only holds inside the identity-mapped low memory.
    if (mb_info_phys >= IDENTITY_MAPPED_LIMIT) {
        KERNEL_PANIC_HALT("Multiboot info outside identity-mapped region!");
    }

    terminal_write("  Stage 6: Initializing Frame Allocator...\n");
    mmap_tag = (struct multiboot_tag_mmap *)multiboot_find_tag(mb_info_phys, MULTIBOOT_TAG_TYPE_MMAP);
    if (!mmap_tag) KERNEL_PANIC_HALT("Cannot find MMAP tag for Frame Allocator!");
    if (frame_init(mmap_tag, kernel_phys_start, kernel_phys_end_aligned, initial_heap_phys_base, initial_heap_phys_base + initial_heap_size) != 0) {
        KERNEL_PANIC_HALT("Frame Allocator initialization failed!");
    }

    terminal_write("  Stage 7: Initializing Kmalloc...\n");
    kmalloc_init();

    terminal_write("  Stage 8: Initializing Temporary VA Mapper...\n");
    if (paging_temp_map_init() != 0) KERNEL_PANIC_HALT("Failed to initialize temporary VA mapper!");

    terminal_write("[OK] Memory Subsystems Initialized Successfully.\n");
    return true;
}

//============================================================================
// Process Launch Helper
//============================================================================

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