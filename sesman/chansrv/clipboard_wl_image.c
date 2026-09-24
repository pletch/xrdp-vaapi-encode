/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) 2026 Tim Pletcher, all xrdp contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * CF_DIB <-> image/bmp and image/png for the Wayland clipboard. DIBs are
 * read at 24 or 32 bpp (BI_RGB, or BI_BITFIELDS masks) into RGBA, and
 * written as 32bpp BI_RGB with a BITMAPINFOHEADER, bottom-up, the form
 * every Windows application reads.
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(XRDP_WAYLAND_PNG)
#include <png.h>
#endif

#include "clipboard_wl_image.h"

#define BMP_FILE_HEADER 14
#define BI_HEADER 40
#define BI_RGB 0
#define BI_BITFIELDS 3
#define MAX_PIXELS (64 * 1024 * 1024)

/*****************************************************************************/
static uint32_t
rd32(const char *p)
{
    const unsigned char *u = (const unsigned char *) p;

    return u[0] | (u[1] << 8) | (u[2] << 16) | ((uint32_t) u[3] << 24);
}

static unsigned int
rd16(const char *p)
{
    const unsigned char *u = (const unsigned char *) p;

    return u[0] | (u[1] << 8);
}

static void
wr32(char *p, uint32_t v)
{
    p[0] = (char) v;
    p[1] = (char) (v >> 8);
    p[2] = (char) (v >> 16);
    p[3] = (char) (v >> 24);
}

static void
wr16(char *p, unsigned int v)
{
    p[0] = (char) v;
    p[1] = (char) (v >> 8);
}

/*****************************************************************************/
/* Where a DIB's pixels start: after the header, the three masks a
   BITMAPINFOHEADER with BI_BITFIELDS carries, and the palette. -1 if the
   DIB is not sane. */
static int
dib_pixel_offset(const char *dib, int len)
{
    uint32_t size;
    unsigned int bpp;
    uint32_t compression;
    uint32_t colors;
    int64_t off;

    if (len < BI_HEADER)
    {
        return -1;
    }
    size = rd32(dib);
    bpp = rd16(dib + 14);
    compression = rd32(dib + 16);
    colors = rd32(dib + 32);
    if (size < BI_HEADER || size > (uint32_t) len)
    {
        return -1;
    }
    off = size;
    if (size == BI_HEADER && compression == BI_BITFIELDS)
    {
        off += 12;
    }
    if (bpp <= 8)
    {
        off += (int64_t) (colors != 0 ? colors : (1u << bpp)) * 4;
    }
    else
    {
        off += (int64_t) colors * 4;
    }
    return off <= len ? (int) off : -1;
}

/*****************************************************************************/
static int
mask_shift(uint32_t mask)
{
    int shift = 0;

    if (mask == 0)
    {
        return -1;
    }
    while ((mask & 1) == 0)
    {
        mask >>= 1;
        shift++;
    }
    return shift;
}

/*****************************************************************************/
/* A 24 or 32 bpp DIB into RGBA rows, top-down. Returns 0 on success. */
static int
dib_decode(const char *dib, int len, uint8_t **rgba, int *width, int *height)
{
    int w;
    int h;
    int top_down;
    unsigned int bpp;
    uint32_t compression;
    uint32_t masks[4] = { 0x00ff0000, 0x0000ff00, 0x000000ff, 0 };
    int shifts[4];
    int off;
    int stride;
    int has_alpha = 0;
    int x;
    int y;
    int i;
    uint8_t *out;

    off = dib_pixel_offset(dib, len);
    if (off < 0)
    {
        return 1;
    }
    w = (int) rd32(dib + 4);
    h = (int) rd32(dib + 8);
    bpp = rd16(dib + 14);
    compression = rd32(dib + 16);
    top_down = h < 0;
    h = top_down ? -h : h;
    if (w <= 0 || h <= 0 || (int64_t) w * h > MAX_PIXELS ||
            (bpp != 24 && bpp != 32) ||
            (compression != BI_RGB && compression != BI_BITFIELDS) ||
            (compression == BI_BITFIELDS && bpp != 32))
    {
        return 1;
    }
    if (compression == BI_BITFIELDS)
    {
        /* after a BITMAPINFOHEADER, or inside a V4/V5 header */
        masks[0] = rd32(dib + 40);
        masks[1] = rd32(dib + 44);
        masks[2] = rd32(dib + 48);
        masks[3] = rd32(dib) >= 56 ? rd32(dib + 52) : 0;
    }
    else if (bpp == 32)
    {
        masks[3] = 0xff000000; /* maybe: see has_alpha below */
    }
    for (i = 0; i < 4; i++)
    {
        shifts[i] = mask_shift(masks[i]);
    }
    if (shifts[0] < 0 || shifts[1] < 0 || shifts[2] < 0)
    {
        return 1;
    }
    stride = ((w * (int) bpp + 31) / 32) * 4;
    if ((int64_t) stride * h > len - off)
    {
        return 1;
    }
    out = (uint8_t *) malloc((size_t) w * h * 4);
    if (out == NULL)
    {
        return 1;
    }
    for (y = 0; y < h; y++)
    {
        const unsigned char *src = (const unsigned char *) dib + off +
                                   (size_t) stride * (top_down ? y : h - 1 - y);
        uint8_t *dst = out + (size_t) y * w * 4;

        for (x = 0; x < w; x++)
        {
            uint32_t p;

            if (bpp == 24)
            {
                p = src[x * 3] | (src[x * 3 + 1] << 8) | (src[x * 3 + 2] << 16);
            }
            else
            {
                p = rd32((const char *) src + x * 4);
            }
            for (i = 0; i < 3; i++)
            {
                dst[x * 4 + i] = (uint8_t) ((p & masks[i]) >> shifts[i]);
            }
            dst[x * 4 + 3] = shifts[3] < 0 ? 0xff
                             : (uint8_t) ((p & masks[3]) >> shifts[3]);
            has_alpha |= dst[x * 4 + 3] != 0;
        }
    }
    if (!has_alpha)
    {
        /* 32bpp BI_RGB leaves the fourth byte undefined, and Windows
           writes 0 there: all zero means opaque, not invisible */
        for (i = 0; i < w * h; i++)
        {
            out[i * 4 + 3] = 0xff;
        }
    }
    *rgba = out;
    *width = w;
    *height = h;
    return 0;
}

