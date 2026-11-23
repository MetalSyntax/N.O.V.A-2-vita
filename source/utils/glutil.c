/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glutil.h"
#include "crash_guard.h"

#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"

#include <stdio.h>
#include <stdarg.h>
#include <malloc.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>

// Helpers for our handling of shaders
GLboolean skip_next_compile = GL_FALSE;
char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);

void gl_preload() {
    if (!file_exists("ur0:/data/libshacccg.suprx")
        && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        fatal_error("Error: libshacccg.suprx is not installed. "
                    "Google \"ShaRKBR33D\" for quick installation.");
    }

#ifdef USE_GLSL_SHADERS
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
#endif
}

// vitaGL (built from lib/vitagl with LOG_ERRORS=1) reports shader compiler
// errors, patching failures, etc. through this hook -> our .log file.
void vgl_log(const char *fmt, ...) {
    if (fmt && strstr(fmt, "Shader Compiler: I]"))
        return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    if (len > 0)
        l_warn("[vitaGL] %s", buf);
}

// Same setup as the shipped Gameloft ports on this engine family (GT Racing
// Motor Academy HD, Asphalt 5/6, BiA2): multi-scene render targets for the
// engine's FBO passes, on-disk shader cache, cached mempools, 12 MB RAM
// threshold and NO MSAA (4X + OES FBOs is a risky combo in vitaGL, see
// Gangstar Miami's port notes). main.c and eglInitialize both call this.
void gl_init() {
    static int initialized = 0;
    if (initialized)
        return;
    initialized = 1;

    vglSetupRenderTargetScenesNum(8, 8);
    // Recolector de vitaGL fuera del núcleo del juego (default: hereda el 0).
    vglSetupGarbageCollector(0x10000100, SCE_KERNEL_CPU_MASK_USER_1);
    sceIoMkdir(DATA_PATH "shader_cache", 0777);
    vglSetShaderCachePath(DATA_PATH "shader_cache");
    vglUseCachedMem(GL_TRUE);

    // = vglInitExtended(12 MB RAM libres), pero SIN pool PHYCONT: la PHYCONT
    // queda libre para los frames de SceAvPlayer del intro (log 0016/0017:
    // sin memoria contigua el decoder se detiene con 0 frames; un buffer en
    // RAM común tampoco sirve). Tras el video, gl_provide_phycont() se la da
    // a vitaGL (vglPhycontMemLazyInit, pensado para este caso).
    GLboolean clamped = vglInitWithCustomThreshold(0, 960, 544, 12 * 1024 * 1024,
                                                   0, 0x7FFFFFFF,
                                                   0x8C6000 /* SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE (vitaGL mem_utils.h) */,
                                                   SCE_GXM_MULTISAMPLE_NONE);
    // NOVA2 port (crash 0007): vglInit creates shader_cache/v and /f, but
    // only when vglSetShaderCachePath ran before it. Re-create them here so
    // a fresh install (or a future reordering) can never leave the cache
    // writer with a bad fd again.
    sceIoMkdir(DATA_PATH "shader_cache/v", 0777);
    sceIoMkdir(DATA_PATH "shader_cache/f", 0777);
    l_success("vitaGL initialized (960x544, no MSAA%s)", clamped ? ", resolution clamped" : "");
}

void gl_provide_phycont() {
    SceKernelFreeMemorySizeInfo info;
    info.size = sizeof(info);
    sceKernelGetFreeMemorySize(&info);
    // Se deja 1 MB de margen para el sistema; el pool va en múltiplos de 1 MB.
    size_t sz = info.size_phycont > 0x100000 ? (info.size_phycont - 0x100000) & ~0xFFFFF : 0;
    if (sz)
        vglPhycontMemLazyInit(sz);
    l_success("vitaGL: PHYCONT pool %u KB entregado tras el intro", (unsigned)(sz / 1024));
}

void gl_swap() {
    crash_guard_poll();
    vglSwapBuffers(GL_FALSE);
}

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glShaderSource<%p>(shader: %i, count: %i, string: %p, length: %p)\n", __builtin_return_address(0), shader, count, string, _length);
#endif
    if (!string) {
        l_error("<%p> Shader source string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    } else if (!*string) {
        l_error("<%p> Shader source *string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    }

    size_t total_length = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            total_length += strlen(string[i]);
        } else {
            total_length += _length[i];
        }
    }

    char * str = malloc(total_length+1);
    size_t l = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            memcpy(str + l, string[i], strlen(string[i]));
            l += strlen(string[i]);
        } else {
            memcpy(str + l, string[i], _length[i]);
            l += _length[i];
        }
    }
    str[total_length] = '\0';

    load_shader(shader, str, total_length);

    free(str);
}

