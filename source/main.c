#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <stdint.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

int _newlib_heap_size_user = 256 * 1024 * 1024;

// The engine has 80 KB stack frames on its normal paths (and 256-512 KB ones
// in ALicenseCheck); the default 256 KB main thread overflowed on boot #2.
// Same value as the other working ports (Advena, ILLUSIA, Inotia3, PES 2012).
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;
so_module storm_mod;
int storm_loaded = 0;

// JNI_OnLoad does (*vm)->GetEnv(vm, &env, JNI_VERSION_1_4) right away, so it
// needs FalsoJNI's real JavaVM (a NULL slot crashed at JNI_OnLoad+0x3c,
// ldr pc,[r3,#24] with r3=0). Every native entry point gets FalsoJNI's JNIEnv.
#define ENV ((void *)&jni)

#define VITA_W 960
#define VITA_H 544

// Mundo lógico del engine: 960x640 (ver glutil.h). Se le reporta ESTO en
// nativeInit/Resize (Is960x640Res TRUE -> layouts de 960); EGL y el display
// reales siguen en 960x544. Touch: el engine hit-testea en lógico.
#define ENG_W 960
#define ENG_H 640
#define TOUCH_LOGICAL_Y(py) ((py) * ENG_H / VITA_H)

// ---- Input: touch (5 stable slots, never the raw hw id -- see
// psvita-porting input_handling.md) + physical buttons as Android keycodes.
// GLGame.onKeyDown/Up forwards keycodes raw to nativeSetOnKeyDown/Up; the
// game is Xperia-Play optimized so gamepad BUTTON_* codes are used too.
//
// Sticks (2026-09-25): el engine no tiene path analógico de keys: mover =
// `AnalogJoystick`, mirar = `FullScreenRotateJoystick` (ver glutil.h, mundo
// 960x640). Anclas y botones en ESPACIO LÓGICO del engine:
//   joystick físico ~(140,440) -> lógico (140,517); fire (886,428) -> (886,503);
//   botón acción esq.inf. (921,482) -> (921,567) (medidos a píxel en kb).
// El ancla de move es ADAPTATIVA: promedia los press reales de la mitad
// izquierda (ahí el usuario camina con el dedo), por si el círculo se mueve
// entre niveles. Slots: 0-2 dedos, 3 move, 4 look, 5 fire/acción.
#define MOVE_SLOT 3
#define LOOK_SLOT 4
#define FIRE_SLOT 5
#define N_SLOTS 6
#define MOVE_ANCHOR_X 140
#define MOVE_ANCHOR_Y 517
#define LOOK_ANCHOR_X 720
#define LOOK_ANCHOR_Y 320
#define FIRE_X 886
#define FIRE_Y 503
#define ACT_X 921
#define ACT_Y 567
#define STICK_RADIUS 90
#define STICK_ENGAGE 0.30f
#define STICK_RELEASE 0.15f
// Paso máximo por frame del drag sintético: el salto directo ancla->radio
// en un frame giraba la cámara violentamente (reporte 0009). Con pasos de 12
// la velocidad de giro queda proporcional a la deflexión.
#define SYN_STEP 12
// Frames que dura el tap de acción (Cross) antes del release automático.
#define ACT_FRAMES 6
static void (* touch_pressed)(void *env, void *cls, int x, int y, int id) = NULL;
static void (* touch_moved)(void *env, void *cls, int x, int y, int id) = NULL;
static void (* touch_released)(void *env, void *cls, int x, int y, int id) = NULL;
static void (* key_down)(void *env, void *cls, int code) = NULL;
static void (* key_up)(void *env, void *cls, int code) = NULL;

static int slot_hw[N_SLOTS] = { -1, -1, -1, -1, -1, -1 };
static int slot_x[N_SLOTS] = { -1, -1, -1, -1, -1, -1 };
static int slot_y[N_SLOTS] = { -1, -1, -1, -1, -1, -1 };
static uint32_t old_pad = 0;

