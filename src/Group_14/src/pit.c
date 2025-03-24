/**
 * pit.c
 *
 * A robust Programmable Interval Timer (PIT) driver for IRQ0 on x86 (32-bit).
 * It demonstrates:
 *   1) Registering an IRQ0 handler to count ticks.
 *   2) Optionally reprogramming the PIT frequency (set_pit_frequency).
 *   3) Printing a debug message every 100 ticks by default.
 */

 #include <stddef.h>      // For NULL
 #include "pit.h"
 #include "idt.h"         // for register_int_handler
 #include "terminal.h"    // for terminal_write
 #include "port_io.h"     // for outb, inb
 
 // The PIT’s input clock frequency is ~1,193,182 Hz.
 #define PIT_BASE_FREQUENCY 1193182
 
 // I/O ports for the PIT channel 0 and command register
 #define PIT_CHANNEL0  0x40
 #define PIT_COMMAND   0x43
 
 // A global tick counter for demonstration
 static volatile unsigned int pit_ticks = 0;
 
 /**
  * pit_irq_handler
  *
  * This function is called by int_handler() every time IRQ0 (vector 32) fires.
  * In other words, each PIT tick triggers this callback.
  */
 static void pit_irq_handler(void *data)
 {
     (void)data;  // unused in this example
 
     pit_ticks++;
 
    
 }
 
 /**
  * set_pit_frequency
  *
  * Reprograms the PIT to fire interrupts at the given 'freq' in Hz.
  * 
  * For example, set_pit_frequency(100) => 100 Hz => ~10ms per tick.
  *
  * The PIT has a base input clock of ~1.193182 MHz. We compute a divisor such that:
  *    divisor = PIT_BASE_FREQUENCY / freq
  * 
  * Then we write this divisor to channel 0 (0x40) in low byte/high byte order,
  * along with a command byte (0x36) that configures the PIT in mode 3 (square wave).
  */
 void set_pit_frequency(unsigned int freq)
 {
     if (freq == 0) {
         // Avoid dividing by zero; do nothing or set a default freq
         freq = 1;
     }
 
     // Compute the divisor
     unsigned int divisor = PIT_BASE_FREQUENCY / freq;
 
     // Command byte: channel 0, access mode lobyte+hibyte, mode 3, binary
     outb(PIT_COMMAND, 0x36);
 
     // Write low byte, then high byte of the divisor
     outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
     outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
 }
 
 /**
  * init_timer
  *
  * Installs our PIT handler on IRQ0 (vector 32).
  * Optionally reprogram the PIT frequency if desired. By default, the PIT might
  * be ~18.2 Hz if you don't call set_pit_frequency, or ~100 Hz if you do.
  */
 void init_timer(void)
 {
     // 32 = IRQ0 after PIC remap
     register_int_handler(32, pit_irq_handler, NULL);
 
     // (Optional) reprogram PIT to 100 Hz. You can change this to another value.
     set_pit_frequency(100);
 }
 