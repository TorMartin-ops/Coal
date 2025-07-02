/**
 * @file paging_early.c
 * @brief Early boot paging initialization for Coal OS
 */

#define LOG_MODULE "paging_early"

#include <kernel/memory/paging_early.h>
#include <kernel/memory/paging_internal.h>
#include <kernel/memory/paging.h>
#include <kernel/core/log.h>
#include <kernel/core/error.h>
#include <kernel/cpu/cpuid.h>
#include <kernel/cpu/msr.h>
#include <kernel/arch/multiboot2.h>
#include <kernel/lib/string.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/drivers/display/serial.h>

// --- Constants ---
#define MAX_EARLY_ALLOCATIONS 256
#define KERN_EINVAL -1
#define KERN_ENOMEM -4

// --- External symbols ---
extern uint8_t _kernel_start_phys;
extern uint8_t _kernel_end_phys;
extern uint32_t g_multiboot_info_phys_addr_global;

// --- Early allocation tracking ---
static uintptr_t early_allocated_frames[MAX_EARLY_ALLOCATIONS];
static int early_allocated_count = 0;
static bool early_allocator_used = false;

// --- External assembly functions ---
extern void paging_activate(uint32_t *page_directory_phys);

// --- CPU control register functions ---
static inline uint32_t read_cr4(void) {
    uint32_t v;
    asm volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(uint32_t v) {
    asm volatile("mov %0, %%cr4" :: "r"(v) : "memory");
}

static inline void enable_cr4_pse(void) {
    write_cr4(read_cr4() | CR4_PSE);
}

/**
 * @brief Find multiboot tag during early boot (before paging)
 */
static struct multiboot_tag *find_multiboot_tag_early(uint32_t mb_info_phys_addr, uint16_t type) {
    if (mb_info_phys_addr == 0 || mb_info_phys_addr >= 0x100000) {
        LOG_ERROR("Invalid MB info address %#lx", (unsigned long)mb_info_phys_addr);
        return NULL;
    }
    
    volatile uint32_t* mb_info_ptr = (volatile uint32_t*)mb_info_phys_addr;
    uint32_t total_size = mb_info_ptr[0];

    if (total_size < 8 || total_size > 16 * 1024) {
        LOG_ERROR("Invalid MB total size %lu", (unsigned long)total_size);
        return NULL;
    }

    struct multiboot_tag *tag = (struct multiboot_tag *)(mb_info_phys_addr + 8);
    uintptr_t info_end = mb_info_phys_addr + total_size;

    while ((uintptr_t)tag < info_end && tag->type != MULTIBOOT_TAG_TYPE_END) {
        uintptr_t current_tag_addr = (uintptr_t)tag;
        if (current_tag_addr + sizeof(struct multiboot_tag) > info_end ||
            tag->size < 8 ||
            current_tag_addr + tag->size > info_end) {
            LOG_ERROR("Invalid tag at %#lx (type %u, size %lu)", 
                     (unsigned long)current_tag_addr, tag->type, (unsigned long)tag->size);
            return NULL;
        }

        if (tag->type == type) {
            return tag;
        }

        uintptr_t next_tag_addr = current_tag_addr + ((tag->size + 7) & ~7);
        if (next_tag_addr <= current_tag_addr || next_tag_addr >= info_end) {
            LOG_ERROR("Invalid next tag address %#lx from tag at %#lx", 
                     (unsigned long)next_tag_addr, (unsigned long)current_tag_addr);
            break;
        }
        tag = (struct multiboot_tag *)next_tag_addr;
    }

    return NULL;
}

/**
 * @brief Allocate a physical frame during early boot
 */
uintptr_t paging_alloc_early_frame_physical(void) {
    early_allocator_used = true;
    
    if (g_multiboot_info_phys_addr_global == 0) {
        LOG_FATAL("Multiboot info address is zero!");
        return 0;
    }

    struct multiboot_tag_mmap *mmap_tag = (struct multiboot_tag_mmap *)
        find_multiboot_tag_early(g_multiboot_info_phys_addr_global, MULTIBOOT_TAG_TYPE_MMAP);

    if (!mmap_tag) {
        LOG_FATAL("Multiboot MMAP tag not found!");
        return 0;
    }

    if (mmap_tag->entry_size < sizeof(multiboot_memory_map_t)) {
        LOG_FATAL("MMAP entry size too small!");
        return 0;
    }

    uintptr_t kernel_start_p = (uintptr_t)&_kernel_start_phys;
    uintptr_t kernel_end_p = PAGE_ALIGN_UP((uintptr_t)&_kernel_end_phys);
    
    uint32_t mb_info_size = *(volatile uint32_t*)g_multiboot_info_phys_addr_global;
    mb_info_size = (mb_info_size >= 8) ? mb_info_size : 8;
    uintptr_t mb_info_start = g_multiboot_info_phys_addr_global;
    uintptr_t mb_info_end = PAGE_ALIGN_UP(mb_info_start + mb_info_size);

    multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
    uintptr_t mmap_tag_end = (uintptr_t)mmap_tag + mmap_tag->size;

    while ((uintptr_t)mmap_entry < mmap_tag_end) {
        if ((uintptr_t)mmap_entry + mmap_tag->entry_size > mmap_tag_end) {
            LOG_FATAL("Invalid MMAP tag structure");
            return 0;
        }

        if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE && mmap_entry->len >= PAGE_SIZE) {
            uintptr_t region_start = (uintptr_t)mmap_entry->addr;
            uintptr_t region_len = (uintptr_t)mmap_entry->len;
            uintptr_t region_end = region_start + region_len;

            if (region_end < region_start) {
                region_end = UINTPTR_MAX;
            }

            uintptr_t current_frame_addr = ALIGN_UP(region_start, PAGE_SIZE);

            while (current_frame_addr < region_end && (current_frame_addr + PAGE_SIZE) > current_frame_addr) {
                if (current_frame_addr + PAGE_SIZE > region_end) {
                    break;
                }

                // Skip low memory
                if (current_frame_addr < 0x100000) {
                    current_frame_addr += PAGE_SIZE;
                    continue;
                }

                // Skip kernel
                bool overlaps_kernel = (current_frame_addr < kernel_end_p &&
                                      (current_frame_addr + PAGE_SIZE) > kernel_start_p);
                if (overlaps_kernel) {
                    current_frame_addr += PAGE_SIZE;
                    continue;
                }

                // Skip multiboot info
                bool overlaps_mb_info = (current_frame_addr < mb_info_end &&
                                       (current_frame_addr + PAGE_SIZE) > mb_info_start);
                if (overlaps_mb_info) {
                    current_frame_addr += PAGE_SIZE;
                    continue;
                }

                // Check if already allocated
                bool already_allocated = false;
                for (int i = 0; i < early_allocated_count; ++i) {
                    if (early_allocated_frames[i] == current_frame_addr) {
                        already_allocated = true;
                        break;
                    }
                }
                if (already_allocated) {
                    current_frame_addr += PAGE_SIZE;
                    continue;
                }

                if (early_allocated_count >= MAX_EARLY_ALLOCATIONS) {
                    LOG_FATAL("Exceeded MAX_EARLY_ALLOCATIONS!");
                    return 0;
                }

                early_allocated_frames[early_allocated_count++] = current_frame_addr;
                memset((void*)current_frame_addr, 0, PAGE_SIZE);
                LOG_TRACE("Allocated early frame at %#lx", current_frame_addr);
                return current_frame_addr;
            }
        }

        uintptr_t next_entry_addr = (uintptr_t)mmap_entry + mmap_tag->entry_size;
        if (next_entry_addr <= (uintptr_t)mmap_entry || next_entry_addr > mmap_tag_end) {
            LOG_FATAL("Invalid MMAP iteration");
            return 0;
        }
        mmap_entry = (multiboot_memory_map_t *)next_entry_addr;
    }

    LOG_FATAL("No suitable physical frame found!");
    return 0;
}

