/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 */

#include <stdint.h>
#include <string.h>

#include <kubridge.h>
#include <so_util/so_util.h>

#include "utils/logger.h"
#include "utils/utils.h"

extern so_module so_mod;

// Not exported by so_util.h: allocates in the 64 KB RX block so_util reserves
// right below the .so (or the text cave after it), within `range` of `dst`.
uintptr_t so_alloc_arena(so_module *so, uintptr_t range, uintptr_t dst, size_t sz);

static void ret_void(void) {}

// Hook `sym` to `dst`, logging instead of crashing when the symbol is gone.
static void hook_sym(const char *sym, void *dst) {
    uintptr_t addr = so_symbol(&so_mod, sym);
    if (!addr) {
        l_warn("patch: %s not found, skipped", sym);
        return;
    }
    hook_addr(addr, (uintptr_t)dst);
    l_success("patch: hooked %s", sym);
}

/*
 * Gameloft ALicenseCheck DRM (same module Dungeon Hunter 2 bypasses in its
 * port). GLGame.nativeInit -> isUseNativeDRM() == 1 ->
 * ALicenseCheck_InitLicense -> LoadConfig, which reserves 256 KB of stack
 * (sub sp, #0x40000) and overflowed the main thread (log 0002 + dump:
 * data abort on the push at CallJNIFuncChar, SP 256 KB below the previous
 * boot's). LoadRMS/SaveRMS reserve 512 KB each and ValidateServer does HTTP.
 * isUseNativeDRM now returns 0 (jni_impl.c), these hooks cover any other
 * entry point (ValidateLicense is called from gameplay code).
 *
 * ALicenseCheck_InitLicense and ALicenseCheck_ValidateLicense are 4-byte
 * tail branches (b Init / b ValidateServer) packed right before the next
 * function: an 8-byte hook there would clobber LoadRMS / ConfigFile(). Hook
 * the branch targets instead.
 */
static void patch_drm(void) {
    hook_sym("_ZN13ALicenseCheck4InitEP7_JNIEnvP7_jclass", &ret_void);
    hook_sym("ALicenseCheck_InitLicenseVM", &ret_void);
    hook_sym("_ZN13ALicenseCheck14ValidateServerEb", &ret_void);
    hook_sym("_ZN13ALicenseCheck7LoadRMSEv", &ret1);
    hook_sym("_ZN13ALicenseCheck7SaveRMSEb", &ret1);
}

/*
 * boost's spinlock pool is inlined as ARMv5 `SWP Rt, Rt2, [Rn]` in
 * glitch::io::CMemoryReadFile (ctor/dtor/clone) and shared_ptr<char>::reset
 * -- i.e. on every file read. SWP is not available on the Vita's Cortex-A9
 * (Dungeon Hunter 2's port hit exactly this: undefined instruction 0x30002).
 *
 * Each site becomes `BL thunk`, and the thunk does the same exchange with
 * LDREX/STREX. Safe by construction, verified offline on all 27 sites:
 *  - all 4 functions are non-leaf (push {..., lr} in the prologue), so
 *    clobbering LR mid-body is harmless;
 *  - every SWP is followed by `cmp`, so the flags the thunk changes are dead.
 * The thunk preserves every register except Rt (ip is saved on the stack).
 */
static const uint32_t swp_sites[] = {
    0x4f3348, 0x4f3360, 0x4f338c, 0x4f3424, 0x4f3440, 0x4f346c,
    0x4f3590, 0x4f35a8, 0x4f35d4, 0x4f365c, 0x4f3674, 0x4f36a0,
    0x4f38c8, 0x4f38e0, 0x4f390c, 0x4f3988, 0x4f39a0, 0x4f39cc,
    0x4f3ab0, 0x4f3ac4, 0x4f3af0, 0x4f3bd4, 0x4f3bec, 0x4f3c18,
    0x4f3cb0, 0x4f3ccc, 0x4f3cf8,
};

#define SWP_MASK   0x0FB00FF0
#define SWP_MATCH  0x01000090 // swp (B bit = 22 masked out below)
#define REG_IP     12

