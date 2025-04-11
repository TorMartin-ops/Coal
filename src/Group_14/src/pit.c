/**
 * pit.c
 * Programmable Interval Timer (PIT) driver for x86 (32-bit).
 * This implementation integrates preemptive scheduling by calling schedule()
 * on every timer tick (PIT IRQ), but only after the scheduler is marked as ready.
 */

 #include "pit.h"
 #include "idt.h"
 #include "terminal.h"
 #include "port_io.h"
 #include "scheduler.h"
 #include "types.h" // Ensure bool is defined via types.h -> stdbool.h
 
 static volatile uint32_t pit_ticks = 0;
 static volatile bool scheduler_ready = false; // <<< ADDED FLAG
 
 /* PIT IRQ handler:
    Increments the tick counter and calls the scheduler *if ready*. */
    static void pit_irq_handler(registers_t *regs) {
        (void)regs; // Mark regs as unused if needed, or remove if truly unused
        pit_ticks++;
        if (scheduler_ready) { // ADDED CHECK
            schedule(); // Preempt current task by scheduling the next one
        }
    }
 
 /* Returns the current tick count since PIT initialization. */
 uint32_t get_pit_ticks(void) {
     return pit_ticks;
 }
 
 /* Configures the PIT frequency. */
 static void set_pit_frequency(uint32_t freq) {
     if (freq == 0) {
         freq = 1; // Avoid division by zero.
     }
     uint32_t divisor = PIT_BASE_FREQUENCY / freq;
     outb(PIT_CMD_PORT, 0x36); // Channel 0, lobyte/hibyte, mode 3 (square wave)
     outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor & 0xFF));
     outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor >> 8) & 0xFF));
 }
 
 /**
  * @brief Marks the scheduler as ready to be called by the PIT handler.
  * Should be called after scheduler init and first task add, before sti.
 */
 void pit_set_scheduler_ready(void) { // <<< ADDED FUNCTION
     scheduler_ready = true;
     terminal_write("[PIT] Scheduler marked as ready for PIT handler.\n");
 }
 
 /* Initializes the PIT:
    - Registers the IRQ handler for vector 32 (PIT IRQ).
    - Sets the PIT to the TARGET_FREQUENCY. */
 void init_pit(void) {
     scheduler_ready = false; // Ensure it starts as false
     register_int_handler(32, pit_irq_handler, NULL);
     set_pit_frequency(TARGET_FREQUENCY);
 }
 
 /* Busy-wait sleep (high CPU usage). */
 void sleep_busy(uint32_t milliseconds) {
     uint32_t start = get_pit_ticks();
     // Calculate ticks based on TARGET_FREQUENCY (e.g., 1000 Hz -> 1 tick per ms)
     // Ensure TICKS_PER_MS calculation is correct if TARGET_FREQUENCY changes
     uint32_t ticks_to_wait = milliseconds * (TARGET_FREQUENCY / 1000);
     if (ticks_to_wait == 0 && milliseconds > 0) {
         ticks_to_wait = 1; // Ensure at least one tick for small durations
     }
     while ((get_pit_ticks() - start) < ticks_to_wait) {
         // Busy-wait - consider adding pause instruction? asm volatile("pause");
     }
 }
 
 /* Sleep using interrupts (low CPU usage). */
 void sleep_interrupt(uint32_t milliseconds) {
     uint32_t start = get_pit_ticks();
     uint32_t ticks_to_wait = milliseconds * (TARGET_FREQUENCY / 1000);
      if (ticks_to_wait == 0 && milliseconds > 0) {
         ticks_to_wait = 1;
     }
     uint32_t end = start + ticks_to_wait;
 
     // Handle potential timer wrap-around (though unlikely with 32-bit ticks at 1000Hz for a while)
     // A more robust implementation might use 64-bit ticks or handle wrap-around explicitly.
     while (get_pit_ticks() < end) {
         // Temporarily enable interrupts and halt
         // Ensure interrupts are disabled again before checking condition if needed,
         // though PIT handler runs with interrupts disabled anyway usually.
         asm volatile("sti");
         asm volatile("hlt");
         asm volatile("cli"); // Disable interrupts immediately after hlt returns
     }
      asm volatile("sti"); // Re-enable interrupts after sleep duration
 }