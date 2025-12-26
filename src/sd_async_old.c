/*
 * Async SD Card Queue - Implementation
 * 
 * Core 1 runs a worker loop that services file read requests from Core 0.
 * Uses spinlocks for thread-safe queue access.
 * Uses mutex for thread-safe SD card / FatFS access.
 * 
 * CRITICAL: SD reads are synchronized to HDMI vblank to avoid signal loss.
 * Core 1 worker functions are placed in RAM to avoid XIP flash contention.
 */

#include "sd_async.h"
#include "pop_fs.h"
#include "psram_allocator.h"
#include "HDMI.h"  // For vblank detection

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "pico/mutex.h"
#include "hardware/sync.h"

#include <string.h>
#include <stdio.h>

// ============================================================================
// Configuration
// ============================================================================

#define SD_ASYNC_MAX_STREAMS     4    // Max concurrent streams
#define SD_ASYNC_MAX_REQUESTS    8    // Max pending one-shot requests
#define SD_ASYNC_MAX_PATH        256  // Max path length

// Chunk size for SD reads - smaller chunks allow HDMI DMA to interleave
// 512 bytes = 1 SD card sector, gives good latency characteristics
#define SD_READ_CHUNK_SIZE       512

// Maximum time to wait for vblank before doing a read anyway (in loop iterations)
// Prevents audio buffer underrun if vblank is missed
#define SD_VBLANK_WAIT_TIMEOUT   50000

// ============================================================================
// Stream state machine
// ============================================================================

typedef enum {
    STREAM_STATE_CLOSED = 0,
    STREAM_STATE_OPENING,       // Core 1 is opening file
    STREAM_STATE_FILLING_A,     // Core 1 is filling buffer A
    STREAM_STATE_FILLING_B,     // Core 1 is filling buffer B
    STREAM_STATE_READY,         // Both buffers ready, idle
    STREAM_STATE_SEEKING,       // Core 1 is seeking
    STREAM_STATE_EOF,           // End of file reached
    STREAM_STATE_ERROR          // Error occurred
} stream_state_t;

struct sd_async_stream {
    volatile stream_state_t state;
    
    // File info
    FIL* file;
    size_t file_size;
    size_t file_pos;            // Current read position in file
    
    // Double buffer system
    uint8_t* buffer_a;
    uint8_t* buffer_b;
    volatile int buffer_a_valid;    // Bytes valid in buffer A
    volatile int buffer_b_valid;    // Bytes valid in buffer B
    volatile int buffer_a_consumed; // Bytes consumed from buffer A by reader
    volatile int buffer_b_consumed; // Bytes consumed from buffer B by reader
    volatile int active_buffer;     // 0=A, 1=B - which buffer Core 0 is reading from
    
    // Request tracking
    char path[SD_ASYNC_MAX_PATH];
    volatile size_t seek_target;
    
    // Slot index
    int slot;
};

// ============================================================================
// One-shot request structure
// ============================================================================

typedef enum {
    REQ_STATE_FREE = 0,
    REQ_STATE_PENDING,
    REQ_STATE_IN_PROGRESS,
    REQ_STATE_COMPLETE,
    REQ_STATE_ERROR
} req_state_t;

typedef struct {
    volatile req_state_t state;
    char path[SD_ASYNC_MAX_PATH];
    void* dest;
    size_t max_bytes;
    volatile int result;        // Bytes read or -1 on error
    sd_async_req_id id;
} oneshot_request_t;

// ============================================================================
// Global state
// ============================================================================

static struct sd_async_stream streams[SD_ASYNC_MAX_STREAMS];
static oneshot_request_t requests[SD_ASYNC_MAX_REQUESTS];
static volatile bool g_initialized = false;
static volatile bool g_core1_running = false;
static volatile sd_async_req_id g_next_req_id = 1;

// Debug counters visible to Core 0
static volatile uint32_t g_core1_loops = 0;
static volatile uint32_t g_core1_last_slot0_state = 0xFF;
static volatile uint32_t g_core1_processing_count = 0;

