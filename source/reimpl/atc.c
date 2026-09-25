/*
 * AMD/Qualcomm ATC texture decoding for glCompressedTexImage2D.
 *
 * GLGame.getTextureCompression() is "atc" (GameRenderer.java:124 forces it)
 * and the downloaded .gla packs ship ATC textures ("[ATC] ATC Texture
 * Loading", "[IMAGE-LOADER] == ATC ==" in log 0006). vitaGL only knows
 * DXT/PVRTC/ETC1, so ATC uploads were rejected -> black screen.
 *
 * Block layouts (4x4 texels):
 *   ATC RGB (0x8C92), 8 bytes:
 *     u16 c0: bit15 = mode, RGB555     u16 c1: RGB565     u32 2-bit indices
 *     mode 0: { c0, (5*c0 + 3*c1)/8, (3*c0 + 5*c1)/8, c1 }
 *     mode 1: { 0,  c0 - c1/4,       c0,              c1 }
 *   ATC RGBA explicit alpha (0x8C93), 16 bytes:
 *     8 bytes of 4-bit alpha (same as DXT3), then an ATC RGB block
 *   ATC RGBA interpolated alpha (0x87EE), 16 bytes:
 *     8 bytes alpha block (same as DXT5/BC4), then an ATC RGB block
 *
 * ATC RGB decodes to RGB565 (no precision lost vs. the 555/565 endpoints,
 * half the memory of RGBA8888); the alpha formats decode to RGBA8888.
 */

#include <vitaGL.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "utils/logger.h"

#define GL_ATC_RGB_AMD                      0x8C92
#define GL_ATC_RGBA_EXPLICIT_ALPHA_AMD      0x8C93
#define GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD  0x87EE

static inline uint8_t expand5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t expand6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }

// Decodes the 8-byte ATC color block into 16 RGB triplets.
static void atc_color_block(const uint8_t *b, uint8_t rgb[16][3]) {
    uint32_t c0 = b[0] | (b[1] << 8);
    uint32_t c1 = b[2] | (b[3] << 8);
    uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);

    int e0[3] = { expand5((c0 >> 10) & 0x1F), expand5((c0 >> 5) & 0x1F), expand5(c0 & 0x1F) };
    int e1[3] = { expand5((c1 >> 11) & 0x1F), expand6((c1 >> 5) & 0x3F), expand5(c1 & 0x1F) };
    int pal[4][3];

    for (int ch = 0; ch < 3; ch++) {
        if (c0 & 0x8000) {
            int v = e0[ch] - e1[ch] / 4;
            pal[0][ch] = 0;
            pal[1][ch] = v < 0 ? 0 : v;
            pal[2][ch] = e0[ch];
            pal[3][ch] = e1[ch];
        } else {
            pal[0][ch] = e0[ch];
            pal[1][ch] = (5 * e0[ch] + 3 * e1[ch]) / 8;
            pal[2][ch] = (3 * e0[ch] + 5 * e1[ch]) / 8;
            pal[3][ch] = e1[ch];
        }
    }

    for (int i = 0; i < 16; i++) {
        int k = (idx >> (2 * i)) & 3;
        rgb[i][0] = pal[k][0];
        rgb[i][1] = pal[k][1];
        rgb[i][2] = pal[k][2];
    }
}

static void explicit_alpha_block(const uint8_t *b, uint8_t a[16]) {
    for (int i = 0; i < 16; i++) {
        uint8_t nib = (b[i / 2] >> ((i & 1) * 4)) & 0xF;
        a[i] = (nib << 4) | nib;
    }
}

static void interpolated_alpha_block(const uint8_t *b, uint8_t a[16]) {
    int a0 = b[0], a1 = b[1];
    int pal[8];
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i < 7; i++)
            pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (int i = 1; i < 5; i++)
            pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++)
        bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++)
        a[i] = pal[(bits >> (3 * i)) & 7];
}

static int is_atc(GLenum fmt) {
    return fmt == GL_ATC_RGB_AMD || fmt == GL_ATC_RGBA_EXPLICIT_ALPHA_AMD ||
           fmt == GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD;
}

void glCompressedTexImage2D_soloader(GLenum target, GLint level, GLenum internalformat,
                                     GLsizei width, GLsizei height, GLint border,
                                     GLsizei imageSize, const void *data) {
    if (!is_atc(internalformat)) {
        glCompressedTexImage2D(target, level, internalformat, width, height,
                               border, imageSize, data);
        return;
    }

    int has_alpha = internalformat != GL_ATC_RGB_AMD;
    int block_bytes = has_alpha ? 16 : 8;
    int bw = (width + 3) / 4, bh = (height + 3) / 4;

    if (!data || imageSize < bw * bh * block_bytes) {
        l_warn("ATC: level %d %dx%d fmt 0x%x: imageSize %d < %d, uploading empty",
               level, width, height, internalformat, imageSize, bw * bh * block_bytes);
        glTexImage2D(target, level, has_alpha ? GL_RGBA : GL_RGB, width, height, 0,
                     has_alpha ? GL_RGBA : GL_RGB,
                     has_alpha ? GL_UNSIGNED_BYTE : GL_UNSIGNED_SHORT_5_6_5, NULL);
        return;
    }

    int bpp = has_alpha ? 4 : 2;
    uint8_t *out = malloc((size_t)width * height * bpp);
    if (!out) {
        l_error("ATC: out of memory decoding %dx%d", width, height);
        return;
    }

    const uint8_t *src = data;
    uint8_t rgb[16][3], alpha[16];

    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++, src += block_bytes) {
            if (internalformat == GL_ATC_RGBA_EXPLICIT_ALPHA_AMD)
                explicit_alpha_block(src, alpha);
            else if (internalformat == GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD)
                interpolated_alpha_block(src, alpha);
            atc_color_block(src + (has_alpha ? 8 : 0), rgb);

            for (int py = 0; py < 4; py++) {
                int y = by * 4 + py;
                if (y >= height)
                    break;
                for (int px = 0; px < 4; px++) {
                    int x = bx * 4 + px;
                    if (x >= width)
                        break;
                    int i = py * 4 + px;
                    size_t o = ((size_t)y * width + x) * bpp;
                    if (has_alpha) {
                        out[o + 0] = rgb[i][0];
                        out[o + 1] = rgb[i][1];
                        out[o + 2] = rgb[i][2];
                        out[o + 3] = alpha[i];
                    } else {
                        uint16_t p = ((rgb[i][0] >> 3) << 11) | ((rgb[i][1] >> 2) << 5) | (rgb[i][2] >> 3);
                        out[o + 0] = p & 0xFF;
                        out[o + 1] = p >> 8;
                    }
                }
            }
        }
    }

    glTexImage2D(target, level, has_alpha ? GL_RGBA : GL_RGB, width, height, 0,
                 has_alpha ? GL_RGBA : GL_RGB,
                 has_alpha ? GL_UNSIGNED_BYTE : GL_UNSIGNED_SHORT_5_6_5, out);
    free(out);

    static int logged = 0;
    if (logged < 5) {
        l_info("ATC: decoded level %d %dx%d fmt 0x%x -> %s", level, width, height,
               internalformat, has_alpha ? "RGBA8888" : "RGB565");
        logged++;
    }
}
