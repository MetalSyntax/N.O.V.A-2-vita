#include "crash_guard.h"
#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "video.h"
#include "port_menu.h"

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
#define TOUCH_LOGICAL_Y(py) (py)
// Aspecto ORIGINAL (menú): el motor corre a 800x480 (g_eng_w/h). Las coords
// de abajo están medidas a 960x544; se reanclan según cómo ancla el motor
// cada control (W-x / H-y / centro). En FULL SCREEN quedan idénticas.
#define RX(x) ((x) - (960 - g_eng_w))
#define BY(y) ((y) - (544 - g_eng_h))
#define CX(x) ((x) - (960 - g_eng_w) / 2)
#define CY(y) ((y) - (544 - g_eng_h) / 2)

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
#define ROCKET_SLOT 7
#define N_SLOTS 8
#define MOVE_ANCHOR_X 84
#define MOVE_ANCHOR_Y BY(462)
#define MOVE_ADAPT_R 60
// Cámara (stick der.) como en Shadow-Guardian-vita: arrastre CONTINUO por
// velocidad (px/frame según inclinación) con "wrap": al salir de la caja
// ±LOOK_BOX alrededor del ancla se suelta y se re-aprieta en el ancla en el
// mismo frame, así mantener el stick gira sin tope (360 completos). Antes el
// dedo iba a ancla+90px y se quedaba quieto -> la cámara frenaba en seco.
// Caja (460..740, 130..390): lejos del disparo (788+), rayo (870+), arma
// (y~40) y joystick. Curva: zona muerta + respuesta cuadrática para apuntar
// fino con poca inclinación.
#define LOOK_ANCHOR_X (600 * g_eng_w / 960)
#define LOOK_ANCHOR_Y (260 * g_eng_h / 544)
#define LOOK_BOX (130 * g_eng_h / 544)
#define LOOK_DZ 0.15f
// Velocidad MÁXIMA (nivel 10 = la de la Fase 17, "muy rápido" en log 0020).
// Sensibilidad 1..10 (SELECT + D-pad arriba/abajo o menú SELECT+START),
// guardada en config.txt (port_menu.c).
#define LOOK_SPEED_X 14.0f
#define LOOK_SPEED_Y 9.0f
#define FIRE_X RX(840)
#define FIRE_Y BY(420)
#define ACT_X RX(915)
#define ACT_Y BY(497)
#define POWER_X RX(905)
#define POWER_Y BY(322)
#define PAUSE_X 118
#define PAUSE_Y 25
#define WEAPON_X RX(790)
#define WEAPON_Y 40
#define GRENADE_X RX(835)
#define GRENADE_Y 105
// Botón "Interactuar" (puertas/switches): CButtonSpr de CLevelInterfaceNormal
// en (W/2-204, H/2+100); con un mensaje en pantalla sube a H/2+20
// (ShowInteractButton/MoveInteractButtonUp). El toque real (318,405) del log
// 0032 activó un switch (sfx_switch_activate) -> se usa ese punto.
#define INTERACT_X CX(318)
#define INTERACT_Y CY(405)
#define INTERACT_MSG_DY 80
static const char *msg_active; // ILevelMessage::bIsMessageActive
// Cohetes del mecha (captura gd 2026-11-22-220043): 2º CButtonShoot (tipo
// 0x1d, 90x90) de los ControlScheme en (W-105, H-225) = (855,319). Solo está
// activo con CLevelInterfaceMech; en esa zona, a pie, está el rayo.
#define ROCKET_X RX(855)
#define ROCKET_Y BY(319)
static char *(*level_get)(void);   // CLevel::GetLevel (m_instance)
static uintptr_t mech_vptr;         // _ZTV19CLevelInterfaceMech + 8

// Disparos mudos con fuego sostenido (2026-11-22): se recuperaban solo al
// pausar y despausar, que llama VoxSoundManager::ResumeAllSounds ->
// vox::VoxEngine::ResumeAllEmitters. Es decir, los emisores nuevos quedan en
// pausa. Causa raíz sin ubicar: se repite lo que hace la despausa al soltar
// el disparo/los cohetes (en el hilo del juego, máx. 1 vez cada 2 s).
// ponytail: mitigación; si aparece la causa (Play+Pause en
// VoxSoundManager::Play por la máscara de ShadowMusic), quitar esto.
static void **vox_mgr;              // VoxSoundManager::Singleton
static void (*vox_resume_all)(void *self);

static void sfx_unstick(void) {
    static uint64_t last;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (!vox_resume_all || !vox_mgr || !*vox_mgr || now - last < 2000000)
        return;
    last = now;
    vox_resume_all(*vox_mgr);
}

