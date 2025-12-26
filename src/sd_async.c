/*
 * Async SD Card Queue - Implementation (Pump-based, Core 0 only)
 * 
 * This version uses a cooperative pump model on Core 0 instead of a Core 1 worker.
 * SD operations happen incrementally during sd_async_pump() calls from the main loop.
 * 
 * Benefits:
 * - No cross-core bus contention (eliminates HDMI signal loss)
 * - HDMI DMA IRQ naturally preempts SD operations
 * - Simpler synchronization (no mutex needed)
 */

#include "sd_async.h"
#include "pop_fs.h"
#include "psram_allocator.h"

#include "pico/stdlib.h"
#include "hardware/sync.h"

#include <string.h>
#include <stdio.h>

// ============================================================================
// Configuration
// ============================================================================

#define SD_ASYNC_MAX_STREAMS     4    // Max concurrent streams
#define SD_ASYNC_MAX_PATH        256  // Max path length

// ============================================================================
// Stream state machine
// ============================================================================

typedef enum {
    STREAM_STATE_CLOSED = 0,
    STREAM_STATE_FILLING_A,     // Incrementally filling buffer A
    STREAM_STATE_FILLING_B,     // Incrementally filling buffer B
    STREAM_STATE_READY,         // Both buffers valid, waiting for consumption
    STREAM_STATE_EOF,           // End of file reached
    STREAM_STATE_ERROR          // Error occurred
} stream_state_t;

struct sd_async_stream {
    stream_state_t state;
    
    // File info
    FIL* file;
    size_t file_size;
    size_t file_pos;            // Current position in file (for next read)
    
    // Double buffer system
    uint8_t* buffer_a;
    uint8_t* buffer_b;
    int buffer_a_valid;         // Bytes valid in buffer A
    int buffer_b_valid;         // Bytes valid in buffer B
    int buffer_a_consumed;      // Bytes consumed from buffer A by reader
    int buffer_b_consumed;      // Bytes consumed from buffer B by reader
    int active_buffer;          // 0=A, 1=B - which buffer is being read from
    
    // Incremental fill state
    int fill_offset;            // How many bytes already filled in current buffer
    
    // Slot index
    int slot;
};

// ============================================================================
// Global state
// ============================================================================

static struct sd_async_stream streams[SD_ASYNC_MAX_STREAMS];
static bool g_initialized = false;

// ============================================================================
// Initialization
// ============================================================================

void sd_async_init(void) {
    if (g_initialized) return;
    
    printf("[SD_ASYNC] Initializing (pump-based, Core 0 only)...\n");
    
    // Initialize structures
    memset(streams, 0, sizeof(streams));
    
    for (int i = 0; i < SD_ASYNC_MAX_STREAMS; i++) {
        streams[i].state = STREAM_STATE_CLOSED;
        streams[i].slot = i;
    }
    
    g_initialized = true;
    printf("[SD_ASYNC] Initialized OK\n");
}

bool sd_async_is_initialized(void) {
    return g_initialized;
}

// ============================================================================
// SD Mutex API - simplified for single-core (just interrupt save/restore)
// ============================================================================

uint32_t sd_mutex_lock(void) {
    return save_and_disable_interrupts();
}

void sd_mutex_unlock(uint32_t save) {
    restore_interrupts(save);
}

// ============================================================================
// Pump function - does incremental SD work
// ============================================================================

// Process one chunk of work for a stream
// Returns true if work was done
static bool pump_stream(sd_async_stream_t* s) {
    if (s->state == STREAM_STATE_CLOSED || 
        s->state == STREAM_STATE_READY ||
        s->state == STREAM_STATE_EOF ||
        s->state == STREAM_STATE_ERROR) {
        return false;
    }
    
    // Determine which buffer we're filling
    uint8_t* buf;
    int* valid;
    
    if (s->state == STREAM_STATE_FILLING_A) {
        buf = s->buffer_a;
        valid = &s->buffer_a_valid;
    } else {
        buf = s->buffer_b;
        valid = &s->buffer_b_valid;
    }
    
    // Check if file exhausted
    if (s->file_pos >= s->file_size) {
        *valid = s->fill_offset;  // Whatever we filled so far
        s->fill_offset = 0;
        
        // Transition to appropriate state
        if (s->state == STREAM_STATE_FILLING_A) {
            s->buffer_a_consumed = 0;
            if (s->buffer_b_valid > 0) {
                s->state = STREAM_STATE_READY;
            } else {
                s->state = STREAM_STATE_EOF;
            }
        } else {
            s->buffer_b_consumed = 0;
            if (s->buffer_a_valid > 0) {
                s->state = STREAM_STATE_READY;
            } else {
                s->state = STREAM_STATE_EOF;
            }
        }
        return true;
    }
    
    // Calculate how much to read this pump call
    size_t remaining_in_buffer = SD_ASYNC_STREAM_BUFFER_BYTES - s->fill_offset;
    size_t remaining_in_file = s->file_size - s->file_pos;
    size_t to_read = SD_ASYNC_PUMP_CHUNK_SIZE;
    
    if (to_read > remaining_in_buffer) to_read = remaining_in_buffer;
    if (to_read > remaining_in_file) to_read = remaining_in_file;
    
    // Do the actual read
    UINT br = 0;
    FRESULT fr = f_read(s->file, buf + s->fill_offset, (UINT)to_read, &br);
    
    if (fr != FR_OK) {
        printf("[SD_ASYNC] Read error %d\n", fr);
        s->state = STREAM_STATE_ERROR;
        return true;
    }
    
    s->fill_offset += br;
    s->file_pos += br;
    
    // Check if buffer is now full or file ended
    if (s->fill_offset >= SD_ASYNC_STREAM_BUFFER_BYTES || s->file_pos >= s->file_size) {
        *valid = s->fill_offset;
        s->fill_offset = 0;
        
        // Transition based on which buffer we just filled
        if (s->state == STREAM_STATE_FILLING_A) {
            s->buffer_a_consumed = 0;
            // If B also needs filling and file has more data, fill B next
            if (s->buffer_b_valid == 0 && s->file_pos < s->file_size) {
                s->state = STREAM_STATE_FILLING_B;
            } else {
                s->state = STREAM_STATE_READY;
            }
        } else {
            s->buffer_b_consumed = 0;
            s->state = STREAM_STATE_READY;
        }
    }
    
    return true;
}

