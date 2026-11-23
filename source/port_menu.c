/*
 * Port options menu (SELECT + START), same idea as the Zenonia 2 / Inotia 4
 * ports: remappable gameplay buttons, on-screen controls hidden/shown,
 * camera sensitivity, V-Sync, aspect ratio + scaling filter, restore
 * defaults, quit. The game is paused
 * while it is open (main.c stops calling nativeRender).
 *
 * Drawn with glScissor + glClear only (8x8 font scaled 2x, one clear per run
 * of lit pixels): no shaders, no textures, so the engine's cached GL state is
 * untouched apart from scissor/clear color/FBO, which are saved and restored.
 * Only redrawn when something changes (a few frames for the swap chain).
 *
 * Settings: DATA_PATH "config.txt" (plain text, one "key value" per line).
 */
#include "port_menu.h"
#include "utils/glutil.h"
#include "utils/logger.h"

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DATA_PATH
#define DATA_PATH "ux0:data/nova2/"
#endif
#define CFG_PATH DATA_PATH "config.txt"
#define OLD_CAM_CFG DATA_PATH "camera_sens.txt"
#define SCR_W 960
#define SCR_H 544
#define CAM_LEVEL_DEFAULT 4

extern const unsigned char font8x8[256 * 8];

#define BTN_L (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)
#define BTN_R (SCE_CTRL_RTRIGGER | SCE_CTRL_R1)

static const uint32_t bind_default[A_COUNT] = {
    [A_FIRE] = BTN_R,
    [A_JUMP] = SCE_CTRL_CROSS,
    [A_INTERACT] = SCE_CTRL_UP,
    [A_POWER] = BTN_L,
    [A_POWER_PREV] = SCE_CTRL_LEFT,
    [A_POWER_NEXT] = SCE_CTRL_RIGHT,
    [A_RELOAD] = SCE_CTRL_CIRCLE,
    [A_WEAPON_NEXT] = SCE_CTRL_TRIANGLE,
    [A_WEAPON_PREV] = SCE_CTRL_DOWN,
    [A_GRENADE] = SCE_CTRL_SQUARE,
    [A_PAUSE] = SCE_CTRL_START,
    [A_ROCKET] = BTN_L,
};

static const struct { const char *key, *label; } act_names[A_COUNT] = {
    [A_FIRE] = { "fire", "Fire" },
    [A_JUMP] = { "jump", "Jump" },
    [A_INTERACT] = { "interact", "Interact (doors/switches)" },
    [A_POWER] = { "power", "Use power" },
    [A_POWER_PREV] = { "power_prev", "Previous power" },
    [A_POWER_NEXT] = { "power_next", "Next power" },
    [A_RELOAD] = { "reload", "Reload" },
    [A_WEAPON_NEXT] = { "weapon_next", "Next weapon" },
    [A_WEAPON_PREV] = { "weapon_prev", "Previous weapon" },
    [A_GRENADE] = { "grenade", "Grenade" },
    [A_PAUSE] = { "pause", "Pause" },
    [A_ROCKET] = { "rocket", "Rockets (mech)" },
};

// SELECT is reserved (menu combo + camera sensitivity), so it can't be bound.
static const struct { uint32_t mask; const char *name; } btn_names[] = {
    { SCE_CTRL_CROSS, "X" }, { SCE_CTRL_CIRCLE, "O" },
    { SCE_CTRL_SQUARE, "[]" }, { SCE_CTRL_TRIANGLE, "/\\" },
    { SCE_CTRL_UP, "Up" }, { SCE_CTRL_DOWN, "Down" },
    { SCE_CTRL_LEFT, "Left" }, { SCE_CTRL_RIGHT, "Right" },
    { BTN_L, "L" }, { BTN_R, "R" }, { SCE_CTRL_START, "Start" },
};
#define N_BTN_NAMES (int)(sizeof(btn_names) / sizeof(btn_names[0]))

uint32_t g_bind[A_COUNT];
int g_touch_hidden = 1;
int g_cam_level = CAM_LEVEL_DEFAULT;
static int g_vsync = 1;
int g_aspect_original = 0, g_filter_smooth = 1, g_fx_low = 1;
int g_eng_w = 960, g_eng_h = 544;
static int aspect_boot, fx_boot; // values in effect this boot (need a restart)

