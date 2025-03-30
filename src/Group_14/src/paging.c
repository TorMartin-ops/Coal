#include "paging.h"
#include "terminal.h"   // For debug output (optional)

#define PAGE_TABLE_ENTRIES     1024
#define PAGE_DIRECTORY_ENTRIES 1024

// Global pointer to the kernel's page directory.
static uint32_t* kernel_page_directory = 0;

// A simple fixed pool for extra page tables.
#define MAX_EXTRA_PAGE_TABLES 256
static uint32_t extra_page_tables[MAX_EXTRA_PAGE_TABLES][PAGE_TABLE_ENTRIES] __attribute__((aligned(4096)));
static int extra_page_table_index = 0;

void paging_set_directory(uint32_t* pd) {
    kernel_page_directory = pd;
}

void paging_map_page(void* virt_addr) {
    if (!kernel_page_directory) {
        terminal_write("Error: kernel_page_directory not set in paging_map_page!\n");
        return;
    }
    uint32_t addr = (uint32_t)virt_addr;
    uint32_t directory_index = addr >> 22;
    uint32_t table_index = (addr >> 12) & 0x3FF;
    
    uint32_t pd_entry = kernel_page_directory[directory_index];
    uint32_t* page_table;
    
    // If the page table is not present, allocate one.
    if (!(pd_entry & PAGE_PRESENT)) {
        if (extra_page_table_index >= MAX_EXTRA_PAGE_TABLES) {
            terminal_write("Error: Out of extra page tables in paging_map_page!\n");
            return;
        }
        page_table = extra_page_tables[extra_page_table_index];
        extra_page_table_index++;
        // Clear the new page table.
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) {
            page_table[i] = 0;
        }
        // Install the page table into the page directory.
        kernel_page_directory[directory_index] = ((uint32_t)page_table) | PAGE_PRESENT | PAGE_RW;
    } else {
        page_table = (uint32_t*)(pd_entry & ~0xFFF);
    }
    
    // Create an identity mapping for the given address.
    page_table[table_index] = addr | PAGE_PRESENT | PAGE_RW;
    
    // Invalidate the TLB for the given virtual address.
    __asm__ volatile ("invlpg (%0)" ::"r" (virt_addr) : "memory");
}