/**
 * @brief Allocate a page table during early boot
 */
static uint32_t* allocate_page_table_phys(void) {
    uintptr_t pt_phys = paging_alloc_early_frame_physical();
    if (!pt_phys) {
        LOG_ERROR("Failed to allocate frame for Page Table");
        return NULL;
    }
    return (uint32_t*)pt_phys;
}

/**
 * @brief Check and enable PSE (Page Size Extension)
 */
bool check_and_enable_pse(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);

    if (edx & CPUID_FEAT_EDX_PSE) {
        LOG_INFO("CPU supports PSE (4MB Pages)");
        enable_cr4_pse();
        if (read_cr4() & CR4_PSE) {
            LOG_INFO("CR4.PSE bit enabled");
            g_pse_supported = true;
            return true;
        } else {
            LOG_ERROR("Failed to enable CR4.PSE bit!");
            g_pse_supported = false;
            return false;
        }
    } else {
        LOG_WARN("CPU does not support PSE (4MB Pages)");
        g_pse_supported = false;
        return false;
    }
}

/**
 * @brief Check and enable NX (No-Execute) bit
 */
bool check_and_enable_nx(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);

    if (eax < 0x80000001) {
        LOG_WARN("CPUID extended function 0x80000001 not supported. Cannot check NX.");
        g_nx_supported = false;
        return false;
    }

    cpuid(0x80000001, &eax, &ebx, &ecx, &edx);

    if (edx & CPUID_FEAT_EDX_NX) {
        LOG_INFO("CPU supports NX (Execute Disable) bit");
        uint64_t efer = rdmsr(MSR_EFER);
        efer |= EFER_NXE;
        wrmsr(MSR_EFER, efer);

        efer = rdmsr(MSR_EFER);
        if (efer & EFER_NXE) {
            LOG_INFO("EFER.NXE bit enabled");
            g_nx_supported = true;
            return true;
        } else {
            LOG_ERROR("Failed to enable EFER.NXE bit!");
            g_nx_supported = false;
            return false;
        }
    } else {
        LOG_WARN("CPU does not support NX bit");
        g_nx_supported = false;
        return false;
    }
}

