// song.h
#ifndef SONG_H
#define SONG_H

#include <kernel/core/types.h>

typedef struct {
    uint32_t frequency;  // e.g. 440 = A4
    uint32_t duration;   // in ms
} Note;

typedef struct {
    Note* notes;         // pointer to array of notes
    uint32_t length;     // how many notes
} Song;

#endif
