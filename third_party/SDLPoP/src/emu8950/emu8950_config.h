/**
 * EMU8950 configuration for Prince of Persia on RP2350
 * Based on MurmDOOM's optimizations
 */
#ifndef EMU8950_CONFIG_H
#define EMU8950_CONFIG_H

// Enable EMU8950 OPL emulator
#define USE_EMU8950_OPL 1

// Performance optimizations from rp2040-doom/murmdoom
#define EMU8950_NO_WAVE_TABLE_MAP 1
#define EMU8950_NO_TLL 1
#define EMU8950_NO_FLOAT 1
#define EMU8950_NO_TIMER 1
#define EMU8950_NO_TEST_FLAG 1
#define EMU8950_SIMPLER_NOISE 1
#define EMU8950_SHORT_NOISE_UPDATE_CHECK 1
#define EMU8950_LINEAR_SKIP 1
#define EMU8950_LINEAR_END_OF_NOTE_OPTIMIZATION 1
#define EMU8950_NO_PERCUSSION_MODE 1
#define EMU8950_LINEAR 1
#define EMU8950_ASM 1
#define EMU8950_SLOT_RENDER 1
#define EMU8950_NO_RATECONV 1

// Platform detection
#define PICO_ON_DEVICE 1

#endif // EMU8950_CONFIG_H
