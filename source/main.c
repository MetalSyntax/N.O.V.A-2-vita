#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "video.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <stdlib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

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

// Pantalla real 960x544 1:1 (2026-09-25): el juego 3D estaba bien así; el
// mundo 960x640 le cambiaba la cámara (Is960x640Res) y se desbordaba.
// Se reporta lo REAL en nativeInit/Resize. Touch 1:1.
#define ENG_W 960
#define ENG_H 544
#define TOUCH_LOGICAL_Y(py) (py)

// ---- Input: touch (5 stable slots, never the raw hw id -- see
// psvita-porting input_handling.md) + physical buttons as Android keycodes.
// GLGame.onKeyDown/Up forwards keycodes raw to nativeSetOnKeyDown/Up; the
// game is Xperia-Play optimized so gamepad BUTTON_* codes are used too.
//
// Sticks (2026-09-25): el engine no tiene path analógico de keys: mover =
// `AnalogJoystick`, mirar = `FullScreenRotateJoystick` (ver glutil.h).
// Anclas y botones en PÍXELES REALES 960x544 (1:1). Medidos en la captura
// kb 2026-09-24-230504 y en los press reales del log 0014 (dedo caminando
// ~(80,465), fire ~(840,420)): joystick (84,462); fire (840,420); botón
// acción (915,497). El (140,440)/(886,428) viejo era del layout 960x640.
// Log 0014: el ancla ADAPTATIVA se había ido a (180,293) por los toques de
// MENÚ en la mitad izquierda -> el press caía fuera del círculo, en la zona
// de FullScreenRotateJoystick, y el stick izq. giraba la cámara. Ahora solo
// adapta con press cerca del círculo (MOVE_ADAPT_R).
// Slots: 0-2 dedos, 3 move, 4 look, 5 fire/acción, 6 gestos HUD (tap/swipe).
// TouchScreenBase acepta ids <= 7.
//
// Botones (2026-09-26, pseudo-C + captura hc 2026-09-26-214009, 960x544):
//   R  -> hold en CButtonShoot(W-119,H-123) = (841,421), rect ±53.
//   Cross -> tap en el botón de salto (CButtonSpr).
//   L  -> TAP en el rayo: CButtonSweep(W-116,H-224) de CLevelInterfaceNormal,
//         rect = frame del sprite (dibujado en ~(905,322)). Tap sin deslizar
//         -> PressedNoSweepCallback (usar poder).
//   D-pad izq/der -> SWIPE en el rayo -> SweepCallback (cambiar poder;
//         umbral = ancho/4).
//   Circle -> TAP en el ícono de arma (CButtonHudSweep): tocar sin deslizar
//         llama HudPressedNoSweepCallback -> CWeaponManager::ReloadWeapon.
//   Triangle / D-pad arriba -> SWIPE izq. en ese ícono -> HudSweepCallback
//         -> SetCurrentWeapon; D-pad abajo -> swipe der. (arma anterior).
//   Square -> tap en la granada (grenade_button, bajo el ícono de arma).
//   START -> tap en el botón de pausa (arriba a la izquierda, "pause_idle").
// Pausa, arma y granada son nodos del HUD 3D (sin coords en código):
// medidas en la captura hc.
// OJO: en la Vita física L/R son SCE_CTRL_LTRIGGER/RTRIGGER (0x100/0x200);
// SCE_CTRL_L1/R1 (0x400/0x800) solo los reporta PSTV/DS4 -> por eso R1 "no
// disparaba". Se aceptan ambos.
#define MOVE_SLOT 3
#define LOOK_SLOT 4
#define FIRE_SLOT 5
#define GEST_SLOT 6
#define N_SLOTS 8
#define MOVE_ANCHOR_X 84
#define MOVE_ANCHOR_Y 462
#define MOVE_ADAPT_R 60
// Cámara (stick der.) como en Shadow-Guardian-vita: arrastre CONTINUO por
// velocidad (px/frame según inclinación) con "wrap": al salir de la caja
// ±LOOK_BOX alrededor del ancla se suelta y se re-aprieta en el ancla en el
// mismo frame, así mantener el stick gira sin tope (360 completos). Antes el
// dedo iba a ancla+90px y se quedaba quieto -> la cámara frenaba en seco.
// Caja (460..740, 130..390): lejos del disparo (788+), rayo (870+), arma
// (y~40) y joystick. Curva: zona muerta + respuesta cuadrática para apuntar
// fino con poca inclinación.
#define LOOK_ANCHOR_X 600
#define LOOK_ANCHOR_Y 260
#define LOOK_BOX 130
#define LOOK_DZ 0.15f
// Velocidad MÁXIMA (nivel 10 = la de la Fase 17, "muy rápido" en log 0020).
// Sensibilidad regulable en juego: SELECT + D-pad arriba/abajo, niveles
// 1..10 (default 4 = 40%), guardada en DATA_PATH"camera_sens.txt". Al
// cambiarla se ve una barra de 10 bloques arriba al centro durante 1.5 s.
#define LOOK_SPEED_X 14.0f
#define LOOK_SPEED_Y 9.0f
#define CAM_LEVEL_DEFAULT 4
#define CAM_LEVEL_MAX 10
#define CAM_CFG DATA_PATH "camera_sens.txt"
#define FIRE_X 840
#define FIRE_Y 420
#define ACT_X 915
#define ACT_Y 497
#define POWER_X 905
#define POWER_Y 322
#define PAUSE_X 118
#define PAUSE_Y 25
#define WEAPON_X 790
#define WEAPON_Y 40
#define GRENADE_X 835
#define GRENADE_Y 105
#define BTN_FIRE (SCE_CTRL_RTRIGGER | SCE_CTRL_R1)
#define BTN_POWER (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)
// Aro exterior del joystick ~70px: deflexión total queda dentro del aro.
#define STICK_RADIUS 60
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

