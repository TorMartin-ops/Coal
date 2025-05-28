#ifndef PC_SPEAKER_H
#define PC_SPEAKER_H

#include <kernel/core/types.h>

/**
 * Enables the PC speaker by setting the appropriate bits in port 0x61.
 */
void enable_speaker(void);

/**
 * Disables the PC speaker (turns it off).
 */
void disable_speaker(void);

/**
 * Programs the PIT Channel 2 to generate a square wave at the given frequency,
 * then enables the speaker so sound is actually audible.
 */
void play_sound(uint32_t frequency);

/**
 * Immediately stops the speaker output.
 */
void stop_sound(void);

#endif
