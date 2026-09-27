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

// Entrega la PHYCONT libre a vitaGL (llamar después del video intro).
void gl_provide_phycont();

void gl_swap();

void glCompileShader_soloader(GLuint shader);

void glLinkProgram_soloader(GLuint program);

// Pantalla 1:1 960x544 (2026-09-25, log 0010): el mundo lógico 960x640
// ROMPE el juego 3D (Is960x640Res -> otra cámara/FOV). Se reporta lo real y
// viewport/scissor pasan intactos. Los fondos estáticos 800x480 (carga/
// título: el motor no tiene bucket para 960x544 y los pinta a tamaño nativo)
// se estiran en patch.c (hook de CSprite::PaintFrame), sin tocar el 3D.
#define ENG_W 960
#define ENG_H 544
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
