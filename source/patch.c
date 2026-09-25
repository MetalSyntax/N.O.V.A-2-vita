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

void so_patch(void) {
    patch_drm();
    patch_swp();
}
