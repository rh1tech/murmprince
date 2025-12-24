/*
 * PS/2 Keyboard Wrapper for SDLPoP
 * Maps PS/2 HID keycodes to SDL scancodes
 * 
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "../../src/board_config.h"
#include "ps2kbd_wrapper.h"
#include "ps2kbd_mrmltr.h"
#include <queue>

// SDL scancode definitions (matching SDL_port.h)
#define SDL_SCANCODE_A 4
#define SDL_SCANCODE_RETURN 40
#define SDL_SCANCODE_ESCAPE 41
#define SDL_SCANCODE_BACKSPACE 42
#define SDL_SCANCODE_TAB 43
#define SDL_SCANCODE_SPACE 44
#define SDL_SCANCODE_RIGHT 79
#define SDL_SCANCODE_LEFT 80
#define SDL_SCANCODE_DOWN 81
#define SDL_SCANCODE_UP 82
#define SDL_SCANCODE_F1 58
#define SDL_SCANCODE_LCTRL 224
#define SDL_SCANCODE_LSHIFT 225
#define SDL_SCANCODE_LALT 226
#define SDL_SCANCODE_RCTRL 228
#define SDL_SCANCODE_RSHIFT 229
#define SDL_SCANCODE_RALT 230

struct KeyEvent {
    int pressed;
    int scancode;  // SDL scancode
    int modifier;  // Modifier flags (shift/ctrl/alt)
};

static std::queue<KeyEvent> event_queue;
static uint8_t current_modifiers = 0;

// HID keycode to SDL scancode mapping
// HID keycodes are already almost identical to SDL scancodes for most keys
static int hid_to_sdl_scancode(uint8_t hid_code) {
    // Letters A-Z: HID 0x04-0x1D map to SDL_SCANCODE_A (4) - SDL_SCANCODE_Z (29)
    // Already the same range!
    if (hid_code >= 0x04 && hid_code <= 0x1D) {
        return hid_code;  // HID and SDL match for letters
    }
    
    // Numbers 1-9, 0: HID 0x1E-0x27 map to SDL_SCANCODE_1 (30) - SDL_SCANCODE_0 (39)
    // SDL scancodes for numbers are 30-39 (1-9,0)
    // HID keycodes for numbers are 0x1E-0x27 (30-39)
    // They're the same! No conversion needed.
    if (hid_code >= 0x1E && hid_code <= 0x27) {
        return hid_code;
    }
    
    // Special keys
    switch (hid_code) {
        case 0x28: return 40;  // Enter -> SDL_SCANCODE_RETURN
        case 0x29: return 41;  // Escape -> SDL_SCANCODE_ESCAPE
        case 0x2A: return 42;  // Backspace -> SDL_SCANCODE_BACKSPACE
        case 0x2B: return 43;  // Tab -> SDL_SCANCODE_TAB
        case 0x2C: return 44;  // Space -> SDL_SCANCODE_SPACE
        case 0x2D: return 45;  // Minus
        case 0x2E: return 46;  // Equals
        case 0x2F: return 47;  // Left bracket
        case 0x30: return 48;  // Right bracket
        case 0x31: return 49;  // Backslash
        case 0x33: return 51;  // Semicolon
        case 0x34: return 52;  // Quote
        case 0x35: return 53;  // Grave/tilde
        case 0x36: return 54;  // Comma
        case 0x37: return 55;  // Period
        case 0x38: return 56;  // Forward slash
        
        // Arrow keys (HID and SDL match)
        case 0x4F: return 79;  // Right -> SDL_SCANCODE_RIGHT
        case 0x50: return 80;  // Left -> SDL_SCANCODE_LEFT
        case 0x51: return 81;  // Down -> SDL_SCANCODE_DOWN
        case 0x52: return 82;  // Up -> SDL_SCANCODE_UP
        
        // Function keys F1-F12: HID 0x3A-0x45 -> SDL 58-69
        default:
            if (hid_code >= 0x3A && hid_code <= 0x45) {
                return hid_code + 0x1E;  // 0x3A + 0x1E = 58 = SDL_SCANCODE_F1
            }
            break;
    }
    
    return 0;  // Unknown key
}

// Calculate modifier flags for SDL event
static int get_sdl_modifier_flags(uint8_t hid_modifier) {
    int flags = 0;
    // KMOD_SHIFT = 0x0003, KMOD_CTRL = 0x00C0, KMOD_ALT = 0x0300
    if (hid_modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) {
        flags |= 0x0003;  // KMOD_SHIFT
    }
    if (hid_modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) {
        flags |= 0x00C0;  // KMOD_CTRL
    }
    if (hid_modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)) {
        flags |= 0x0300;  // KMOD_ALT
    }
    return flags;
}

static void key_handler(hid_keyboard_report_t *curr, hid_keyboard_report_t *prev) {
    // Handle modifier key changes (generate events for shift/ctrl/alt)
    uint8_t changed_mods = curr->modifier ^ prev->modifier;
    current_modifiers = curr->modifier;
    
    if (changed_mods) {
        // Left Ctrl
        if (changed_mods & KEYBOARD_MODIFIER_LEFTCTRL) {
            int pressed = (curr->modifier & KEYBOARD_MODIFIER_LEFTCTRL) != 0;
            event_queue.push({pressed, SDL_SCANCODE_LCTRL, get_sdl_modifier_flags(curr->modifier)});
        }
        // Left Shift
        if (changed_mods & KEYBOARD_MODIFIER_LEFTSHIFT) {
            int pressed = (curr->modifier & KEYBOARD_MODIFIER_LEFTSHIFT) != 0;
            event_queue.push({pressed, SDL_SCANCODE_LSHIFT, get_sdl_modifier_flags(curr->modifier)});
        }
        // Left Alt
        if (changed_mods & KEYBOARD_MODIFIER_LEFTALT) {
            int pressed = (curr->modifier & KEYBOARD_MODIFIER_LEFTALT) != 0;
            event_queue.push({pressed, SDL_SCANCODE_LALT, get_sdl_modifier_flags(curr->modifier)});
        }
        // Right Ctrl
        if (changed_mods & KEYBOARD_MODIFIER_RIGHTCTRL) {
            int pressed = (curr->modifier & KEYBOARD_MODIFIER_RIGHTCTRL) != 0;
            event_queue.push({pressed, SDL_SCANCODE_RCTRL, get_sdl_modifier_flags(curr->modifier)});
        }
        // Right Shift
        if (changed_mods & KEYBOARD_MODIFIER_RIGHTSHIFT) {
            int pressed = (curr->modifier & KEYBOARD_MODIFIER_RIGHTSHIFT) != 0;
            event_queue.push({pressed, SDL_SCANCODE_RSHIFT, get_sdl_modifier_flags(curr->modifier)});
        }
        // Right Alt
        if (changed_mods & KEYBOARD_MODIFIER_RIGHTALT) {
            int pressed = (curr->modifier & KEYBOARD_MODIFIER_RIGHTALT) != 0;
            event_queue.push({pressed, SDL_SCANCODE_RALT, get_sdl_modifier_flags(curr->modifier)});
        }
    }

    // Check for newly pressed keys
    for (int i = 0; i < 6; i++) {
        if (curr->keycode[i] != 0) {
            bool found = false;
            for (int j = 0; j < 6; j++) {
                if (prev->keycode[j] == curr->keycode[i]) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                int scancode = hid_to_sdl_scancode(curr->keycode[i]);
                if (scancode) {
                    event_queue.push({1, scancode, get_sdl_modifier_flags(curr->modifier)});
                }
            }
        }
    }

    // Check for released keys
    for (int i = 0; i < 6; i++) {
        if (prev->keycode[i] != 0) {
            bool found = false;
            for (int j = 0; j < 6; j++) {
                if (curr->keycode[j] == prev->keycode[i]) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                int scancode = hid_to_sdl_scancode(prev->keycode[i]);
                if (scancode) {
                    event_queue.push({0, scancode, get_sdl_modifier_flags(curr->modifier)});
                }
            }
        }
    }
}

static Ps2Kbd_Mrmltr* kbd = nullptr;

// Track which SDL scancodes are currently held (for direct state query)
static uint8_t pressed_keys[256] = {0};

extern "C" void ps2kbd_init(void) {
    // PS2 keyboard driver expects base_gpio as CLK, and base_gpio+1 as DATA
    kbd = new Ps2Kbd_Mrmltr(pio0, PS2_PIN_CLK, key_handler);
    kbd->init_gpio();
}

extern "C" void ps2kbd_tick(void) {
    if (kbd) kbd->tick();
}

extern "C" int ps2kbd_get_key(int* pressed, int* scancode, int* modifier) {
    if (event_queue.empty()) return 0;
    KeyEvent e = event_queue.front();
    event_queue.pop();
    *pressed = e.pressed;
    *scancode = e.scancode;
    *modifier = e.modifier;
    
    // Update pressed_keys tracking
    if (e.scancode >= 0 && e.scancode < 256) {
        pressed_keys[e.scancode] = e.pressed ? 1 : 0;
    }
    
    return 1;
}

extern "C" int ps2kbd_is_key_pressed(int scancode) {
    if (scancode < 0 || scancode >= 256) return 0;
    return pressed_keys[scancode];
}

extern "C" int ps2kbd_events_pending(void) {
    return (int)event_queue.size();
}