// Spinlock for protecting queue operations
static spin_lock_t* g_spinlock;
static uint32_t g_spinlock_num;

// Mutex for protecting SD card / FatFS access from both cores
static mutex_t g_sd_mutex;

// ============================================================================
// Core 1 worker
// ============================================================================

static void core1_worker(void);

// Helper: lock the spinlock
static inline uint32_t async_lock(void) {
    return spin_lock_blocking(g_spinlock);
}

// Helper: unlock the spinlock
static inline void async_unlock(uint32_t save) {
    spin_unlock(g_spinlock, save);
}

// ============================================================================
// Initialization
// ============================================================================

void sd_async_init(void) {
    if (g_initialized) return;
    
    printf("[SD_ASYNC] Initializing...\n");
    
    // Initialize structures
    memset(streams, 0, sizeof(streams));
    memset(requests, 0, sizeof(requests));
    
    for (int i = 0; i < SD_ASYNC_MAX_STREAMS; i++) {
        streams[i].state = STREAM_STATE_CLOSED;
        streams[i].slot = i;
    }
    
    // Claim a spinlock for queue operations
    g_spinlock_num = spin_lock_claim_unused(true);
    g_spinlock = spin_lock_instance(g_spinlock_num);
    
    // Initialize mutex for SD card access
    mutex_init(&g_sd_mutex);
    
    printf("[SD_ASYNC] Spinlock %u claimed, mutex initialized\n", g_spinlock_num);
    
    // Launch Core 1 worker
    printf("[SD_ASYNC] Launching Core 1 worker...\n");
    multicore_launch_core1(core1_worker);
    
    // Wait for Core 1 to signal ready
    while (!g_core1_running) {
        tight_loop_contents();
    }
    
    g_initialized = true;
    printf("[SD_ASYNC] Initialized OK\n");
}

bool sd_async_is_initialized(void) {
    return g_initialized;
}

// ============================================================================
// SD Mutex API
// ============================================================================

uint32_t sd_mutex_lock(void) {
    mutex_enter_blocking(&g_sd_mutex);
    return 0;  // Return value for API compatibility
}

void sd_mutex_unlock(uint32_t save) {
    (void)save;
    mutex_exit(&g_sd_mutex);
}

// ============================================================================
// Core 1 Worker - Main Loop
// NOTE: Core 1 functions placed in RAM (__not_in_flash_func) to avoid
// XIP flash contention with Core 0's HDMI DMA
// ============================================================================

static void __not_in_flash_func(process_stream_work)(sd_async_stream_t* s);
static void process_oneshot_work(oneshot_request_t* r);

// Helper: Wait for vblank or timeout
// Returns true if we're in vblank, false if we timed out
static inline bool __not_in_flash_func(wait_for_vblank_or_timeout)(void) {
    // If already in vblank, good to go
    if (graphics_is_in_vblank()) {
        return true;
    }
    
    // Wait for vblank with timeout to prevent audio buffer underrun
    for (volatile int i = 0; i < SD_VBLANK_WAIT_TIMEOUT; i++) {
        if (graphics_is_in_vblank()) {
            return true;
        }
        tight_loop_contents();
    }
    
    // Timed out - we'll do the read anyway to prevent audio underrun
    return false;
}

// Helper: Read from SD in small chunks, synchronized to vblank
// This allows HDMI DMA to run undisturbed during active video
// Returns total bytes read, or 0 on error
// NOTE: __not_in_flash_func puts this in RAM to avoid XIP contention
static size_t __not_in_flash_func(chunked_sd_read)(FIL* file, uint8_t* dest, size_t total_bytes) {
    size_t bytes_read = 0;
    
    while (bytes_read < total_bytes) {
        size_t chunk = total_bytes - bytes_read;
        if (chunk > SD_READ_CHUNK_SIZE) {
            chunk = SD_READ_CHUNK_SIZE;
        }
        
        // Wait for vblank before each chunk to avoid HDMI interference
        wait_for_vblank_or_timeout();
        
        UINT br = 0;
        mutex_enter_blocking(&g_sd_mutex);
        FRESULT fr = f_read(file, dest + bytes_read, (UINT)chunk, &br);
        mutex_exit(&g_sd_mutex);
        
        if (fr != FR_OK) {
            return 0;  // Error
        }
        
        bytes_read += br;
        
        if (br < chunk) {
            // EOF reached
            break;
        }
    }
    
    return bytes_read;
}

