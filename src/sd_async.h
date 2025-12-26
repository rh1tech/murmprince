/*
 * Async SD Card Queue - Header
 * 
 * Provides asynchronous file reading on Core 0 using a cooperative pump model.
 * SD operations happen incrementally during sd_async_pump() calls, which should
 * be called frequently from the main game loop.
 * 
 * This approach avoids Core 1 entirely, eliminating cross-core bus contention
 * that was causing HDMI signal loss.
 * 
 * Usage for streaming (e.g., MIDI audio):
 *   1. Call sd_async_stream_open() to start streaming a file
 *   2. Call sd_async_pump() frequently from main loop (does incremental SD I/O)
 *   3. Call sd_async_stream_read() to get data (non-blocking, double-buffered)
 *   4. Call sd_async_stream_close() when done
 * 
 * The internal double-buffering ensures one buffer is being filled incrementally
 * while the other is being read from.
 */

#ifndef SD_ASYNC_H
#define SD_ASYNC_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

// Stream buffer size (samples per buffer, stereo 16-bit = 4 bytes per sample)
// Keep this large enough to cover SD latency but small enough for memory
#define SD_ASYNC_STREAM_BUFFER_SAMPLES 2048
#define SD_ASYNC_STREAM_BUFFER_BYTES   (SD_ASYNC_STREAM_BUFFER_SAMPLES * 4)

// How many bytes to read per pump call (keeps individual SD ops short)
#define SD_ASYNC_PUMP_CHUNK_SIZE       512

// Stream handle for async file streaming
typedef struct sd_async_stream sd_async_stream_t;

// Initialize the async SD subsystem (no Core 1 worker - uses pump model)
// Call this once after pop_fs_init() and before any async operations
void sd_async_init(void);

// Check if async SD is initialized
bool sd_async_is_initialized(void);

// Pump function - call this frequently from Core 0 main loop
// Does incremental SD I/O work (reads one small chunk per call)
// Returns true if there's more work pending, false if idle
bool sd_async_pump(void);

// ============================================================================
// Streaming API (for audio/continuous reads)
// ============================================================================

// Open a file for async streaming
// Returns NULL on failure
// The file will be opened (synchronously) but buffers filled via pump()
sd_async_stream_t* sd_async_stream_open(const char* path);

// Read data from stream (non-blocking)
// Copies up to max_bytes from the ready buffer to dest
// Returns number of bytes actually copied (may be 0 if buffer not ready yet)
// Returns -1 on end-of-file or error
int sd_async_stream_read(sd_async_stream_t* stream, void* dest, size_t max_bytes);

// Check how many bytes are available in the current buffer (non-blocking)
int sd_async_stream_available(sd_async_stream_t* stream);

// Check if stream has reached end of file
bool sd_async_stream_eof(sd_async_stream_t* stream);

// Seek to position in file (synchronous - blocks briefly)
// Returns 0 on success, -1 on error
int sd_async_stream_seek(sd_async_stream_t* stream, size_t position);

// Close stream and release resources
void sd_async_stream_close(sd_async_stream_t* stream);

// Get total file size
size_t sd_async_stream_size(sd_async_stream_t* stream);

// Get current position
size_t sd_async_stream_tell(sd_async_stream_t* stream);

// ============================================================================
// SD Mutex API - for protecting FatFS access (single-core, but keeps API)
// ============================================================================

// Lock the SD mutex - call before any pop_fs_* operations outside of async
uint32_t sd_mutex_lock(void);

// Unlock the SD mutex
void sd_mutex_unlock(uint32_t save);

// ============================================================================
// One-shot read API (for loading entire files)
// ============================================================================

// Request ID for tracking async reads
typedef uint32_t sd_async_req_id;
#define SD_ASYNC_INVALID_REQ 0

// Submit a one-shot file read request
// Returns request ID, or SD_ASYNC_INVALID_REQ on failure
// The read will happen asynchronously on Core 1
sd_async_req_id sd_async_read_file(const char* path, void* dest, size_t max_bytes);

// Check if a one-shot request is complete
// Returns true if done, false if still pending
bool sd_async_is_complete(sd_async_req_id req);

// Get the result of a completed one-shot request
// Returns bytes read, or -1 on error
// Only valid after sd_async_is_complete() returns true
int sd_async_get_result(sd_async_req_id req);

// Cancel a pending request (if not yet started)
void sd_async_cancel(sd_async_req_id req);

#ifdef __cplusplus
}
#endif

#endif // SD_ASYNC_H
