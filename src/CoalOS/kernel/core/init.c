/**
 * @file init.c
 * @brief Kernel Initialization Management Implementation
 * @author Refactored for SOLID principles
 * @version 1.0
 * 
 * @details Modular initialization functions following Single Responsibility
 * Principle. Each function handles one specific aspect of kernel initialization.
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
#include <kernel/arch/multiboot2.h>
#include <kernel/process/process.h>
#include <libc/string.h>

//============================================================================
// Linker Symbols
//============================================================================
extern uint8_t _kernel_start_phys;
extern uint8_t _kernel_end_phys;

//============================================================================
// Static Function Prototypes for Memory Management
//============================================================================
static struct multiboot_tag *find_multiboot_tag_phys(uint32_t mb_info_phys, uint16_t type);
static struct multiboot_tag *find_multiboot_tag_virt(uintptr_t mb_info_virt, uint16_t type);
static bool parse_memory_map_for_heap(struct multiboot_tag_mmap *mmap_tag,
                                      uintptr_t *out_total_mem_span,
                                      uintptr_t *out_heap_base, size_t *out_heap_size);
static bool initialize_memory_management(uint32_t mb_info_phys);

//============================================================================
// Utility Macros
//============================================================================
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

//============================================================================
// External Global Variables
//============================================================================
extern uint32_t g_multiboot_info_phys_addr_global;
extern uintptr_t g_multiboot_info_virt_addr_global;

//============================================================================
// Global Constants
//============================================================================
const char *KERNEL_VERSION_STRING = "0.1.0";
const char *INITIAL_TEST_PROGRAM_PATH = "/hello.elf";
const char *SYSTEM_SHELL_PATH = "/bin/shell.elf";

//============================================================================
// Constants from kernel.c
//============================================================================
#define MIN_USABLE_HEAP_SIZE (1 * 1024 * 1024)        // 1MB
#define MAX_CLAMPED_INITIAL_HEAP_SIZE (256 * 1024 * 1024) // 256MB

//============================================================================
// Function Prototypes
//============================================================================
static void launch_program(const char *path_on_disk, const char *program_description);

//============================================================================
// Constants
//============================================================================
#define MULTIBOOT2_BOOTLOADER_MAGIC_EXPECTED  0x36d76289U
#define KBC_FORCE_CONFIG_DELAY               25000U
#define KBC_STATUS_CLEAR_DELAY               5000U

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
    
    // Validate multiboot info address
    if (mb_info_phys_addr == 0 || mb_info_phys_addr >= 0x100000) {
        return init_failure("Multiboot Validation", E_INVAL,
                           "Invalid Multiboot info physical address");
    }
    
    // Store global reference
    g_multiboot_info_phys_addr_global = mb_info_phys_addr;
    
    terminal_printf("  Multiboot magic OK (Info at phys 0x%lx)\n", 
                   (unsigned long)mb_info_phys_addr);
    
    return init_success("Multiboot Validation");
}

//============================================================================
// Core System Initialization Implementation
//============================================================================

init_result_t init_basic_io(void)
{
    // Initialize serial communication first
    serial_init();
    
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
    serial_printf("[INIT] Re-checking and forcing KBC configuration...\n");
    
    // Read current KBC configuration
    uint8_t current_config = 0;
    outb(KBC_CMD_PORT, KBC_CMD_READ_CONFIG);
    
    // Wait for response
    for (volatile int wait = 0; wait < SHORT_DELAY_CYCLES; wait++) {
        asm volatile("pause");
    }
    
    if (inb(KBC_STATUS_PORT) & KBC_SR_OBF) {
        current_config = inb(KBC_DATA_PORT);
    }
    
    serial_printf("   Read KBC Config Byte: 0x%x\n", current_config);
    
    // Set desired configuration
    uint8_t desired_config = 0x41;
    serial_printf("   Forcing KBC Config Byte to 0x%x\n", desired_config);
    
    // Wait for input buffer to be ready
    for (volatile int wait = 0; (inb(KBC_STATUS_PORT) & KBC_SR_IBF) && wait < KBC_WAIT_TIMEOUT_CYCLES; wait++) {
        asm volatile("pause");
    }
    
    // Send write config command
    outb(KBC_CMD_PORT, KBC_CMD_WRITE_CONFIG);
    
    // Wait for input buffer to be ready for data
    for (volatile int wait = 0; (inb(KBC_STATUS_PORT) & KBC_SR_IBF) && wait < KBC_WAIT_TIMEOUT_CYCLES; wait++) {
        asm volatile("pause");
    }
    
    // Send configuration data
    outb(KBC_DATA_PORT, desired_config);
    
    // Delay for configuration to take effect
    for (volatile int wait = 0; wait < KBC_FORCE_CONFIG_DELAY; wait++) {
        asm volatile("pause");
    }
    
    // Clear output buffer
    int bytes_cleared = 0;
    uint8_t status;
    
    serial_printf("   [KBC FIX] Clearing output buffer...\n");
    while ((status = inb(KBC_STATUS_PORT)) & KBC_SR_OBF) {
        uint8_t discarded = inb(KBC_DATA_PORT);
        bytes_cleared++;
        
        serial_printf("     Cleared byte %d: 0x%x (status: 0x%x)\n", 
                     bytes_cleared, discarded, status);
        
        if (bytes_cleared >= KBC_FLUSH_MAX_ATTEMPTS) {
            serial_printf("   [WARNING] KBC buffer clear maxed out at %d bytes\n", 
                         KBC_FLUSH_MAX_ATTEMPTS);
            break;
        }
        
        // Small delay between reads
        for (volatile int wait = 0; wait < KBC_STATUS_CLEAR_DELAY; wait++) {
            asm volatile("pause");
        }
    }
    
    if (bytes_cleared > 0) {
        serial_printf("   [KBC FIX] Cleared %d bytes from output buffer\n", bytes_cleared);
    } else {
        serial_printf("   [KBC INFO] Output buffer was already clear\n");
    }
    
    uint8_t final_status = inb(KBC_STATUS_PORT);
    serial_printf("   KBC Status after configuration: 0x%x\n", final_status);
    
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
    
    serial_printf("[INIT] KBC Status before enabling interrupts: 0x%08x\n", 
                 inb(KBC_STATUS_PORT));
    
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
    
    // Debug: Check KBC status after filesystem
    serial_printf("[Debug] KBC Status after filesystem init: 0x%08x\n", 
                 inb(KBC_STATUS_PORT));
    
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