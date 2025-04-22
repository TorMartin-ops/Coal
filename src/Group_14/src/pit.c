// src/pit.c (Corrected - Removed extraneous text)

/**
 * pit.c
 * Programmable Interval Timer (PIT) driver for x86 (32-bit).
 * Updated to use isr_frame_t and aggressive workaround for 64-bit division.
 */

 #include "pit.h"
 #include "idt.h"       // Needed for register_int_handler
 #include <isr_frame.h>  // Include the frame definition
 #include "terminal.h"
 #include "port_io.h"
 #include "scheduler.h" // Need scheduler_tick() or schedule() declaration
 #include "types.h"     // Ensure bool is defined via types.h -> stdbool.h
 #include "assert.h"    // For KERNEL_ASSERT
 #include <libc/stdint.h> // For UINT32_MAX

 // Global state variables
 // static volatile uint32_t pit_ticks = 0; // Can be removed if using scheduler_get_ticks()

 // Define TARGET_FREQUENCY if not defined elsewhere (e.g., in pit.h or build system)
 #ifndef TARGET_FREQUENCY
 #define TARGET_FREQUENCY 1000 // Default to 1000 Hz if not defined
 #endif

 // --- Revised Workaround Helper ---
 // Calculates approximately (ms * freq_hz) / 1000 using only 32-bit math.
 // May lose precision compared to 64-bit calculation.
 static inline uint32_t calculate_ticks_32bit(uint32_t ms, uint32_t freq_hz) {
     // ... (implementation remains the same) ...
     if (ms == 0) return 0;
     if (freq_hz == 0) return 0; // Avoid division by zero later

     // Simplest case: Freq is multiple of 1000
     if ((freq_hz % 1000) == 0) {
         uint32_t ticks_per_ms = freq_hz / 1000;
         // Check for potential overflow before multiplying
         if (ticks_per_ms > 0 && ms > UINT32_MAX / ticks_per_ms) {
             return UINT32_MAX; // Return max ticks on overflow
         }
         return ms * ticks_per_ms;
     }

     // Try calculating (ms / 1000) * freq_hz first (loses precision if ms < 1000)
     // This avoids large intermediate values.
     uint32_t ms_div_1000 = ms / 1000;
     uint32_t ticks_from_whole_seconds = 0;
     if (ms_div_1000 > 0) {
         // Check for overflow
         if (freq_hz > 0 && ms_div_1000 > UINT32_MAX / freq_hz) {
              return UINT32_MAX;
         }
         ticks_from_whole_seconds = ms_div_1000 * freq_hz;
     }

     // Now handle the remaining milliseconds (ms % 1000)
     uint32_t remaining_ms = ms % 1000;
     uint32_t ticks_from_remaining_ms = 0;
     if (remaining_ms > 0) {
         // Calculate (remaining_ms * freq_hz) / 1000
         // Check for overflow before intermediate multiplication
         if (freq_hz > 0 && remaining_ms > UINT32_MAX / freq_hz) {
             // Intermediate would overflow 32 bits, hard to calculate precisely without 64 bits.
             // Return max as a safe fallback.
             return UINT32_MAX;
         }
         uint32_t intermediate_product = remaining_ms * freq_hz;
         ticks_from_remaining_ms = intermediate_product / 1000;

         // Add rounding based on the remainder of the division
         if ((intermediate_product % 1000) >= 500) {
             ticks_from_remaining_ms++;
         }
     }

     // Combine the two parts, checking for overflow
     if (ticks_from_remaining_ms > UINT32_MAX - ticks_from_whole_seconds) {
         return UINT32_MAX;
     }
     uint32_t total_ticks = ticks_from_whole_seconds + ticks_from_remaining_ms;

     // Ensure at least 1 tick for small non-zero ms
     if (total_ticks == 0 && ms > 0) {
         total_ticks = 1;
     }

     return total_ticks;

 }


 /**
  * PIT IRQ handler:
  * Calls the scheduler's tick function.
  */
 static void pit_irq_handler(isr_frame_t *frame) {
     (void)frame;
     // --- Call scheduler tick handler ---
     scheduler_tick(); // <<< Ensure this matches your advanced scheduler function name
 }

 /**
  * Returns the current tick count since PIT initialization.
  * (Delegates to scheduler if scheduler manages the global tick)
  */
 uint32_t get_pit_ticks(void) {
     // Assumes scheduler_get_ticks() provides the canonical tick count
     return scheduler_get_ticks();
 }

 /**
  * Configures the PIT frequency.
  * (No changes needed in this function body)
  */
 static void set_pit_frequency(uint32_t freq) {
      if (freq == 0) { freq = 1; }
      if (freq > PIT_BASE_FREQUENCY) { freq = PIT_BASE_FREQUENCY; }

      uint32_t divisor = PIT_BASE_FREQUENCY / freq;
      if (divisor == 0) divisor = 0x10000; // Use max divisor if freq is too low
      if (divisor > 0xFFFF) divisor = 0xFFFF; // Clamp to max 16-bit value
      if (divisor < 1) divisor = 1;          // Minimum divisor is 1

      outb(PIT_CMD_PORT, 0x36); // Channel 0, Lobyt/Hibyte, Mode 3 (Square Wave)
      io_wait(); // Short delay after command write
      uint16_t divisor_16 = (uint16_t)divisor;
      outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor_16 & 0xFF)); // Send low byte
      io_wait(); // Short delay between byte writes
      outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor_16 >> 8) & 0xFF)); // Send high byte
      io_wait(); // Short delay after data write
 }


 /**
  * Initializes the PIT:
  * Registers the IRQ handler and sets the desired frequency.
  */
 void init_pit(void) {
     // pit_ticks = 0; // Local tick count likely not needed if scheduler manages it
     register_int_handler(32, pit_irq_handler, NULL); // Register IRQ 0 (vector 32)
     set_pit_frequency(TARGET_FREQUENCY);
     terminal_printf("[PIT] Initialized (Target Frequency: %lu Hz)\n", (unsigned long)TARGET_FREQUENCY);
 }

 /**
  * Busy-wait sleep (high CPU usage).
  * Uses the 32-bit workaround calculation.
  */
 void sleep_busy(uint32_t milliseconds) {
     uint32_t ticks_to_wait = calculate_ticks_32bit(milliseconds, TARGET_FREQUENCY);
     uint32_t start = get_pit_ticks(); // Use the canonical tick getter
     if (ticks_to_wait == UINT32_MAX) return; // Avoid infinite loop if calculation capped

     uint32_t target_ticks = start + ticks_to_wait;
     bool wrapped = target_ticks < start; // Check for counter wrap-around

     while (true) {
         uint32_t current = get_pit_ticks();
         bool condition_met;
         if (wrapped) {
             // Handle wrap-around: wait until current tick passes start AND reaches target
             condition_met = (current < start && current >= target_ticks);
         } else {
             // Normal case: wait until current tick reaches or exceeds target
             condition_met = (current >= target_ticks);
         }
         if (condition_met) break;

         asm volatile("pause"); // Hint to CPU that this is a spin-wait loop
     }
 }

 /**
  * Sleep using interrupts (low CPU usage).
  * Uses the 32-bit workaround calculation.
  * NOTE: This version is basic. A real implementation should use scheduler's
  * sleep queue mechanism (e.g., `scheduler_sleep_ms(milliseconds)`).
  * This function is kept for compatibility/example but likely shouldn't be used
  * directly if the advanced scheduler's sleep is available.
  */
 void sleep_interrupt(uint32_t milliseconds) {
     // --- PREFER using scheduler_sleep_ms(ms) if available! ---

     uint32_t eflags;
     asm volatile("pushf; pop %0" : "=r"(eflags));
     bool interrupts_were_enabled = (eflags & 0x200) != 0;

     uint32_t ticks_to_wait = calculate_ticks_32bit(milliseconds, TARGET_FREQUENCY);
     if (ticks_to_wait == UINT32_MAX) {
         if (interrupts_were_enabled) asm volatile("sti");
         return; // Avoid infinite loop if calculation capped
     }
     if (ticks_to_wait == 0 && milliseconds > 0) ticks_to_wait = 1; // Ensure minimum wait

     uint32_t start = get_pit_ticks();
     uint32_t end_ticks = start + ticks_to_wait;
     bool wrapped = end_ticks < start;

     while (true) {
         uint32_t current = get_pit_ticks();
         bool condition_met;
          if (wrapped) { condition_met = (current < start && current >= end_ticks); }
          else { condition_met = (current >= end_ticks); }
          if (condition_met) break;

         // Atomically enable interrupts (if they were on) and halt
         if (interrupts_were_enabled) {
              asm volatile("sti; hlt; cli");
         } else {
              asm volatile("hlt"); // Halt without enabling interrupts if they were off
         }
         // Interrupts are off upon waking from HLT via interrupt
     }

     // Restore interrupt state
     if (interrupts_were_enabled) { asm volatile("sti"); }
 }

 // --- REMOVE extraneous text that caused errors ---
 // (Lines 212 and 238 from the error log should not exist)