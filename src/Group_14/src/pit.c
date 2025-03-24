/**
 * pit.c
 *
 * A “world-class” Programmable Interval Timer (PIT) driver for x86 (32-bit).
 *
 * Features:
 *   - Installs an IRQ0 handler (vector 32) that increments a global tick counter.
 *   - Sets default frequency to TARGET_FREQUENCY (e.g., 1000 Hz => 1ms per tick).
 *   - Provides sleep_interrupt(ms) using sti+hlt for low-CPU usage.
 *   - Provides sleep_busy(ms) with a high-CPU busy-wait loop.
 */

 #include "pit.h"
 #include "idt.h"        // for register_int_handler(32, ...)
 #include "terminal.h"   // for terminal_write (optional debug)
 #include "port_io.h"    // for outb, inb
 #include <libc/stdint.h>
 #include <libc/stddef.h>
 
 // A global tick counter, incremented each time PIT triggers IRQ0
 static volatile uint32_t pit_ticks = 0;
 
 /**
  * pit_irq_handler
  *
  * Called by int_handler(32) whenever IRQ0 (the PIT) fires.
  * We increment the pit_ticks counter each tick.
  */
 static void pit_irq_handler(void* data)
 {
     (void)data; // unused
     pit_ticks++;
 
     // Optional debug: print every 100 ticks, etc.
     // if (pit_ticks % 100 == 0) {
     //     terminal_write("[PIT] 100 ticks!\n");
     // }
 }
 
 /**
  * get_pit_ticks
  *
  * Returns how many PIT ticks have elapsed since init_pit() was called.
  * If you set the frequency to 1000 Hz, each tick = 1 ms.
  */
 uint32_t get_pit_ticks(void)
 {
     return pit_ticks;
 }
 
 /**
  * set_pit_frequency
  *
  * Reprogram the PIT to 'freq' Hz.
  * If freq=1000 => 1000 Hz => 1 ms per tick.
  * 
  * The PIT base frequency is ~1,193,180 Hz. The divisor = base/freq.
  */
 static void set_pit_frequency(uint32_t freq)
 {
     if (freq == 0) {
         freq = 1; // avoid dividing by zero
     }
 
     // Calculate the divisor
     uint32_t divisor = PIT_BASE_FREQUENCY / freq;
 
     // Command byte: channel 0, access lobyte+hibyte, mode 3 (square wave), binary
     outb(PIT_CMD_PORT, 0x36);
 
     // Send low byte, then high byte of the divisor
     outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor & 0xFF));
     outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor >> 8) & 0xFF));
 }
 
 /**
  * init_pit
  *
  * Installs the PIT handler on IRQ0 (vector 32) via register_int_handler,
  * then sets the PIT frequency to TARGET_FREQUENCY (e.g. 1000 Hz).
  * This ensures each tick is 1 ms if you use the default macros.
  */
 void init_pit(void)
 {
     // Register our C-level handler for IRQ0
     register_int_handler(32, pit_irq_handler, NULL);
 
     // Set the PIT to our chosen frequency (default 1000 Hz => 1ms/tick)
     set_pit_frequency(TARGET_FREQUENCY);
 
     // Optionally reset pit_ticks to 0:
     // pit_ticks = 0;
 
     // terminal_write("PIT initialized at 1000 Hz.\n");
 }
 
 /**
  * sleep_busy
  *
  * A high-CPU usage sleep. We simply poll pit_ticks in a loop
  * until the desired time has elapsed.
  *
  * TICKS_PER_MS is typically 1 if freq=1000, so 1 tick = 1 ms.
  */
 void sleep_busy(uint32_t milliseconds)
 {
     uint32_t start = get_pit_ticks();
     uint32_t ticks_to_wait = milliseconds * TICKS_PER_MS;
     while ((get_pit_ticks() - start) < ticks_to_wait) {
         // busy-wait
     }
 }
 
 /**
  * sleep_interrupt
  *
  * A low-CPU usage sleep. We enable interrupts and hlt in a loop,
  * so the CPU sleeps until the next PIT interrupt. This significantly
  * reduces CPU usage compared to busy-waiting.
  */
 void sleep_interrupt(uint32_t milliseconds)
 {
     uint32_t start = get_pit_ticks();
     uint32_t end   = start + (milliseconds * TICKS_PER_MS);
 
     while (get_pit_ticks() < end) {
         // Enable interrupts, then halt until next interrupt
         __asm__ volatile("sti\n\thlt\n\tcli");
     }
     // Re-enable interrupts at the end
     __asm__ volatile("sti");
 }
 