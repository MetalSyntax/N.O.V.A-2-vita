/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021-2022 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"

#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/settings.h"

#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/kernel/clib.h>
#include <psp2/power.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <fios/fios.h>

// Base address for the Android .so to be loaded at
#define LOAD_ADDRESS 0x98000000
// libnova2.so is ~7.2 MB on disk; leave 32 MB before the renderer .so.
#define STORM_LOAD_ADDRESS (LOAD_ADDRESS + 0x2000000)

#ifndef DATA_PATH
#define DATA_PATH "ux0:data/nova2/"
#endif

extern so_module so_mod;
extern so_module storm_mod;
extern int storm_loaded;

void soloader_init_all() {
	// Launch `app0:configurator.bin` on `-config` init param
    sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    SceAppUtilAppEventParam eventParam;
    sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
    sceAppUtilReceiveAppEvent(&eventParam);
    if (eventParam.type == 0x05) {
        char buffer[2048];
        sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
        if (strstr(buffer, "-config"))
            sceAppMgrLoadExec("app0:/configurator.bin", NULL, NULL);
    }

    // Set default overclock values
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

#ifdef USE_SCELIBC_IO
    if (fios_init(DATA_PATH) == 0)
        l_success("FIOS initialized.");
#endif

    if (!module_loaded("kubridge")) {
        l_fatal("kubridge is not loaded.");
        fatal_error("Error: kubridge.skprx is not installed.");
    }
    l_success("kubridge check passed.");

    if (!file_exists(SO_PATH)) {
        fatal_error("Looks like you haven't installed the data files for this "
                    "port, or they are in an incorrect location. Please make "
                    "sure that you have %s file exactly at that path.", SO_PATH);
    }

    if (so_file_load(&so_mod, SO_PATH, LOAD_ADDRESS) < 0) {
        l_fatal("SO could not be loaded.");
        fatal_error("Error: could not load %s.", SO_PATH);
    }

    // libStormGLOFT.so is NOT a renderer: it only exports JNI_OnLoad, which
    // inline-hooks/GOT-patches libnova2 (gl*Tex*, glViewport, glShaderSource,
    // __android_log_*) using bionic internals (dlopen handle + 0x8c = base,
    // mprotect on /data/data/.../lib/libnova2.so). Under so_util that would
    // patch random memory, and vitaGL already is our GL layer -> skip it.
    // Build with -DLOAD_STORM_HOOKS to experiment with it anyway.
    storm_loaded = 0;
#ifdef LOAD_STORM_HOOKS
    if (file_exists(DATA_PATH "libStormGLOFT.so")) {
        if (so_file_load(&storm_mod, DATA_PATH "libStormGLOFT.so", STORM_LOAD_ADDRESS) < 0) {
            l_error("libStormGLOFT.so present but could not be loaded; continuing with libnova2 only.");
        } else {
            storm_loaded = 1;
            l_success("libStormGLOFT.so loaded.");
        }
    }
#endif

    settings_load();
    l_success("Settings loaded.");

    so_relocate(&so_mod);
    l_success("SO relocated.");

    resolve_imports(&so_mod);
    l_success("SO imports resolved.");

    if (storm_loaded) {
        so_relocate(&storm_mod);
        l_success("Storm SO relocated.");

        resolve_imports(&storm_mod);
        l_success("Storm SO imports resolved.");
    }

    so_patch();
    l_success("SO patched.");

    so_flush_caches(&so_mod);
    if (storm_loaded)
        so_flush_caches(&storm_mod);
    l_success("SO caches flushed.");

    so_initialize(&so_mod);
    if (storm_loaded)
        so_initialize(&storm_mod);
    l_success("SO initialized.");

    gl_preload();
    l_success("OpenGL preloaded.");

    jni_init();
    l_success("FalsoJNI initialized.");
}
