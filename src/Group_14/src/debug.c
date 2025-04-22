#include <terminal.h> // Or your printing header
#include <debug.h>   // For DEBUG_PRINTK

// Function to be called immediately after CR3 switch in assembly
void after_cr3_test(void) {
    terminal_write("\n*** KERNEL: Successfully executed code AFTER loading process CR3! ***\n");
    // Halt indefinitely to show we got here
    while(1) { asm volatile("cli; hlt"); }
}