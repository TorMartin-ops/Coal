/**
 * pit.c
 * Programmable Interval Timer (PIT) driver for x86 (32-bit).
 * This implementation integrates preemptive scheduling by calling schedule()
 * unconditionally on every timer tick (PIT IRQ). The scheduler itself
 * determines if it's ready to perform a context switch.
 */

 #include "pit.h"
 #include "idt.h"       // Needed for register_int_handler
 #include "terminal.h"
 #include "port_io.h"
 #include "scheduler.h" // Need schedule() declaration
 #include "types.h"     // Ensure bool is defined via types.h -> stdbool.h
 #include "assert.h"    // For KERNEL_ASSERT
 
 // Global state variables
 static volatile uint32_t pit_ticks = 0;
 
 /**
  * PIT IRQ handler:
  * Increments the tick counter and calls the scheduler unconditionally.
  */
 static void pit_irq_handler(registers_t *regs) {
      (void)regs; // Mark regs as unused
 
      // Increment tick counter safely
      pit_ticks++;
 
      // Log periodically to avoid console spam (optional)
      // if (pit_ticks % 1000 == 0) {
      //     terminal_printf("[PIT] Tick count: %lu\n", (unsigned long)pit_ticks);
      // }
 
      // Call scheduler unconditionally. The scheduler itself will check
      // if it's ready (g_scheduler_ready flag) before switching context.
      schedule();
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
      } else if (freq < 1) { // Also check minimum reasonable frequency
           terminal_printf("[PIT] Warning: Requested frequency %lu too low, using 1 Hz.\n",
                           (unsigned long)freq);
           freq = 1;
      }
 
      // Calculate divisor
      uint32_t divisor = PIT_BASE_FREQUENCY / freq;
      // Ensure divisor is within the 16-bit range (1 to 65535). 0 maps to 65536.
      if (divisor == 0) divisor = 0x10000; // Use max divisor if freq is too low (approx 18.2 Hz minimum)
      if (divisor > 0xFFFF) divisor = 0xFFFF; // Clamp to max 16-bit value if freq is slightly too low
      if (divisor < 1) divisor = 1;          // Clamp to min 1 if freq is too high
 
      terminal_printf("[PIT] Setting frequency to %lu Hz (Calculated Divisor: %lu)\n",
                      (unsigned long)freq, (unsigned long)divisor);
 
      // Program the PIT
      // Channel 0, lobyte/hibyte access mode, mode 3 (square wave generator)
      outb(PIT_CMD_PORT, 0x36);
 
      // Send the divisor (low byte, then high byte)
      outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor & 0xFF));
      outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor >> 8) & 0xFF));
 
      terminal_printf("[PIT] Frequency set (Target=%lu Hz, Divisor=%lu)\n",
                      (unsigned long)freq, (unsigned long)divisor);
 }
 
 
 /* Function pit_set_scheduler_ready() removed */
 /* Function pit_is_scheduler_ready() removed */
 
 /**
  * Initializes the PIT:
  * - Registers the IRQ handler for vector 32 (PIT IRQ).
  * - Sets the PIT to the TARGET_FREQUENCY.
  */
 void init_pit(void) {
      // Initialize state
      pit_ticks = 0;
 
      // Register IRQ handler (IRQ0 = vector 32)
      // Use the literal vector number 32 for PIT IRQ0
      register_int_handler(32, pit_irq_handler, NULL); // <<< FIXED: Use 32 instead of IRQ0
 
      // Set frequency
      set_pit_frequency(TARGET_FREQUENCY);
 
      terminal_printf("[PIT] Initialized (Target Frequency: %lu Hz)\n", (unsigned long)TARGET_FREQUENCY);
 }
 
 /**
  * Busy-wait sleep (high CPU usage).
  *
  * @param milliseconds Duration to sleep
  */
 void sleep_busy(uint32_t milliseconds) {
      // Basic check for TICKS_PER_MS validity
      KERNEL_ASSERT(TICKS_PER_MS > 0 || TARGET_FREQUENCY < 1000, "TICKS_PER_MS calculation error");
 
      // Calculate ticks to wait
      uint32_t ticks_to_wait = 0;
      if (TARGET_FREQUENCY >= 1000) {
          ticks_to_wait = milliseconds * (TARGET_FREQUENCY / 1000); // Use defined TICKS_PER_MS implicitly
      } else { // Handle frequencies < 1000 Hz
           // Calculate carefully to avoid integer division truncation
           ticks_to_wait = ((uint64_t)milliseconds * TARGET_FREQUENCY) / 1000;
      }
 
 
      if (ticks_to_wait == 0 && milliseconds > 0) {
          ticks_to_wait = 1; // Ensure at least one tick for small durations
      }
 
      // Record start time
      uint32_t start = get_pit_ticks();
 
      // Wait until enough ticks have passed
      // Handle potential wrap-around of pit_ticks (though unlikely for short sleeps)
      uint32_t target_ticks = start + ticks_to_wait;
      bool wrapped = target_ticks < start; // Check if target tick count wrapped around
 
      while (true) {
         uint32_t current = get_pit_ticks();
         if (wrapped) {
             // If wrapped, wait until current ticks also wrap OR exceed target
             if (current < start && current >= target_ticks) break;
         } else {
             // Normal case: wait until current ticks reach or exceed target
             if (current >= target_ticks) break;
         }
         // Busy-wait
         asm volatile("pause"); // Hint to CPU that we're spinning
      }
 }
 
 /**
  * Sleep using interrupts (low CPU usage).
  * Caution: Basic implementation, assumes single CPU and simple interrupt model.
  *
  * @param milliseconds Duration to sleep
  */
 void sleep_interrupt(uint32_t milliseconds) {
      // Save current interrupt state
      uint32_t eflags;
      asm volatile("pushf; pop %0" : "=r"(eflags));
      // Use the literal mask 0x200 for the interrupt flag
      bool interrupts_were_enabled = (eflags & 0x200) != 0; // <<< FIXED: Use 0x200
 
      // Basic check for TICKS_PER_MS validity
      KERNEL_ASSERT(TICKS_PER_MS > 0 || TARGET_FREQUENCY < 1000, "TICKS_PER_MS calculation error");
 
      // Calculate ticks to wait
      uint32_t ticks_to_wait = 0;
      if (TARGET_FREQUENCY >= 1000) {
           ticks_to_wait = milliseconds * (TARGET_FREQUENCY / 1000); // Use defined TICKS_PER_MS implicitly
      } else { // Handle frequencies < 1000 Hz
           // Calculate carefully to avoid integer division truncation
           ticks_to_wait = ((uint64_t)milliseconds * TARGET_FREQUENCY) / 1000;
      }
 
 
      if (ticks_to_wait == 0 && milliseconds > 0) {
          ticks_to_wait = 1; // Ensure at least one tick for small durations
      }
 
      // Calculate end time
      uint32_t start = get_pit_ticks();
      uint32_t end_ticks = start + ticks_to_wait;
      bool wrapped = end_ticks < start; // Check for wrap-around
 
      // Loop until the target tick count is reached
      while (true) {
         uint32_t current = get_pit_ticks();
         bool condition_met = false;
 
         if (wrapped) {
             // If wrapped, condition is met if current ticks also wrapped AND passed the end_ticks
             condition_met = (current < start && current >= end_ticks);
         } else {
             // Normal case: condition met if current ticks reach or exceed end_ticks
             condition_met = (current >= end_ticks);
         }
 
         if (condition_met) {
             break; // Exit loop
         }
 
         // Enable interrupts and halt until next interrupt
         // Note: This assumes HLT doesn't interfere with other critical sections
         // and that the PIT interrupt is the primary wakeup source.
         asm volatile("sti");
         asm volatile("hlt");
         // Interrupts are disabled by CPU on entry to handler.
         // They might be re-enabled by IRET, so CLI after HLT might be needed
         // if subsequent code relies on interrupts being off, but usually
         // the loop condition check is sufficient.
      }
 
      // Restore original interrupt state carefully
      if (interrupts_were_enabled) {
          asm volatile("sti");
      } else {
          asm volatile("cli");
      }
 }