// Synthetic-drag state per reserved slot: 0 = free, 1 = owned by stick.
static int syn_owned[N_SLOTS] = { 0, 0, 0, 0, 0, 0 };
static int syn_x[N_SLOTS] = { 0, 0, 0, 0, 0, 0 };
static int syn_y[N_SLOTS] = { 0, 0, 0, 0, 0, 0 };

// Ancla adaptativa del joystick de move, en coords FÍSICAS (se convierte a
// lógico al usar). Semilla = medida a píxel (140,440); cada press real en la
// mitad izquierda la acerca al punto real (ahí el usuario camina con el dedo).
static int move_anchor_px = MOVE_ANCHOR_X;
static int move_anchor_py = MOVE_ANCHOR_Y * VITA_H / ENG_H;

// Drives one reserved slot from one stick. (cx,cy) = anchor in ENGINE LOGICAL
// coords, (dx,dy) = deflection in -1..1. Engage/release with hysteresis; the
// drag point walks toward its target SYN_STEP px/frame so full deflection
// turns at a proportional rate instead of one violent 90px jump.
static void syn_stick(int slot, int cx, int cy, float dx, float dy) {
    float mag = __builtin_sqrtf(dx * dx + dy * dy);
    if (!syn_owned[slot]) {
        if (mag < STICK_ENGAGE)
            return;
        syn_owned[slot] = 1;
        syn_x[slot] = cx;
        syn_y[slot] = cy;
        slot_hw[slot] = -2; // marker: owned by stick, not a finger
        slot_x[slot] = cx;
        slot_y[slot] = cy;
        touch_pressed(ENV, NULL, cx, cy, slot);
        l_error("[syntouch] slot %d press(%d, %d)", slot, cx, cy);
    }
    if (mag < STICK_RELEASE) {
        syn_owned[slot] = 0;
        touch_released(ENV, NULL, syn_x[slot], syn_y[slot], slot);
        l_error("[syntouch] slot %d release", slot);
        slot_hw[slot] = -1;
        slot_x[slot] = -1;
        slot_y[slot] = -1;
        return;
    }
    if (mag > 1.0f) {
        dx /= mag;
        dy /= mag;
    }
    int tx = cx + (int)(dx * STICK_RADIUS);
    int ty = cy + (int)(dy * STICK_RADIUS);
    int nx = syn_x[slot], ny = syn_y[slot];
    int ddx = tx - nx, ddy = ty - ny;
    if (ddx > SYN_STEP) nx += SYN_STEP; else if (ddx < -SYN_STEP) nx -= SYN_STEP; else nx = tx;
    if (ddy > SYN_STEP) ny += SYN_STEP; else if (ddy < -SYN_STEP) ny -= SYN_STEP; else ny = ty;
    if (nx != syn_x[slot] || ny != syn_y[slot]) {
        syn_x[slot] = nx;
        syn_y[slot] = ny;
        slot_x[slot] = nx;
        slot_y[slot] = ny;
        touch_moved(ENV, NULL, nx, ny, slot);
    }
}

// Fire (R1, hold) y botón de acción (Cross, tap) como touches sintéticos en
// FIRE_SLOT, coords lógicas. fire tiene prioridad: si se dispara, el tap de
// acción se ignora ese frame.
static int act_frames_left = 0;