static void patch_swp(void) {
    int patched = 0;
    for (unsigned i = 0; i < sizeof(swp_sites) / sizeof(swp_sites[0]); i++) {
        uint32_t *site = (uint32_t *)(so_mod.text_base + swp_sites[i]);
        uint32_t ins = *site;
        if ((ins & 0xFFB00FF0) != (0xE0000000 | SWP_MATCH) || (ins & (1 << 22))) {
            l_error("patch_swp: 0x%x is not a word SWP (0x%08x), skipped", swp_sites[i], ins);
            continue;
        }
        uint32_t rn = (ins >> 16) & 0xF, rt = (ins >> 12) & 0xF, rt2 = ins & 0xF;

        uint32_t thunk[] = {
            0xE52D0004 | (REG_IP << 12),                        // push {ip}
            0xF57FF05B,                                         // dmb ish
            0xE1900F9F | (rn << 16) | (rt << 12),               // 1: ldrex rt, [rn]
            0xE1800F90 | (rn << 16) | (REG_IP << 12) | rt2,     //    strex ip, rt2, [rn]
            0xE3500000 | (REG_IP << 16),                        //    cmp ip, #0
            0x1AFFFFFB,                                         //    bne 1b
            0xF57FF05B,                                         // dmb ish
            0xE49D0004 | (REG_IP << 12),                        // pop {ip}
            0xE12FFF1E,                                         // bx lr
        };

        uintptr_t t = so_alloc_arena(&so_mod, (1 << 24) - 1, (uintptr_t)site + 8, sizeof(thunk));
        if (!t) {
            l_error("patch_swp: no arena space for 0x%x", swp_sites[i]);
            continue;
        }
        uint32_t bl = 0xEB000000 | ((((intptr_t)t - ((intptr_t)site + 8)) >> 2) & 0xFFFFFF);
        kuKernelCpuUnrestrictedMemcpy((void *)t, thunk, sizeof(thunk));
        // so_flush_caches() only covers .text, not the arena block.
        kuKernelFlushCaches((void *)t, sizeof(thunk));
        kuKernelCpuUnrestrictedMemcpy(site, &bl, sizeof(bl));
        patched++;
    }
    l_success("patch_swp: %d/%d SWP sites replaced with LDREX/STREX thunks",
              patched, (int)(sizeof(swp_sites) / sizeof(swp_sites[0])));
}

/*
 * Fondos estáticos 800x480 -> pantalla completa 960x544 (2026-09-25).
 * Capturas kd/hd 2026-09-24: carga y título pintan el fondo a tamaño nativo
 * 800x480 arriba a la izquierda (barra negra de 160px a la derecha y 64 abajo),
 * mientras la UI (barra de progreso, textos) ya se reparte en g_ScreenW/H =
 * 960x544. GS_Loading::Render / GS_Splash::Render lo hacen con
 * CSprite::PaintFrame(bg, frame, 0, 0, 0, ...): ningún Is*Res() matchea
 * 960x544, así que no hay variante de fondo para esta resolución.
 *
 * El intento previo (zoom de glViewport 960x640) no podía funcionar: el fondo
 * se pinta bajo viewport 960x544 y el 960x640 es un FBO del juego 3D. Aquí se
 * toca SOLO el sprite: PaintFrame con frame de ~800x480 en (0,0) activa un
 * flag, y mientras dura, los draw2DRectangle de CSprite::PaintModule escalan
 * el rect destino (x*960/w, y*544/h). HUD, textos y el 3D no pasan por ahí.
 *
 * PaintFrame arranca con `push {r4-fp,lr}; mov sl,r3` (independientes del PC)
 * -> trampolín real en el arena, sin des-hookear en cada llamada (se llama
 * decenas de veces por frame para el HUD). Los 2 `bl draw2DRectangle` de
 * PaintModule se redirigen a un thunk `ldr pc` hacia el wrapper en C.
 */
#define PAINTFRAME_OFF      0x273038
#define DRAW2DRECT_OFF      0x5a5a14
static const uint32_t d2d_sites[] = { 0x272df8, 0x272e2c };

#define BG_SCREEN_W 960
#define BG_SCREEN_H 544

typedef struct { int x1, y1, x2, y2; } rect_i;

typedef void (*paintframe_fn)(void *spr, int frame, int x, int y, unsigned flags,
                              int ox, int oy, unsigned char alpha);
typedef void (*draw2drect_fn)(void *drv, const rect_i *dst, const rect_i *src,
                              const void *colors, const rect_i *clip);

static paintframe_fn paintframe_orig;
static draw2drect_fn draw2drect_orig;
static int bg_stretch;
static float bg_sx, bg_sy;

static inline int bg_scale(int v, float s) {
    // Mismo redondeo en ambos bordes: tiles contiguos no dejan costuras.
    float f = (float)v * s + 0.5f;
    return f >= 0.0f ? (int)f : (int)f - 1;
}

