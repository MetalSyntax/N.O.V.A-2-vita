/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  glutil.h
 * @brief OpenGL API initializer, related functions.
 */

#ifndef SOLOADER_GLUTIL_H
#define SOLOADER_GLUTIL_H

#include <vitaGL.h>

#ifdef __cplusplus
extern "C" {
#endif

void gl_init();

void gl_preload();

void gl_swap();

void glCompileShader_soloader(GLuint shader);

void glLinkProgram_soloader(GLuint program);

// Mundo lógico 960x640 -> pantalla física 960x544 (2026-09-25, confirmado por
// log 0009 + desensamblado, mismo caso que Dungeon-Hunter-2-vita):
// - El motor emite viewport(0,0,960,640) (espacio "iPhone retina"/HD) aunque
//   conoce 960x544; el 2D cae al bucket default de 800x480 (barra negra
//   derecha de 160px medida a píxel en carga/título) y el 3D pierde 96 filas.
// - main.c reporta 960x640 al engine (nativeInit/Resize) -> Is960x640Res TRUE,
//   layouts de 960. EGL sigue reportando lo REAL (960x544, como DH2).
// - Estos wrappers reescalan y (×544/640) cuando se dibuja en pantalla (FBO 0);
//   pases a FBO propios (sombras 1024) pasan intactos vía glBindFramebuffer.
// - Touch inverso global en main.c: ly = py*640/544 (DH2).
#define LOGICAL_W 960
#define LOGICAL_H 640
#define REAL_W 960
#define REAL_H 544

void glViewport_soloader(GLint x, GLint y, GLsizei width, GLsizei height);
void glScissor_soloader(GLint x, GLint y, GLsizei width, GLsizei height);
// Rastrea el FBO bindeado para el guard de los remaps (0 = pantalla).
void glBindFramebuffer_soloader(GLenum target, GLuint framebuffer);

// source/reimpl/atc.c: decodes ATC (AMD) textures, passes others through.
void glCompressedTexImage2D_soloader(GLenum target, GLint level, GLenum internalformat,
                                     GLsizei width, GLsizei height, GLint border,
                                     GLsizei imageSize, const void *data);

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GLUTIL_H