static void syn_buttons(uint32_t buttons) {
    int firing = (buttons & SCE_CTRL_R1) != 0;
    int act = (buttons & SCE_CTRL_CROSS) != 0;
    if (firing) {
        // Si había un tap de acción a medias, se suelta antes del FIRE para
        // no dejar un press colgado en coords de acción.
        if (syn_owned[FIRE_SLOT] && slot_x[FIRE_SLOT] != FIRE_X) {
            touch_released(ENV, NULL, slot_x[FIRE_SLOT], slot_y[FIRE_SLOT], FIRE_SLOT);
            syn_owned[FIRE_SLOT] = 0;
        }
        if (!syn_owned[FIRE_SLOT]) {
            syn_owned[FIRE_SLOT] = 1;
            slot_hw[FIRE_SLOT] = -2;
            slot_x[FIRE_SLOT] = FIRE_X;
            slot_y[FIRE_SLOT] = FIRE_Y;
            touch_pressed(ENV, NULL, FIRE_X, FIRE_Y, FIRE_SLOT);
            l_error("[syntouch] slot %d FIRE press", FIRE_SLOT);
        }
        act_frames_left = 0;
        return;
    }
    if (syn_owned[FIRE_SLOT] && slot_x[FIRE_SLOT] == FIRE_X) {
        syn_owned[FIRE_SLOT] = 0;
        touch_released(ENV, NULL, FIRE_X, FIRE_Y, FIRE_SLOT);
        slot_hw[FIRE_SLOT] = -1;
        slot_x[FIRE_SLOT] = -1;
        slot_y[FIRE_SLOT] = -1;
    }
    if (act && act_frames_left == 0 && !syn_owned[FIRE_SLOT]) {
        syn_owned[FIRE_SLOT] = 1;
        slot_hw[FIRE_SLOT] = -2;
        slot_x[FIRE_SLOT] = ACT_X;
        slot_y[FIRE_SLOT] = ACT_Y;
        touch_pressed(ENV, NULL, ACT_X, ACT_Y, FIRE_SLOT);
        act_frames_left = ACT_FRAMES;
    }
    if (act_frames_left > 0 && !firing) {
        if (--act_frames_left == 0 && syn_owned[FIRE_SLOT]) {
            syn_owned[FIRE_SLOT] = 0;
            touch_released(ENV, NULL, ACT_X, ACT_Y, FIRE_SLOT);
            slot_hw[FIRE_SLOT] = -1;
            slot_x[FIRE_SLOT] = -1;
            slot_y[FIRE_SLOT] = -1;
        }
    }
}

static const struct { uint32_t sce; int android; } pad_map[] = {
    { SCE_CTRL_UP, 19 },       // KEYCODE_DPAD_UP
    { SCE_CTRL_DOWN, 20 },     // KEYCODE_DPAD_DOWN
    { SCE_CTRL_LEFT, 21 },     // KEYCODE_DPAD_LEFT
    { SCE_CTRL_RIGHT, 22 },    // KEYCODE_DPAD_RIGHT
    // Cross/R1 NO van por keys: son touches sintéticos (acción/fire en
    // FIRE_SLOT, ver syn_buttons) porque el engine ignora las keys en juego.
    { SCE_CTRL_CIRCLE, 97 },   // KEYCODE_BUTTON_B
    { SCE_CTRL_SQUARE, 99 },   // KEYCODE_BUTTON_X
    { SCE_CTRL_TRIANGLE, 100 },// KEYCODE_BUTTON_Y
    { SCE_CTRL_L1, 102 },      // KEYCODE_BUTTON_L1
    { SCE_CTRL_START, 108 },   // KEYCODE_BUTTON_START
    { SCE_CTRL_SELECT, 109 },  // KEYCODE_BUTTON_SELECT
};

