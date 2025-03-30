#ifndef INPUT_H
#define INPUT_H

#include "keyboard.h"

typedef struct {
    KeyEvent key;
    // Future: mouse events
} InputEvent;

void input_init();
bool input_poll(InputEvent* event);
void input_flush();

#endif