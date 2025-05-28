#include <kernel/drivers/audio/pc_speaker.h>
#include <kernel/lib/port_io.h>   // for inb, outb
#include <kernel/drivers/timer/pit.h>       // for PIT_BASE_FREQUENCY
#include <kernel/core/types.h>

#define PC_SPEAKER_PORT 0x61  // Control port for the PC speaker

void enable_speaker(void) {
    uint8_t tmp = inb(PC_SPEAKER_PORT);      // Read current port state
    outb(PC_SPEAKER_PORT, tmp | 0x03);         // Set bits 0 and 1 to enable speaker
}

void disable_speaker(void) {
    uint8_t tmp = inb(PC_SPEAKER_PORT);      // Read current port state
    outb(PC_SPEAKER_PORT, tmp & 0xFC);         // Clear bits 0 and 1 to disable speaker
}

void play_sound(uint32_t frequency) {
    if (frequency == 0)
        return; // No sound if frequency is 0

    // Calculate divisor for PIT Channel 2.
    uint32_t divisor = PIT_BASE_FREQUENCY / frequency;

    // Configure PIT Channel 2: mode 3 (square wave) with lobyte/hibyte access.
    outb(PIT_CMD_PORT, 0xB6);
    outb(PIT_CHANNEL2_PORT, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL2_PORT, (uint8_t)((divisor >> 8) & 0xFF));

    enable_speaker();
}

void stop_sound(void) {
    disable_speaker();
}