// ---- config ----

static void config_save(void) {
    SceUID fd = sceIoOpen(CFG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0)
        return;
    char b[64];
    int n = sceClibSnprintf(b, sizeof(b), "cam %d\ntouch %s\nvsync %d\naspect %d\nfilter %d\nfxlow %d\n",
                            g_cam_level, g_touch_hidden ? "hidden" : "shown", g_vsync,
                            g_aspect_original, g_filter_smooth, g_fx_low);
    sceIoWrite(fd, b, n);
    for (int a = 0; a < A_COUNT; a++) {
        n = sceClibSnprintf(b, sizeof(b), "bind %s %u\n", act_names[a].key, (unsigned)g_bind[a]);
        sceIoWrite(fd, b, n);
    }
    sceIoClose(fd);
}

static int read_file(const char *path, char *buf, int sz) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return -1;
    int n = sceIoRead(fd, buf, sz - 1);
    sceIoClose(fd);
    buf[n > 0 ? n : 0] = 0;
    return n;
}

void port_config_load(void) {
    memcpy(g_bind, bind_default, sizeof(g_bind));
    char buf[1024];
    if (read_file(CFG_PATH, buf, sizeof(buf)) < 0) {
        // First run with this build: keep the old camera_sens.txt value.
        if (read_file(OLD_CAM_CFG, buf, 16) > 0 && atoi(buf) >= 1 && atoi(buf) <= CAM_LEVEL_MAX)
            g_cam_level = atoi(buf);
    } else {
        for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
            char k[24], v[24];
            if (sscanf(line, "cam %d", &g_cam_level) == 1 ||
                sscanf(line, "vsync %d", &g_vsync) == 1 ||
                sscanf(line, "aspect %d", &g_aspect_original) == 1 ||
                sscanf(line, "filter %d", &g_filter_smooth) == 1 ||
                sscanf(line, "fxlow %d", &g_fx_low) == 1)
                continue;
            if (sscanf(line, "touch %23s", v) == 1)
                g_touch_hidden = strcmp(v, "shown") != 0;
            else if (sscanf(line, "bind %23s %23s", k, v) == 2)
                for (int a = 0; a < A_COUNT; a++)
                    if (!strcmp(k, act_names[a].key))
                        g_bind[a] = strtoul(v, NULL, 10);
        }
        if (g_cam_level < 1 || g_cam_level > CAM_LEVEL_MAX)
            g_cam_level = CAM_LEVEL_DEFAULT;
    }
    vglWaitVblankStart(g_vsync ? GL_TRUE : GL_FALSE);
    aspect_boot = g_aspect_original;
    fx_boot = g_fx_low;
    if (g_aspect_original) {
        g_eng_w = 800;
        g_eng_h = 480;
    }
    l_info("config: cam %d/%d, touch %s, vsync %d", g_cam_level, CAM_LEVEL_MAX,
           g_touch_hidden ? "hidden" : "shown", g_vsync);
}

// ---- 2D drawing (scissor + clear) ----

static GLint sv_fbo, sv_box[4];
static GLfloat sv_cc[4];
static GLboolean sv_sc;

static void gl2d_begin(void) {
    sv_sc = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &sv_fbo);
    glGetIntegerv(GL_SCISSOR_BOX, sv_box);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, sv_cc);
    if (sv_fbo)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEnable(GL_SCISSOR_TEST);
}

static void gl2d_end(void) {
    glScissor(sv_box[0], sv_box[1], sv_box[2], sv_box[3]);
    glClearColor(sv_cc[0], sv_cc[1], sv_cc[2], sv_cc[3]);
    if (!sv_sc)
        glDisable(GL_SCISSOR_TEST);
    if (sv_fbo)
        glBindFramebuffer(GL_FRAMEBUFFER, sv_fbo);
}

