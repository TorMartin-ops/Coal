/**
 * @file x86_32_cpu_arch.c
 * @brief x86-32 CPU Architecture HAL Implementation
 * @author CoalOS HAL Implementation
 * @version 1.0
 * 
 * @details Implements CPU architecture abstraction for x86-32 processors,
 * providing hardware-specific implementations for segment management,
 * interrupt handling, context switching, and other CPU operations.
 */

//============================================================================
// Includes
//============================================================================
#include <kernel/hal/cpu_arch.h>
#include <kernel/cpu/gdt.h>
#include <kernel/cpu/idt.h>
#include <kernel/process/process.h>
#include <kernel/memory/paging.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <libc/string.h>

//============================================================================
// x86-32 Specific Includes and Declarations
//============================================================================

// External assembly functions
extern void gdt_flush(uint32_t gdt_ptr);
extern void idt_flush(uint32_t idt_ptr);
extern void enable_interrupts(void);
extern void disable_interrupts(void);
extern uint32_t get_eflags(void);
extern void set_eflags(uint32_t flags);
// Context switch will be implemented when HAL is integrated
extern void jump_to_user_mode(uint32_t entry_point, uint32_t user_stack);
extern void syscall_handler_asm(void);

// External initialization functions
extern void gdt_init(void);
extern void idt_init(void);

//============================================================================
// x86-32 Architecture Information
//============================================================================

static const cpu_arch_info_t x86_32_arch_info = {
    .type = CPU_ARCH_X86_32,
    .features = CPU_FEATURE_PAGING | 
                CPU_FEATURE_SEGMENTATION | 
                CPU_FEATURE_FLOATING_POINT |
                CPU_FEATURE_CACHE_CONTROL |
                CPU_FEATURE_MMU |
                CPU_FEATURE_INTERRUPTS |
                CPU_FEATURE_PRIVILEGED_MODE |
                CPU_FEATURE_ATOMIC_OPS,
    .word_size = 4,
    .page_size = 4096,
    .cache_line_size = 64,
    .arch_name = "Intel x86-32"
};

//============================================================================
// x86-32 CPU Operations Implementation
//============================================================================

static error_t x86_32_init(void)
{
    serial_write("[x86-32 HAL] Initializing CPU architecture layer...\n");
    
    // x86-32 specific initialization
    serial_write("[x86-32 HAL] CPU architecture layer initialized\n");
    return E_SUCCESS;
}

static void x86_32_shutdown(void)
{
    serial_write("[x86-32 HAL] Shutting down CPU architecture layer\n");
    // Perform any cleanup needed
}

static void x86_32_halt(void)
{
    __asm__ volatile ("hlt");
}

static void x86_32_idle(void)
{
    __asm__ volatile ("hlt");
}

static error_t x86_32_init_segmentation(void)
{
    serial_write("[x86-32 HAL] Initializing GDT...\n");
    gdt_init();
    return E_SUCCESS;
}

static error_t x86_32_setup_kernel_segments(void)
{
    // Kernel segments are set up during GDT initialization
    return E_SUCCESS;
}

static error_t x86_32_setup_user_segments(void)
{
    // User segments are set up during GDT initialization
    return E_SUCCESS;
}

static error_t x86_32_init_interrupts(void)
{
    serial_write("[x86-32 HAL] Initializing IDT...\n");
    idt_init();
    return E_SUCCESS;
}

static void x86_32_enable_interrupts(void)
{
    __asm__ volatile ("sti");
}

static void x86_32_disable_interrupts(void)
{
    __asm__ volatile ("cli");
}

static bool x86_32_interrupts_enabled(void)
{
    uint32_t eflags;
    __asm__ volatile ("pushf; pop %0" : "=r"(eflags));
    return (eflags & 0x200) != 0; // IF flag
}

static void x86_32_context_switch(uint32_t **old_esp, uint32_t *new_esp)
{
    // TODO: Implement HAL context switch integration
    // For now, this is a stub that will be implemented when HAL is integrated
    (void)old_esp;
    (void)new_esp;
    serial_write("[x86-32 HAL] Context switch not yet integrated\n");
}

static error_t x86_32_prepare_user_context(pcb_t *process)
{
    if (!process) {
        return E_INVAL;
    }
    
    // x86-32 specific user context preparation
    // This would set up the process's page directory and other context
    
    return E_SUCCESS;
}

static void x86_32_enter_user_mode(uint32_t entry_point, uint32_t user_stack)
{
    jump_to_user_mode(entry_point, user_stack);
}