/**
 * @brief Map physical memory during early boot
 */
static int paging_map_physical_early(uintptr_t page_directory_phys,
                                    uintptr_t phys_addr_start,
                                    size_t size,
                                    uint32_t flags,
                                    bool map_to_higher_half) {
    if (page_directory_phys == 0 || (page_directory_phys % PAGE_SIZE) != 0 || size == 0) {
        LOG_ERROR("Invalid PD phys (%#lx) or size (%lu)", page_directory_phys, size);
        return KERN_EINVAL;
    }

    uintptr_t current_phys = PAGE_ALIGN_DOWN(phys_addr_start);
    uintptr_t end_phys = phys_addr_start + size;
    
    // Handle overflow
    if (end_phys < phys_addr_start) {
        end_phys = UINTPTR_MAX;
    }
    
    end_phys = PAGE_ALIGN_UP(end_phys);
    if (end_phys < (phys_addr_start + size)) {
        end_phys = UINTPTR_MAX;
    }

    if (end_phys <= current_phys) {
        return 0;
    }

    size_t map_size = end_phys - current_phys;
    volatile uint32_t* pd_phys_ptr = (volatile uint32_t*)page_directory_phys;

    LOG_INFO("Mapping Phys [%#lx - %#lx) -> %s (Size: %lu KB)", 
             current_phys, end_phys,
             map_to_higher_half ? "HigherHalf" : "Identity",
             map_size / 1024);

    int page_count = 0;
    int safety_counter = 0;
    const int max_pages_early = (1024 * 1024 * 1024) / PAGE_SIZE;

    while (current_phys < end_phys) {
        if (++safety_counter > max_pages_early) {
            LOG_ERROR("Safety break after %d pages", safety_counter);
            return KERN_EINVAL;
        }

        uintptr_t target_vaddr;
        if (map_to_higher_half) {
            if (current_phys > UINTPTR_MAX - KERNEL_SPACE_VIRT_START) {
                LOG_ERROR("Virtual address overflow for Phys %#lx", current_phys);
                return KERN_EINVAL;
            }
            target_vaddr = KERNEL_SPACE_VIRT_START + current_phys;
        } else {
            target_vaddr = current_phys;
            if (target_vaddr >= KERNEL_SPACE_VIRT_START) {
                LOG_WARN("Identity map target %#lx overlaps kernel space", target_vaddr);
            }
        }

        uint32_t pd_idx = PDE_INDEX(target_vaddr);
        uint32_t pt_idx = PTE_INDEX(target_vaddr);

        uint32_t pde = pd_phys_ptr[pd_idx];
        uintptr_t pt_phys_addr;
        volatile uint32_t* pt_phys_ptr;

        if (!(pde & PAGE_PRESENT)) {
            // Allocate new page table
            uint32_t* new_pt = allocate_page_table_phys();
            if (!new_pt) {
                LOG_ERROR("Failed to allocate PT frame for VAddr %#lx", target_vaddr);
                return KERN_ENOMEM;
            }
            pt_phys_addr = (uintptr_t)new_pt;
            pt_phys_ptr = (volatile uint32_t*)pt_phys_addr;

            uint32_t pde_flags = PAGE_PRESENT | PAGE_RW;
            if (flags & PAGE_USER) { 
                pde_flags |= PAGE_USER; 
            }
            pd_phys_ptr[pd_idx] = (pt_phys_addr & PAGING_ADDR_MASK) | pde_flags;
            LOG_TRACE("Created PT for PDE[%u]", pd_idx);
        } else {
            if (pde & PAGE_SIZE_4MB) {
                LOG_ERROR("Attempt to map 4K page over existing 4M page at VAddr %#lx", target_vaddr);
                return KERN_EINVAL;
            }
            pt_phys_addr = (uintptr_t)(pde & PAGING_ADDR_MASK);
            pt_phys_ptr = (volatile uint32_t*)pt_phys_addr;

            // Update PDE flags if needed
            uint32_t needed_pde_flags = PAGE_PRESENT;
            if (flags & PAGE_RW) needed_pde_flags |= PAGE_RW;
            if (flags & PAGE_USER) needed_pde_flags |= PAGE_USER;

            if ((pde & needed_pde_flags) != needed_pde_flags) {
                pd_phys_ptr[pd_idx] |= (needed_pde_flags & (PAGE_RW | PAGE_USER));
            }
        }

        // Map the page
        uint32_t pte = pt_phys_ptr[pt_idx];
        uint32_t pte_final_flags = (flags & (PAGE_RW | PAGE_USER | PAGE_PWT | PAGE_PCD)) | PAGE_PRESENT;
        uint32_t new_pte = (current_phys & PAGING_ADDR_MASK) | pte_final_flags;

        if (pte & PAGE_PRESENT) {
            if (pte != new_pte) {
                LOG_ERROR("PTE already present/different for VAddr %#lx", target_vaddr);
                return KERN_EINVAL;
            }
        }

        pt_phys_ptr[pt_idx] = new_pte;
        page_count++;

        if (current_phys > UINTPTR_MAX - PAGE_SIZE) {
            break;
        }
        current_phys += PAGE_SIZE;
    }

    LOG_DEBUG("Mapped %d pages successfully", page_count);
    return 0;
}