/*****************************************************************************/
/* RGBA rows, top-down, into a 32bpp BI_RGB DIB, bottom-up */
static int
dib_encode(const uint8_t *rgba, int w, int h, char **out, int *out_len)
{
    size_t bytes = BI_HEADER + (size_t) w * h * 4;
    char *dib;
    int x;
    int y;

    dib = (char *) malloc(bytes);
    if (dib == NULL)
    {
        return 1;
    }
    memset(dib, 0, BI_HEADER);
    wr32(dib, BI_HEADER);
    wr32(dib + 4, w);
    wr32(dib + 8, h);
    wr16(dib + 12, 1);                          /* planes */
    wr16(dib + 14, 32);                         /* bpp */
    wr32(dib + 16, BI_RGB);
    wr32(dib + 20, (uint32_t) w * h * 4);
    for (y = 0; y < h; y++)
    {
        const uint8_t *src = rgba + (size_t) (h - 1 - y) * w * 4;
        uint8_t *dst = (uint8_t *) dib + BI_HEADER + (size_t) y * w * 4;

        for (x = 0; x < w; x++)
        {
            dst[x * 4] = src[x * 4 + 2];        /* B G R A */
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }
    *out = dib;
    *out_len = (int) bytes;
    return 0;
}

/*****************************************************************************/
int
wl_image_dib_to_bmp(const char *dib, int len, char **out, int *out_len)
{
    int off = dib_pixel_offset(dib, len);
    char *bmp;

    if (off < 0)
    {
        return 1;
    }
    bmp = (char *) malloc(BMP_FILE_HEADER + len);
    if (bmp == NULL)
    {
        return 1;
    }
    bmp[0] = 'B';
    bmp[1] = 'M';
    wr32(bmp + 2, BMP_FILE_HEADER + len);
    wr32(bmp + 6, 0);
    wr32(bmp + 10, BMP_FILE_HEADER + off);
    memcpy(bmp + BMP_FILE_HEADER, dib, len);
    *out = bmp;
    *out_len = BMP_FILE_HEADER + len;
    return 0;
}

/*****************************************************************************/
int
wl_image_bmp_to_dib(const char *bmp, int len, char **out, int *out_len)
{
    const char *dib = bmp + BMP_FILE_HEADER;
    int dib_len = len - BMP_FILE_HEADER;
    uint8_t *rgba;
    int w;
    int h;
    int rv;

    if (len < BMP_FILE_HEADER + BI_HEADER || bmp[0] != 'B' || bmp[1] != 'M')
    {
        return 1;
    }
    if (rd32(dib) != BI_HEADER && dib_decode(dib, dib_len, &rgba, &w, &h) == 0)
    {
        rv = dib_encode(rgba, w, h, out, out_len);
        free(rgba);
        return rv;
    }
    *out = (char *) malloc(dib_len);
    if (*out == NULL)
    {
        return 1;
    }
    memcpy(*out, dib, dib_len);
    *out_len = dib_len;
    return 0;
}

#if defined(XRDP_WAYLAND_PNG)

/*****************************************************************************/
struct membuf
{
    char *data;
    size_t len;
    size_t size;
    size_t pos;         /* reading */
};

static void
png_write_mem(png_structp png, png_bytep data, png_size_t len)
{
    struct membuf *m = (struct membuf *) png_get_io_ptr(png);

    if (m->len + len > m->size)
    {
        size_t size = m->size * 2 > m->len + len ? m->size * 2 : m->len + len;
        char *p = (char *) realloc(m->data, size);

        if (p == NULL)
        {
            png_error(png, "out of memory");
        }
        m->data = p;
        m->size = size;
    }
    memcpy(m->data + m->len, data, len);
    m->len += len;
}

static void
png_flush_mem(png_structp png)
{
}

static void
png_read_mem(png_structp png, png_bytep data, png_size_t len)
{
    struct membuf *m = (struct membuf *) png_get_io_ptr(png);

    if (m->pos + len > m->len)
    {
        png_error(png, "truncated");
    }
    memcpy(data, m->data + m->pos, len);
    m->pos += len;
}

/*****************************************************************************/
int
wl_image_have_png(void)
{
    return 1;
}

/*****************************************************************************/
int
wl_image_dib_to_png(const char *dib, int len, char **out, int *out_len)
{
    png_structp png;
    png_infop info;
    struct membuf m;
    uint8_t *decoded = NULL;
    uint8_t *volatile rgba;
    png_bytep *volatile rows;
    int w;
    int h;
    int y;

    if (dib_decode(dib, len, &decoded, &w, &h) != 0)
    {
        return 1;
    }
    rgba = decoded;
    memset(&m, 0, sizeof(m));
    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    info = png != NULL ? png_create_info_struct(png) : NULL;
    rows = (png_bytep *) malloc(sizeof(png_bytep) * h);
    m.size = (size_t) w * h + 1024;
    m.data = (char *) malloc(m.size);
    if (info == NULL || rows == NULL || m.data == NULL)
    {
        png_destroy_write_struct(&png, &info);
        free(rows);
        free(rgba);
        free(m.data);
        return 1;
    }
    if (setjmp(png_jmpbuf(png)))
    {
        /* m lives in memory (libpng holds its address): m.data is current */
        png_destroy_write_struct(&png, &info);
        free(rows);
        free(rgba);
        free(m.data);
        return 1;
    }
    png_set_write_fn(png, &m, png_write_mem, png_flush_mem);
    /* a clipboard image is read once, soon: favour speed */
    png_set_compression_level(png, 1);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB_ALPHA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    for (y = 0; y < h; y++)
    {
        rows[y] = rgba + (size_t) y * w * 4;
    }
    png_set_rows(png, info, rows);
    png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);
    png_destroy_write_struct(&png, &info);
    free(rows);
    free(rgba);
    *out = m.data;
    *out_len = (int) m.len;
    return 0;
}

