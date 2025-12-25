/*
 * murmprince - I2S Audio Driver Implementation
 * Uses pico-extras audio_i2s library to provide SDL-compatible audio callback interface
 * 
 * PIO/SM Allocation:
 *   - HDMI: pio1 SM0, SM1 (video output)
 *   - PS/2 Keyboard: pio0 SM0 (input)
 *   - I2S Audio: pio0 SM2, DMA channel 6
 * 
 * Architecture:
 *   Audio is pumped from the main game loop via audio_i2s_driver_pump().
 *   This matches murmdoom's approach and avoids multicore complexity.
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "audio_i2s_driver.h"
#include "board_config.h"

#include "pico/stdlib.h"
#include "pico/audio_i2s.h"
#include "pico/sync.h"
#include "hardware/gpio.h"
#include <string.h>
#include <stdio.h>

// ============================================================================
// Configuration - Avoid conflicts with other PIO users
// ============================================================================

// Use pio0 SM2 for I2S (PS/2 keyboard uses pio0 SM0)
// HDMI uses pio1 SM0, SM1 - so pio0 is safer
#ifndef AUDIO_I2S_PIO
#define AUDIO_I2S_PIO 0
#endif

#ifndef AUDIO_I2S_SM
#define AUDIO_I2S_SM 2
#endif

// Use DMA channel 6 for audio (avoid channels used by HDMI/SD)
#ifndef AUDIO_I2S_DMA_CHANNEL
#define AUDIO_I2S_DMA_CHANNEL 6
#endif

// Use DMA IRQ 0 for audio (share with SD card on Core 0)
// HDMI on Core 1 uses IRQ 1 exclusively
#ifndef AUDIO_I2S_DMA_IRQ
#define AUDIO_I2S_DMA_IRQ 0
#endif

// ============================================================================
// State
// ============================================================================

static struct {
    bool initialized;
    volatile bool enabled;
    uint32_t sample_rate;
    uint8_t channels;
    audio_callback_fn callback;
    void *userdata;
    struct audio_buffer_pool *producer_pool;
    critical_section_t lock;
} audio_state = {0};

// Audio format configuration
static struct audio_format audio_format = {
    .format = AUDIO_BUFFER_FORMAT_PCM_S16,
    .sample_freq = AUDIO_SAMPLE_RATE,
    .channel_count = 2,
};

static struct audio_buffer_format producer_format = {
    .format = &audio_format,
    .sample_stride = 4  // 2 bytes per sample * 2 channels
};

// ============================================================================
// Public API Implementation
// ============================================================================

bool audio_i2s_driver_init(uint32_t sample_rate, uint8_t channels,
                           audio_callback_fn callback, void *userdata) {
    if (audio_state.initialized) {
        DBG_PRINTF("audio_i2s_driver: already initialized\n");
        return false;
    }

    DBG_PRINTF("audio_i2s_driver: initializing (rate=%lu, ch=%d)\n",
           sample_rate, channels);

    // Store configuration
    audio_state.sample_rate = sample_rate;
    audio_state.channels = channels;
    audio_state.callback = callback;
    audio_state.userdata = userdata;

    // Initialize critical section for locking
    critical_section_init(&audio_state.lock);

    // Update audio format
    audio_format.sample_freq = sample_rate;
    audio_format.channel_count = channels;
    producer_format.sample_stride = (channels == 2) ? 4 : 2;

    // Create producer buffer pool
    audio_state.producer_pool = audio_new_producer_pool(
        &producer_format,
        AUDIO_BUFFER_COUNT,
        AUDIO_BUFFER_SAMPLES
    );

    if (!audio_state.producer_pool) {
        DBG_PRINTF("audio_i2s_driver: failed to create producer pool\n");
        return false;
    }

    // Configure I2S pins and hardware
    struct audio_i2s_config config = {
        .data_pin = I2S_DATA_PIN,
        .clock_pin_base = I2S_CLOCK_PIN_BASE,
        .dma_channel = AUDIO_I2S_DMA_CHANNEL,
        .pio_sm = AUDIO_I2S_SM,
    };

    DBG_PRINTF("audio_i2s_driver: I2S pins DATA=%d, CLK_BASE=%d\n",
           config.data_pin, config.clock_pin_base);
    DBG_PRINTF("audio_i2s_driver: PIO%d SM%d, DMA ch%d, IRQ%d\n",
           AUDIO_I2S_PIO, AUDIO_I2S_SM,
           AUDIO_I2S_DMA_CHANNEL, AUDIO_I2S_DMA_IRQ);

    // Setup I2S audio output
    const struct audio_format *output_format;
    output_format = audio_i2s_setup(&audio_format, &config);

    if (!output_format) {
        DBG_PRINTF("audio_i2s_driver: audio_i2s_setup failed\n");
        return false;
    }

    // Increase drive strength for cleaner signal
    gpio_set_drive_strength(I2S_DATA_PIN, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(I2S_CLOCK_PIN_BASE, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(I2S_CLOCK_PIN_BASE + 1, GPIO_DRIVE_STRENGTH_12MA);

    // Connect audio pipeline (pass-through, no internal buffering)
    bool ok = audio_i2s_connect_extra(audio_state.producer_pool, false, 0, 0, NULL);
    if (!ok) {
        DBG_PRINTF("audio_i2s_driver: audio_i2s_connect_extra failed\n");
        return false;
    }

    // Enable I2S immediately - DMA will start consuming buffers
    audio_i2s_set_enabled(true);

    audio_state.initialized = true;
    audio_state.enabled = false;  // Paused until SDL_PauseAudio(0)

    DBG_PRINTF("audio_i2s_driver: initialization complete\n");
    return true;
}

void audio_i2s_driver_set_enabled(bool enable) {
    if (!audio_state.initialized) return;

    if (enable && !audio_state.enabled) {
        DBG_PRINTF("audio_i2s_driver: unpausing audio\n");
        audio_state.enabled = true;
    } else if (!enable && audio_state.enabled) {
        DBG_PRINTF("audio_i2s_driver: pausing audio\n");
        audio_state.enabled = false;
    }
}

bool audio_i2s_driver_is_enabled(void) {
    return audio_state.enabled;
}

void audio_i2s_driver_shutdown(void) {
    if (!audio_state.initialized) return;

    DBG_PRINTF("audio_i2s_driver: shutting down\n");
    audio_state.enabled = false;
    audio_i2s_set_enabled(false);
    audio_state.initialized = false;
}

uint8_t audio_i2s_driver_get_silence(void) {
    return 0; // Signed 16-bit format uses 0 for silence
}

void audio_i2s_driver_lock(void) {
    if (audio_state.initialized) {
        critical_section_enter_blocking(&audio_state.lock);
    }
}

void audio_i2s_driver_unlock(void) {
    if (audio_state.initialized) {
        critical_section_exit(&audio_state.lock);
    }
}

// Pump audio - call from main loop to fill audio buffers
void audio_i2s_driver_pump(void) {
    if (!audio_state.initialized) return;

    audio_buffer_t *buffer;
    
    // Process all available buffers in a non-blocking loop
    while ((buffer = take_audio_buffer(audio_state.producer_pool, false)) != NULL) {
        int buffer_bytes = buffer->max_sample_count * producer_format.sample_stride;

        if (audio_state.enabled && audio_state.callback) {
            // Call user callback to fill the buffer
            audio_state.callback(audio_state.userdata, buffer->buffer->bytes, buffer_bytes);
        } else {
            // Output silence when paused or no callback
            memset(buffer->buffer->bytes, 0, buffer_bytes);
        }

        // Mark buffer as full and queue for playback
        buffer->sample_count = buffer->max_sample_count;
        give_audio_buffer(audio_state.producer_pool, buffer);
    }
}