static error_t x86_32_init_syscall_handler(void)
{
    // System call handler is set up during IDT initialization
    return E_SUCCESS;
}

static void x86_32_syscall_entry(void)
{
    // This would be called from assembly syscall handler
    syscall_handler_asm();
}

static void x86_32_flush_tlb(void)
{
    uint32_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

static void x86_32_flush_tlb_page(uintptr_t vaddr)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
}

static void x86_32_flush_cache(void)
{
    __asm__ volatile ("wbinvd");
}

static uint32_t x86_32_get_cpu_flags(void)
{
    uint32_t eflags;
    __asm__ volatile ("pushf; pop %0" : "=r"(eflags));
    return eflags;
}

static void x86_32_set_cpu_flags(uint32_t flags)
{
    __asm__ volatile ("push %0; popf" : : "r"(flags) : "memory");
}

static error_t x86_32_register_exception_handler(uint8_t exception, void (*handler)(struct isr_frame *))
{
    if (!handler) {
        return E_INVAL;
    }
    
    // This would register the handler in the IDT
    // For now, return success as this is handled by existing IDT code
    (void)exception;
    return E_SUCCESS;
}

static error_t x86_32_register_irq_handler(uint8_t irq, void (*handler)(struct isr_frame *))
{
    if (!handler) {
        return E_INVAL;
    }
    
    // This would register the IRQ handler
    // For now, return success as this is handled by existing IDT code
    (void)irq;
    return E_SUCCESS;
}

static void x86_32_memory_barrier(void)
{
    __asm__ volatile ("mfence" : : : "memory");
}

static void x86_32_read_barrier(void)
{
    __asm__ volatile ("lfence" : : : "memory");
}

static void x86_32_write_barrier(void)
{
    __asm__ volatile ("sfence" : : : "memory");
}

//============================================================================
// x86-32 Operations Table
//============================================================================

static const cpu_arch_ops_t x86_32_ops = {
    .init = x86_32_init,
    .shutdown = x86_32_shutdown,
    .halt = x86_32_halt,
    .idle = x86_32_idle,
    
    .init_segmentation = x86_32_init_segmentation,
    .setup_kernel_segments = x86_32_setup_kernel_segments,
    .setup_user_segments = x86_32_setup_user_segments,
    
    .init_interrupts = x86_32_init_interrupts,
    .enable_interrupts = x86_32_enable_interrupts,
    .disable_interrupts = x86_32_disable_interrupts,
    .interrupts_enabled = x86_32_interrupts_enabled,
    
    .context_switch = x86_32_context_switch,
    .prepare_user_context = x86_32_prepare_user_context,
    .enter_user_mode = x86_32_enter_user_mode,
    
    .init_syscall_handler = x86_32_init_syscall_handler,
    .syscall_entry = x86_32_syscall_entry,
    
    .flush_tlb = x86_32_flush_tlb,
    .flush_tlb_page = x86_32_flush_tlb_page,
    .flush_cache = x86_32_flush_cache,
    .get_cpu_flags = x86_32_get_cpu_flags,
    .set_cpu_flags = x86_32_set_cpu_flags,
    
    .register_exception_handler = x86_32_register_exception_handler,
    .register_irq_handler = x86_32_register_irq_handler,
    
    .memory_barrier = x86_32_memory_barrier,
    .read_barrier = x86_32_read_barrier,
    .write_barrier = x86_32_write_barrier
};

//============================================================================
// Global State and Interface Implementation
//============================================================================

static bool x86_32_initialized = false;

const cpu_arch_info_t *cpu_arch_get_info(void)
{
    return &x86_32_arch_info;
}

const cpu_arch_ops_t *cpu_arch_get_ops(void)
{
    return &x86_32_ops;
}

error_t cpu_arch_init(void)
{
    if (x86_32_initialized) {
        return 0; // Already initialized
    }
    
    serial_write("[CPU HAL] Initializing x86-32 CPU architecture HAL\n");
    
    error_t result = x86_32_init();
    if (result == E_SUCCESS) {
        x86_32_initialized = true;
        serial_write("[CPU HAL] x86-32 CPU architecture HAL initialized successfully\n");
    } else {
        serial_write("[CPU HAL] Failed to initialize x86-32 CPU architecture HAL\n");
    }
    
    return result;
}

error_t x86_32_cpu_arch_init(void)
{
    return cpu_arch_init();
}