bool sd_async_pump(void) {
    if (!g_initialized) return false;
    
    bool did_work = false;
    
    // Process all active streams
    for (int i = 0; i < SD_ASYNC_MAX_STREAMS; i++) {
        sd_async_stream_t* s = &streams[i];
        
        // Check if this stream needs buffer refill
        if (s->state == STREAM_STATE_READY) {
            // Check if the inactive buffer needs refilling
            int inactive = 1 - s->active_buffer;
            
            if (inactive == 0 && s->buffer_a_valid == 0 && s->file_pos < s->file_size) {
                s->state = STREAM_STATE_FILLING_A;
                s->fill_offset = 0;
            } else if (inactive == 1 && s->buffer_b_valid == 0 && s->file_pos < s->file_size) {
                s->state = STREAM_STATE_FILLING_B;
                s->fill_offset = 0;
            }
        }
        
        // Do incremental work
        if (pump_stream(s)) {
            did_work = true;
        }
    }
    
    return did_work;
}

// ============================================================================
// Streaming API Implementation
// ============================================================================

sd_async_stream_t* sd_async_stream_open(const char* path) {
    if (!g_initialized) {
        printf("[SD_ASYNC] Not initialized!\n");
        return NULL;
    }
    
    // Find a free slot
    sd_async_stream_t* s = NULL;
    for (int i = 0; i < SD_ASYNC_MAX_STREAMS; i++) {
        if (streams[i].state == STREAM_STATE_CLOSED) {
            s = &streams[i];
            break;
        }
    }
    
    if (!s) {
        printf("[SD_ASYNC] No free stream slots\n");
        return NULL;
    }
    
    // Open the file synchronously
    s->file = pop_fs_open(path, "r");
    if (!s->file) {
        printf("[SD_ASYNC] Failed to open: %s\n", path);
        return NULL;
    }
    
    s->file_size = f_size(s->file);
    s->file_pos = 0;
    
    printf("[SD_ASYNC] Opened: %s (%u bytes)\n", path, (unsigned)s->file_size);
    
    // Allocate buffers from PSRAM
    s->buffer_a = (uint8_t*)psram_malloc(SD_ASYNC_STREAM_BUFFER_BYTES);
    s->buffer_b = (uint8_t*)psram_malloc(SD_ASYNC_STREAM_BUFFER_BYTES);
    
    if (!s->buffer_a || !s->buffer_b) {
        printf("[SD_ASYNC] Failed to allocate buffers\n");
        if (s->buffer_a) psram_free(s->buffer_a);
        if (s->buffer_b) psram_free(s->buffer_b);
        pop_fs_close(s->file);
        s->file = NULL;
        return NULL;
    }
    
    // Initialize buffer state
    s->buffer_a_valid = 0;
    s->buffer_b_valid = 0;
    s->buffer_a_consumed = 0;
    s->buffer_b_consumed = 0;
    s->active_buffer = 0;
    s->fill_offset = 0;
    
    // Start filling buffer A
    s->state = STREAM_STATE_FILLING_A;
    
    // Do initial fill synchronously to get some data ready
    // This ensures the audio callback has data immediately
    while (s->state == STREAM_STATE_FILLING_A || s->state == STREAM_STATE_FILLING_B) {
        if (!pump_stream(s)) break;
        // Limit initial sync fill to just buffer A
        if (s->state == STREAM_STATE_FILLING_B) {
            // Continue filling B too for double-buffer warmup
            while (s->state == STREAM_STATE_FILLING_B) {
                if (!pump_stream(s)) break;
            }
            break;
        }
    }
    
    printf("[SD_ASYNC] Stream ready, A=%d B=%d bytes\n", 
           s->buffer_a_valid, s->buffer_b_valid);
    
    return s;
}

