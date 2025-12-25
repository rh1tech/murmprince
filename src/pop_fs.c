#include "pop_fs.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "diskio.h"

static FATFS g_fs;
static bool g_mounted = false;

bool pop_fs_init(void) {
    if (g_mounted) return true;

    // Ensure physical drive is initialized.
    (void)disk_initialize(0);

    // Mount as logical drive 0:
    FRESULT fr = f_mount(&g_fs, "0:", 1);
    if (fr != FR_OK) {
        return false;
    }
    g_mounted = true;
    return true;
}

const char* pop_fs_make_path(char* dst, size_t dst_size, const char* pop_path) {
    if (!dst || dst_size == 0) return "";
    if (!pop_path) pop_path = "";

    // Strip leading "./"
    while (pop_path[0] == '.' && pop_path[1] == '/') {
        pop_path += 2;
    }

    // If already has a drive prefix, return as-is.
    if (strchr(pop_path, ':') != NULL) {
        strncpy(dst, pop_path, dst_size - 1);
        dst[dst_size - 1] = '\0';
        return dst;
    }

    // Normalize leading slash.
    if (pop_path[0] == '/') {
        // "0:/..."
        snprintf(dst, dst_size, "0:%s", pop_path);
    } else {
        snprintf(dst, dst_size, "0:/%s", pop_path);
    }

    // Convert backslashes to slashes.
    for (char* p = dst; *p; ++p) {
        if (*p == '\\') *p = '/';
    }

    return dst;
}

static BYTE fatfs_mode_from_stdio(const char* mode) {
    if (!mode || !mode[0]) return FA_READ;

    bool want_read = strchr(mode, 'r') != NULL;
    bool want_write = strchr(mode, 'w') != NULL;
    bool want_append = strchr(mode, 'a') != NULL;
    bool plus = strchr(mode, '+') != NULL;

    BYTE m = 0;
    if (want_read || plus) m |= FA_READ;
    if (want_write || want_append || plus) m |= FA_WRITE;
    if (want_write) m |= FA_CREATE_ALWAYS;
    if (want_append) m |= FA_OPEN_APPEND;
    return m;
}

FIL* pop_fs_open(const char* pop_path, const char* mode) {
    if (!g_mounted && !pop_fs_init()) return NULL;

    char full[256];
    pop_fs_make_path(full, sizeof(full), pop_path);

    FIL* fil = (FIL*)calloc(1, sizeof(FIL));
    if (!fil) return NULL;

    FRESULT fr = f_open(fil, full, fatfs_mode_from_stdio(mode));
    if (fr != FR_OK) {
        free(fil);
        return NULL;
    }
    return fil;
}

size_t pop_fs_read(void* ptr, size_t size, size_t nmemb, FIL* fil) {
    if (!fil || !ptr) return 0;
    UINT br = 0;
    UINT to_read = (UINT)(size * nmemb);
    if (to_read == 0) return 0;

    FRESULT fr = f_read(fil, ptr, to_read, &br);
    if (fr != FR_OK) return 0;
    return (size > 0) ? (br / (UINT)size) : 0;
}

size_t pop_fs_write(const void* ptr, size_t size, size_t nmemb, FIL* fil) {
    if (!fil || !ptr) return 0;
    UINT bw = 0;
    UINT to_write = (UINT)(size * nmemb);
    if (to_write == 0) return 0;

    FRESULT fr = f_write(fil, ptr, to_write, &bw);
    if (fr != FR_OK) return 0;
    return (size > 0) ? (bw / (UINT)size) : 0;
}

int pop_fs_seek(FIL* fil, long offset, int whence) {
    if (!fil) return -1;

    FSIZE_t base = 0;
    if (whence == SEEK_SET) {
        base = 0;
    } else if (whence == SEEK_CUR) {
        base = f_tell(fil);
    } else if (whence == SEEK_END) {
        base = f_size(fil);
    } else {
        return -1;
    }

    FSIZE_t target = base + (FSIZE_t)offset;
    FRESULT fr = f_lseek(fil, target);
    return (fr == FR_OK) ? 0 : -1;
}

long pop_fs_tell(FIL* fil) {
    if (!fil) return -1;
    return (long)f_tell(fil);
}

int pop_fs_close(FIL* fil) {
    if (!fil) return 0;
    (void)f_close(fil);
    free(fil);
    return 0;
}

bool pop_fs_exists(const char* pop_path) {
    if (!g_mounted && !pop_fs_init()) return false;

    char full[256];
    pop_fs_make_path(full, sizeof(full), pop_path);

    FILINFO fno;
    FRESULT fr = f_stat(full, &fno);
    return fr == FR_OK;
}
bool pop_fs_mkdir(const char* pop_path) {
    if (!g_mounted && !pop_fs_init()) return false;

    char full[256];
    pop_fs_make_path(full, sizeof(full), pop_path);

    FRESULT fr = f_mkdir(full);
    // FR_OK = created, FR_EXIST = already exists (both are success)
    return fr == FR_OK || fr == FR_EXIST;
}

bool pop_fs_delete(const char* pop_path) {
    if (!g_mounted && !pop_fs_init()) return false;

    char full[256];
    pop_fs_make_path(full, sizeof(full), pop_path);

    FRESULT fr = f_unlink(full);
    return fr == FR_OK;
}