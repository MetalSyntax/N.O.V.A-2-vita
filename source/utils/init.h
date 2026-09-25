/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  init.h
 * @brief so-loader initialization routines.
 */

#ifndef SOLOADER_INIT_H
#define SOLOADER_INIT_H

#include <so_util/so_util.h>

#ifdef __cplusplus
extern "C" {
#endif

void resolve_imports(so_module *mod);

void so_patch();

void soloader_init_all();

// Second .so (renderer). Defined in main.c next to so_mod.
// storm_loaded == 0 when ux0:data/nova2/libStormGLOFT.so is absent;
// the game then falls back to libnova2's own GameRenderer symbols.
extern so_module storm_mod;
extern int storm_loaded;

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_INIT_H
