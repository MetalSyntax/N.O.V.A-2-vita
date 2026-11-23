/*
 * Crash guard -- see crash_guard.h.
 *
 * Everything that runs after a fault avoids malloc, stdio and the logger:
 * the crashed thread may hold their locks, and a corrupt heap (dump 0034,
 * double free in Vox) would crash the reporter too. Only static buffers,
 * sceClib* and sceIo*.
 */
#include "crash_guard.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <kubridge.h>
#include <vitaGL.h>
#include <stdarg.h>
#include <string.h>

#include <so_util/so_util.h>
#include "utils/dialog.h"
#include "utils/init.h"
#include "utils/logger.h"

#define REPORT_PATH "ux0:data/nova2/crash_report.txt"
#define STACK_SCAN_WORDS 1024
#define STACK_SCAN_HITS  24

extern so_module so_mod;
void crash_guard_cxx_init(void);

static volatile int g_crashed;
static volatile int g_render_parked;
static volatile int g_frames;
static SceUID g_ui_sema = -1;
static SceUID g_done_sema = -1;
static SceUID g_ui_thid = -1;
static SceUID g_main_thid = -1;
static uintptr_t g_eboot_lo, g_eboot_hi;

static char g_report[6144];
static int g_len;
static char g_msg[512];
static char g_where[160];

static void put(const char *fmt, ...) {
    if (g_len >= (int)sizeof(g_report) - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    sceClibVsnprintf(g_report + g_len, sizeof(g_report) - g_len, fmt, ap);
    va_end(ap);
    g_len += strlen(g_report + g_len);
}

static const char *so_func(const so_module *m, uint32_t off, uint32_t *sym_off) {
    for (int i = 0; i < m->num_dynsym; i++) {
        const Elf32_Sym *s = &m->dynsym[i];
        uint32_t lo = s->st_value & ~1u;
        if (ELF32_ST_TYPE(s->st_info) == STT_FUNC && lo && off >= lo && off < lo + s->st_size) {
            *sym_off = off - lo;
            return m->dynstr + s->st_name;
        }
    }
    return NULL;
}

// "libnova2.so+0x445448 (_ZN3vox7DataObj17UnregisterEmitterEx+0x70)",
// "eboot+0x17e4d6" or the raw address. Returns 1 if the address is code.
static int describe(char *out, int n, uint32_t a) {
    const so_module *mods[2] = { &so_mod, storm_loaded ? &storm_mod : NULL };
    const char *names[2] = { "libnova2.so", "libStormGLOFT.so" };
    for (int i = 0; i < 2; i++) {
        const so_module *m = mods[i];
        if (!m || a < m->text_base || a >= m->text_base + m->text_size)
            continue;
        uint32_t off = a - m->text_base, so;
        const char *f = so_func(m, off, &so);
        if (f)
            sceClibSnprintf(out, n, "%s+0x%x (%s+0x%x)", names[i], off, f, so);
        else
            sceClibSnprintf(out, n, "%s+0x%x", names[i], off);
        return 1;
    }
    if (a >= g_eboot_lo && a < g_eboot_hi) {
        sceClibSnprintf(out, n, "eboot 0x%08x (addr2line -e build/nova2)", a);
        return 1;
    }
    sceClibSnprintf(out, n, "0x%08x", a);
    return 0;
}

static void park_forever(void) {
    for (;;)
        sceKernelDelayThread(1000000);
}

static void write_report(void) {
    SceUID fd = sceIoOpen(REPORT_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, g_report, g_len);
        sceIoClose(fd);
    }
    sceClibPrintf("%s", g_report);
}

// Writes the report and shows the dialog; returns once the user closed it
// (or at once when there is no screen). The caller then lets the system crash
// for real so a .psp2dmp is still written.
static void report_and_show(const char *kind, const char *detail, uint32_t pc, uint32_t lr,
                            const uint32_t *regs, uint32_t far) {
    SceUID self = sceKernelGetThreadId();
    if (__sync_lock_test_and_set(&g_crashed, 1)) {
        // Another thread is already reporting; just stop this one.
        if (self == g_main_thid)
            g_render_parked = 1;
        park_forever();
    }

    char line[160];
    SceKernelThreadInfo ti;
    sceClibMemset(&ti, 0, sizeof(ti));
    ti.size = sizeof(ti);
    sceKernelGetThreadInfo(self, &ti);

    put("N.O.V.A 2 (PS Vita) crash report -- build " __DATE__ " " __TIME__ "\n\n");
    put("What : %s\n", kind);
    if (detail && detail[0])
        put("Info : %s\n", detail);
    put("Thread: '%s' (0x%08x)%s\n", ti.name, self, self == g_main_thid ? " [game/render thread]" : "");
    g_where[0] = 0;
    if (pc) {
        describe(g_where, sizeof(g_where), pc);
        put("PC   : %s\n", g_where);
    }
    if (lr) {
        describe(line, sizeof(line), lr);
        put("LR   : %s\n", line);
        if (!g_where[0])
            sceClibStrncpy(g_where, line, sizeof(g_where) - 1);
    }
    if (regs) {
        put("FAR  : 0x%08x (address that could not be accessed)\n\nRegisters:\n", far);
        for (int i = 0; i < 13; i++)
            put("  r%-2d 0x%08x%s", i, regs[i], (i % 4 == 3) ? "\n" : "");
        put("\n  sp  0x%08x  lr  0x%08x  pc  0x%08x\n", regs[13], regs[14], regs[15]);
    }

    // Call chain guess: code addresses found on the stack (same idea as the
    // toolkit's psp2dmp analyzer).
    uint32_t sp = regs ? regs[13] : (uint32_t)__builtin_frame_address(0);
    uint32_t lo = (uint32_t)ti.stack, hi = lo + ti.stackSize;
    if (sp >= lo && sp < hi) {
        put("\nStack (code addresses, most recent first):\n");
        int hits = 0;
        for (uint32_t *p = (uint32_t *)sp; (uint32_t)p < hi && p < (uint32_t *)sp + STACK_SCAN_WORDS
                                           && hits < STACK_SCAN_HITS; p++) {
            if (describe(line, sizeof(line), *p)) {
                put("  [sp+0x%03x] %s\n", (uint32_t)p - sp, line);
                hits++;
            }
        }
    }
    put("\nSend this file, the newest ux0:data/nova2/logs/nova2_NNNN.log and the\n"
        "newest ux0:data/psp2core-*.psp2dmp (written right after this) to the developer.\n");
    write_report();

    sceClibSnprintf(g_msg, sizeof(g_msg),
                    "N.O.V.A 2 has stopped because of an error.\n\n"
                    "%s\n%s\n\n"
                    "A report was saved to\n" REPORT_PATH "\n"
                    "Press OK: the system will then save a crash dump.\n"
                    "Please send both to the developer.",
                    kind, g_where);

    if (self == g_ui_thid || g_ui_sema < 0 || !g_frames)
        return; // no screen to show it on (crash during boot or in the reporter)
    if (self == g_main_thid)
        g_render_parked = 1;
    sceKernelSignalSema(g_ui_sema, 1);
    sceKernelWaitSema(g_done_sema, 1, NULL);
}

