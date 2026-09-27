/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glutil.h"

#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"

#include <stdio.h>
#include <stdarg.h>
#include <malloc.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>
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

void glBindFramebuffer_soloader(GLenum target, GLuint framebuffer) {
    s_bound_fbo = framebuffer;
    glBindFramebuffer(target, framebuffer);
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