static void poll_input(void) {
    // Touch: front panel reports 1920x1088, game surface is 960x544 physical.
    // El engine hit-testea en su mundo lógico 960x640: todo evento real se
    // convierte (x igual, y*640/544). Solo se loguea el press (los moves
    // saturarían); coords del log = físicas.
    // Slots 3/4/5 reservados (sticks/fire): los dedos solo usan 0-2.
    SceTouchData touch;
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    int seen[N_SLOTS] = { 0, 0, 0, 0, 0, 0 };
    for (int r = 0; r < touch.reportNum && r < N_SLOTS; r++) {
        int hw = touch.report[r].id;
        int x = touch.report[r].x / 2;
        int y = touch.report[r].y / 2;
        int slot = -1;
        for (int s = 0; s < N_SLOTS; s++)
            if (slot_hw[s] == hw) { slot = s; break; }
        if (slot == -1) {
            for (int s = 0; s < N_SLOTS; s++)
                if (s != MOVE_SLOT && s != LOOK_SLOT && s != FIRE_SLOT && slot_hw[s] == -1) { slot = s; break; }
            if (slot == -1)
                continue;
            slot_hw[slot] = hw;
            slot_x[slot] = -1;
        }
        seen[slot] = 1;
        int ly = TOUCH_LOGICAL_Y(y);
        if (slot_x[slot] == -1) {
            touch_pressed(ENV, NULL, x, ly, slot);
            l_error("[touch] press(%d, %d) slot %d", x, y, slot);
            // Ancla adaptativa: press en mitad izquierda = donde camina el
            // dedo (dentro del círculo sí o sí). Media móvil suave.
            if (x < 400 && !syn_owned[MOVE_SLOT]) {
                move_anchor_px = (move_anchor_px * 3 + x) / 4;
                move_anchor_py = (move_anchor_py * 3 + y) / 4;
            }
        } else if (slot_x[slot] != x || slot_y[slot] != ly)
            touch_moved(ENV, NULL, x, ly, slot);
        slot_x[slot] = x;
        slot_y[slot] = ly;
    }
    for (int s = 0; s < N_SLOTS; s++) {
        if (slot_hw[s] != -1 && slot_hw[s] != -2 && !seen[s]) {
            touch_released(ENV, NULL, slot_x[s], slot_y[s], s);
            slot_x[s] = -1; slot_y[s] = -1; slot_hw[s] = -1;
        }
    }

    // Buttons with edge detection (no auto-repeat KeyDown).
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    for (unsigned i = 0; i < sizeof(pad_map) / sizeof(pad_map[0]); i++) {
        int now = (pad.buttons & pad_map[i].sce) != 0;
        int before = (old_pad & pad_map[i].sce) != 0;
        if (now && !before)
            key_down(ENV, NULL, pad_map[i].android);
        else if (!now && before)
            key_up(ENV, NULL, pad_map[i].android);
    }
    old_pad = pad.buttons;
    syn_buttons(pad.buttons);

    // Sticks need ANALOG sampling (set in main); in DIGITAL mode lx/ly read
    // a constant 128. Range 0..255, center 128, y-down (ly<128 = UP).
    // El ancla de move va en lógico (ancla física adaptativa convertida).
    float mdx = ((float)pad.lx - 128.0f) / 128.0f;
    float mdy = ((float)pad.ly - 128.0f) / 128.0f;
    float ldx = ((float)pad.rx - 128.0f) / 128.0f;
    float ldy = ((float)pad.ry - 128.0f) / 128.0f;
    syn_stick(MOVE_SLOT, move_anchor_px, TOUCH_LOGICAL_Y(move_anchor_py), mdx, mdy);
    syn_stick(LOOK_SLOT, LOOK_ANCHOR_X, LOOK_ANCHOR_Y, ldx, ldy);
}

