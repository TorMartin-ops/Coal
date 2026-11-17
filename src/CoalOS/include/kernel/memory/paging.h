/**
 * @file paging.h
 * @brief 32-bit non-PAE paging: constants, page-table walker API, address-space helpers.
 *
 * Virtual layout (3G/1G split, Linux style)
 *   0x00000000 - 0x003FFFFF  identity map of the first 4 MiB (kernel image, VGA,
 *                             boot info). Shared by every page directory; never
 *                             usable by user mappings.
 *   0x00400000 - 0xBFFFFFFF  user space (per process).
 *   0xC0000000 - 0xF7FFFFFF  direct map of lowmem, 896 MiB:
 *                             virt = phys + KERNEL_SPACE_VIRT_START.
 *   0xF8000000 - 0xF8FFFFFF  kernel stacks (page tables pre-allocated, shared).
 *   0xFA000000 - 0xFA003FFF  kmap slots (highmem frames, see kmap.h).
 *   0xFB000000 - 0xFB3FFFFF  device registers (uncached).
 *   0xFFC00000 - 0xFFFFFFFF  recursive page-directory mapping.
 *
 * RAM above 896 MiB is highmem: it is handed out only for user pages and is
 * mapped on demand with kmap(). Every frame from frame_alloc() is lowmem, so
 * page directories and tables are accessed with phys_to_virt(). All kernel
 * PDEs are populated at boot, so copying them into a new directory keeps every
 * address space in sync.
 *
 * Frame ownership: a present PTE owns one reference on its frame. Unmapping
 * drops that reference (the frame is freed when the count reaches zero);
 * frames outside the allocator (MMIO, boot memory) are ignored by put_frame().
 */
#ifndef PAGING_H
#define PAGING_H

#include <kernel/core/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAGE_SIZE       4096u
#define PAGE_SHIFT      12

/* Page table / directory entry flags */
#define PAGE_PRESENT    0x001
#define PAGE_RW         0x002
#define PAGE_USER       0x004
#define PAGE_PWT        0x008
#define PAGE_PCD        0x010
#define PAGE_ACCESSED   0x020
#define PAGE_DIRTY      0x040
#define PAGE_COW        0x200   /* software bit: private page shared after fork, copy on write */

/* Plain 32-bit paging has no execute-disable bit (it needs PAE). Kept as 0 so
 * callers can state intent without it having a hardware effect. */
#define PAGE_NX_BIT     0

#define PTE_KERNEL_DATA_FLAGS (PAGE_PRESENT | PAGE_RW)
#define PTE_USER_DATA_FLAGS   (PAGE_PRESENT | PAGE_RW | PAGE_USER)
#define PTE_USER_CODE_FLAGS   (PAGE_PRESENT | PAGE_USER)

#define PAGING_FLAG_MASK    0xFFFu
#define PAGING_ADDR_MASK    0xFFFFF000u
#define PAGING_PAGE_MASK    (~(PAGE_SIZE - 1u))
#define PAGING_OFFSET_MASK  (PAGE_SIZE - 1u)

#define PDE_INDEX(addr)     (((uintptr_t)(addr) >> 22) & 0x3FFu)
#define PTE_INDEX(addr)     (((uintptr_t)(addr) >> 12) & 0x3FFu)

#define PAGE_ALIGN_DOWN(addr) ALIGN_DOWN(addr, PAGE_SIZE)
#define PAGE_ALIGN_UP(addr)   ALIGN_UP(addr, PAGE_SIZE)

#define KERNEL_SPACE_VIRT_START 0xC0000000u
#define KERNEL_PDE_INDEX        PDE_INDEX(KERNEL_SPACE_VIRT_START)
#define IDENTITY_MAP_SIZE       0x400000u   /* PDE 0 */

#define RECURSIVE_PDE_INDEX 1023u
#define RECURSIVE_PD_VADDR  0xFFFFF000u

/* Fixed kernel windows above the direct map. Their page tables exist from
 * boot so that every page directory shares them. */
#define HIGHMEM_START_PHYS       0x38000000u   /* 896 MiB: end of the direct map */
#define KERNEL_STACK_VADDR_START 0xF8000000u
#define KERNEL_STACK_VADDR_END   0xF9000000u
#define KMAP_VADDR_START         0xFA000000u
#define KERNEL_MMIO_VADDR_START  0xFB000000u   /* device registers, mapped uncached (4 MiB window) */

#define VGA_PHYS_ADDR 0xB8000u
#define VGA_VIRT_ADDR (KERNEL_SPACE_VIRT_START + VGA_PHYS_ADDR)

/** Saved CPU state at an interrupt or exception. */
typedef struct registers {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no;
    uint32_t err_code;
    uint32_t eip, cs, eflags;
    uint32_t useresp;
    uint32_t ss;
} __attribute__((packed)) registers_t;

extern bool      g_nx_supported;               /* always false, see PAGE_NX_BIT */
extern uint32_t *g_kernel_page_directory_virt;  /* direct-mapped kernel PD */
extern uint32_t  g_kernel_page_directory_phys;

/* --- Direct map ---------------------------------------------------------- */

/** Kernel virtual address of a lowmem frame (frame_alloc(), page tables, kmalloc memory). */
static inline void *phys_to_virt(uintptr_t phys) { return (void *)(phys + KERNEL_SPACE_VIRT_START); }
/** Inverse of phys_to_virt() for direct-mapped addresses. */
static inline uintptr_t virt_to_phys(const void *virt) { return (uintptr_t)virt - KERNEL_SPACE_VIRT_START; }

/* --- Mapping API ----------------------------------------------------------
 * pd is the physical address of a page directory (the kernel's is
 * g_kernel_page_directory_phys). Calls return 0 on success, -1 on failure. */

/** Map one page, replacing (and releasing) any previous frame at vaddr.
 *  Page tables are allocated on demand; user PTs get PAGE_USER. */
int paging_map_single_4k(uint32_t *pd, uintptr_t vaddr, uintptr_t paddr, uint32_t flags);

/** Unmap [vaddr, vaddr+size) and drop the reference on each mapped frame.
 *  Page tables of user ranges that become entirely unmapped are freed. */
int paging_unmap_range(uint32_t *pd, uintptr_t vaddr, size_t size);

/** Translate vaddr; *flags_out gets the PTE flag bits. -1 if not mapped. */
int paging_get_physical_address_and_flags(uint32_t *pd, uintptr_t vaddr,
                                          uintptr_t *paddr_out, uint32_t *flags_out);

/** Pointer to the PTE for vaddr in pd, or NULL if the page table is absent
 *  (and create is false) or cannot be allocated. */
uint32_t *paging_pte(uint32_t *pd, uintptr_t vaddr, bool create);

void paging_invalidate_page(void *vaddr);   /* invlpg */
void paging_flush_tlb(void);                /* reload CR3 */
void paging_activate(uint32_t *pd_phys);    /* load CR3, set CR0.PG and CR0.WP */

#include <kernel/memory/paging_early.h>
#include <kernel/memory/paging_process.h>
#include <kernel/memory/paging_fault.h>

#ifdef __cplusplus
}
#endif

#endif /* PAGING_H */
