#include <kernel/drivers/audio/song_player.h>
#include <kernel/drivers/audio/pc_speaker.h>
#include <kernel/drivers/timer/pit.h>
#include <kernel/drivers/display/terminal.h>

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