static int slot_hw[N_SLOTS] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static int slot_x[N_SLOTS] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static int slot_y[N_SLOTS] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static uint32_t old_pad = 0;

// Synthetic-drag state per reserved slot: 0 = free, 1 = owned by stick.
static int syn_owned[N_SLOTS] = { 0 };
static int syn_x[N_SLOTS] = { 0 };
static int syn_y[N_SLOTS] = { 0 };

// Ancla adaptativa del joystick de move, en coords de PANTALLA 960x544.
// Semilla = medida a píxel (84,462); solo los press reales a <MOVE_ADAPT_R
// de la semilla la ajustan (los de menú quedan fuera).
static int move_anchor_px = MOVE_ANCHOR_X;
static int move_anchor_py = MOVE_ANCHOR_Y;

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

static float look_fx, look_fy;
static int cam_level = CAM_LEVEL_DEFAULT;
static uint64_t cam_osd_until;

static void cam_load(void) {
    SceUID fd = sceIoOpen(CAM_CFG, SCE_O_RDONLY, 0);
    if (fd < 0)
        return;
    char b[8] = { 0 };
    sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    int v = atoi(b);
    if (v >= 1 && v <= CAM_LEVEL_MAX)
        cam_level = v;
}

static void cam_save(void) {
    SceUID fd = sceIoOpen(CAM_CFG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0)
        return;
    char b[8];
    int n = sceClibSnprintf(b, sizeof(b), "%d\n", cam_level);
    sceIoWrite(fd, b, n);
    sceIoClose(fd);
}

static void cam_adjust(int d) {
    int v = cam_level + d;
    if (v < 1) v = 1;
    if (v > CAM_LEVEL_MAX) v = CAM_LEVEL_MAX;
    cam_level = v;
    cam_save();
    cam_osd_until = sceKernelGetProcessTimeWide() + 1500000;
    l_error("[cam] sensibilidad %d/%d", cam_level, CAM_LEVEL_MAX);
}