void crash_fatal(const char *kind, const char *detail, uint32_t pc, uint32_t lr,
                 const uint32_t *regs, uint32_t far) {
    report_and_show(kind, detail, pc, lr, regs, far);
    // Software error (abort, exception, assert): fault on purpose with the
    // handler gone, so the system writes a .psp2dmp of this thread.
    kuKernelReleaseAbortHandler();
    *(volatile uint32_t *)0 = 0xDEADC0DE;
    sceKernelExitProcess(0);
    __builtin_unreachable();
}

void crash_guard_poll(void) {
    // Self-test: create ux0:data/nova2/crash_test -> a NULL write ~10 s in.
    static int test_frames = -1;
    if (test_frames < 0) {
        SceIoStat st;
        test_frames = sceIoGetstat("ux0:data/nova2/crash_test", &st) >= 0 ? 600 : 0;
    } else if (test_frames > 0 && --test_frames == 0) {
        l_error("crash_guard: crash_test file found, forcing a data abort");
        *(volatile int *)0 = 0;
    }

    g_frames = 1;
    if (g_crashed) {
        g_render_parked = 1;
        park_forever();
    }
}

static int ui_thread(SceSize args, void *argp) {
    sceKernelWaitSema(g_ui_sema, 1, NULL);
    // Let the render thread stop at its next frame; if it is blocked (e.g.
    // waiting on the crashed thread) it is not touching the GPU anyway.
    for (int i = 0; i < 150 && !g_render_parked; i++)
        sceKernelDelayThread(10000);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, 960, 544);
    init_msg_dialog(g_msg);
    while (!get_msg_dialog_result()) {
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        vglSwapBuffers(GL_TRUE);
    }
    sceKernelSignalSema(g_done_sema, 1);
    park_forever();
    return 0;
}

static void abort_handler(KuKernelAbortContext *c) {
    report_and_show(c->abortType == KU_KERNEL_ABORT_TYPE_DATA_ABORT
                        ? "Data abort (invalid memory access)"
                        : "Prefetch abort (jump to invalid code)",
                    NULL, c->pc, c->lr, &c->r0, c->FAR);
    // Returning restores the faulting context; without our handler the same
    // instruction faults again and the system writes the usual .psp2dmp with
    // the ORIGINAL registers and stack.
    kuKernelReleaseAbortHandler();
}

void crash_guard_init(void) {
    g_main_thid = sceKernelGetThreadId();

    SceKernelModuleInfo mi;
    sceClibMemset(&mi, 0, sizeof(mi));
    mi.size = sizeof(mi);
    SceUID mod = sceKernelGetModuleIdByAddr((void *)&crash_guard_init);
    if (mod >= 0 && sceKernelGetModuleInfo(mod, &mi) >= 0) {
        g_eboot_lo = (uintptr_t)mi.segments[0].vaddr;
        g_eboot_hi = g_eboot_lo + mi.segments[0].memsz;
    }

    g_ui_sema = sceKernelCreateSema("crash_ui", 0, 0, 1, NULL);
    g_done_sema = sceKernelCreateSema("crash_done", 0, 0, 1, NULL);
    g_ui_thid = sceKernelCreateThread("crash_ui", ui_thread, 0x10000100, 0x10000, 0, 0, NULL);
    if (g_ui_thid >= 0)
        sceKernelStartThread(g_ui_thid, 0, NULL);
    else
        g_ui_sema = -1;

    int r = kuKernelRegisterAbortHandler(abort_handler, NULL, NULL);
    if (r < 0)
        l_warn("crash_guard: kuKernelRegisterAbortHandler failed 0x%x (old kubridge?), "
               "memory faults will only produce a psp2dmp", r);

    crash_guard_cxx_init();
    l_success("crash_guard: ready (report -> " REPORT_PATH ")");
}

// Every abort() in the eboot (newlib, vitaGL, libstdc++) lands here via
// -Wl,--wrap=abort.
void __wrap_abort(void) {
    crash_fatal("abort() called", NULL, 0, (uint32_t)__builtin_return_address(0), NULL, 0);
}