int sd_async_stream_read(sd_async_stream_t* stream, void* dest, size_t max_bytes) {
    if (!stream) return -1;
    
    // Handle EOF/Error states
    if (stream->state == STREAM_STATE_ERROR) return -1;
    if (stream->state == STREAM_STATE_CLOSED) return -1;
    
    // Get active buffer info
    uint8_t* buf;
    int* valid;
    int* consumed;
    
    if (stream->active_buffer == 0) {
        buf = stream->buffer_a;
        valid = &stream->buffer_a_valid;
        consumed = &stream->buffer_a_consumed;
    } else {
        buf = stream->buffer_b;
        valid = &stream->buffer_b_valid;
        consumed = &stream->buffer_b_consumed;
    }
    
    int available = *valid - *consumed;
    
    // If current buffer exhausted, try to swap
    if (available <= 0) {
        // Check if other buffer is ready
        int other = 1 - stream->active_buffer;
        int other_valid = (other == 0) ? stream->buffer_a_valid : stream->buffer_b_valid;
        
        if (other_valid > 0) {
            // Mark current buffer as needing refill
            *valid = 0;
            *consumed = 0;
            
            // Swap to other buffer
            stream->active_buffer = other;
            
            // Update pointers
            if (other == 0) {
                buf = stream->buffer_a;
                valid = &stream->buffer_a_valid;
                consumed = &stream->buffer_a_consumed;
            } else {
                buf = stream->buffer_b;
                valid = &stream->buffer_b_valid;
                consumed = &stream->buffer_b_consumed;
            }
            
            available = *valid - *consumed;
        } else if (stream->state == STREAM_STATE_EOF) {
            return -1;  // No more data
        } else {
            return 0;   // Data not ready yet, pump more
        }
    }
    
    if (available <= 0) {
        return (stream->state == STREAM_STATE_EOF) ? -1 : 0;
    }
    
    // Copy data
    size_t to_copy = (max_bytes < (size_t)available) ? max_bytes : (size_t)available;
    memcpy(dest, buf + *consumed, to_copy);
    *consumed += (int)to_copy;
    
    return (int)to_copy;
}

int sd_async_stream_available(sd_async_stream_t* stream) {
    if (!stream) return 0;
    
    if (stream->active_buffer == 0) {
        return stream->buffer_a_valid - stream->buffer_a_consumed;
    } else {
        return stream->buffer_b_valid - stream->buffer_b_consumed;
    }
}

bool sd_async_stream_eof(sd_async_stream_t* stream) {
    if (!stream) return true;
    return stream->state == STREAM_STATE_EOF && sd_async_stream_available(stream) <= 0;
}

int sd_async_stream_seek(sd_async_stream_t* stream, size_t position) {
    if (!stream || !stream->file) return -1;
    
    FRESULT fr = f_lseek(stream->file, (FSIZE_t)position);
    if (fr != FR_OK) {
        printf("[SD_ASYNC] Seek error %d\n", fr);
        stream->state = STREAM_STATE_ERROR;
        return -1;
    }
    
    stream->file_pos = position;
    
    // Invalidate both buffers
    stream->buffer_a_valid = 0;
    stream->buffer_b_valid = 0;
    stream->buffer_a_consumed = 0;
    stream->buffer_b_consumed = 0;
    stream->active_buffer = 0;
    stream->fill_offset = 0;
    
    // Start refilling
    if (position < stream->file_size) {
        stream->state = STREAM_STATE_FILLING_A;
    } else {
        stream->state = STREAM_STATE_EOF;
    }
    
    return 0;
}

void sd_async_stream_close(sd_async_stream_t* stream) {
    if (!stream) return;
    
    printf("[SD_ASYNC] Closing stream slot %d\n", stream->slot);
    
    if (stream->file) {
        pop_fs_close(stream->file);
        stream->file = NULL;
    }
    
    if (stream->buffer_a) {
        psram_free(stream->buffer_a);
        stream->buffer_a = NULL;
    }
    
    if (stream->buffer_b) {
        psram_free(stream->buffer_b);
        stream->buffer_b = NULL;
    }
    
    stream->state = STREAM_STATE_CLOSED;
}

size_t sd_async_stream_size(sd_async_stream_t* stream) {
    if (!stream) return 0;
    return stream->file_size;
}

size_t sd_async_stream_tell(sd_async_stream_t* stream) {
    if (!stream) return 0;
    
    // Current logical position = file_pos - data still buffered
    int buffered = 0;
    if (stream->active_buffer == 0) {
        buffered = stream->buffer_a_valid - stream->buffer_a_consumed;
        buffered += stream->buffer_b_valid;
    } else {
        buffered = stream->buffer_b_valid - stream->buffer_b_consumed;
        buffered += stream->buffer_a_valid;
    }
    
    size_t pos = (stream->file_pos > (size_t)buffered) ? 
                 (stream->file_pos - buffered) : 0;
    return pos;
}
