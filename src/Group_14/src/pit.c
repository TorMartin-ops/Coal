// src/pit.c

/**
 * pit.c
 * Programmable Interval Timer (PIT) driver for x86 (32-bit).
 * Updated to send EOI for IRQ0 before calling scheduler logic that might not return
 * to the original IRQ handler instance.
 */

 #include "pit.h"
 #include "idt.h"       // Needed for register_int_handler and PIC/EOI defines
 #include <isr_frame.h>  // Include the frame definition
 #include "terminal.h"
 #include "port_io.h"   // For outb, io_wait
 #include "scheduler.h" // Need scheduler_tick() declaration
 #include "types.h"     // Ensure bool is defined via types.h -> stdbool.h
 #include "assert.h"    // For KERNEL_ASSERT
 #include <libc/stdint.h> // For UINT32_MAX
 #include "serial.h"    // For serial_write (if used in debugging)

 // Define TARGET_FREQUENCY if not defined elsewhere (e.g., in pit.h or build system)
 #ifndef TARGET_FREQUENCY
 #define TARGET_FREQUENCY 1000 // Default to 1000 Hz if not defined
 #endif

 // PIC EOI definitions (can also be from idt.h or a dedicated pic.h)
 #ifndef PIC_EOI
 #define PIC_EOI      0x20      /* End-of-interrupt command code */
 #endif
 #ifndef PIC1_COMMAND
 #define PIC1_COMMAND 0x20
 #endif
 #ifndef PIC2_COMMAND
 #define PIC2_COMMAND 0xA0
 #endif
 #define IRQ_PIT 0         // Timer is IRQ line 0 on the master PIC

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
 * @brief Sends End-Of-Interrupt (EOI) to the PIC(s).
 * @param irq_line The IRQ line number (0-15) that was serviced.
 */
static inline void pic_send_eoi_line(uint8_t irq_line)
{
    if (irq_line >= 8) { // IRQ 8-15 are on the slave PIC
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI); // Always send EOI to master PIC
}

 /**
  * PIT IRQ handler:
  * Performs essential timekeeping (implicitly via scheduler_tick's start),
  * ACKs the interrupt with the PIC *before* potentially rescheduling,
  * then calls the scheduler logic which might switch tasks.
  */
 static void pit_irq_handler(isr_frame_t *frame) {
     (void)frame;    // Mark frame as unused if not directly accessed

     // As per the latest advice:
     // 1. Timekeeping / scheduler-tick bookkeeping (scheduler_tick() handles g_tick_count++)
     // 2. ACK the interrupt **before** doing anything that may reschedule.
     // 3. Hand control to the scheduler (scheduler_tick() may call schedule()).

     // Increment tick count (this happens as the first step in scheduler_tick)
     // If scheduler_tick() were not called, and this handler directly called schedule(),
     // then g_tick_count++ would happen here.

     pic_send_eoi_line(IRQ_PIT); // Send EOI for IRQ 0 (timer) *BEFORE* scheduler_tick

     // Now, call the scheduler's tick processing.
     // This function handles g_tick_count increment, waking sleeping tasks,
     // managing time slices, and potentially calling schedule().
     scheduler_tick();
 }

 uint32_t get_pit_ticks(void) {
     return scheduler_get_ticks(); // Delegate to scheduler's tick counter
 }

 static void set_pit_frequency(uint32_t freq) {
      if (freq == 0) { freq = 1; }
      if (freq > PIT_BASE_FREQUENCY) { freq = PIT_BASE_FREQUENCY; }

      uint32_t divisor = PIT_BASE_FREQUENCY / freq;
      if (divisor == 0) divisor = 0x10000; // Max period for 16-bit counter
      if (divisor > 0xFFFF) divisor = 0xFFFF; // Clamp to max 16-bit value
      if (divisor < 1) divisor = 1;       // Ensure divisor is at least 1

      outb(PIT_CMD_PORT, 0x36); // Channel 0, lobyte/hibyte, mode 3 (square wave)
      io_wait(); // Short delay after command

      uint16_t divisor_16 = (uint16_t)divisor;
      outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor_16 & 0xFF)); // Send low byte of divisor
      io_wait(); // Short delay
      outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor_16 >> 8) & 0xFF)); // Send high byte
      io_wait(); // Short delay
 }

 void init_pit(void) {
     register_int_handler(IRQ0_VECTOR, pit_irq_handler, NULL); // IRQ0 is vector 32
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