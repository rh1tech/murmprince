#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mount the SD card (FatFS) on drive 0.
// Returns true on success.
bool pop_fs_init(void);

// Reset mounted state (for re-entry after quit)
void pop_fs_reset(void);

// Convert a SDLPoP relative path (e.g. "data/PRINCE.DAT") to a FatFS path ("0:/data/PRINCE.DAT").
// dst must be large enough (POP_MAX_PATH in SDLPoP is typically 4096, but we keep it simple here).
const char* pop_fs_make_path(char* dst, size_t dst_size, const char* pop_path);

FIL* pop_fs_open(const char* pop_path, const char* mode);
size_t pop_fs_read(void* ptr, size_t size, size_t nmemb, FIL* fil);
size_t pop_fs_write(const void* ptr, size_t size, size_t nmemb, FIL* fil);
int pop_fs_seek(FIL* fil, long offset, int whence);
long pop_fs_tell(FIL* fil);
int pop_fs_close(FIL* fil);

bool pop_fs_exists(const char* pop_path);
bool pop_fs_mkdir(const char* pop_path);
bool pop_fs_delete(const char* pop_path);

#ifdef __cplusplus
}
#endif
