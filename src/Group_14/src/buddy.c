#include "buddy.h"     // Now includes MAX_ORDER definition
#include "terminal.h"
#include "types.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

// Constants MIN_ORDER and MAX_ORDER are now defined in buddy.h
// #define MIN_ORDER 5 // REMOVED
// #define MAX_ORDER 22 // REMOVED

// Smallest block size is derived from MIN_ORDER
#define MIN_BLOCK_SIZE (1 << MIN_ORDER)

// buddy_block_t definition remains the same
typedef struct buddy_block {
    struct buddy_block *next;
} buddy_block_t;

// free_lists definition remains the same
static buddy_block_t *free_lists[MAX_ORDER + 1] = {0};

// Global stats variables remain the same
static size_t g_buddy_total_size = 0;
static size_t g_buddy_free_bytes = 0;

// Helper functions (round_up_to_power_of_two, buddy_size_to_order) remain the same
static size_t buddy_round_up_to_power_of_two(size_t size) { /* ... same ... */
    size_t power = MIN_BLOCK_SIZE;
    while (power < size) power <<= 1;
    return power;
}
static int buddy_size_to_order(size_t size) { /* ... same ... */
    int order = MIN_ORDER;
    size_t block_size = (size_t)1 << MIN_ORDER;
    while (block_size < size) { block_size <<= 1; order++; }
    return order;
}


// buddy_init, buddy_alloc, buddy_free, buddy_free_space implementations remain the same
// as the last correct version, they just use MAX_ORDER from the header now.

void buddy_init(void *heap, size_t size) { /* ... same implementation as before ... */
    for (int i = MIN_ORDER; i <= MAX_ORDER; i++) { free_lists[i] = NULL; }
    int order = buddy_size_to_order(size);
    if (order > MAX_ORDER) {
        terminal_printf("[Buddy] Error: heap size (%u bytes, requires order %d) too large for buddy MAX_ORDER (%d).\n", size, order, MAX_ORDER);
        g_buddy_total_size = 0; g_buddy_free_bytes = 0; return;
    }
    g_buddy_total_size = (size_t)1 << order;
    g_buddy_free_bytes = g_buddy_total_size;
    buddy_block_t *block = (buddy_block_t*)heap;
    block->next = NULL; free_lists[order] = block;
    terminal_write("[Buddy] init done. size=0x");
    char buf[9]; unsigned val = g_buddy_total_size;
    for (int i = 0; i < 8; i++) { unsigned nibble = (val >> ((7-i)*4)) & 0xF; buf[i] = (nibble < 10) ? ('0'+nibble) : ('A'+nibble-10); }
    buf[8] = '\0'; terminal_write(buf);
    terminal_printf(" bytes (Order %d)\n", order);
}

void *buddy_alloc(size_t size) { /* ... same implementation as before ... */
    if (size == 0) return NULL;
    size_t req_size = buddy_round_up_to_power_of_two(size);
    int req_order = buddy_size_to_order(req_size);
    if (req_order > MAX_ORDER) { terminal_printf("[Buddy] Error: Requested size %u exceeds MAX_ORDER %d.\n", size, MAX_ORDER); return NULL; }
    int order = req_order;
    while (order <= MAX_ORDER && free_lists[order] == NULL) { order++; }
    if (order > MAX_ORDER) { terminal_write("[Buddy] Out of memory\n"); return NULL; }
    buddy_block_t *block = free_lists[order]; free_lists[order] = block->next;
    while (order > req_order) {
        order--; size_t half_size = (size_t)1 << order;
        buddy_block_t *buddy = (buddy_block_t*)((uint8_t*)block + half_size);
        buddy->next = free_lists[order]; free_lists[order] = buddy;
    }
    g_buddy_free_bytes -= req_size; return (void*)block;
}

void buddy_free(void *ptr, size_t size) { /* ... same implementation as before ... */
     if (!ptr || size == 0) return;
    size_t req_size = buddy_round_up_to_power_of_two(size);
    int order = buddy_size_to_order(req_size);
     if (order > MAX_ORDER) { terminal_printf("[Buddy] Error: Freeing block invalid size %u (order %d > MAX_ORDER %d).\n", size, order, MAX_ORDER); return; }
    g_buddy_free_bytes += req_size; uintptr_t addr = (uintptr_t)ptr;
    while (order < MAX_ORDER) {
        uintptr_t buddy_addr = addr ^ ((uintptr_t)1 << order);
        buddy_block_t *prev = NULL; buddy_block_t *curr = free_lists[order]; bool merged = false;
        while (curr) {
            if ((uintptr_t)curr == buddy_addr) {
                if (prev) prev->next = curr->next; else free_lists[order] = curr->next;
                if (buddy_addr < addr) addr = buddy_addr;
                order++; merged = true; break;
            }
            prev = curr; curr = curr->next;
        }
        if (!merged) break;
    }
    buddy_block_t *block = (buddy_block_t*)addr; block->next = free_lists[order]; free_lists[order] = block;
}

size_t buddy_free_space(void) { /* ... same implementation as before ... */
    return g_buddy_free_bytes;
}