static void draw2drect_hook(void *drv, const rect_i *dst, const rect_i *src,
                            const void *colors, const rect_i *clip) {
    if (!bg_stretch) {
        draw2drect_orig(drv, dst, src, colors, clip);
        return;
    }
    rect_i d = { bg_scale(dst->x1, bg_sx), bg_scale(dst->y1, bg_sy),
                 bg_scale(dst->x2, bg_sx), bg_scale(dst->y2, bg_sy) };
    // s_clipRect está en coords sin escalar: el fondo va a pantalla completa.
    rect_i c = { 0, 0, BG_SCREEN_W, BG_SCREEN_H };
    draw2drect_orig(drv, &d, src, colors, &c);
}

static void paintframe_hook(void *spr, int frame, int x, int y, unsigned flags,
                            int ox, int oy, unsigned char alpha) {
    if (bg_stretch || x != 0 || y != 0 || flags != 0) {
        paintframe_orig(spr, frame, x, y, flags, ox, oy, alpha);
        return;
    }
    // Tabla de frames en spr+0x5c: {x, y, w, h} (short) por frame
    // (CSprite::GetFrameRect); escala del sprite en spr+0xa4.
    const int16_t *fr = *(const int16_t **)((uintptr_t)spr + 0x5c);
    float scale = *(const float *)((uintptr_t)spr + 0xa4);
    int w = fr ? (int)(fr[frame * 4 + 2] * scale) : 0;
    int h = fr ? (int)(fr[frame * 4 + 3] * scale) : 0;
    if (w < 780 || w > 820 || h < 460 || h > 500) {
        paintframe_orig(spr, frame, x, y, flags, ox, oy, alpha);
        return;
    }
    static void *last_spr;
    static int last_frame = -1;
    if (spr != last_spr || frame != last_frame) {
        last_spr = spr;
        last_frame = frame;
        l_info("[bg] stretch sprite %p frame %d (%dx%d) -> %dx%d",
               spr, frame, w, h, BG_SCREEN_W, BG_SCREEN_H);
    }
    bg_sx = (float)BG_SCREEN_W / (float)w;
    bg_sy = (float)BG_SCREEN_H / (float)h;
    bg_stretch = 1;
    paintframe_orig(spr, frame, x, y, flags, ox, oy, alpha);
    bg_stretch = 0;
}

static uintptr_t arena_code(uintptr_t near, const uint32_t *code, size_t sz) {
    uintptr_t t = so_alloc_arena(&so_mod, (1 << 24) - 1, near, sz);
    if (!t)
        return 0;
    kuKernelCpuUnrestrictedMemcpy((void *)t, code, sz);
    kuKernelFlushCaches((void *)t, sz);
    return t;
}

static void patch_bg_stretch(void) {
    uint32_t *pf = (uint32_t *)(so_mod.text_base + PAINTFRAME_OFF);
    if (pf[0] != 0xE92D4FF0 || pf[1] != 0xE1A0A003) {
        l_error("patch_bg: PaintFrame prologue changed (%08x %08x), skipped", pf[0], pf[1]);
        return;
    }
    for (unsigned i = 0; i < sizeof(d2d_sites) / sizeof(d2d_sites[0]); i++) {
        uint32_t ins = *(uint32_t *)(so_mod.text_base + d2d_sites[i]);
        int32_t off = ((int32_t)(ins << 8)) >> 6;
        if ((ins & 0xFF000000) != 0xEB000000 || d2d_sites[i] + 8 + off != DRAW2DRECT_OFF) {
            l_error("patch_bg: 0x%x is not bl draw2DRectangle (0x%08x), skipped",
                    d2d_sites[i], ins);
            return;
        }
    }

    // Trampolín: las 2 instrucciones que pisa el hook + salto a PaintFrame+8.
    uint32_t tramp[] = {
        0xE92D4FF0,                                 // push {r4-fp, lr}
        0xE1A0A003,                                 // mov sl, r3
        0xE51FF004,                                 // ldr pc, [pc, #-4]
        (uint32_t)(so_mod.text_base + PAINTFRAME_OFF + 8),
    };
    uintptr_t t = arena_code((uintptr_t)pf, tramp, sizeof(tramp));
    uint32_t thunk[] = { 0xE51FF004, (uint32_t)(uintptr_t)&draw2drect_hook };
    uintptr_t k = arena_code(so_mod.text_base + d2d_sites[0], thunk, sizeof(thunk));
    if (!t || !k) {
        l_error("patch_bg: no arena space, skipped");
        return;
    }
    paintframe_orig = (paintframe_fn)t;
    draw2drect_orig = (draw2drect_fn)(so_mod.text_base + DRAW2DRECT_OFF);

    for (unsigned i = 0; i < sizeof(d2d_sites) / sizeof(d2d_sites[0]); i++) {
        uintptr_t site = so_mod.text_base + d2d_sites[i];
        uint32_t bl = 0xEB000000 | ((((intptr_t)k - ((intptr_t)site + 8)) >> 2) & 0xFFFFFF);
        kuKernelCpuUnrestrictedMemcpy((void *)site, &bl, sizeof(bl));
    }
    hook_addr((uintptr_t)pf, (uintptr_t)&paintframe_hook);
    l_success("patch_bg: 800x480 background sprites stretched to %dx%d",
              BG_SCREEN_W, BG_SCREEN_H);
}