void glCompileShader_soloader(GLuint shader) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glCompileShader<%p>(shader: %i)\n", __builtin_return_address(0), shader);
#endif

#ifndef USE_GXP_SHADERS
    if (!skip_next_compile) {
        glCompileShader(shader);

        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char info[1024] = { 0 };
            glGetShaderInfoLog(shader, sizeof(info), NULL, info);
            l_error("glCompileShader(%u) FAILED: %s", shader, info);
        }
#ifdef DUMP_COMPILED_SHADERS
        else {
            void *bin = vglMalloc(32 * 1024);
            GLsizei len = 0;
            vglGetShaderBinary(shader, 32 * 1024, &len, bin);
            if (len > 0) {
                file_mkpath(next_shader_fname, 0777);
                file_save(next_shader_fname, bin, len);
            }
            vglFree(bin);
        }
#endif
    }
    skip_next_compile = GL_FALSE;
#endif
}

void glLinkProgram_soloader(GLuint program) {
    glLinkProgram(program);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char info[1024] = { 0 };
        glGetProgramInfoLog(program, sizeof(info), NULL, info);
        l_error("glLinkProgram(%u) FAILED: %s", program, info);
    }
}

// Ver glutil.h: pantalla 1:1 960x544, passthrough total. El 960x640 que
// emite el motor es un FBO propio (log 0010) y los fondos estáticos 800x480
// se estiran a nivel sprite (patch.c), NO por viewport: tocar viewport/
// scissor aquí rompía el juego 3D. Solo se rastrea el FBO para el log.
static GLuint s_bound_fbo = 0;

// Aspecto ORIGINAL (menú SELECT+START): el motor renderiza a 800x480 en
// s_game_fbo (el "FBO 0" del motor se redirige ahí) y gl_game_present lo
// escala a pantalla. Cuesta FPS: escena extra + un quad de pantalla completa.
static GLuint s_game_fbo, s_game_tex, s_game_rb, s_blit_prog;

void glBindFramebuffer_soloader(GLenum target, GLuint framebuffer) {
    s_bound_fbo = framebuffer;
    glBindFramebuffer(target, framebuffer ? framebuffer : s_game_fbo);
}

// Diagnóstico (caída de FPS con líquidos, 2026-11-22): glitch emula RTT con
// glCopyTexSubImage2D si el driver no tiene FBO (CRenderTarget::unbind); en
// vitaGL eso cierra la escena GXM. Se loguean solo las primeras llamadas.
void glCopyTexSubImage2D_soloader(GLenum target, GLint level, GLint xoff, GLint yoff,
                                  GLint x, GLint y, GLsizei w, GLsizei h) {
    static int n;
    if (n < 4)
        l_error("[glcopy] #%d CopyTexSubImage2D %dx%d FBO%u", ++n, w, h, s_bound_fbo);
    glCopyTexSubImage2D(target, level, xoff, yoff, x, y, w, h);
}

void gl_screen_fb(int on) {
    glBindFramebuffer(GL_FRAMEBUFFER, on ? 0 : (s_bound_fbo ? s_bound_fbo : s_game_fbo));
}

int gl_game_fbo_init(int w, int h) {
    GLint tex;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGenTextures(1, &s_game_tex);
    glBindTexture(GL_TEXTURE_2D, s_game_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGenRenderbuffers(1, &s_game_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, s_game_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glGenFramebuffers(1, &s_game_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_game_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_game_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_game_rb);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        l_error("game FBO %dx%d incomplete (0x%x): back to direct rendering", w, h, st);
        glBindFramebuffer(GL_FRAMEBUFFER, s_bound_fbo);
        glDeleteFramebuffers(1, &s_game_fbo);
        s_game_fbo = 0;
        return 0;
    }
    // Si el motor tenía un RT propio enlazado, se respeta.
    if (s_bound_fbo)
        glBindFramebuffer(GL_FRAMEBUFFER, s_bound_fbo);

    static const char *vs =
        "attribute vec2 aPos;\n"
        "attribute vec2 aUV;\n"
        "varying vec2 vUV;\n"
        "void main() { gl_Position = vec4(aPos, 0.0, 1.0); vUV = aUV; }\n";
    static const char *fs =
        "precision mediump float;\n"
        "varying vec2 vUV;\n"
        "uniform sampler2D uTex;\n"
        "void main() { gl_FragColor = vec4(texture2D(uTex, vUV).rgb, 1.0); }\n";
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &vs, NULL);
    glCompileShader_soloader(v);
    glShaderSource(f, 1, &fs, NULL);
    glCompileShader_soloader(f);
    s_blit_prog = glCreateProgram();
    glAttachShader(s_blit_prog, v);
    glAttachShader(s_blit_prog, f);
    glBindAttribLocation(s_blit_prog, 0, "aPos");
    glBindAttribLocation(s_blit_prog, 1, "aUV");
    glLinkProgram_soloader(s_blit_prog);
    glDeleteShader(v);
    glDeleteShader(f);
    l_info("game FBO %dx%d ready", w, h);
    return 1;
}