int main() {
    soloader_init_all();

    // NOTE: boot milestones use l_error so they are visible in release logs
    // too during bring-up; downgrade to l_debug once the game renders.

    int (* nova_OnLoad)(void *jvm, void *reserved) =
        (void *)so_symbol(&so_mod, "JNI_OnLoad");
    if (nova_OnLoad) {
        l_error("BOOT: calling libnova2 JNI_OnLoad");
        nova_OnLoad(&jvm, NULL);
    } else {
        l_error("BOOT: libnova2 has no JNI_OnLoad export");
    }

    if (storm_loaded) {
        int (* storm_OnLoad)(void *jvm, void *reserved) =
            (void *)so_symbol(&storm_mod, "JNI_OnLoad");
        if (storm_OnLoad) {
            l_error("BOOT: calling libStormGLOFT JNI_OnLoad");
            storm_OnLoad(&jvm, NULL);
        }
    }

    gl_init();

    // Mirror of GameRenderer.onSurfaceCreated (GameRenderer.java:120-144):
    //   GLResLoader.init() -> GLMediaPlayer.init() -> GLGame.nativeInit()
    //   -> GameRenderer.nativeInit(w, h, GLGame.a)
    // env = FalsoJNI's JNIEnv, cls = NULL: static methods take
    // (JNIEnv*, jclass, ...); instance ones (none called here) would take jobject.
    void (* res_init)(void *env, void *cls) =
        (void *)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftN2HP_GLResLoader_nativeInit");
    void (* media_init)(void *env, void *cls) =
        (void *)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftN2HP_GLMediaPlayer_nativeInit");
    void (* game_init)(void *env, void *cls) =
        (void *)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftN2HP_GLGame_nativeInit");
    void (* device_init)(void *env, void *cls) =
        (void *)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftN2HP_GLUtils_Device_nativeInit");

    void (* renderer_init)(void *env, void *cls, int w, int h, int flag) =
        (void *)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftN2HP_GameRenderer_nativeInit");
    void (* renderer_resize)(void *env, void *cls, int w, int h) =
        (void *)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftN2HP_GameRenderer_nativeResize");
    void (* renderer_render)(void *env, void *cls) =
        (void *)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftN2HP_GameRenderer_nativeRender");

    touch_pressed =
        (void *)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftN2HP_GLGame_nativeTouchPressed");
    touch_moved =
        (void *)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftN2HP_GLGame_nativeTouchMoved");
    touch_released =
        (void *)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftN2HP_GLGame_nativeTouchReleased");
    key_down =
        (void *)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftN2HP_GLGame_nativeSetOnKeyDown");
    key_up =
        (void *)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftN2HP_GLGame_nativeSetOnKeyUp");

    if (!res_init || !media_init || !game_init || !renderer_init || !renderer_render) {
        l_error("BOOT: missing entry point(s): res=%p media=%p game=%p rinit=%p rrender=%p",
                res_init, media_init, game_init, renderer_init, renderer_render);
        sceKernelExitDeleteThread(0);
    }

    // GLMediaPlayer.init() sizes its Java arrays from these two first.
    int (* total_sounds)(void *env, void *cls) =
        (void *)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftN2HP_GLMediaPlayer_nativeGetTotalSounds");
    int (* total_same)(void *env, void *cls) =
        (void *)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftN2HP_GLMediaPlayer_nativeGetTotalSoundsOfSameInstance");
    if (total_sounds && total_same)
        l_error("BOOT: sounds=%d same=%d", total_sounds(ENV, NULL), total_same(ENV, NULL));

    l_error("BOOT: GLResLoader.nativeInit");
    res_init(ENV, NULL);
    l_error("BOOT: GLMediaPlayer.nativeInit");
    media_init(ENV, NULL);
    if (device_init) {
        l_error("BOOT: Device.nativeInit");
        device_init(ENV, NULL);
    }
    l_error("BOOT: GLGame.nativeInit");
    game_init(ENV, NULL);

    // GLGame.a = getLanguage(...) = 1 ("eng", same value our getLanguage stub
    // returns); passed as the 3rd arg exactly like GameRenderer.java does.
    // OJO: se reporta el mundo LÓGICO 960x640 (no la pantalla física): así el
    // engine entra al bucket Is960x640Res (layouts de 960) y g_ScreenW/H sale
    // 960/640; glutil reescala a 960x544 reales. Ver glutil.h.
    l_error("BOOT: GameRenderer.nativeInit(%d, %d, 1) [logical]", ENG_W, ENG_H);
    renderer_init(ENV, NULL, ENG_W, ENG_H, 1);
    if (renderer_resize) {
        l_error("BOOT: GameRenderer.nativeResize [logical]");
        renderer_resize(ENV, NULL, ENG_W, ENG_H);
    }

    // Touch sensor is off by default -- without this sceTouchPeek reports 0.
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    // Sticks are unreadable in the default DIGITAL mode (lx/ly stuck at 128);
    // ANALOG makes pad.lx/ly/rx/ry report 0..255. dialog.c sets ANALOG_WIDE
    // around IME but the boot default here is what the game loop sees.
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    l_error("BOOT: entering render loop");
    while (1) {
        if (touch_pressed && touch_moved && touch_released && key_down && key_up)
            poll_input();
        renderer_render(ENV, NULL);
        gl_swap();
    }

    sceKernelExitDeleteThread(0);
}