static int in_mech(void) {
    char *lv = level_get && mech_vptr ? level_get() : NULL;
    uintptr_t *ifc = lv ? *(uintptr_t **)(lv + 0x6c) : NULL; // CLevel::ShowInteractButton
    return ifc && *ifc == mech_vptr;
}
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
static int move_anchor_py = 462; // = MOVE_ANCHOR_Y, se fija en main()

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
    }
    if (mag < STICK_RELEASE) {
        syn_owned[slot] = 0;
        touch_released(ENV, NULL, syn_x[slot], syn_y[slot], slot);
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
    float k = (float)g_cam_level / (float)CAM_LEVEL_MAX;
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
    uint32_t up = prev & ~buttons;
    gest_update();
    if (up & (g_bind[A_FIRE] | g_bind[A_ROCKET]))
        sfx_unstick();
    int mech = (buttons & g_bind[A_ROCKET]) ? in_mech() : 0;

    // Cohetes (hold) en su propio slot, mientras se mantenga el botón.
    if (mech && !syn_owned[ROCKET_SLOT]) {
        syn_owned[ROCKET_SLOT] = 1;
        slot_hw[ROCKET_SLOT] = -2;
        slot_x[ROCKET_SLOT] = ROCKET_X;
        slot_y[ROCKET_SLOT] = ROCKET_Y;
        touch_pressed(ENV, NULL, ROCKET_X, ROCKET_Y, ROCKET_SLOT);
    } else if (!mech && syn_owned[ROCKET_SLOT]) {
        touch_released(ENV, NULL, slot_x[ROCKET_SLOT], slot_y[ROCKET_SLOT], ROCKET_SLOT);
        syn_owned[ROCKET_SLOT] = 0;
        slot_hw[ROCKET_SLOT] = slot_x[ROCKET_SLOT] = slot_y[ROCKET_SLOT] = -1;
    }
    // En el mecha no hay poderes: el mismo botón (L) no toca también el rayo.
    if (mech)
        down &= ~g_bind[A_ROCKET];
    if (buttons & SCE_CTRL_SELECT) {
        // SELECT + D-pad arriba/abajo: sensibilidad de cámara (sin gestos).
        if (down & SCE_CTRL_UP)
            cam_adjust(+1);
        else if (down & SCE_CTRL_DOWN)
            cam_adjust(-1);
        down &= ~(SCE_CTRL_UP | SCE_CTRL_DOWN);
    }
    // Botones remapeables desde el menú SELECT+START (g_bind, port_menu.c).
#define DN(a) (down & g_bind[a])
    if (DN(A_RELOAD))
        gest_start(WEAPON_X, WEAPON_Y, 0, GEST_TAP_FRAMES);
    else if (DN(A_WEAPON_NEXT))
        gest_start(WEAPON_X, WEAPON_Y, -GEST_SWIPE_STEP, GEST_SWIPE_FRAMES);
    else if (DN(A_WEAPON_PREV))
        gest_start(WEAPON_X, WEAPON_Y, GEST_SWIPE_STEP, GEST_SWIPE_FRAMES);
    else if (DN(A_POWER))
        gest_start(POWER_X, POWER_Y, 0, GEST_TAP_FRAMES);
    else if (DN(A_POWER_PREV))
        gest_start(POWER_X, POWER_Y, -POWER_SWIPE_STEP, POWER_SWIPE_FRAMES);
    else if (DN(A_POWER_NEXT))
        gest_start(POWER_X, POWER_Y, POWER_SWIPE_STEP, POWER_SWIPE_FRAMES);
    else if (DN(A_GRENADE))
        gest_start(GRENADE_X, GRENADE_Y, 0, GEST_TAP_FRAMES);
    else if (DN(A_INTERACT))
        gest_start(INTERACT_X, (msg_active && *msg_active) ? INTERACT_Y - INTERACT_MSG_DY : INTERACT_Y,
                   0, GEST_TAP_FRAMES);
    else if (DN(A_PAUSE))
        gest_start(PAUSE_X, PAUSE_Y, 0, GEST_TAP_FRAMES);
#undef DN

    int firing = (buttons & g_bind[A_FIRE]) != 0;
    int act = (buttons & g_bind[A_JUMP]) != 0;
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

// Al abrir el menú: soltar todo lo que el juego cree apretado (dedos, touches
// sintéticos, keys) para que no quede nada colgado durante la pausa.
static int wait_release = 0;

static void release_all(void) {
    for (int s = 0; s < N_SLOTS; s++) {
        if (slot_hw[s] != -1 && slot_x[s] != -1)
            touch_released(ENV, NULL, slot_x[s], slot_y[s], s);
        slot_hw[s] = slot_x[s] = slot_y[s] = -1;
        syn_owned[s] = 0;
    }
    gest_frames = 0;
    act_frames_left = 0;
    for (unsigned i = 0; i < sizeof(pad_map) / sizeof(pad_map[0]); i++)
        if (old_pad & pad_map[i].sce)
            key_up(ENV, NULL, pad_map[i].android);
    old_pad = 0;
}

// 1 = el menú se abrió este frame.
static int check_menu_combo(void) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if (wait_release) {
        // Tras cerrar el menú: ignorar input hasta soltar todo (si no, el O
        // que lo cerró llegaría al juego como recargar/B).
        if (pad.buttons)
            return 0;
        wait_release = 0;
    }
    if ((pad.buttons & SCE_CTRL_SELECT) && (pad.buttons & SCE_CTRL_START) &&
        !(old_pad & SCE_CTRL_START)) {
        release_all();
        menu_open();
        wait_release = 1;
        return 1;
    }
    return 0;
}

