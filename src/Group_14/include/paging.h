#ifndef PAGING_H
#define PAGING_H

#include "types.h"
#include "spinlock.h"
// Include kmalloc_internal for ALIGN_UP needed by PAGE_ALIGN_UP
#include "kmalloc_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Page Size Definitions ---
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#ifndef PAGE_SIZE_LARGE
#define PAGE_SIZE_LARGE (4 * 1024 * 1024) // 4MiB
#endif

// --- Page Table/Directory Entry Flags ---
#define PAGE_PRESENT    0x001
#define PAGE_RW         0x002
#define PAGE_USER       0x004
#define PAGE_PWT        0x008 // Page Write-Through
#define PAGE_PCD        0x010 // Page Cache Disable
#define PAGE_ACCESSED   0x020 // Accessed bit
#define PAGE_DIRTY      0x040 // Dirty bit (only in PTE)
#define PAGE_SIZE_4MB   0x080 // PSE bit in PDE (Page Size Extension)
#define PAGE_GLOBAL     0x100 // Global bit (PGE enabled in CR4)
// Bits 9-11 available for OS use

// --- Common Flag Combinations ---
#define PTE_KERNEL_DATA      (PAGE_PRESENT | PAGE_RW)
#define PTE_KERNEL_READONLY  (PAGE_PRESENT)
#define PTE_USER_DATA        (PAGE_PRESENT | PAGE_RW | PAGE_USER)
#define PTE_USER_READONLY    (PAGE_PRESENT | PAGE_USER)

// --- Virtual Memory Layout ---
#ifndef KERNEL_SPACE_VIRT_START
#define KERNEL_SPACE_VIRT_START 0xC0000000 // Default higher half start
#endif
#define KERNEL_PDE_INDEX (KERNEL_SPACE_VIRT_START >> 22)

// --- *** MOVED MACRO DEFINITIONS HERE *** ---
#define PDE_INDEX(addr)  (((uintptr_t)(addr) >> 22) & 0x3FF)
#define PTE_INDEX(addr)  (((uintptr_t)(addr) >> 12) & 0x3FF)
#ifndef PAGE_ALIGN_DOWN // Guard against redefinition if also in types.h
#define PAGE_ALIGN_DOWN(addr) ((uintptr_t)(addr) & ~(PAGE_SIZE - 1))
#endif
// PAGE_ALIGN_UP requires ALIGN_UP from kmalloc_internal.h (included above)
// Ensure kmalloc_internal.h provides ALIGN_UP, or define PAGE_ALIGN_UP differently
#ifndef PAGE_ALIGN_UP
#define PAGE_ALIGN_UP(addr)   ALIGN_UP(addr, PAGE_SIZE)
// Alternative if ALIGN_UP not available: (((uintptr_t)(addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#endif
#define PAGE_LARGE_ALIGN_DOWN(addr) ((uintptr_t)(addr) & ~(PAGE_SIZE_LARGE - 1))
#define PAGE_LARGE_ALIGN_UP(addr)   (((uintptr_t)(addr) + PAGE_SIZE_LARGE - 1) & ~(PAGE_SIZE_LARGE - 1))
// --- End Moved Macros ---

// --- Temp Mapping Addrs --- (Still defined in paging.c implementation)

// --- CPU State Structure ---
typedef struct registers {
    uint32_t ds, es, fs, gs;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, user_esp, user_ss;
} registers_t;

// --- Globals (Declared Extern) ---
extern bool g_pse_supported;
extern uint32_t* g_kernel_page_directory_virt;
extern uint32_t g_kernel_page_directory_phys;

// --- Function Signatures --- (Ensure these match the implementation)
bool check_and_enable_pse(void);
uintptr_t paging_initialize_directory(uintptr_t initial_pd_phys);
int paging_setup_early_maps(uintptr_t page_directory_phys,
                            uintptr_t kernel_phys_start, uintptr_t kernel_phys_end,
                            uintptr_t heap_phys_start, size_t heap_size);
int paging_finalize_and_activate(uintptr_t page_directory_phys, uintptr_t total_memory_bytes);
void paging_set_kernel_directory(uint32_t* pd_virt, uint32_t pd_phys);
int paging_map_single(uint32_t *page_directory_phys, uint32_t vaddr, uint32_t paddr, uint32_t flags);
int paging_map_range(uint32_t *page_directory_phys, uint32_t virt_start_addr, uint32_t phys_start_addr, uint32_t memsz, uint32_t flags);
int paging_unmap_range(uint32_t *page_directory_phys, uint32_t virt_start_addr, uint32_t memsz);
int paging_identity_map_range(uint32_t *page_directory_phys, uint32_t start_addr, uint32_t size, uint32_t flags);
void paging_invalidate_page(void *vaddr);
void tlb_flush_range(void* start, size_t size);
void paging_activate(uint32_t *page_directory_phys);
void page_fault_handler(registers_t *regs);
void paging_free_user_space(uint32_t *page_directory_phys);


#ifdef __cplusplus
}
#endif

#endif // PAGING_H