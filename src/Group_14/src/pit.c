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
 #include "types.h"  // Ensure bool is defined via types.h
 
 // Global state variables
 static volatile uint32_t pit_ticks = 0;
 static volatile bool scheduler_ready = false;
 
 /**
  * PIT IRQ handler:
  * Increments the tick counter and calls the scheduler *if ready*.
  */
 static void pit_irq_handler(registers_t *regs) {
     (void)regs; // Mark regs as unused
 
     // Increment tick counter safely
     pit_ticks++;
     
     // Log periodically to avoid console spam
     if (pit_ticks % 1000 == 0) {
         terminal_printf("[PIT] Tick count: %lu\n", (unsigned long)pit_ticks);
     }
     
     // Call scheduler if it has been marked as ready
     if (scheduler_ready) {
         // Log first few calls for debugging
         if (pit_ticks < 10) {
             terminal_printf("[PIT] IRQ: Calling schedule() at tick #%lu\n", 
                            (unsigned long)pit_ticks);
         }
         
         // Preempt current task by scheduling the next one
         schedule();
     }
 }
 
 /**
  * Returns the current tick count since PIT initialization.
  */
 uint32_t get_pit_ticks(void) {
     return pit_ticks;
 }
 
 /**
  * Configures the PIT frequency.
  * 
  * @param freq Desired frequency in Hz
  */
 static void set_pit_frequency(uint32_t freq) {
     // Validate frequency
     if (freq == 0) {
         terminal_printf("[PIT] Warning: Invalid frequency value 0, using 1 Hz instead.\n");
         freq = 1; // Avoid division by zero
     }
     
     // Sanity check - frequencies should be within reasonable limits
     if (freq > PIT_BASE_FREQUENCY) {
         terminal_printf("[PIT] Warning: Requested frequency %lu exceeds PIT max, clamping.\n", 
                        (unsigned long)freq);
         freq = PIT_BASE_FREQUENCY;
     }
     
     // Calculate divisor
     uint32_t divisor = PIT_BASE_FREQUENCY / freq;
     if (divisor == 0) divisor = 1;  // Ensure non-zero divisor
     
     // Program the PIT
     // Channel 0, lobyte/hibyte, mode 3 (square wave)
     outb(PIT_CMD_PORT, 0x36);
     
     // Send the divisor
     outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor & 0xFF));         // Low byte
     outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor >> 8) & 0xFF));  // High byte
     
     terminal_printf("[PIT] Frequency set to %lu Hz (divisor: %lu)\n", 
                    (unsigned long)freq, (unsigned long)divisor);
 }
 
 /**
  * Marks the scheduler as ready to be called by the PIT handler.
  * Should be called after scheduler init and first task add, before sti.
  */
 void pit_set_scheduler_ready(void) {
     // Check if scheduler subsystem is properly initialized
     if (!scheduler_is_ready()) {
         terminal_printf("[PIT] Warning: Attempted to mark scheduler ready, but scheduler_is_ready() returns false.\n");
         terminal_printf("[PIT] Will continue, but scheduling might not work properly.\n");
     }
     
     // Set flag (atomically in a single-threaded environment)
     scheduler_ready = true;
     terminal_write("[PIT] Scheduler marked as ready for PIT handler.\n");
 }
 
 /**
  * Returns whether the scheduler has been marked as ready for PIT callbacks.
  */
 bool pit_is_scheduler_ready(void) {
     return scheduler_ready;
 }
 
 /**
  * Initializes the PIT:
  * - Registers the IRQ handler for vector 32 (PIT IRQ).
  * - Sets the PIT to the TARGET_FREQUENCY.
  */
 void init_pit(void) {
     // Initialize state
     pit_ticks = 0;
     scheduler_ready = false;
     
     // Register IRQ handler (IRQ0 = vector 32)
     register_int_handler(32, pit_irq_handler, NULL);
     
     // Set frequency
     set_pit_frequency(TARGET_FREQUENCY);
     
     terminal_printf("[PIT] Initialized with frequency %lu Hz\n", (unsigned long)TARGET_FREQUENCY);
 }
 
 /**
  * Busy-wait sleep (high CPU usage).
  * 
  * @param milliseconds Duration to sleep
  */
 void sleep_busy(uint32_t milliseconds) {
     // Calculate ticks to wait
     uint32_t ticks_to_wait = milliseconds * TICKS_PER_MS;
     if (ticks_to_wait == 0 && milliseconds > 0) {
         ticks_to_wait = 1; // Ensure at least one tick for small durations
     }
     
     // Record start time
     uint32_t start = get_pit_ticks();
     
     // Wait until enough ticks have passed
     while ((get_pit_ticks() - start) < ticks_to_wait) {
         // Busy-wait
         asm volatile("pause"); // Hint to CPU that we're spinning
     }
 }
 
 /**
  * Sleep using interrupts (low CPU usage).
  * 
  * @param milliseconds Duration to sleep
  */
 void sleep_interrupt(uint32_t milliseconds) {
     // Save current interrupt state
     uint32_t eflags;
     asm volatile("pushf; pop %0" : "=r"(eflags));
     bool interrupts_were_enabled = (eflags & 0x200) != 0;
     
     // Calculate ticks to wait
     uint32_t ticks_to_wait = milliseconds * TICKS_PER_MS;
     if (ticks_to_wait == 0 && milliseconds > 0) {
         ticks_to_wait = 1; // Ensure at least one tick for small durations
     }
     
     // Calculate end time
     uint32_t start = get_pit_ticks();
     uint32_t end = start + ticks_to_wait;
     
     // Handle potential timer wrap-around (unlikely with 32-bit counter at 1000Hz)
     while (get_pit_ticks() < end) {
         // Enable interrupts and halt until next interrupt
         asm volatile("sti");
         asm volatile("hlt");
     }
     
     // Restore original interrupt state
     if (!interrupts_were_enabled) {
         asm volatile("cli");
     } else {
         asm volatile("sti");
     }
 }