// Core 1 worker loop - placed in RAM to avoid XIP flash contention with Core 0
static void __not_in_flash_func(core1_worker)(void) __attribute__((noreturn));
static void __not_in_flash_func(core1_worker)(void) {
    // Don't use printf here - not thread-safe with USB stdio
    // Just signal ready
    g_core1_running = true;
    
    while (1) {
        g_core1_loops++;
        
        // Memory barrier to ensure we see updates from Core 0
        __dmb();
        
        // Track what we see for slot 0 state (for debugging)
        g_core1_last_slot0_state = streams[0].state;
        
        // Process all streams that need work
        for (int i = 0; i < SD_ASYNC_MAX_STREAMS; i++) {
            sd_async_stream_t* s = &streams[i];
            stream_state_t state = s->state;  // Read once
            
            if (state != STREAM_STATE_CLOSED && 
                state != STREAM_STATE_READY &&
                state != STREAM_STATE_EOF &&
                state != STREAM_STATE_ERROR) {
                g_core1_processing_count++;
                process_stream_work(s);
            }
            
            // Check if a ready stream needs buffer refill
            if (s->state == STREAM_STATE_READY) {
                // Check if the inactive buffer needs refilling
                int inactive = 1 - s->active_buffer;
                bool needs_fill = false;
                
                if (inactive == 0 && s->buffer_a_valid == 0 && s->file_pos < s->file_size) {
                    needs_fill = true;
                    s->state = STREAM_STATE_FILLING_A;
                } else if (inactive == 1 && s->buffer_b_valid == 0 && s->file_pos < s->file_size) {
                    needs_fill = true;
                    s->state = STREAM_STATE_FILLING_B;
                }
                
                if (needs_fill) {
                    process_stream_work(s);
                }
            }
        }
        
        // Process pending one-shot requests
        for (int i = 0; i < SD_ASYNC_MAX_REQUESTS; i++) {
            oneshot_request_t* r = &requests[i];
            if (r->state == REQ_STATE_PENDING) {
                r->state = REQ_STATE_IN_PROGRESS;
                process_oneshot_work(r);
            }
        }
        
        // Yield CPU briefly to avoid starving other cores
        // Don't use sleep_us() - it can conflict with Core 0's timer/DMA
        for (volatile int delay = 0; delay < 1000; delay++) {
            tight_loop_contents();
        }
    }
}

// ============================================================================
// Core 1 Worker - Stream Processing
// All FatFS operations are protected by the SD mutex
// NOTE: __not_in_flash_func puts this in RAM to avoid XIP contention
// ============================================================================

