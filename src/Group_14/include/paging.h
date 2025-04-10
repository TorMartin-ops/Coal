#ifndef PAGING_H
#define PAGING_H

#include "types.h" // Includes size_t, uintptr_t, PAGE_SIZE, etc.

#ifdef __cplusplus
extern "C" {
#endif

// --- Page Size Definition ---
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

// --- Page Table/Directory Entry Flags ---
#define PAGE_PRESENT    0x001 // Page is present in memory
#define PAGE_RW         0x002 // Read/Write permissions
#define PAGE_USER       0x004 // User/Supervisor level
#define PAGE_ACCESSED   0x020 // Page has been accessed (set by CPU)
#define PAGE_DIRTY      0x040 // Page has been written to (set by CPU)
// Add other flags as needed (PCD, PWT, Global, etc.)

// --- Common Flag Combinations ---
// Kernel pages (Supervisor only)
#define PTE_KERNEL_READONLY  (PAGE_PRESENT)
#define PTE_KERNEL_DATA      (PAGE_PRESENT | PAGE_RW)
// User pages (User accessible)
#define PTE_USER_READONLY    (PAGE_PRESENT | PAGE_USER)
#define PTE_USER_DATA        (PAGE_PRESENT | PAGE_RW | PAGE_USER)


// --- Virtual Memory Layout Constants ---
#ifndef KERNEL_SPACE_VIRT_START
#define KERNEL_SPACE_VIRT_START 0xC0000000 // Virtual address where kernel space begins
#endif

/**
 * @brief Structure representing CPU state pushed by ISR stubs.
 * Order matches pusha, pushed segments, error code (if any), and iret frame.
 */
typedef struct registers {
    // Pushed segments (order depends on ISR stub)
    uint32_t ds, es, fs, gs;
    // Pushed by pusha
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    // Pushed by CPU for interrupt/exception
    uint32_t int_no, err_code;
    // Pushed by CPU for iret
    uint32_t eip, cs, eflags, user_esp, user_ss;
} registers_t;


// Global pointer to the kernel's page directory (virtual address).
extern uint32_t* kernel_page_directory;

/**
 * @brief Sets the global kernel_page_directory pointer.
 * @param pd_virt Virtual address of the page directory.
 */
void paging_set_directory(uint32_t* pd_virt);

/**
 * @brief Maps a range of virtual addresses to physical addresses.
 * Allocates page tables if needed. Rounds the range to page boundaries.
 *
 * @param page_directory_virt Virtual address of the page directory to modify.
 * @param virt_start_addr Start virtual address of the range.
 * @param phys_start_addr Start physical address the virtual range should map to.
 * @param memsz Size of the range in bytes.
 * @param flags Flags to apply to the page table entries (e.g., PTE_USER_DATA).
 * @return 0 on success, -1 on failure.
 */
int paging_map_range(uint32_t *page_directory_virt, uint32_t virt_start_addr,
                     uint32_t phys_start_addr, uint32_t memsz, uint32_t flags);

/**
 * @brief Unmaps a range of virtual addresses.
 * Removes PTEs and frees page tables if they become empty.
 *
 * @param page_directory_virt Virtual address of the page directory to modify.
 * @param virt_start_addr Start virtual address (page-aligned).
 * @param memsz Size of the range in bytes (page-aligned).
 * @return 0 on success, -1 on failure.
 */
int paging_unmap_range(uint32_t *page_directory_virt, uint32_t virt_start_addr, uint32_t memsz);


/**
 * @brief Helper to map a single physical page to a virtual address.
 * Uses paging_map_range internally.
 *
 * @param page_directory_virt Virtual address of the page directory.
 * @param vaddr Virtual address to map.
 * @param paddr Physical address to map to.
 * @param flags Flags for the PTE.
 * @return 0 on success, -1 on failure.
 */
int paging_map_single(uint32_t *page_directory_virt, uint32_t vaddr, uint32_t paddr, uint32_t flags);


/**
 * @brief Helper to create identity mapping for a range [0..size).
 *
 * @param page_directory_virt Virtual address of the page directory to modify.
 * @param size The upper bound (exclusive) of physical memory to map.
 * @param flags Flags to apply (e.g., PTE_KERNEL_DATA).
 * @return 0 on success, -1 on failure.
 */
int paging_init_identity_map(uint32_t *page_directory_virt, uint32_t size, uint32_t flags);

/**
 * @brief Invalidates the TLB entry for a single virtual address.
 * Uses the 'invlpg' instruction.
 *
 * @param vaddr The virtual address to invalidate.
 */
void paging_invalidate_page(void *vaddr);

/**
 * @brief Invalidates TLB entries for a range of virtual addresses.
 * Uses a loop of 'invlpg' instructions.
 *
 * @param start Virtual start address of the range.
 * @param size Size of the range in bytes.
 */
void tlb_flush_range(void* start, size_t size);


/**
 * @brief Activates a given page directory and enables paging (if not already).
 * Loads the physical address of the page directory into CR3.
 *
 * @param page_directory_phys Physical address pointer to the page directory to activate.
 */
void paging_activate(uint32_t *page_directory_phys);

/**
 * @brief Page Fault Exception Handler (#PF, Interrupt 14).
 * Called by the assembly stub isr14 when a page fault occurs.
 *
 * @param regs Pointer to the register state saved on the stack by the ISR stub.
 */
void page_fault_handler(registers_t *regs);


#ifdef __cplusplus
}
#endif

#endif // PAGING_H