// Barra de sensibilidad con glScissor+glClear (sin shaders). Estado GL
// guardado/restaurado: el motor cachea el suyo (ver video_avc.c).
static void cam_osd_draw(void) {
    if (!cam_osd_until || sceKernelGetProcessTimeWide() > cam_osd_until)
        return;
    GLint fbo, box[4];
    GLfloat cc[4];
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_SCISSOR_BOX, box);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    if (fbo)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEnable(GL_SCISSOR_TEST);
    const int bw = 22, bh = 14, gap = 4;
    int x0 = (VITA_W - (CAM_LEVEL_MAX * (bw + gap) - gap)) / 2;
    int y0 = VITA_H - 20 - bh;   // GL: origen abajo -> 20 px desde arriba
    glScissor(x0 - 4, y0 - 4, CAM_LEVEL_MAX * (bw + gap) - gap + 8, bh + 8);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    for (int i = 0; i < CAM_LEVEL_MAX; i++) {
        glScissor(x0 + i * (bw + gap), y0, bw, bh);
        if (i < cam_level)
            glClearColor(0.2f, 0.8f, 1.0f, 1.0f);
        else
            glClearColor(0.25f, 0.25f, 0.25f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glScissor(box[0], box[1], box[2], box[3]);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    if (!sc)
        glDisable(GL_SCISSOR_TEST);
    if (fbo)
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}

static float look_curve(float v) {
    float a = v < 0 ? -v : v;
    if (a <= LOOK_DZ)
        return 0.0f;
    float t = (a - LOOK_DZ) / (1.0f - LOOK_DZ);
    if (t > 1.0f)
        t = 1.0f;
    t = 0.35f * t + 0.65f * t * t;
    return v < 0 ? -t : t;
}

static void syn_look(float dx, float dy) {
    float k = (float)cam_level / (float)CAM_LEVEL_MAX;
    float vx = look_curve(dx) * LOOK_SPEED_X * k;
    float vy = look_curve(dy) * LOOK_SPEED_Y * k;
    if (vx == 0.0f && vy == 0.0f) {
        if (syn_owned[LOOK_SLOT]) {
            touch_released(ENV, NULL, syn_x[LOOK_SLOT], syn_y[LOOK_SLOT], LOOK_SLOT);
            syn_owned[LOOK_SLOT] = 0;
            slot_hw[LOOK_SLOT] = -1;
            slot_x[LOOK_SLOT] = -1;
            slot_y[LOOK_SLOT] = -1;
        }
        return;
    }
    if (!syn_owned[LOOK_SLOT]) {
        syn_owned[LOOK_SLOT] = 1;
        slot_hw[LOOK_SLOT] = -2;
        look_fx = LOOK_ANCHOR_X;
        look_fy = LOOK_ANCHOR_Y;
        syn_x[LOOK_SLOT] = LOOK_ANCHOR_X;
        syn_y[LOOK_SLOT] = LOOK_ANCHOR_Y;
        touch_pressed(ENV, NULL, LOOK_ANCHOR_X, LOOK_ANCHOR_Y, LOOK_SLOT);
    }
    // Acumulador float: inclinaciones leves avanzan <1px/frame sin perderse.
    look_fx += vx;
    look_fy += vy;
    int nx = (int)look_fx, ny = (int)look_fy;
    if (nx != syn_x[LOOK_SLOT] || ny != syn_y[LOOK_SLOT]) {
        syn_x[LOOK_SLOT] = nx;
        syn_y[LOOK_SLOT] = ny;
        touch_moved(ENV, NULL, nx, ny, LOOK_SLOT);
    }
    if (nx < LOOK_ANCHOR_X - LOOK_BOX || nx > LOOK_ANCHOR_X + LOOK_BOX ||
        ny < LOOK_ANCHOR_Y - LOOK_BOX || ny > LOOK_ANCHOR_Y + LOOK_BOX) {
        touch_released(ENV, NULL, nx, ny, LOOK_SLOT);
        look_fx = LOOK_ANCHOR_X;
        look_fy = LOOK_ANCHOR_Y;
        syn_x[LOOK_SLOT] = LOOK_ANCHOR_X;
        syn_y[LOOK_SLOT] = LOOK_ANCHOR_Y;
        touch_pressed(ENV, NULL, LOOK_ANCHOR_X, LOOK_ANCHOR_Y, LOOK_SLOT);
    }
    slot_x[LOOK_SLOT] = syn_x[LOOK_SLOT];
    slot_y[LOOK_SLOT] = syn_y[LOOK_SLOT];
}

// Fire (R1, hold) y botón de acción (Cross, tap) como touches sintéticos en
// FIRE_SLOT, coords lógicas. fire tiene prioridad: si se dispara, el tap de
// acción se ignora ese frame.
static int act_frames_left = 0;

// Gestos de un disparo en GEST_SLOT: tap (press, GEST_TAP_FRAMES quieto,
// release) o swipe (press, GEST_SWIPE_FRAMES pasos de dx px, release).
// Se ignora un gesto nuevo mientras otro está en curso.
#define GEST_TAP_FRAMES 4
// Arma: 8 x 16 = 128px (el sweep del HUD cambia al superar su ancho).
#define GEST_SWIPE_FRAMES 8
#define GEST_SWIPE_STEP 16
// Rayo: 5 x 10 = 50px (umbral ancho/4; más largo podría saltar 2 poderes).
#define POWER_SWIPE_FRAMES 5
#define POWER_SWIPE_STEP 10
static int gest_frames = 0;
static int gest_dx = 0;

static void gest_start(int x, int y, int dx, int frames) {
    if (gest_frames > 0)
        return;
    syn_owned[GEST_SLOT] = 1;
    slot_hw[GEST_SLOT] = -2;
    slot_x[GEST_SLOT] = x;
    slot_y[GEST_SLOT] = y;
    gest_dx = dx;
    gest_frames = frames;
    touch_pressed(ENV, NULL, x, y, GEST_SLOT);
    l_error("[syntouch] gesture %s at (%d, %d)", dx ? "swipe" : "tap", x, y);
}

static void gest_update(void) {
    if (gest_frames <= 0)
        return;
    if (gest_dx) {
        slot_x[GEST_SLOT] += gest_dx;
        touch_moved(ENV, NULL, slot_x[GEST_SLOT], slot_y[GEST_SLOT], GEST_SLOT);
    }
    if (--gest_frames == 0) {
        touch_released(ENV, NULL, slot_x[GEST_SLOT], slot_y[GEST_SLOT], GEST_SLOT);
        syn_owned[GEST_SLOT] = 0;
        slot_hw[GEST_SLOT] = -1;
        slot_x[GEST_SLOT] = -1;
        slot_y[GEST_SLOT] = -1;
    }
}

static void syn_buttons(uint32_t buttons, uint32_t prev) {
    uint32_t down = buttons & ~prev;
    gest_update();
    if (buttons & SCE_CTRL_SELECT) {
        // SELECT + D-pad arriba/abajo: sensibilidad de cámara (sin gestos).
        if (down & SCE_CTRL_UP)
            cam_adjust(+1);
        else if (down & SCE_CTRL_DOWN)
            cam_adjust(-1);
        down &= ~(SCE_CTRL_UP | SCE_CTRL_DOWN);
    }
    if (down & SCE_CTRL_CIRCLE)                                   // recargar
        gest_start(WEAPON_X, WEAPON_Y, 0, GEST_TAP_FRAMES);
    else if (down & (SCE_CTRL_TRIANGLE | SCE_CTRL_UP))            // arma siguiente
        gest_start(WEAPON_X, WEAPON_Y, -GEST_SWIPE_STEP, GEST_SWIPE_FRAMES);
    else if (down & SCE_CTRL_DOWN)                                // arma anterior
        gest_start(WEAPON_X, WEAPON_Y, GEST_SWIPE_STEP, GEST_SWIPE_FRAMES);
    else if (down & BTN_POWER)                                    // usar poder
        gest_start(POWER_X, POWER_Y, 0, GEST_TAP_FRAMES);
    else if (down & SCE_CTRL_LEFT)                                // cambiar poder
        gest_start(POWER_X, POWER_Y, -POWER_SWIPE_STEP, POWER_SWIPE_FRAMES);
    else if (down & SCE_CTRL_RIGHT)
        gest_start(POWER_X, POWER_Y, POWER_SWIPE_STEP, POWER_SWIPE_FRAMES);
    else if (down & SCE_CTRL_SQUARE)                              // granada
        gest_start(GRENADE_X, GRENADE_Y, 0, GEST_TAP_FRAMES);
    else if (down & SCE_CTRL_START)                               // pausa
        gest_start(PAUSE_X, PAUSE_Y, 0, GEST_TAP_FRAMES);

    int firing = (buttons & BTN_FIRE) != 0;
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
    // Cross/R/L NO van por keys: son touches sintéticos (ver syn_buttons)
    // porque el engine ignora las keys en juego. El resto se manda como key
    // (menús) Y como gesto táctil sobre el HUD (juego).
    { SCE_CTRL_CIRCLE, 97 },   // KEYCODE_BUTTON_B
    { SCE_CTRL_SQUARE, 99 },   // KEYCODE_BUTTON_X
    { SCE_CTRL_TRIANGLE, 100 },// KEYCODE_BUTTON_Y
    { SCE_CTRL_START, 108 },   // KEYCODE_BUTTON_START
    { SCE_CTRL_SELECT, 109 },  // KEYCODE_BUTTON_SELECT
};

static void poll_input(void) {
    // Touch 1:1 960x544 (el engine hit-testea en real; el *640/544 de la
    // Fase 12 se revierte con el mundo lógico por el zoom que causaba).
    // Solo se loguea el press (los moves saturarían); coords del log = reales.
    // Slots 3-6 reservados (sticks/fire/gestos): los dedos usan 0-2.
    SceTouchData touch;
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    int seen[N_SLOTS] = { 0 };
    for (int r = 0; r < touch.reportNum && r < N_SLOTS; r++) {
        int hw = touch.report[r].id;
        int x = touch.report[r].x / 2;
        int y = touch.report[r].y / 2;
        int slot = -1;
        for (int s = 0; s < N_SLOTS; s++)
            if (slot_hw[s] == hw) { slot = s; break; }
        if (slot == -1) {
            for (int s = 0; s < N_SLOTS; s++)
                if (s < MOVE_SLOT && slot_hw[s] == -1) { slot = s; break; }
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
            // Ancla adaptativa: solo press cerca del joystick (dentro del
            // círculo). Los de menú en la mitad izquierda la desplazaban.
            int ax = x - MOVE_ANCHOR_X, ay = y - MOVE_ANCHOR_Y;
            if (ax * ax + ay * ay < MOVE_ADAPT_R * MOVE_ADAPT_R && !syn_owned[MOVE_SLOT]) {
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
    syn_buttons(pad.buttons, old_pad);
    old_pad = pad.buttons;

    // Sticks need ANALOG sampling (set in main); in DIGITAL mode lx/ly read
    // a constant 128. Range 0..255, center 128, y-down (ly<128 = UP).
    // Anclas en píxeles reales 960x544 (1:1).
    float mdx = ((float)pad.lx - 128.0f) / 128.0f;
    float mdy = ((float)pad.ly - 128.0f) / 128.0f;
    float ldx = ((float)pad.rx - 128.0f) / 128.0f;
    float ldy = ((float)pad.ry - 128.0f) / 128.0f;
    syn_stick(MOVE_SLOT, move_anchor_px, TOUCH_LOGICAL_Y(move_anchor_py), mdx, mdy);
    syn_look(ldx, ldy);
}

int main() {
    soloader_init_all();

    // El motor guarda en /data/data/<pkg>/ (remapeado a saves/ en io.c) y nunca
    // hace mkdir: en Android esa carpeta ya existe. Sin esto, en una instalación
    // nueva cada fopen(..., "wb") de game_save.dat/level_save.dat falla y no se
    // guarda nada.
    sceIoMkdir(DATA_PATH "saves", 0777);

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
    video_init();

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
    // Se reporta la pantalla REAL 960x544 (el 960x640 metía otra cámara y el
    // juego se veía con zoom/desbordado, log 0010). Ver glutil.h.
    l_error("BOOT: GameRenderer.nativeInit(%d, %d, 1) [real]", ENG_W, ENG_H);
    renderer_init(ENV, NULL, ENG_W, ENG_H, 1);
    if (renderer_resize) {
        l_error("BOOT: GameRenderer.nativeResize [real]");
        renderer_resize(ENV, NULL, ENG_W, ENG_H);
    }

    // GameRenderer.onSurfaceCreated: tras nativeInit, si el intro no se vio,
    // lanza MyVideoView con GLVideo.mp4 y onDrawFrame no llama nativeRender
    // hasta que termina. Mismo orden acá: bloqueante, Cross/START lo saltan.
    video_play("GLVideo.mp4");
    gl_provide_phycont();

    // Touch sensor is off by default -- without this sceTouchPeek reports 0.
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    // Sticks are unreadable in the default DIGITAL mode (lx/ly stuck at 128);
    // ANALOG makes pad.lx/ly/rx/ry report 0..255. dialog.c sets ANALOG_WIDE
    // around IME but the boot default here is what the game loop sees.
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    cam_load();
    l_error("BOOT: camera sensitivity %d/%d (SELECT + D-pad arriba/abajo)", cam_level, CAM_LEVEL_MAX);
    l_error("BOOT: entering render loop");
    while (1) {
        if (touch_pressed && touch_moved && touch_released && key_down && key_up)
            poll_input();
        renderer_render(ENV, NULL);
        cam_osd_draw();
        gl_swap();
    }

    sceKernelExitDeleteThread(0);
}
