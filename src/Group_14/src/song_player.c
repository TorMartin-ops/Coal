#include "song_player.h"
#include "pc_speaker.h"
#include "pit.h"
#include "terminal.h"

void play_song(Song *song) {
    if (!song || !song->notes || song->length == 0)
        return;

    for (uint32_t i = 0; i < song->length; i++) {
        Note n = song->notes[i];

        if (n.frequency == 0)
            stop_sound();
        else
            play_sound(n.frequency);

        sleep_interrupt(n.duration);
        stop_sound();
    }
}
