#ifndef PORT_MENU_H
#define PORT_MENU_H

#include <stdint.h>

// Gameplay actions with remappable physical buttons (SELECT+START menu).
enum {
    A_FIRE, A_JUMP, A_INTERACT, A_POWER, A_POWER_PREV, A_POWER_NEXT,
    A_RELOAD, A_WEAPON_NEXT, A_WEAPON_PREV, A_GRENADE, A_PAUSE, A_ROCKET, A_COUNT
};

extern uint32_t g_bind[A_COUNT];   // SCE_CTRL_* mask per action
extern int g_touch_hidden;         // 1 = virtual controls drawn at alpha 0
extern int g_cam_level;            // camera sensitivity 1..CAM_LEVEL_MAX
// Aspect ORIGINAL = engine at 800x480 (Android HD) in an FBO, shown 5:3 at
// 906x544 with black bars. Read at boot only (engine size is fixed at init).
extern int g_aspect_original, g_filter_smooth;
// Post-effects RTT (whole scene when liquids/refraction are on screen):
// 1 = 1/2 screen (faster), 0 = engine default 3/4. Boot-time only.
extern int g_fx_low;
extern int g_eng_w, g_eng_h;       // resolution reported to the engine
#define ORIG_X 27
#define ORIG_W 906
#define CAM_LEVEL_MAX 10

void port_config_load(void);
void cam_adjust(int d);            // SELECT + D-pad up/down in game
void cam_osd_draw(void);

void menu_open(void);
int menu_active(void);
// Runs one menu frame. Returns 1 if it drew (caller must swap).
int menu_frame(void);

#endif