static void poll_input(void) {
    // Touch 1:1 960x544 (el engine hit-testea en real; el *640/544 de la
    // Fase 12 se revierte con el mundo lógico por el zoom que causaba).
    // Sin logs por evento: cada línea es un sceIoWrite síncrono (tirones).
    // Slots 3-6 reservados (sticks/fire/gestos): los dedos usan 0-2.
    SceTouchData touch;
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    int seen[N_SLOTS] = { 0 };
    for (int r = 0; r < touch.reportNum && r < N_SLOTS; r++) {
        int hw = touch.report[r].id;
        int x = touch.report[r].x / 2;
        int y = touch.report[r].y / 2;
        if (g_aspect_original) {
            // Pantalla -> motor 800x480 (imagen en x 27..933, ver ORIG_*).
            x = (x - ORIG_X) * g_eng_w / ORIG_W;
            y = y * g_eng_h / VITA_H;
            x = x < 0 ? 0 : x >= g_eng_w ? g_eng_w - 1 : x;
        }
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
    // Núcleo 0 exclusivo para el hilo del juego (lógica + render, el motor
    // es de un solo hilo); el resto de hilos va a los núcleos 1-2.
    sceKernelChangeThreadCpuAffinityMask(SCE_KERNEL_THREAD_ID_SELF, SCE_KERNEL_CPU_MASK_USER_0);
    soloader_init_all();
    crash_guard_init();

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
    port_config_load();
    msg_active = (const char *)so_symbol(&so_mod, "_ZN13ILevelMessage16bIsMessageActiveE");
    if (g_aspect_original && !gl_game_fbo_init(g_eng_w, g_eng_h)) {
        g_aspect_original = 0;
        g_eng_w = 960;
        g_eng_h = 544;
    }
    move_anchor_py = MOVE_ANCHOR_Y;
    level_get = (void *)so_symbol(&so_mod, "_ZN6CLevel8GetLevelEv");
    vox_mgr = (void **)so_symbol(&so_mod, "_ZN15VoxSoundManager9SingletonE");
    vox_resume_all = (void *)so_symbol(&so_mod, "_ZN15VoxSoundManager15ResumeAllSoundsEv");
    mech_vptr = so_symbol(&so_mod, "_ZTV19CLevelInterfaceMech");
    if (mech_vptr)
        mech_vptr += 8;
    // RTT de post-efectos (toda la escena cuando hay líquidos/refracción en
    // pantalla): el motor usa k_screenSize * A/B = 3/4. 1/2 = ~44% menos
    // píxeles en esas escenas. Tiene que ir antes de nativeInit (OnResize).
    if (g_fx_low) {
        int *ra = (int *)so_symbol(&so_mod, "_ZN11PostEffects16s_Scale_Raport_AE");
        int *rb = (int *)so_symbol(&so_mod, "_ZN11PostEffects16s_Scale_Raport_BE");
        if (ra && rb && *ra == 3 && *rb == 4) {
            *ra = 1;
            *rb = 2;
        } else
            l_error("fx: PostEffects scale not patched (%p %p)", ra, rb);
    }
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
    l_error("BOOT: GameRenderer.nativeInit(%d, %d, 1)", g_eng_w, g_eng_h);
    renderer_init(ENV, NULL, g_eng_w, g_eng_h, 1);
    if (renderer_resize) {
        l_error("BOOT: GameRenderer.nativeResize [real]");
        renderer_resize(ENV, NULL, g_eng_w, g_eng_h);
    }

    // GameRenderer.onSurfaceCreated: tras nativeInit, si el intro no se vio,
    // lanza MyVideoView con GLVideo.mp4 y onDrawFrame no llama nativeRender
    // hasta que termina. Mismo orden acá: bloqueante, Cross/START lo saltan.
    gl_screen_fb(1);
    video_play("GLVideo.mp4");
    gl_screen_fb(0);
    gl_provide_phycont();

    // Touch sensor is off by default -- without this sceTouchPeek reports 0.
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    // Sticks are unreadable in the default DIGITAL mode (lx/ly stuck at 128);
    // ANALOG makes pad.lx/ly/rx/ry report 0..255. dialog.c sets ANALOG_WIDE
    // around IME but the boot default here is what the game loop sees.
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    l_error("BOOT: entering render loop");
    while (1) {
        if (menu_active()) {
            // Juego en pausa: sin nativeRender; solo se redibuja el menú
            // cuando cambia algo.
            if (menu_frame())
                gl_swap();
            else
                sceKernelDelayThread(16000);
            continue;
        }
        if (touch_pressed && touch_moved && touch_released && key_down && key_up &&
            !check_menu_combo() && !wait_release)
            poll_input();
        renderer_render(ENV, NULL);
        if (g_aspect_original)
            gl_game_present(ORIG_X, 0, ORIG_W, VITA_H, g_filter_smooth);
        cam_osd_draw();
        gl_swap();
    }

    sceKernelExitDeleteThread(0);
}
