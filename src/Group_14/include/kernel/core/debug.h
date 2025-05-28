// include/debug.h
#ifndef DEBUG_H
#define DEBUG_H
#pragma once

#include <kernel/drivers/display/serial.h> // Use serial for debug output

// Define DEBUG_SYSCALLS (or other flags) globally via CFLAGS or here to enable
// #define DEBUG_SYSCALLS
// #define DEBUG_KMALLOC
// #define DEBUG_VFS
// ... etc.

#ifdef DEBUG_SYSCALLS
    #define DEBUG_PRINTK_SYSCALL(...) serial_printf(__VA_ARGS__)
#else
    #define DEBUG_PRINTK_SYSCALL(...) ((void)0) // Compiles to nothing
#endif

// Example for another debug category
#ifdef DEBUG_KMALLOC
    #define DEBUG_PRINTK_KMALLOC(...) serial_printf(__VA_ARGS__)
#else
    #define DEBUG_PRINTK_KMALLOC(...) ((void)0)
#endif

// Generic Debug Printk (you might want a more specific one like above)
// Or use a general DEBUG flag
#ifdef DEBUG
    #define DEBUG_PRINTK(...) serial_printf(__VA_ARGS__)
#else
    // Default DEBUG_PRINTK to be disabled unless a specific category is enabled
    // Or tie it to a general DEBUG flag if preferred.
    // Using the SYSCALL definition for now as that's what syscall.c uses.
    #define DEBUG_PRINTK(...) DEBUG_PRINTK_SYSCALL(__VA_ARGS__)
#endif


#endif // DEBUG_H