static void set_color(uint32_t rgb) {
    glClearColor(((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f,
                 (rgb & 0xff) / 255.0f, 1.0f);
}

// Top-left origin (GL's is bottom-left).
static void fill(int x, int y, int w, int h, uint32_t rgb) {
    glScissor(x, SCR_H - y - h, w, h);
    set_color(rgb);
    glClear(GL_COLOR_BUFFER_BIT);
}

// 8x8 font at 2x (16x16 cells): one clear per horizontal run of lit pixels.
static void text(int x, int y, uint32_t rgb, const char *s) {
    set_color(rgb);
    for (; *s && x + 16 <= SCR_W; s++, x += 16) {
        const unsigned char *g = font8x8 + (unsigned char)*s * 8;
        for (int row = 0; row < 8; row++) {
            for (int c = 0; c < 8;) {
                if (!(g[row] & (0x80 >> c))) { c++; continue; }
                int c0 = c;
                while (c < 8 && (g[row] & (0x80 >> c)))
                    c++;
                glScissor(x + c0 * 2, SCR_H - (y + row * 2) - 2, (c - c0) * 2, 2);
                glClear(GL_COLOR_BUFFER_BIT);
            }
        }
    }
}

// ---- camera sensitivity (SELECT + D-pad up/down in game) ----

static uint64_t cam_osd_until;

void cam_adjust(int d) {
    int v = g_cam_level + d;
    g_cam_level = v < 1 ? 1 : v > CAM_LEVEL_MAX ? CAM_LEVEL_MAX : v;
    config_save();
    cam_osd_until = sceKernelGetProcessTimeWide() + 1500000;
}

// 10-block bar at the top center for 1.5 s after a change.
void cam_osd_draw(void) {
    if (!cam_osd_until || sceKernelGetProcessTimeWide() > cam_osd_until)
        return;
    const int bw = 22, bh = 14, gap = 4, tw = CAM_LEVEL_MAX * (bw + gap) - gap;
    int x0 = (SCR_W - tw) / 2;
    gl2d_begin();
    fill(x0 - 4, 16, tw + 8, bh + 8, 0x000000);
    for (int i = 0; i < CAM_LEVEL_MAX; i++)
        fill(x0 + i * (bw + gap), 20, bw, bh, i < g_cam_level ? 0x33ccff : 0x404040);
    gl2d_end();
}

// ---- menu ----

enum { ROW_TOUCH = A_COUNT, ROW_CAM, ROW_VSYNC, ROW_FX, ROW_ASPECT, ROW_FILTER,
       ROW_DEFAULTS, ROW_QUIT, ROW_COUNT };
enum { CAP_NONE, CAP_REPLACE, CAP_ADD };

static int m_active, m_cursor, m_capture, m_need_release, m_redraw;
static uint32_t m_prev;

void menu_open(void) {
    m_active = 1;
    m_capture = CAP_NONE;
    m_need_release = 1;
    m_redraw = 3;
}

int menu_active(void) {
    return m_active;
}

static void binding_str(uint32_t m, char *out, int n) {
    out[0] = 0;
    for (int b = 0; b < N_BTN_NAMES; b++)
        if (m & btn_names[b].mask) {
            if (out[0])
                strncat(out, ", ", n - strlen(out) - 1);
            strncat(out, btn_names[b].name, n - strlen(out) - 1);
        }
    if (!out[0])
        strncpy(out, "NONE", n);
}

static void menu_draw(void) {
    gl2d_begin();
    fill(0, 0, SCR_W, SCR_H, 0x081020);
    text(24, 16, 0xffc040, "N.O.V.A. 2  -  PS VITA OPTIONS");
    for (int r = 0; r < ROW_COUNT; r++) {
        int y = 44 + r * 22;
        if (r == m_cursor)
            fill(0, y - 3, SCR_W, 22, 0x183860);
        char val[64];
        const char *label;
        if (r < A_COUNT) {
            label = act_names[r].label;
            if (r == m_cursor && m_capture != CAP_NONE)
                strcpy(val, m_capture == CAP_ADD ? "press a button to add..." : "press a button...");
            else
                binding_str(g_bind[r], val, sizeof(val));
        } else if (r == ROW_TOUCH) {
            label = "On-screen controls";
            strcpy(val, g_touch_hidden ? "HIDDEN" : "SHOWN");
        } else if (r == ROW_CAM) {
            label = "Camera sensitivity";
            sceClibSnprintf(val, sizeof(val), "%d / %d", g_cam_level, CAM_LEVEL_MAX);
        } else if (r == ROW_VSYNC) {
            label = "V-Sync (off = more FPS)";
            strcpy(val, g_vsync ? "ON" : "OFF");
        } else if (r == ROW_FX) {
            label = "Water/FX resolution";
            strcpy(val, g_fx_low ? "1/2 (FAST)" : "3/4 (ORIGINAL)");
            if (g_fx_low != fx_boot)
                strcat(val, " (restart)");
        } else if (r == ROW_ASPECT) {
            label = "Aspect ratio (-FPS)";
            strcpy(val, g_aspect_original ? "ORIGINAL 5:3" : "FULL SCREEN");
            if (g_aspect_original != aspect_boot)
                strcat(val, " (restart)");
        } else if (r == ROW_FILTER) {
            label = "Scaling filter (-FPS)";
            strcpy(val, !g_aspect_original ? "- (ORIGINAL only)"
                        : g_filter_smooth ? "SMOOTH" : "SHARP");
        } else if (r == ROW_DEFAULTS) {
            label = "Restore default buttons";
            val[0] = 0;
        } else {
            label = "Quit game";
            val[0] = 0;
        }
        text(24, y, 0xe0e0e0, label);
        text(456, y, 0xe0e0e0, val);
    }
    text(24, 496, 0x909090, "Up/Down choose  X change  [] add  /\\ unbind");
    text(24, 518, 0x909090, "Left/Right value  O save & close");
    gl2d_end();
}

int menu_frame(void) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t b = pad.buttons & ~SCE_CTRL_SELECT;
    uint32_t down = b & ~m_prev;
    m_prev = b;
    int r = m_cursor;
    if (m_need_release) {
        // Wait until the combo / the button that started a capture is let go.
        if (b)
            down = 0;
        else
            m_need_release = 0;
    }
    if (!down) {
        // nothing
    } else if (m_capture != CAP_NONE) {
        for (int i = 0; i < N_BTN_NAMES; i++)
            if (down & btn_names[i].mask) {
                g_bind[r] = (m_capture == CAP_ADD ? g_bind[r] : 0) | btn_names[i].mask;
                m_capture = CAP_NONE;
                m_need_release = 1;
                m_redraw = 3;
                break;
            }
    } else if (down & SCE_CTRL_CIRCLE) {
        config_save();
        m_active = 0;
        return 0;
    } else if (down & (SCE_CTRL_UP | SCE_CTRL_DOWN)) {
        m_cursor = (r + (down & SCE_CTRL_UP ? ROW_COUNT - 1 : 1)) % ROW_COUNT;
        m_redraw = 3;
    } else if (r < A_COUNT) {
        if (down & (SCE_CTRL_CROSS | SCE_CTRL_SQUARE)) {
            m_capture = (down & SCE_CTRL_CROSS) ? CAP_REPLACE : CAP_ADD;
            m_need_release = 1;
        } else if (down & SCE_CTRL_TRIANGLE)
            g_bind[r] = 0;
        m_redraw = 3;
    } else {
        int toggle = down & (SCE_CTRL_CROSS | SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
        if (r == ROW_TOUCH && toggle)
            g_touch_hidden = !g_touch_hidden;
        else if (r == ROW_VSYNC && toggle) {
            g_vsync = !g_vsync;
            vglWaitVblankStart(g_vsync ? GL_TRUE : GL_FALSE);
        } else if (r == ROW_FX && toggle)
            g_fx_low = !g_fx_low;
        else if (r == ROW_ASPECT && toggle)
            g_aspect_original = !g_aspect_original;
        else if (r == ROW_FILTER && toggle)
            g_filter_smooth = !g_filter_smooth;
        else if (r == ROW_CAM && (down & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) {
            int v = g_cam_level + ((down & SCE_CTRL_RIGHT) ? 1 : -1);
            g_cam_level = v < 1 ? 1 : v > CAM_LEVEL_MAX ? CAM_LEVEL_MAX : v;
        } else if (r == ROW_DEFAULTS && (down & SCE_CTRL_CROSS))
            memcpy(g_bind, bind_default, sizeof(g_bind));
        else if (r == ROW_QUIT && (down & SCE_CTRL_CROSS)) {
            config_save();
            sceKernelExitProcess(0);
        }
        m_redraw = 3;
    }
    if (m_redraw <= 0)
        return 0;
    m_redraw--;
    menu_draw();
    return 1;
}
