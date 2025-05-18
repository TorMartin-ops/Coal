// src/pit.c

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
 #include "serial.h"    // <<< ADDED INCLUDE for serial_write

 // Define TARGET_FREQUENCY if not defined elsewhere (e.g., in pit.h or build system)
 #ifndef TARGET_FREQUENCY
 #define TARGET_FREQUENCY 1000 // Default to 1000 Hz if not defined
 #endif

 // --- Revised Workaround Helper ---
 static inline uint32_t calculate_ticks_32bit(uint32_t ms, uint32_t freq_hz) {
     if (ms == 0) return 0;
     if (freq_hz == 0) return 0;

     if ((freq_hz % 1000) == 0) {
         uint32_t ticks_per_ms = freq_hz / 1000;
         if (ticks_per_ms > 0 && ms > UINT32_MAX / ticks_per_ms) {
             return UINT32_MAX;
         }
         return ms * ticks_per_ms;
     }

     uint32_t ms_div_1000 = ms / 1000;
     uint32_t ticks_from_whole_seconds = 0;
     if (ms_div_1000 > 0) {
         if (freq_hz > 0 && ms_div_1000 > UINT32_MAX / freq_hz) {
              return UINT32_MAX;
         }
         ticks_from_whole_seconds = ms_div_1000 * freq_hz;
     }

     uint32_t remaining_ms = ms % 1000;
     uint32_t ticks_from_remaining_ms = 0;
     if (remaining_ms > 0) {
         if (freq_hz > 0 && remaining_ms > UINT32_MAX / freq_hz) {
             return UINT32_MAX;
         }
         uint32_t intermediate_product = remaining_ms * freq_hz;
         ticks_from_remaining_ms = intermediate_product / 1000;

         if ((intermediate_product % 1000) >= 500) {
             ticks_from_remaining_ms++;
         }
     }

     if (ticks_from_remaining_ms > UINT32_MAX - ticks_from_whole_seconds) {
         return UINT32_MAX;
     }
     uint32_t total_ticks = ticks_from_whole_seconds + ticks_from_remaining_ms;

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
    serial_write("[PIT] Enter pit_irq_handler\n"); // This print is fine
     (void)frame;
     scheduler_tick();
 }

 uint32_t get_pit_ticks(void) {
     return scheduler_get_ticks();
 }

 static void set_pit_frequency(uint32_t freq) {
      if (freq == 0) { freq = 1; }
      if (freq > PIT_BASE_FREQUENCY) { freq = PIT_BASE_FREQUENCY; }

      uint32_t divisor = PIT_BASE_FREQUENCY / freq;
      if (divisor == 0) divisor = 0x10000;
      if (divisor > 0xFFFF) divisor = 0xFFFF;
      if (divisor < 1) divisor = 1;

      outb(PIT_CMD_PORT, 0x36);
      io_wait();
      uint16_t divisor_16 = (uint16_t)divisor;
      outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor_16 & 0xFF));
      io_wait();
      outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor_16 >> 8) & 0xFF));
      io_wait();
 }

 void init_pit(void) {
     register_int_handler(32, pit_irq_handler, NULL);
     set_pit_frequency(TARGET_FREQUENCY);
     terminal_printf("[PIT] Initialized (Target Frequency: %lu Hz)\n", (unsigned long)TARGET_FREQUENCY);
 }

 void sleep_busy(uint32_t milliseconds) {
     uint32_t ticks_to_wait = calculate_ticks_32bit(milliseconds, TARGET_FREQUENCY);
     uint32_t start = get_pit_ticks();
     if (ticks_to_wait == UINT32_MAX) return;

     uint32_t target_ticks = start + ticks_to_wait;
     bool wrapped = target_ticks < start;

     while (true) {
         uint32_t current = get_pit_ticks();
         bool condition_met;
         if (wrapped) {
             condition_met = (current < start && current >= target_ticks);
         } else {
             condition_met = (current >= target_ticks);
         }
         if (condition_met) break;
         asm volatile("pause");
     }
 }

 void sleep_interrupt(uint32_t milliseconds) {
     uint32_t eflags;
     asm volatile("pushf; pop %0" : "=r"(eflags));
     bool interrupts_were_enabled = (eflags & 0x200) != 0;

     uint32_t ticks_to_wait = calculate_ticks_32bit(milliseconds, TARGET_FREQUENCY);
     if (ticks_to_wait == UINT32_MAX) {
         if (interrupts_were_enabled) asm volatile("sti");
         return;
     }
     if (ticks_to_wait == 0 && milliseconds > 0) ticks_to_wait = 1;

     uint32_t start = get_pit_ticks();
     uint32_t end_ticks = start + ticks_to_wait;
     bool wrapped = end_ticks < start;

     while (true) {
         uint32_t current = get_pit_ticks();
         bool condition_met;
          if (wrapped) { condition_met = (current < start && current >= end_ticks); }
          else { condition_met = (current >= end_ticks); }
          if (condition_met) break;

         if (interrupts_were_enabled) {
              asm volatile("sti; hlt; cli");
         } else {
              asm volatile("hlt");
         }
     }
     if (interrupts_were_enabled) { asm volatile("sti"); }
 }