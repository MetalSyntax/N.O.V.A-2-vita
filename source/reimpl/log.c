#include <stdint.h>
/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/log.h"
#include "utils/logger.h"
#include <psp2/kernel/clib.h>
#include <stdlib.h>

// Mensajes INFO/DEBUG repetidos del juego ("== ATC ==", "ATC Texture Loading
// 0" x341 en el log 0020): cada línea es un sceIoWrite síncrono. Se loguean
// las primeras ALOG_REPEAT veces de cada texto y después se silencian.
#define ALOG_REPEAT 3
#define ALOG_SLOTS 256
static uint32_t alog_hash[ALOG_SLOTS];
static uint8_t alog_cnt[ALOG_SLOTS];

static int alog_quiet(const char *tag, const char *text) {
    uint32_t h = 2166136261u;
    for (const char *p = tag; p && *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    for (const char *p = text; p && *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    uint32_t i = h % ALOG_SLOTS;
    if (alog_hash[i] != h) {
        alog_hash[i] = h;
        alog_cnt[i] = 0;
    }
    if (alog_cnt[i] >= ALOG_REPEAT)
        return 1;
    alog_cnt[i]++;
    return 0;
}

#define print_common \
    if (prio < ANDROID_LOG_WARN && alog_quiet(tag, text)) \
        return 0; \
    switch (prio) { \
        case ANDROID_LOG_INFO: \
            l_info("[ALOG][%s] %s", tag, text); \
            break; \
        case ANDROID_LOG_WARN: \
            l_warn("[ALOG][%s] %s", tag, text); \
            break; \
        case ANDROID_LOG_ERROR: \
        case ANDROID_LOG_FATAL: \
            l_error("[ALOG][%s] %s", tag, text); \
            break; \
        case ANDROID_LOG_UNKNOWN: \
        case ANDROID_LOG_DEFAULT: \
        case ANDROID_LOG_VERBOSE: \
        case ANDROID_LOG_DEBUG: \
        case ANDROID_LOG_SILENT: \
        default: \
            l_debug("[ALOG][%s] %s", tag, text); \
            break; \
    }

int __android_log_write(int prio, const char* tag, const char* text) {
    print_common
    return 0;
}

int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    va_list list;
    char text[1024];

    va_start(list, fmt);
    sceClibVsnprintf(text, sizeof(text), fmt, list);
    va_end(list);

    print_common

    return 0;
}

int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap) {
    char text[1024];

    sceClibVsnprintf(text, sizeof(text), fmt, ap);

    print_common

    return 0;
}

void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
    if (fmt) {
        va_list list;
        char text[1024];

        va_start(list, fmt);
        sceClibVsnprintf(text, sizeof(text), fmt, list);
        va_end(list);

        l_fatal("[ALOG][ASSERT] %s", text);
    } else {
        if (cond) {
            l_fatal("[ALOG][ASSERT] Assertion failed: %s", cond);
        } else {
            l_fatal("[ALOG][ASSERT] Unspecified assertion failed");
        }
    }

    abort();
}