/**
 * @brief Initialize page directory for early boot
 */
int paging_initialize_directory(uintptr_t* out_pd_phys) {
    LOG_INFO("Initializing Page Directory...");
    
    uintptr_t pd_phys = paging_alloc_early_frame_physical();
    if (!pd_phys) {
        LOG_ERROR("Failed to allocate initial Page Directory frame!");
        return KERN_ENOMEM;
    }
    
    LOG_INFO("Allocated initial PD at Phys: %#lx", pd_phys);

    if (!check_and_enable_pse()) {
        LOG_ERROR("PSE support is required but not available/enabled!");
        return KERN_EINVAL;
    }
    
    check_and_enable_nx();

    *out_pd_phys = pd_phys;
    LOG_INFO("Directory allocated, features checked/enabled");
    return 0;
}

/**
 * @brief Set up early memory mappings
 */
int paging_setup_early_maps(uintptr_t page_directory_phys,
                           uintptr_t kernel_phys_start,
                           uintptr_t kernel_phys_end,
                           uintptr_t heap_phys_start,
                           size_t heap_size) {
    LOG_INFO("Setting up early memory maps...");
    volatile uint32_t* pd_phys_ptr = (volatile uint32_t*)page_directory_phys;

    // Identity mapping for first 4MB
    size_t identity_map_size = 4 * 1024 * 1024;
    LOG_INFO("Mapping Identity [0x0 - 0x%zx)", identity_map_size);
    if (paging_map_physical_early(page_directory_phys, 0x0, identity_map_size, 
                                 PTE_KERNEL_DATA_FLAGS, false) != 0) {
        LOG_ERROR("Failed to set up early identity mapping!");
        return KERN_EINVAL;
    }

    // Map kernel to higher half
    uintptr_t kernel_phys_aligned_start = PAGE_ALIGN_DOWN(kernel_phys_start);
    uintptr_t kernel_phys_aligned_end = PAGE_ALIGN_UP(kernel_phys_end);
    size_t kernel_size = kernel_phys_aligned_end - kernel_phys_aligned_start;
    
    LOG_INFO("Mapping Kernel Phys [%#lx - %#lx) to Higher Half",
             kernel_phys_aligned_start, kernel_phys_aligned_end);
    if (paging_map_physical_early(page_directory_phys, kernel_phys_aligned_start, 
                                 kernel_size, PTE_KERNEL_DATA_FLAGS, true) != 0) {
        LOG_ERROR("Failed to map kernel to higher half!");
        return KERN_EINVAL;
    }

    // Map heap if provided
    if (heap_size > 0) {
        uintptr_t heap_phys_aligned_start = PAGE_ALIGN_DOWN(heap_phys_start);
        uintptr_t heap_end = heap_phys_start + heap_size;
        uintptr_t heap_phys_aligned_end = PAGE_ALIGN_UP(heap_end);
        if (heap_phys_aligned_end < heap_end) {
            heap_phys_aligned_end = UINTPTR_MAX;
        }
        size_t heap_aligned_size = heap_phys_aligned_end - heap_phys_aligned_start;
        
        LOG_INFO("Mapping Kernel Heap Phys [%#lx - %#lx) to Higher Half",
                 heap_phys_aligned_start, heap_phys_aligned_end);
        if (paging_map_physical_early(page_directory_phys, heap_phys_aligned_start, 
                                     heap_aligned_size, PTE_KERNEL_DATA_FLAGS, true) != 0) {
            LOG_ERROR("Failed to map early kernel heap!");
            return KERN_EINVAL;
        }
    }

    // Map VGA buffer
    LOG_INFO("Mapping VGA Buffer Phys %#lx to Virt %#lx", VGA_PHYS_ADDR, VGA_VIRT_ADDR);
    if (paging_map_physical_early(page_directory_phys, VGA_PHYS_ADDR, PAGE_SIZE, 
                                 PTE_KERNEL_DATA_FLAGS, true) != 0) {
        LOG_ERROR("Failed to map VGA buffer!");
        return KERN_EINVAL;
    }

    // Pre-allocate Kernel Stack Page Tables
    LOG_INFO("Pre-allocating Page Tables for Kernel Stack Range [%#lx - %#lx)",
             KERNEL_STACK_VADDR_START, KERNEL_TEMP_MAP_START);

    uint32_t stack_start_pde_idx = PDE_INDEX(KERNEL_STACK_VADDR_START);
    uint32_t stack_end_pde_idx = PDE_INDEX(KERNEL_TEMP_MAP_START);

    for (uint32_t pde_idx = stack_start_pde_idx; pde_idx < stack_end_pde_idx; ++pde_idx) {
        if (!(pd_phys_ptr[pde_idx] & PAGE_PRESENT)) {
            uintptr_t new_pt_phys = paging_alloc_early_frame_physical();
            if (!new_pt_phys) {
                LOG_ERROR("Failed to allocate PT frame for kernel stack!");
                return KERN_ENOMEM;
            }

            uint32_t pde_flags = PAGE_PRESENT | PAGE_RW;
            if (g_nx_supported) {
                pde_flags |= PAGE_NX_BIT;
            }
            pd_phys_ptr[pde_idx] = (new_pt_phys & PAGING_ADDR_MASK) | pde_flags;
        }
    }
    LOG_DEBUG("Kernel Stack PTs pre-allocation complete");

    // Pre-allocate Temp Map Area PT
    const uint32_t temp_map_pde_index = PDE_INDEX(KERNEL_TEMP_MAP_START);
    LOG_INFO("Pre-allocating Page Table for Temporary Mapping Area (PDE %u)", temp_map_pde_index);
    if (!(pd_phys_ptr[temp_map_pde_index] & PAGE_PRESENT)) {
        uintptr_t temp_pt_phys = paging_alloc_early_frame_physical();
        if (!temp_pt_phys) {
            LOG_ERROR("Failed to allocate PT frame for temporary mapping area!");
            return KERN_ENOMEM;
        }
        uint32_t temp_pde_flags = PAGE_PRESENT | PAGE_RW;
        if (g_nx_supported) {
            temp_pde_flags |= PAGE_NX_BIT;
        }
        pd_phys_ptr[temp_map_pde_index] = (temp_pt_phys & PAGING_ADDR_MASK) | temp_pde_flags;
        LOG_DEBUG("Mapped PDE[%u] to PT Phys %#lx", temp_map_pde_index, temp_pt_phys);
    } else {
        LOG_DEBUG("PT for Temporary Mapping Area (PDE %u) already exists", temp_map_pde_index);
    }

    LOG_INFO("Early memory maps configured");
    return 0;
}

