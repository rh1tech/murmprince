#ifndef PS2KBD_WRAPPER_H
#define PS2KBD_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

void ps2kbd_init(void);
void ps2kbd_tick(void);
int ps2kbd_get_key(int* pressed, int* scancode, int* modifier);

// Get the current state of a specific key (1 = pressed, 0 = not pressed)
// scancode: SDL scancode to check
int ps2kbd_is_key_pressed(int scancode);

// Get count of pending events in the queue
int ps2kbd_events_pending(void);

#ifdef __cplusplus
}
#endif

#endif
