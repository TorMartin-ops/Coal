#ifndef SONG_PLAYER_H
#define SONG_PLAYER_H

#include <kernel/core/types.h>
#include <kernel/drivers/audio/song.h>  // for Song struct

/**
 * play_song
 *   Plays the given song using the PC speaker device.
 */
void play_song(Song *song);

#endif