static void __not_in_flash_func(process_stream_work)(sd_async_stream_t* s) {
    switch (s->state) {
        case STREAM_STATE_OPENING: {
            // Open the file (with mutex protection)
            // NOTE: No printf here - not thread-safe on Core 1
            mutex_enter_blocking(&g_sd_mutex);
            s->file = pop_fs_open(s->path, "r");
            if (s->file) {
                s->file_size = f_size(s->file);
            }
            mutex_exit(&g_sd_mutex);
            
            if (!s->file) {
                s->state = STREAM_STATE_ERROR;
                break;
            }
            
            s->file_pos = 0;
            
            // Fill buffer A first
            s->state = STREAM_STATE_FILLING_A;
            process_stream_work(s);  // Tail-recurse to fill
            break;
        }
        
        case STREAM_STATE_FILLING_A: {
            if (!s->file || s->file_pos >= s->file_size) {
                s->buffer_a_valid = 0;
                s->state = (s->buffer_b_valid > 0) ? STREAM_STATE_READY : STREAM_STATE_EOF;
                break;
            }
            
            size_t to_read = SD_ASYNC_STREAM_BUFFER_BYTES;
            if (s->file_pos + to_read > s->file_size) {
                to_read = s->file_size - s->file_pos;
            }
            
            // Use chunked read to avoid HDMI DMA starvation
            size_t br = chunked_sd_read(s->file, s->buffer_a, to_read);
            
            if (br == 0 && to_read > 0) {
                s->state = STREAM_STATE_ERROR;
                break;
            }
            
            s->buffer_a_valid = (int)br;
            s->buffer_a_consumed = 0;
            s->file_pos += br;
            
            // If this was initial fill, also fill B
            if (s->buffer_b_valid == 0 && s->file_pos < s->file_size) {
                s->state = STREAM_STATE_FILLING_B;
                process_stream_work(s);
            } else {
                s->state = STREAM_STATE_READY;
            }
            break;
        }
        
        case STREAM_STATE_FILLING_B: {
            if (!s->file || s->file_pos >= s->file_size) {
                s->buffer_b_valid = 0;
                s->state = (s->buffer_a_valid > 0) ? STREAM_STATE_READY : STREAM_STATE_EOF;
                break;
            }
            
            size_t to_read = SD_ASYNC_STREAM_BUFFER_BYTES;
            if (s->file_pos + to_read > s->file_size) {
                to_read = s->file_size - s->file_pos;
            }
            
            // Use chunked read to avoid HDMI DMA starvation
            size_t br = chunked_sd_read(s->file, s->buffer_b, to_read);
            
            if (br == 0 && to_read > 0) {
                s->state = STREAM_STATE_ERROR;
                break;
            }
            
            s->buffer_b_valid = (int)br;
            s->buffer_b_consumed = 0;
            s->file_pos += br;
            s->state = STREAM_STATE_READY;
            break;
        }
        
        case STREAM_STATE_SEEKING: {
            if (!s->file) {
                s->state = STREAM_STATE_ERROR;
                break;
            }
            
            mutex_enter_blocking(&g_sd_mutex);
            FRESULT fr = f_lseek(s->file, (FSIZE_t)s->seek_target);
            mutex_exit(&g_sd_mutex);
            
            if (fr != FR_OK) {
                printf("[SD_ASYNC/Core1] Seek error %d\n", fr);
                s->state = STREAM_STATE_ERROR;
                break;
            }
            
            s->file_pos = s->seek_target;
            
            // Invalidate both buffers and refill
            s->buffer_a_valid = 0;
            s->buffer_b_valid = 0;
            s->buffer_a_consumed = 0;
            s->buffer_b_consumed = 0;
            s->active_buffer = 0;
            
            s->state = STREAM_STATE_FILLING_A;
            process_stream_work(s);
            break;
        }
        
        default:
            break;
    }
}

// ============================================================================
// Core 1 Worker - One-shot Processing
// ============================================================================

static void process_oneshot_work(oneshot_request_t* r) {
    mutex_enter_blocking(&g_sd_mutex);
    FIL* f = pop_fs_open(r->path, "r");
    if (!f) {
        mutex_exit(&g_sd_mutex);
        r->result = -1;
        r->state = REQ_STATE_ERROR;
        return;
    }
    
    size_t file_size = f_size(f);
    size_t to_read = (r->max_bytes < file_size) ? r->max_bytes : file_size;
    
    UINT br = 0;
    FRESULT fr = f_read(f, r->dest, (UINT)to_read, &br);
    pop_fs_close(f);
    mutex_exit(&g_sd_mutex);
    
    if (fr != FR_OK) {
        r->result = -1;
        r->state = REQ_STATE_ERROR;
        return;
    }
    
    r->result = (int)br;
    r->state = REQ_STATE_COMPLETE;
}

// ============================================================================
// Streaming API Implementation
// ============================================================================

