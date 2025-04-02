/**
 * paging.c
 *
 * World-Class Paging Module for a 32-bit x86 OS.
 *
 * Features:
 *   - A global kernel_page_directory pointer
 *   - Functions to allocate and set up new page tables using the buddy allocator
 *   - Functions to map single pages or a range of pages (identity-mapped or custom flags)
 *   - A helper to do large identity mapping (paging_init_identity_map)
 *   - Thorough debugging messages
 */

 #include "paging.h"
 #include "buddy.h"
 #include "terminal.h"
 
#include "types.h"
 // --------------------------------------------------------------------
 // Constants for PDE/PTE
 // --------------------------------------------------------------------
 #define PAGE_PRESENT 0x1
 #define PAGE_RW      0x2
 #define PAGE_USER    0x4
 
 // Helper macros to extract directory/table indices
 #define PDE_INDEX(addr)  ((addr) >> 22)
 #define PTE_INDEX(addr)  (((addr) >> 12) & 0x3FF)
 
 // --------------------------------------------------------------------
 // Global kernel_page_directory pointer
 // --------------------------------------------------------------------
uint32_t* kernel_page_directory = NULL;
 
 /**
  * paging_set_directory
  *
  * Stores the pointer to the (kernel) page directory in a global variable
  * so other paging functions can use it.
  *
  * @param pd pointer to the page directory to set as active
  */
 void paging_set_directory(uint32_t* pd) {
     kernel_page_directory = pd;
     // Optionally, you can do "mov cr3, pd" here if you want to activate it,
     // but usually that is done in a separate function or after building it fully.
     // e.g., "asm volatile('mov %0, %%cr3' : : 'r'(pd));"
 }
 
 /**
  * allocate_page_table
  *
  * Uses the buddy allocator to get a 4 KB page for a page table. 
  * Initializes all entries to 0 (not present). 
  * 
  * @return pointer to the new page table, or NULL on failure
  */
 static uint32_t* allocate_page_table(void) {
     // Attempt to allocate one 4096–byte page from buddy
     uint32_t* pt = (uint32_t*)buddy_alloc(4096);
     if (!pt) {
         terminal_write("[Paging] allocate_page_table: buddy_alloc(4KB) failed!\n");
         return NULL;
     }
     // Clear the page table
     for (int i = 0; i < 1024; i++) {
         pt[i] = 0;
     }
     return pt;
 }
 
 /**
  * paging_invalidate_page
  *
  * Helper to invalidate the TLB entry for a single virtual page.
  * 
  * @param addr The virtual address to invalidate.
  */
 static void paging_invalidate_page(void *addr) {
     __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
 }
 
 /**
  * paging_map_page
  *
  * Maps a single virtual address to an identity mapping in the global
  * kernel_page_directory. 
  * 
  * If the PDE is not present, we allocate a new page table from buddy 
  * and insert it with RW permissions. Then we set the page table entry 
  * to "addr | present + rw".
  *
  * @param virt_addr The virtual address to map (identical to physical).
  */
 void paging_map_page(void* virt_addr) {
     if (!kernel_page_directory) {
         terminal_write("[Paging] paging_map_page: kernel_page_directory not set.\n");
         return;
     }
     uint32_t addr   = (uint32_t)virt_addr;
     uint32_t pd_idx = PDE_INDEX(addr);
     uint32_t pt_idx = PTE_INDEX(addr);
 
     // Check if PDE is present
     uint32_t pd_entry = kernel_page_directory[pd_idx];
     uint32_t* page_table;
     if (!(pd_entry & PAGE_PRESENT)) {
         // Allocate a new page table
         page_table = allocate_page_table();
         if (!page_table)
             return;
         // PDE = base + flags
         kernel_page_directory[pd_idx] = ((uint32_t)page_table) | PAGE_PRESENT | PAGE_RW;
     } else {
         // PDE is present => get the page_table address
         page_table = (uint32_t*)(pd_entry & ~0xFFF);
     }
 
     // Set the mapping in the page table => identity map
     page_table[pt_idx] = addr | PAGE_PRESENT | PAGE_RW;
 
     // Invalidate TLB for that page
     paging_invalidate_page(virt_addr);
 }
 
 /**
  * paging_map_range
  *
  * Maps a contiguous range of addresses [virt_addr..virt_addr+memsz) 
  * to identity–mapped physical addresses with 'flags'.
  * The size is rounded up to a multiple of 4096. 
  * If PDEs are missing, we allocate them from buddy.
  *
  * @param page_directory The page directory to update (often kernel_page_directory).
  * @param virt_addr      The start of the region to map
  * @param memsz          The length in bytes
  * @param flags          e.g. PAGE_PRESENT | PAGE_RW | PAGE_USER
  * @return 0 on success, -1 on failure
  */
 int paging_map_range(uint32_t *page_directory, uint32_t virt_addr, 
                      uint32_t memsz, uint32_t flags) {
     if (!page_directory) {
         terminal_write("[Paging] map_range: page_directory is NULL!\n");
         return -1;
     }
     // Page-align the start
     uint32_t start_addr = virt_addr & ~(0xFFF);
     // Round up end address
     uint32_t end_addr = (virt_addr + memsz + 0xFFF) & ~0xFFF;
 
     // 4KB steps
     for (uint32_t addr = start_addr; addr < end_addr; addr += 0x1000) {
         uint32_t pd_idx = PDE_INDEX(addr);
         uint32_t pt_idx = PTE_INDEX(addr);
 
         uint32_t pd_entry = page_directory[pd_idx];
         uint32_t* page_table;
         if (!(pd_entry & PAGE_PRESENT)) {
             // PDE not present => allocate PT
             page_table = allocate_page_table();
             if (!page_table) {
                 terminal_write("[Paging] map_range: no memory for page table.\n");
                 return -1;
             }
             // Insert PDE
             page_directory[pd_idx] = (uint32_t)page_table | (flags & 0x7);
             // Ensure it's always RW at least if 'flags' includes PAGE_RW
             page_directory[pd_idx] |= PAGE_PRESENT;
         } else {
             page_table = (uint32_t*)(pd_entry & ~0xFFF);
         }
 
         // Build final PTE = physical=addr (identity map) + flags
         page_table[pt_idx] = (addr & ~0xFFF) | (flags & 0xFFF);
         // Ensure we always have present + maybe RW
         page_table[pt_idx] |= PAGE_PRESENT;
 
         // Invalidate TLB for 'addr'
         __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
     }
     return 0;
 }
 
 /**
  * paging_init_identity_map
  *
  * An optional helper function to identity–map [0..size] in both the PDE
  * and the actual PT. Also sets a global pointer and loads CR3 & CR0 
  * if desired.
  *
  * This is optional, but commonly used in simple kernels to do a big identity 
  * map for e.g. the first 64MB or more, plus a higher-half mapping.
  */
 int paging_init_identity_map(uint32_t *page_directory, uint32_t size, uint32_t flags) {
     // Example usage: map [0..size], identity style
     if (!page_directory) {
         terminal_write("[Paging] init_identity_map: PD is NULL!\n");
         return -1;
     }
     // Round size up
     size = (size + 0xFFF) & ~0xFFF;
 
     // Let's map in 4KB increments
     for (uint32_t addr = 0; addr < size; addr += 0x1000) {
         uint32_t pd_idx = PDE_INDEX(addr);
         uint32_t pt_idx = PTE_INDEX(addr);
         uint32_t pd_entry = page_directory[pd_idx];
         uint32_t* page_table;
         if (!(pd_entry & PAGE_PRESENT)) {
             page_table = allocate_page_table();
             if (!page_table) {
                 terminal_write("[Paging] init_identity_map: no mem for PT.\n");
                 return -1;
             }
             page_directory[pd_idx] = (uint32_t)page_table | (flags & 0x7);
             page_directory[pd_idx] |= PAGE_PRESENT;
         } else {
             page_table = (uint32_t*)(pd_entry & ~0xFFF);
         }
 
         // Identity map each page
         page_table[pt_idx] = addr | (flags & 0xFFF);
         page_table[pt_idx] |= PAGE_PRESENT;
     }
     return 0;
 }
 
 /**
  * Example function to load the page_directory into CR3 and enable paging in CR0.
  * 
  * If you want this integrated, you can call it after building your PDE.
  * 
  * @param page_directory The PDE to activate
  */
 void paging_activate(uint32_t *page_directory) {
     // Load CR3
     __asm__ volatile("mov %0, %%cr3" : : "r"(page_directory));
 
     // Enable paging bit in CR0
     uint32_t cr0;
     __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
     cr0 |= 0x80000000; // set PG bit
     __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
 }
 