/**
 * @brief Finalize paging setup and activate
 */
int paging_finalize_and_activate(uintptr_t page_directory_phys, uintptr_t total_memory_bytes) {
    LOG_INFO("Finalizing and activating paging...");
    
    if (page_directory_phys == 0 || (page_directory_phys % PAGE_SIZE) != 0) {
        LOG_ERROR("Invalid PD physical address!");
        return KERN_EINVAL;
    }

    volatile uint32_t* pd_phys_ptr = (volatile uint32_t*)page_directory_phys;

    // Set up recursive mapping
    uint32_t recursive_pde_flags = PAGE_PRESENT | PAGE_RW | PAGE_NX_BIT;
    pd_phys_ptr[RECURSIVE_PDE_INDEX] = (page_directory_phys & PAGING_ADDR_MASK) | recursive_pde_flags;
    LOG_INFO("Set recursive PDE[%u] to point to PD Phys=%#lx", RECURSIVE_PDE_INDEX, page_directory_phys);

    LOG_INFO("Activating Paging (Loading CR3, Setting CR0.PG)...");
    paging_activate((uint32_t*)page_directory_phys);
    LOG_INFO("Paging HW Activated");

    // Set global pointers
    uintptr_t kernel_pd_virt_addr = RECURSIVE_PD_VADDR;
    paging_set_kernel_directory((uint32_t*)kernel_pd_virt_addr, page_directory_phys);

    // Verify recursive mapping
    LOG_DEBUG("Verifying recursive mapping via virtual access...");
    volatile uint32_t recursive_value_read_virt = g_kernel_page_directory_virt[RECURSIVE_PDE_INDEX];
    
    uint32_t actual_phys_in_pte = recursive_value_read_virt & PAGING_ADDR_MASK;
    uint32_t expected_phys = page_directory_phys & PAGING_ADDR_MASK;

    if (actual_phys_in_pte != expected_phys) {
        LOG_ERROR("Recursive PDE verification failed! Expected %#lx, got %#lx",
                  expected_phys, actual_phys_in_pte);
        return KERN_EINVAL;
    }
    
    LOG_INFO("Recursive mapping verified successfully");
    LOG_INFO("Paging enabled and active. Higher half operational");
    
    early_allocator_used = false;
    return 0;
}