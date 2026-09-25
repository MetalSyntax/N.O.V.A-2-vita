/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifndef DATA_PATH
#define DATA_PATH "ux0:data/nova2/"
#endif

// Every boot writes a new file: ux0:data/nova2/logs/nova2_NNNN.log, where
// NNNN = (highest existing number) + 1. Written with sceIo (unbuffered) so
// the tail survives a crash.
#define LOG_DIR    DATA_PATH "logs"
#define LOG_PREFIX "nova2_"
#define LOG_EXT    ".log"

#define COLOR_RED    "\x1B[38;5;196m"
#define COLOR_PINK   "\x1B[38;5;212m"
#define COLOR_ORANGE "\x1B[38;5;202m"
#define COLOR_BLUE   "\x1B[38;5;32m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_CYAN   "\x1B[36m"

#define COLOR_END    "\033[0m"

static SceKernelLwMutexWork _log_mutex;
static atomic_bool _log_mutex_ready = ATOMIC_VAR_INIT(false);

static SceUID _log_fd = -1;
static bool _log_file_tried = false;

// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];

static int _log_next_index(void) {
    int max = 0;
    SceUID dfd = sceIoDopen(LOG_DIR);
    if (dfd < 0)
        return 1;

    SceIoDirent entry;
    size_t plen = strlen(LOG_PREFIX), elen = strlen(LOG_EXT);
    while (sceIoDread(dfd, &entry) > 0) {
        const char *name = entry.d_name;
        size_t len = strlen(name);
        if (len <= plen + elen || strncmp(name, LOG_PREFIX, plen) != 0 ||
            strcmp(name + len - elen, LOG_EXT) != 0)
            continue;
        int n = atoi(name + plen);
        if (n > max)
            max = n;
    }
    sceIoDclose(dfd);
    return max + 1;
}

static void _log_open_file(void) {
    _log_file_tried = true;
    sceIoMkdir(DATA_PATH, 0777);
    sceIoMkdir(LOG_DIR, 0777);

    char path[256];
    sceClibSnprintf(path, sizeof(path), LOG_DIR "/" LOG_PREFIX "%04d" LOG_EXT,
                    _log_next_index());
    _log_fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (_log_fd < 0)
        sceClibPrintf("Error: failed to open log file %s: 0x%x\n", path, _log_fd);
    else
        sceClibPrintf("Logging to %s\n", path);
}

void _log_print(int t, const char* fmt, ...) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return;
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);

    if (!_log_file_tried)
        _log_open_file();

    const char *color, *label;
    switch (t) {
        case LT_DEBUG:   color = COLOR_PINK;   label = "• debug  "; break;
        case LT_INFO:    color = COLOR_BLUE;   label = "ℹ info   "; break;
        case LT_WARN:    color = COLOR_ORANGE; label = "⚠ warning"; break;
        case LT_ERROR:   color = COLOR_RED;    label = "⨯ error  "; break;
        case LT_FATAL:   color = COLOR_RED;    label = "! fatal  "; break;
        case LT_SUCCESS: color = COLOR_GREEN;  label = "! success"; break;
        case LT_WAIT:    color = COLOR_CYAN;   label = "… waiting"; break;
        default:
            sceKernelUnlockLwMutex(&_log_mutex, 1);
            return;
    }

    va_list list;

    sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s%s%s  %s\n",
                    color, label, COLOR_END, fmt);
    va_start(list, fmt);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
    va_end(list);
    sceClibPrintf(buffer_b);

    if (_log_fd >= 0) {
        // Plain copy for the file: no ANSI colors.
        sceClibSnprintf(buffer_a, sizeof(buffer_a), "%s  %s\n", label, fmt);
        va_start(list, fmt);
        int len = sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
        va_end(list);
        if (len > (int)sizeof(buffer_b) - 1)
            len = sizeof(buffer_b) - 1;
        if (len > 0)
            sceIoWrite(_log_fd, buffer_b, len);
    }

    sceKernelUnlockLwMutex(&_log_mutex, 1);
}
