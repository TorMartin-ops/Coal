/**
 * @file paging.h
 * @brief x86_64 4-level paging: constants, page-table walker API, address-space helpers.
 *
 * Virtual layout
 *   0x0000000000010000 - 0x00007FFFFFFFEFFF  user space (per process, 47 bits). The first 64 KiB stay
 *                                            unmapped so that NULL dereferences fault.
 *   0xFFFF800000000000 -                     direct map of physical memory: virt = phys + PHYS_OFFSET,
 *                                            2 MiB pages, no execute. Every frame is reachable this way.
 *   0xFFFFC00000000000 - +1 GiB              kernel stacks, [guard page][stack pages] per task.
 *   0xFFFFFFFF80000000 -                     kernel image (linked here, mcmodel=kernel); text is
 *                                            executable and read-only, data is not executable.
 *   0xFFFFFFFFFB000000 - +16 MiB             device registers (uncached), see ioremap().
 *
 * All kernel PML4 entries are populated at boot, so copying them into a new PML4 keeps every address
 * space in sync: tables below them are shared and may be created later.
 *
 * Frame ownership: a present PTE owns one reference on its frame. Unmapping drops that reference (the
 * frame is freed when the count reaches zero); frames outside the allocator (MMIO, boot memory) are
 * ignored by put_frame().
 *
 * The page-table root ("pd" parameters) is passed as the physical address of the PML4, typed as a
 * pointer so that callers can cast it from g_kernel_page_directory_phys.
 */
#ifndef PAGING_H
#define PAGING_H

#include <kernel/core/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAGE_SIZE       4096ul
#define PAGE_SHIFT      12

typedef uint64_t pte_t;

/* Page table entry flags */
#define PAGE_PRESENT    0x001ul
#define PAGE_RW         0x002ul
#define PAGE_USER       0x004ul
#define PAGE_PWT        0x008ul
#define PAGE_PCD        0x010ul
#define PAGE_ACCESSED   0x020ul
#define PAGE_DIRTY      0x040ul
#define PAGE_HUGE       0x080ul     /* 2 MiB page (PDE only) */
#define PAGE_GLOBAL     0x100ul
#define PAGE_COW        0x200ul     /* software bit: private page shared after fork, copy on write */
#define PAGE_NX         (1ul << 63)

#define PTE_KERNEL_DATA_FLAGS (PAGE_PRESENT | PAGE_RW | PAGE_NX)
#define PTE_USER_DATA_FLAGS   (PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_NX)
#define PTE_USER_CODE_FLAGS   (PAGE_PRESENT | PAGE_USER)

#define PAGING_ADDR_MASK    0x000FFFFFFFFFF000ul
#define PAGING_FLAG_MASK    (~PAGING_ADDR_MASK)
#define PAGING_PAGE_MASK    (~(PAGE_SIZE - 1ul))
#define PAGING_OFFSET_MASK  (PAGE_SIZE - 1ul)
#define PTES_PER_TABLE      512u

#define PML4_INDEX(a)       (((uintptr_t)(a) >> 39) & 0x1FFu)
#define PDPT_INDEX(a)       (((uintptr_t)(a) >> 30) & 0x1FFu)
#define PD_INDEX(a)         (((uintptr_t)(a) >> 21) & 0x1FFu)
#define PT_INDEX(a)         (((uintptr_t)(a) >> 12) & 0x1FFu)
#define PTE_INDEX           PT_INDEX

#define PAGE_ALIGN_DOWN(addr) ALIGN_DOWN(addr, PAGE_SIZE)
#define PAGE_ALIGN_UP(addr)   ALIGN_UP(addr, PAGE_SIZE)

#define USER_SPACE_MIN          0x10000ul               /* Linux mmap_min_addr */
#define USER_SPACE_MAX          0x00007FFFFFFFF000ul    /* end of user space; sysret needs the last page unused */

#define PHYS_OFFSET             0xFFFF800000000000ul
#define KERNEL_IMAGE_VIRT       0xFFFFFFFF80000000ul
#define KERNEL_STACK_VADDR_START 0xFFFFC00000000000ul
#define KERNEL_STACK_VADDR_END   (KERNEL_STACK_VADDR_START + (1ul << 30))
#define KERNEL_MMIO_VADDR_START  0xFFFFFFFFFB000000ul   /* device registers, mapped uncached */
#define KERNEL_MMIO_VADDR_END    0xFFFFFFFFFC000000ul
#define KERNEL_PML4_FIRST        256u                   /* first PML4 entry of kernel space */

#define ZONE_HIGH_START_PHYS    0x100000000ul   /* 4 GiB: DMA-capable memory ends here */

#define VGA_PHYS_ADDR 0xB8000ul
#define VGA_VIRT_ADDR (PHYS_OFFSET + VGA_PHYS_ADDR)

extern void  *g_kernel_page_directory_virt;     /* the kernel PML4, direct-mapped */
extern uintptr_t g_kernel_page_directory_phys;

/* --- Direct map ---------------------------------------------------------- */

/** Kernel virtual address of any physical frame. */
static inline void *phys_to_virt(uintptr_t phys) { return (void *)(phys + PHYS_OFFSET); }
/** Inverse of phys_to_virt() for direct-mapped addresses (heap, frames; not the kernel image or stacks). */
static inline uintptr_t virt_to_phys(const void *virt) { return (uintptr_t)virt - PHYS_OFFSET; }

/* --- Mapping API ----------------------------------------------------------
 * Calls return 0 on success, -1 on failure. */

/** Map one page, replacing (and releasing) any previous frame at vaddr.
 *  Page tables are allocated on demand; user addresses get PAGE_USER on the tables. */
int paging_map_single_4k(void *pd, uintptr_t vaddr, uintptr_t paddr, uint64_t flags);

/** Unmap [vaddr, vaddr+size) and drop the reference on each mapped frame.
 *  Page tables of user ranges that become entirely unmapped are freed. */
int paging_unmap_range(void *pd, uintptr_t vaddr, size_t size);

/** Translate vaddr; *flags_out gets the PTE flag bits. -1 if not mapped. */
int paging_get_physical_address_and_flags(void *pd, uintptr_t vaddr,
                                          uintptr_t *paddr_out, uint64_t *flags_out);

/** Pointer to the PTE for vaddr in pd, or NULL if a table is absent
 *  (and create is false) or cannot be allocated. */
pte_t *paging_pte(void *pd, uintptr_t vaddr, bool create);

/** Maps size bytes of device registers at phys uncached into the MMIO window; NULL if the window is full.
 *  The window is never freed. The mapping keeps phys' offset within its page. */
volatile void *ioremap(uint64_t phys, size_t size);

static inline void paging_invalidate_page(void *vaddr) { asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory"); }
void paging_flush_tlb(void);                /* reload CR3 */

#include <kernel/memory/paging_early.h>
#include <kernel/memory/paging_process.h>
#include <kernel/memory/paging_fault.h>

#ifdef __cplusplus
}
#endif

#endif /* PAGING_H */
