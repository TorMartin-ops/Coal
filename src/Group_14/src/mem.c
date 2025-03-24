/**
 * mem.c
 *
 * A “world-class” minimal memory manager for an OS dev assignment (32-bit x86).
 *
 * Features:
 *   - Naive bump allocator: we track a "current_ptr" that moves forward on each malloc.
 *   - free() is effectively a no-op (cannot reclaim individual blocks).
 *   - init_kernel_memory(&end) sets the start of the heap after the kernel image.
 *   - Optional alignment and optional max heap limit if desired.
 *
 * Enough to satisfy assignment tasks for memory usage (malloc, free).
 */

 #include "mem.h"
 #include "terminal.h"  // for debug prints if needed
 #include <libc/stdint.h>
 #include <libc/stddef.h>
 
 // The start of the kernel heap
 static uint8_t* heap_start  = 0;
 
 // The next free address (bump pointer)
 static uint8_t* current_ptr = 0;
 
 // Optional: a maximum heap boundary. If zero, no enforced limit.
 static uint8_t* heap_end    = 0;
 
 /**
  * init_kernel_memory
  *
  * Called once at kernel startup, passing the address of the 'end' symbol
  * from the linker script. That marks where the kernel ends, so we can
  * start allocating memory after that point.
  *
  * @param kernel_end The address of 'end' from linker.ld (uint32_t*).
  */
 void init_kernel_memory(uint32_t* kernel_end)
 {
     // Convert kernel_end to a byte pointer
     heap_start  = (uint8_t*)kernel_end;
     current_ptr = heap_start;
 
     // If you want a fixed limit, e.g. 2MB beyond the kernel:
     // heap_end = heap_start + 0x200000;  // 2 MB
 
     // Optionally, print debug info:
     // terminal_write("Kernel memory manager (bump allocator) initialized.\n");
 }
 
 /**
  * malloc
  *
  * A naive bump allocator. Each call:
  *   - returns the current_ptr,
  *   - advances current_ptr by 'size'.
  * No real free-lists or coalescing, so free() is effectively a no-op.
  *
  * @param size The number of bytes to allocate
  * @return Pointer to the allocated block, or NULL if uninitialized or out of memory
  */
 void* malloc(size_t size)
 {
     // If not initialized, return NULL
     if (!current_ptr) {
         return NULL;
     }
 
     // Optional alignment: e.g. 4-byte align
     // size = (size + 3) & ~3;  // uncomment if you want alignment
 
     // Grab the current pointer
     uint8_t* allocated = current_ptr;
 
     // Advance by 'size'
     current_ptr += size;
 
     // If we have a limit, check if we've exceeded it
     if (heap_end && current_ptr > heap_end) {
         // Out of memory
         // Optionally revert current_ptr or do something else
         // For now, return NULL to indicate failure
         return NULL;
     }
 
     // Optionally, zero-initialize the block:
     // for (size_t i = 0; i < size; i++) {
     //     allocated[i] = 0;
     // }
 
     return (void*)allocated;
 }
 
 /**
  * free
  *
  * A no-op in this naive bump allocator. We can't reclaim individual blocks.
  * For a real OS, you'd implement a free list or buddy system.
  *
  * @param ptr The pointer to free (ignored).
  */
 void free(void* ptr)
 {
     (void)ptr;
     // No action taken
     // In a naive approach, you can only reset the entire heap if you want
 }
 