/**
 * pit.c
 * Programmable Interval Timer (PIT) driver for x86 (32-bit).
 * Implements IRQ0 handling and sleep functions (busy and interrupt-based).
 */

 #include "pit.h"
 #include "idt.h"
 #include "terminal.h"
 #include "port_io.h"
 #include <libc/stdint.h>
 #include <libc/stddef.h>
 
 // Global tick counter incremented on each PIT interrupt.
 static volatile uint32_t pit_ticks = 0;
 
 static void pit_irq_handler(void* data) {
     (void)data; // Unused.
     pit_ticks++;
 }
 
 uint32_t get_pit_ticks(void) {
     return pit_ticks;
 }
 
 static void set_pit_frequency(uint32_t freq) {
     if (freq == 0) {
         freq = 1; // Avoid division by zero.
     }
     uint32_t divisor = PIT_BASE_FREQUENCY / freq;
     // Configure channel 0: mode 3 (square wave), lobyte/hibyte.
     outb(PIT_CMD_PORT, 0x36);
     outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor & 0xFF));
     outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor >> 8) & 0xFF));
 }
 
 void init_pit(void) {
     register_int_handler(32, pit_irq_handler, NULL);
     set_pit_frequency(TARGET_FREQUENCY);
 }
 
 void sleep_busy(uint32_t milliseconds) {
     uint32_t start = get_pit_ticks();
     uint32_t ticks_to_wait = milliseconds * TICKS_PER_MS;
     while ((get_pit_ticks() - start) < ticks_to_wait) {
         // Busy-wait.
     }
 }
 
 void sleep_interrupt(uint32_t milliseconds) {
     uint32_t start = get_pit_ticks();
     uint32_t end = start + (milliseconds * TICKS_PER_MS);
     while (get_pit_ticks() < end) {
         // Enable interrupts and halt until the next interrupt.
         __asm__ volatile("sti\n\thlt\n\tcli");
     }
     __asm__ volatile("sti");
 }
 