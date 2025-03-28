/**
 * mem.c
 * Minimal memory manager using a naive bump allocator.
 */

 #include "mem.h"
 #include "terminal.h"  // Optional debug prints
 #include <libc/stdint.h>
 #include <libc/stddef.h>
 
 // Heap start and bump pointer
 static uint8_t* heap_start  = 0;
 static uint8_t* current_ptr = 0;
 
 // Optional: maximum heap boundary (0 = no limit)
 static uint8_t* heap_end    = 0;
 
 /**
  * init_kernel_memory
  * Initializes the heap using the address of 'end' from the linker.
  */
 void init_kernel_memory(uint32_t* kernel_end)
 {
     heap_start  = (uint8_t*)kernel_end;
     current_ptr = heap_start;
     // Optionally set a heap limit (e.g., 2 MB beyond the kernel)
     // heap_end = heap_start + 0x200000;
 }
 
 /**
  * malloc
  * Allocates 'size' bytes by returning the current pointer and advancing it.
  * Returns NULL if not initialized or out of memory.
  */
 void* malloc(size_t size)
 {
     if (!current_ptr)
         return NULL;
     
     uint8_t* allocated = current_ptr;
     current_ptr += size;
     
     if (heap_end && current_ptr > heap_end)
         return NULL;
     
     return (void*)allocated;
 }
 
 /**
  * free
  * No-op for this bump allocator.
  */
 void free(void* ptr)
 {
     (void)ptr;
 }
 