/*****************************************************************************/
int
wl_image_png_to_dib(const char *data, int len, char **out, int *out_len)
{
    png_structp png;
    png_infop info;
    struct membuf m;
    uint8_t *volatile rgba = NULL;
    png_bytep *volatile rows = NULL;
    png_uint_32 w;
    png_uint_32 h;
    png_uint_32 y;
    int rv;

    if (len < 8 || png_sig_cmp((png_const_bytep) data, 0, 8) != 0)
    {
        return 1;
    }
    memset(&m, 0, sizeof(m));
    m.data = (char *) data;
    m.len = len;
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    info = png != NULL ? png_create_info_struct(png) : NULL;
    if (info == NULL)
    {
        png_destroy_read_struct(&png, &info, NULL);
        return 1;
    }
    if (setjmp(png_jmpbuf(png)))
    {
        png_destroy_read_struct(&png, &info, NULL);
        free(rows);
        free(rgba);
        return 1;
    }
    png_set_read_fn(png, &m, png_read_mem);
    png_set_user_limits(png, 32768, 32768);
    png_read_info(png, info);
    w = png_get_image_width(png, info);
    h = png_get_image_height(png, info);
    if ((uint64_t) w * h > MAX_PIXELS)
    {
        png_error(png, "too big");
    }
    /* any PNG to 8-bit RGBA */
    png_set_expand(png);
    png_set_strip_16(png);
    png_set_gray_to_rgb(png);
    png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    rgba = (uint8_t *) malloc((size_t) w * h * 4);
    rows = (png_bytep *) malloc(sizeof(png_bytep) * h);
    if (rgba == NULL || rows == NULL)
    {
        png_error(png, "out of memory");
    }
    for (y = 0; y < h; y++)
    {
        rows[y] = rgba + (size_t) y * w * 4;
    }
    png_read_image(png, rows);
    png_destroy_read_struct(&png, &info, NULL);
    rv = dib_encode(rgba, (int) w, (int) h, out, out_len);
    free(rows);
    free(rgba);
    return rv;
}

#else

int
wl_image_have_png(void)
{
    return 0;
}

int
wl_image_dib_to_png(const char *dib, int len, char **out, int *out_len)
{
    return 1;
}

int
wl_image_png_to_dib(const char *png, int len, char **out, int *out_len)
{
    return 1;
}

#endif