sd_async_stream_t* sd_async_stream_open(const char* path) {
    if (!g_initialized) {
        printf("[SD_ASYNC] Not initialized!\n");
        return NULL;
    }
    
    // Find a free slot (mark it reserved but don't set OPENING yet)
    sd_async_stream_t* s = NULL;
    int slot_idx = -1;
    uint32_t save = async_lock();
    for (int i = 0; i < SD_ASYNC_MAX_STREAMS; i++) {
        if (streams[i].state == STREAM_STATE_CLOSED) {
            s = &streams[i];
            slot_idx = i;
            // Don't change state yet - we need to initialize first
            break;
        }
    }
    async_unlock(save);
    
    if (!s) {
        printf("[SD_ASYNC] No free stream slots!\n");
        return NULL;
    }
    
    // Allocate buffers from PSRAM
    s->buffer_a = (uint8_t*)psram_malloc(SD_ASYNC_STREAM_BUFFER_BYTES);
    s->buffer_b = (uint8_t*)psram_malloc(SD_ASYNC_STREAM_BUFFER_BYTES);
    
    if (!s->buffer_a || !s->buffer_b) {
        printf("[SD_ASYNC] Buffer allocation failed!\n");
        if (s->buffer_a) psram_free(s->buffer_a);
        if (s->buffer_b) psram_free(s->buffer_b);
        s->buffer_a = NULL;
        s->buffer_b = NULL;
        return NULL;
    }
    
    // Initialize ALL stream state BEFORE setting to OPENING
    s->file = NULL;
    s->file_size = 0;
    s->file_pos = 0;
    s->buffer_a_valid = 0;
    s->buffer_b_valid = 0;
    s->buffer_a_consumed = 0;
    s->buffer_b_consumed = 0;
    s->active_buffer = 0;
    s->seek_target = 0;
    
    strncpy(s->path, path, SD_ASYNC_MAX_PATH - 1);
    s->path[SD_ASYNC_MAX_PATH - 1] = '\0';
    
    // NOW set state to OPENING - Core 1 can start processing
    __dmb();  // Ensure all writes above are visible
    s->state = STREAM_STATE_OPENING;
    __dmb();  // Ensure state change is visible to Core 1
    
    printf("[SD_ASYNC] Opening stream: %s (slot=%d, state=%d)\n", path, s->slot, s->state);
    
    // Wait for Core 1 to open and fill initial buffers
    // This is a blocking wait on open, but reads will be non-blocking
    int wait_loops = 0;
    while (s->state == STREAM_STATE_OPENING || 
           s->state == STREAM_STATE_FILLING_A ||
           s->state == STREAM_STATE_FILLING_B) {
        tight_loop_contents();
        wait_loops++;
        if ((wait_loops & 0x3FFFF) == 0) {
            // Print Core 1 debug counters to diagnose
            printf("[SD_ASYNC] Still waiting, state=%d loops=%d core1_loops=%u core1_sees=%u core1_proc=%u\n", 
                   s->state, wait_loops, 
                   (unsigned)g_core1_loops, 
                   (unsigned)g_core1_last_slot0_state,
                   (unsigned)g_core1_processing_count);
        }
    }
    
    if (s->state == STREAM_STATE_ERROR) {
        printf("[SD_ASYNC] Stream open failed\n");
        psram_free(s->buffer_a);
        psram_free(s->buffer_b);
        s->buffer_a = NULL;
        s->buffer_b = NULL;
        s->state = STREAM_STATE_CLOSED;
        return NULL;
    }
    
    printf("[SD_ASYNC] Stream ready, size=%u\n", (unsigned)s->file_size);
    return s;
}

int sd_async_stream_read(sd_async_stream_t* stream, void* dest, size_t max_bytes) {
    if (!stream || !dest || max_bytes == 0) return 0;
    
    if (stream->state == STREAM_STATE_ERROR) return -1;
    
    // Get active buffer info
    uint8_t* buf;
    volatile int* valid;
    volatile int* consumed;
    
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
            return 0;   // Data not ready yet
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
    if (!stream || stream->state == STREAM_STATE_ERROR) return -1;
    
    stream->seek_target = position;
    stream->state = STREAM_STATE_SEEKING;
    
    // Block until seek completes
    while (stream->state == STREAM_STATE_SEEKING) {
        tight_loop_contents();
    }
    
    return (stream->state == STREAM_STATE_ERROR) ? -1 : 0;
}

