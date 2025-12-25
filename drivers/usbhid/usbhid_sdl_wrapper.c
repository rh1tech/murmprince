/*
 * USB HID Wrapper for SDLPoP
 * Maps USB HID keyboard events to SDL-compatible events
 * 
 * SPDX-License-Identifier: MIT
 */

#include "usbhid.h"
#include <stdint.h>
#include <string.h>

#ifdef USB_HID_ENABLED

//--------------------------------------------------------------------
// Event Queue
//--------------------------------------------------------------------

#define USBHID_EVENT_QUEUE_SIZE 32

typedef struct {
    int pressed;      // 1 = key down, 0 = key up
    int scancode;     // SDL/HID scancode
    int modifier;     // Modifier state
} usbhid_key_event_t;

static usbhid_key_event_t event_queue[USBHID_EVENT_QUEUE_SIZE];
static volatile int queue_head = 0;
static volatile int queue_tail = 0;

// Track currently pressed keys for is_key_pressed queries
static uint8_t key_state[256];

// Track current modifier state
static uint8_t current_modifiers = 0;

//--------------------------------------------------------------------
// HID Modifier pseudo-keycodes (from hid_app.c)
//--------------------------------------------------------------------

// hid_app.c uses these pseudo-keycodes for modifiers:
// 0xE0 = CTRL, 0xE1 = SHIFT, 0xE2 = ALT
// These match USB HID modifier usage codes

// SDL scancodes for modifiers
#define SDL_SCANCODE_LCTRL  224
#define SDL_SCANCODE_LSHIFT 225
#define SDL_SCANCODE_LALT   226
#define SDL_SCANCODE_LGUI   227
#define SDL_SCANCODE_RCTRL  228
#define SDL_SCANCODE_RSHIFT 229
#define SDL_SCANCODE_RALT   230
#define SDL_SCANCODE_RGUI   231

// SDL modifier bits
#define KMOD_LSHIFT 0x0001
#define KMOD_RSHIFT 0x0002
#define KMOD_LCTRL  0x0040
#define KMOD_RCTRL  0x0080
#define KMOD_LALT   0x0100
#define KMOD_RALT   0x0200
#define KMOD_SHIFT  (KMOD_LSHIFT | KMOD_RSHIFT)
#define KMOD_CTRL   (KMOD_LCTRL | KMOD_RCTRL)
#define KMOD_ALT    (KMOD_LALT | KMOD_RALT)

//--------------------------------------------------------------------
// Queue Management
//--------------------------------------------------------------------

static void queue_push(int pressed, int scancode, int modifier) {
    int next = (queue_head + 1) % USBHID_EVENT_QUEUE_SIZE;
    if (next != queue_tail) {
        event_queue[queue_head].pressed = pressed;
        event_queue[queue_head].scancode = scancode;
        event_queue[queue_head].modifier = modifier;
        queue_head = next;
    }
}

static int queue_pop(int* pressed, int* scancode, int* modifier) {
    if (queue_tail == queue_head) {
        return 0;  // Queue empty
    }
    *pressed = event_queue[queue_tail].pressed;
    *scancode = event_queue[queue_tail].scancode;
    *modifier = event_queue[queue_tail].modifier;
    queue_tail = (queue_tail + 1) % USBHID_EVENT_QUEUE_SIZE;
    return 1;
}

//--------------------------------------------------------------------
// Convert HID keycode to SDL scancode
//--------------------------------------------------------------------

static int hid_to_sdl_scancode(uint8_t hid_keycode) {
    // Modifier pseudo-keycodes from hid_app.c
    switch (hid_keycode) {
        case 0xE0: return SDL_SCANCODE_LCTRL;   // CTRL
        case 0xE1: return SDL_SCANCODE_LSHIFT;  // SHIFT
        case 0xE2: return SDL_SCANCODE_LALT;    // ALT
        case 0xE3: return SDL_SCANCODE_LGUI;    // GUI/Super
        case 0xE4: return SDL_SCANCODE_RCTRL;   // Right CTRL
        case 0xE5: return SDL_SCANCODE_RSHIFT;  // Right SHIFT
        case 0xE6: return SDL_SCANCODE_RALT;    // Right ALT
        case 0xE7: return SDL_SCANCODE_RGUI;    // Right GUI
        default:
            // Regular keycodes: HID and SDL use the same values
            return hid_keycode;
    }
}

//--------------------------------------------------------------------
// Process events from hid_app.c key action queue
//--------------------------------------------------------------------

static void process_pending_key_actions(void) {
    uint8_t hid_keycode;
    int down;
    
    // Drain all pending key actions from hid_app.c
    while (usbhid_get_key_action(&hid_keycode, &down)) {
        int scancode = hid_to_sdl_scancode(hid_keycode);
        
        // Update modifier tracking
        if (hid_keycode == 0xE1) {  // SHIFT
            if (down) current_modifiers |= KMOD_LSHIFT;
            else current_modifiers &= ~KMOD_LSHIFT;
        } else if (hid_keycode == 0xE0) {  // CTRL
            if (down) current_modifiers |= KMOD_LCTRL;
            else current_modifiers &= ~KMOD_LCTRL;
        } else if (hid_keycode == 0xE2) {  // ALT
            if (down) current_modifiers |= KMOD_LALT;
            else current_modifiers &= ~KMOD_LALT;
        }
        
        // Update key state
        if (scancode >= 0 && scancode < 256) {
            key_state[scancode] = down ? 1 : 0;
        }
        
        // Queue the event
        queue_push(down, scancode, current_modifiers);
    }
}

//--------------------------------------------------------------------
// Public API (matching ps2kbd_wrapper interface)
//--------------------------------------------------------------------

static int usb_hid_initialized = 0;

void usbhid_sdl_init(void) {
    usbhid_init();
    usb_hid_initialized = 1;
    queue_head = 0;
    queue_tail = 0;
    memset(key_state, 0, sizeof(key_state));
    current_modifiers = 0;
}

void usbhid_sdl_tick(void) {
    if (!usb_hid_initialized) return;
    
    // Process USB host events
    usbhid_task();
    
    // Process any pending key actions from hid_app.c
    process_pending_key_actions();
}

int usbhid_sdl_get_key(int* pressed, int* scancode, int* modifier) {
    if (!usb_hid_initialized) return 0;
    return queue_pop(pressed, scancode, modifier);
}

int usbhid_sdl_is_key_pressed(int scancode) {
    if (!usb_hid_initialized) return 0;
    if (scancode < 0 || scancode >= 256) return 0;
    return key_state[scancode];
}

int usbhid_sdl_events_pending(void) {
    if (!usb_hid_initialized) return 0;
    int count = queue_head - queue_tail;
    if (count < 0) count += USBHID_EVENT_QUEUE_SIZE;
    return count;
}

int usbhid_sdl_keyboard_connected(void) {
    if (!usb_hid_initialized) return 0;
    return usbhid_keyboard_connected();
}

#else // !USB_HID_ENABLED

// Stub implementations when USB HID is disabled
void usbhid_sdl_init(void) {}
void usbhid_sdl_tick(void) {}
int usbhid_sdl_get_key(int* pressed, int* scancode, int* modifier) { return 0; }
int usbhid_sdl_is_key_pressed(int scancode) { return 0; }
int usbhid_sdl_events_pending(void) { return 0; }
int usbhid_sdl_keyboard_connected(void) { return 0; }

#endif // USB_HID_ENABLED
