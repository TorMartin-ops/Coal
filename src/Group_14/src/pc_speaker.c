#include "pc_speaker.h"
#include "port_io.h"   // for inb, outb
#include "pit.h"       // for PIT_BASE_FREQUENCY if needed
#include <libc/stdint.h>

#define PC_SPEAKER_PORT  0x61  // Typical control port for PC Speaker

void enable_speaker(void) {
    // 1. Read current port 0x61
    uint8_t tmp = inb(PC_SPEAKER_PORT);
    // 2. Set bits 0 and 1 => 0x03 to enable speaker
    //    Bit 0 = enable, Bit 1 = data
    outb(PC_SPEAKER_PORT, tmp | 0x03);
}

void disable_speaker(void) {
    // 1. Read current port 0x61
    uint8_t tmp = inb(PC_SPEAKER_PORT);
    // 2. Clear bits 0 and 1 to disable speaker
    outb(PC_SPEAKER_PORT, tmp & 0xFC);  // 0xFC = 1111 1100
}

void play_sound(uint32_t frequency) {
    if (frequency == 0) {
        return; // no sound
    }

    // Calculate the PIT divisor for Channel 2.
    // PIT_BASE_FREQUENCY is typically 1193180 Hz.
    uint32_t divisor = PIT_BASE_FREQUENCY / frequency;

    // Send command to set up Channel 2, mode 3 (square wave), lobyte/hibyte.
    outb(PIT_CMD_PORT, 0xB6);  
    // 0xB6 = 10110110 (channel 2, access mode lobyte/hibyte, mode 3, binary)

    // Send the low byte and high byte of divisor
    outb(PIT_CHANNEL2_PORT, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL2_PORT, (uint8_t)((divisor >> 8) & 0xFF));

    // Finally enable the speaker
    enable_speaker();
}

void stop_sound(void) {
    // We could either just disable the speaker bits,
    // or specifically read port 0x61 and clear bit1. The simplest is:
    disable_speaker();
}