void sd_async_stream_close(sd_async_stream_t* stream) {
    if (!stream) return;
    
    printf("[SD_ASYNC] Closing stream slot %d\n", stream->slot);
    
    // Wait for any pending operations
    while (stream->state == STREAM_STATE_FILLING_A ||
           stream->state == STREAM_STATE_FILLING_B ||
           stream->state == STREAM_STATE_SEEKING ||
           stream->state == STREAM_STATE_OPENING) {
        tight_loop_contents();
    }
    
    // Close file (with mutex protection)
    if (stream->file) {
        mutex_enter_blocking(&g_sd_mutex);
        pop_fs_close(stream->file);
        mutex_exit(&g_sd_mutex);
        stream->file = NULL;
    }
    
    // Free buffers
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
    return stream ? stream->file_size : 0;
}

size_t sd_async_stream_tell(sd_async_stream_t* stream) {
    if (!stream) return 0;
    
    // Calculate logical position (file position minus unconsumed buffer data)
    size_t buffered = 0;
    if (stream->active_buffer == 0) {
        buffered = stream->buffer_a_valid - stream->buffer_a_consumed;
        // Also count the other buffer if valid
        buffered += stream->buffer_b_valid;
    } else {
        buffered = stream->buffer_b_valid - stream->buffer_b_consumed;
        buffered += stream->buffer_a_valid;
    }
    
    if (stream->file_pos >= buffered) {
        return stream->file_pos - buffered;
    }
    return 0;
}

// ============================================================================
// One-shot API Implementation
// ============================================================================

sd_async_req_id sd_async_read_file(const char* path, void* dest, size_t max_bytes) {
    if (!g_initialized || !path || !dest) return SD_ASYNC_INVALID_REQ;
    
    uint32_t save = async_lock();
    
    // Find a free request slot
    oneshot_request_t* r = NULL;
    for (int i = 0; i < SD_ASYNC_MAX_REQUESTS; i++) {
        if (requests[i].state == REQ_STATE_FREE ||
            requests[i].state == REQ_STATE_COMPLETE ||
            requests[i].state == REQ_STATE_ERROR) {
            r = &requests[i];
            break;
        }
    }
    
    if (!r) {
        async_unlock(save);
        return SD_ASYNC_INVALID_REQ;
    }
    
    strncpy(r->path, path, SD_ASYNC_MAX_PATH - 1);
    r->path[SD_ASYNC_MAX_PATH - 1] = '\0';
    r->dest = dest;
    r->max_bytes = max_bytes;
    r->result = 0;
    r->id = g_next_req_id++;
    if (g_next_req_id == SD_ASYNC_INVALID_REQ) g_next_req_id = 1;
    r->state = REQ_STATE_PENDING;
    
    sd_async_req_id id = r->id;
    async_unlock(save);
    
    return id;
}

bool sd_async_is_complete(sd_async_req_id req) {
    if (req == SD_ASYNC_INVALID_REQ) return true;
    
    for (int i = 0; i < SD_ASYNC_MAX_REQUESTS; i++) {
        if (requests[i].id == req) {
            return requests[i].state == REQ_STATE_COMPLETE ||
                   requests[i].state == REQ_STATE_ERROR;
        }
    }
    return true;  // Not found = complete (or invalid)
}

int sd_async_get_result(sd_async_req_id req) {
    if (req == SD_ASYNC_INVALID_REQ) return -1;
    
    for (int i = 0; i < SD_ASYNC_MAX_REQUESTS; i++) {
        if (requests[i].id == req) {
            return requests[i].result;
        }
    }
    return -1;
}

void sd_async_cancel(sd_async_req_id req) {
    if (req == SD_ASYNC_INVALID_REQ) return;
    
    uint32_t save = async_lock();
    for (int i = 0; i < SD_ASYNC_MAX_REQUESTS; i++) {
        if (requests[i].id == req && requests[i].state == REQ_STATE_PENDING) {
            requests[i].state = REQ_STATE_FREE;
            break;
        }
    }
    async_unlock(save);
}