// Copia el frame del motor a la pantalla en (x,y,w,h) (origen arriba-izq.),
// bordes negros. Guarda/restaura todo el estado GL que toca: el motor
// (glitch) cachea el suyo y no lo vuelve a fijar.
void gl_game_present(int x, int y, int w, int h, int smooth) {
    if (!s_game_fbo)
        return;
    GLint prog, active, tex, abuf, vp[4], en[2];
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST),
              sc = glIsEnabled(GL_SCISSOR_TEST), cull = glIsEnabled(GL_CULL_FACE),
              sten = glIsEnabled(GL_STENCIL_TEST);
    GLfloat cc[4];
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    for (int i = 0; i < 2; i++)
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en[i]);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glViewport(0, 0, 960, 544);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    float x0 = x / 480.0f - 1.0f, x1 = (x + w) / 480.0f - 1.0f;
    float y0 = 1.0f - y / 272.0f, y1 = 1.0f - (y + h) / 272.0f;
    // Textura de FBO: la fila 0 es la de abajo -> v=1 arriba.
    const GLfloat pos[8] = { x0, y0, x1, y0, x0, y1, x1, y1 };
    const GLfloat uv[8] = { 0, 1, 1, 1, 0, 0, 1, 0 };
    glUseProgram(s_blit_prog);
    glBindTexture(GL_TEXTURE_2D, s_game_tex);
    GLint filt = smooth ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    glUniform1i(glGetUniformLocation(s_blit_prog, "uTex"), 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    for (int i = 0; i < 2; i++)
        if (!en[i])
            glDisableVertexAttribArray(i);
    glBindBuffer(GL_ARRAY_BUFFER, abuf);
    glBindTexture(GL_TEXTURE_2D, tex);
    glActiveTexture(active);
    glUseProgram(prog);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    if (blend) glEnable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (sc) glEnable(GL_SCISSOR_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (sten) glEnable(GL_STENCIL_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, s_bound_fbo ? s_bound_fbo : s_game_fbo);
}

void glViewport_soloader(GLint x, GLint y, GLsizei width, GLsizei height) {
    static GLint lx = -1, ly = -1;
    static GLsizei lw = -1, lh = -1;
    // El motor alterna 3-4 viewports por frame (log 0014: ~1000 líneas
    // síncronas a ux0 en juego); solo se loguean los primeros cambios.
    static int logged = 0;
    if ((x != lx || y != ly || width != lw || height != lh) && logged < 24) {
        logged++;
        lx = x; ly = y; lw = width; lh = height;
        l_error("[glvp] viewport FBO%u(%d, %d, %d, %d)",
                s_bound_fbo, x, y, width, height);
    }
    glViewport(x, y, width, height);
}

void glScissor_soloader(GLint x, GLint y, GLsizei width, GLsizei height) {
    glScissor(x, y, width, height);
}

#if defined(USE_GLSL_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else {
        glShaderSource(shader, 1, &string, &length);
        strcpy(next_shader_fname, gxp_path);
    }

    free(sha_name);
}
#elif defined(USE_GLSL_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    glShaderSource(shader, 1, &string, &length);
}
#elif defined(USE_CG_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    char cg_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);
    snprintf(cg_path, sizeof(cg_path), DATA_PATH"cg/%s.cg", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else if (file_exists(cg_path)) {
        char *buffer;
        size_t size;

        file_load(cg_path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);
        strcpy(next_shader_fname, gxp_path);

        free(buffer);
        skip_next_compile = GL_FALSE;
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }

        skip_next_compile = GL_FALSE;
    }

    free(sha_name);
}
#elif defined(USE_CG_SHADERS) || defined(USE_GXP_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char path[256];
#ifdef USE_CG_SHADERS
    snprintf(path, sizeof(path), DATA_PATH"cg/%s.cg", sha_name);
#else
    snprintf(path, sizeof(path), DATA_PATH"gxp/%s.gxp", sha_name);
#endif

    if (file_exists(path)) {
#ifdef USE_CG_SHADERS
        char *buffer;
        size_t size;

        file_load(path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);

        free(buffer);
#else
        uint8_t *buffer;
        size_t size;

        file_load(path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
#endif
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }
    }

    free(sha_name);
}
#else
#error "Define one of (USE_GLSL_SHADERS, USE_CG_SHADERS, USE_GXP_SHADERS)"
#endif
