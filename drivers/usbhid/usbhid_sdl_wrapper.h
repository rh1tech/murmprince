/*
 * USB HID Wrapper for SDLPoP - Header
 * Provides interface to USB keyboard for SDL-based applications
 * 
 * This wrapper provides the same interface as ps2kbd_wrapper so it can
 * be easily integrated into SDL_port.c alongside PS/2 keyboard support.
 * 
 * SPDX-License-Identifier: MIT
 */

#ifndef USBHID_SDL_WRAPPER_H
#define USBHID_SDL_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize USB HID keyboard support
 * Call during system initialization
 */
void usbhid_sdl_init(void);

/**
 * Process USB HID events
 * Call every frame to poll for keyboard events
 */
void usbhid_sdl_tick(void);

/**
 * Get next pending key event
 * @param pressed Output: 1 if key pressed, 0 if released
 * @param scancode Output: SDL/HID scancode
 * @param modifier Output: SDL modifier state
 * @return Non-zero if event was available
 */
int usbhid_sdl_get_key(int* pressed, int* scancode, int* modifier);

/**
 * Check if a specific key is currently pressed
 * @param scancode SDL scancode to check
 * @return 1 if pressed, 0 if not
 */
int usbhid_sdl_is_key_pressed(int scancode);

/**
 * Get count of pending events in the queue
 * @return Number of events pending
 */
int usbhid_sdl_events_pending(void);

/**
 * Check if USB keyboard is connected
 * @return Non-zero if a USB keyboard is connected
 */
int usbhid_sdl_keyboard_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* USBHID_SDL_WRAPPER_H */