/*
 * Controles virtuales al 1% de opacidad (2026-09-26). Todo se maneja con los
 * botones físicos (main.c), pero los controles táctiles siguen activos.
 * `Draw(int alpha)` de cada ControlSchemeN pasa el alpha (0-255) a sus
 * controles (WalkJoystick/AnalogJoystick, CButtonShoot, CButtonSpr de salto)
 * y termina en el byte alfa del SColor de CSprite::PaintModule. Se
 * reemplaza el slot Draw en la vtable (sin trampolín) y se llama al original
 * con alpha*1%. El rayo es un CButtonSweep suelto de CLevelInterfaceNormal
 * (frames 0xa5/0xa6); CButtonSweep se usa en otros lados, así que solo se
 * atenúa la instancia con frame 0xa5 (+0x58). El HUD 3D (pausa, arma,
 * granada) muestra munición/cantidades y queda intacto.
 */
typedef void (*draw_fn)(void *self, int alpha);

static inline int dim_alpha(int a) {
    int d = (a + 50) / 100;
    return (a > 0 && d == 0) ? 1 : d;
}

// Busca `fn` en los primeros slots de la vtable `vt` y lo cambia por `hook`.
static draw_fn vt_swap(const char *vt_sym, const char *fn_sym, draw_fn hook) {
    uintptr_t *vt = (uintptr_t *)so_symbol(&so_mod, vt_sym);
    uintptr_t fn = so_symbol(&so_mod, fn_sym);
    if (!vt || !fn) {
        l_warn("patch_dim: %s / %s not found", vt_sym, fn_sym);
        return NULL;
    }
    for (int i = 2; i < 48; i++) {
        if (vt[i] == fn) {
            uintptr_t h = (uintptr_t)hook;
            kuKernelCpuUnrestrictedMemcpy(&vt[i], &h, sizeof(h));
            l_success("patch_dim: %s slot %d dimmed", vt_sym, i);
            return (draw_fn)fn;
        }
    }
    l_warn("patch_dim: %s not in %s", fn_sym, vt_sym);
    return NULL;
}

#define DIM_SCHEMES(X) X(0, "13ControlScheme") X(1, "14ControlScheme1") \
    X(2, "14ControlScheme2") X(3, "14ControlScheme3") X(4, "14ControlScheme4") \
    X(5, "14ControlScheme5") X(7, "14ControlScheme7") X(8, "14ControlScheme8")

#define DIM_DECL(n, m) \
    static draw_fn scheme##n##_draw; \
    static void scheme##n##_hook(void *self, int a) { scheme##n##_draw(self, dim_alpha(a)); }
DIM_SCHEMES(DIM_DECL)

static draw_fn sweep_draw;
static void sweep_hook(void *self, int a) {
    if (*(int *)((uint8_t *)self + 0x58) == 0xa5)
        a = dim_alpha(a);
    sweep_draw(self, a);
}

static void patch_dim_controls(void) {
#define DIM_APPLY(n, m) scheme##n##_draw = vt_swap("_ZTV" m, "_ZN" m "4DrawEi", scheme##n##_hook);
    DIM_SCHEMES(DIM_APPLY)
    sweep_draw = vt_swap("_ZTV12CButtonSweep", "_ZN12CButtonSweep4DrawEi", sweep_hook);
}

void so_patch(void) {
    patch_drm();
    patch_swp();
    patch_bg_stretch();
    patch_dim_controls();
}
