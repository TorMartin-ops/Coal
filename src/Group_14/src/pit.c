/**
 * pit.c
 * Programmable Interval Timer (PIT) driver for x86 (32-bit).
 * This implementation integrates preemptive scheduling by calling schedule()
 * on every timer tick (PIT IRQ).
 */

 #include "pit.h"
 #include "idt.h"
 #include "terminal.h"
 #include "port_io.h"
 #include "scheduler.h"  
 
#include "types.h"
 
 static volatile uint32_t pit_ticks = 0;
 
 /* PIT IRQ handler:
    Increments the tick counter and calls the scheduler to preempt the current task.
    Note: In production, you may need to add safeguards (e.g., not switching in critical sections). */
 static void pit_irq_handler(void* data) {
     (void)data; // Unused parameter
 
     pit_ticks++;
     schedule();  // Preempt current task by scheduling the next one
 }
 
 /* Returns the current tick count since PIT initialization. */
 uint32_t get_pit_ticks(void) {
     return pit_ticks;
 }
 
 /* Configures the PIT frequency.
    freq: desired frequency in Hz (e.g., 1000 Hz for 1ms per tick). */
 static void set_pit_frequency(uint32_t freq) {
     if (freq == 0) {
         freq = 1; // Avoid division by zero.
     }
     uint32_t divisor = PIT_BASE_FREQUENCY / freq;
     // Configure channel 0: mode 3 (square wave), using lobyte/hibyte access.
     outb(PIT_CMD_PORT, 0x36);
     outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor & 0xFF));
     outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor >> 8) & 0xFF));
 }
 
 /* Initializes the PIT:
    - Registers the IRQ handler for vector 32 (PIT IRQ).
    - Sets the PIT to the TARGET_FREQUENCY. */
 void init_pit(void) {
     register_int_handler(32, pit_irq_handler, NULL);
     set_pit_frequency(TARGET_FREQUENCY);
 }
 
 /* Busy-wait sleep (high CPU usage). */
 void sleep_busy(uint32_t milliseconds) {
     uint32_t start = get_pit_ticks();
     uint32_t ticks_to_wait = milliseconds * TICKS_PER_MS;
     while ((get_pit_ticks() - start) < ticks_to_wait) {
         // Busy-wait.
     }
 }
 
 /* Sleep using interrupts (low CPU usage).
    Enables interrupts and halts until the next PIT tick. */
 void sleep_interrupt(uint32_t milliseconds) {
     uint32_t start = get_pit_ticks();
     uint32_t end = start + (milliseconds * TICKS_PER_MS);
     while (get_pit_ticks() < end) {
         __asm__ volatile("sti\n\thlt\n\tcli");
     }
     __asm__ volatile("sti");
 }
 