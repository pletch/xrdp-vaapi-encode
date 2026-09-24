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
 *
 * The two pixel conversions are ported from xorgxrdp's rdpCapture.c:
 * Copyright 2005-2017 Jay Sorg; MIT licence:
 *
 *   Permission to use, copy, modify, distribute, and sell this software and
 *   its documentation for any purpose is hereby granted without fee,
 *   provided that the above copyright notice appear in all copies and that
 *   both that copyright notice and this permission notice appear in
 *   supporting documentation.
 */

/*
 * wlxrdp's CPU path: frames the helper does not encode (a client without
 * GFX H.264, or no working VA-API) are converted here into the layouts
 * xorgxrdp gives xrdp for each capture mode, and xrdp encodes them.
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdint.h>
#include <string.h>

#include "wlxrdp_cpu.h"

#define CLAMP255(_v) ((_v) < 0 ? 0 : (_v) > 255 ? 255 : (_v))

/*****************************************************************************/
/* XRGB8888 to NV12, full-range BT.709, one box with even origin and size
   (xorgxrdp's a8r8g8b8_to_nv12_709fr_box) */
static void
xrgb_to_nv12_709fr_box(const uint8_t *s8, int src_stride,
                       uint8_t *d8_y, int dst_stride_y,
                       uint8_t *d8_uv, int dst_stride_uv,
                       int width, int height, int bgr)
{
    int rs = bgr ? 0 : 16;
    int bs = bgr ? 16 : 0;

    int i;
    int j;

    for (j = 0; j < height; j += 2)
    {
        const uint32_t *sa = (const uint32_t *) (s8 + src_stride * j);
        const uint32_t *sb = (const uint32_t *) (s8 + src_stride * (j + 1));
        uint8_t *ya = d8_y + dst_stride_y * j;
        uint8_t *yb = d8_y + dst_stride_y * (j + 1);
        uint8_t *uv = d8_uv + dst_stride_uv * (j / 2);

        for (i = 0; i < width; i += 2)
        {
            const uint32_t px[4] = { sa[0], sa[1], sb[0], sb[1] };
            uint8_t *yd[4] = { ya, ya + 1, yb, yb + 1 };
            int us = 0;
            int vs = 0;
            int k;

            for (k = 0; k < 4; k++)
            {
                int r = (px[k] >> rs) & 0xff;
                int g = (px[k] >> 8) & 0xff;
                int b = (px[k] >> bs) & 0xff;
                int y = (54 * r + 183 * g + 18 * b) >> 8;
                int u = ((-29 * r - 99 * g + 128 * b) >> 8) + 128;
                int v = ((128 * r - 116 * g - 12 * b) >> 8) + 128;

                *yd[k] = CLAMP255(y);
                us += CLAMP255(u);
                vs += CLAMP255(v);
            }
            uv[0] = (us + 2) / 4;
            uv[1] = (vs + 2) / 4;
            sa += 2;
            sb += 2;
            ya += 2;
            yb += 2;
            uv += 2;
        }
    }
}

/*****************************************************************************/
/* XRGB8888 rows into one 64x64 planar YUVA tile (xorgxrdp's
   a8r8g8b8_to_yuvalp_box; the RFX colour matrix) */
static void
xrgb_to_yuvalp_box(const uint8_t *s8, int src_stride,
                   uint8_t *d8, int width, int height, int bgr)
{
    int rs = bgr ? 0 : 16;
    int bs = bgr ? 16 : 0;

    int i;
    int j;

    for (j = 0; j < height; j++)
    {
        const uint32_t *s32 = (const uint32_t *) (s8 + src_stride * j);
        uint8_t *yp = d8 + 64 * j;
        uint8_t *up = yp + 64 * 64;
        uint8_t *vp = up + 64 * 64;
        uint8_t *ap = vp + 64 * 64;

        for (i = 0; i < width; i++)
        {
            uint32_t p = s32[i];
            int r = (p >> rs) & 0xff;
            int g = (p >> 8) & 0xff;
            int b = (p >> bs) & 0xff;
            int y = (r * 19595 + g * 38470 + b * 7471) >> 16;
            int u = ((r * -11071 + g * -21736 + b * 32807) >> 16) + 128;
            int v = ((r * 32756 + g * -27429 + b * -5327) >> 16) + 128;

            yp[i] = CLAMP255(y);
            up[i] = CLAMP255(u);
            vp[i] = CLAMP255(v);
            ap[i] = 0xff; /* XRGB: opaque */
        }
    }
}

/*****************************************************************************/
static uint64_t
tile_hash(const uint8_t *tile)
{
    const uint64_t *w = (const uint64_t *) tile;
    uint64_t h = 0x243f6a8885a308d3ull; /* pi */
    int i;

    for (i = 0; i < WLXRDP_TILE_BYTES / 8; i++)
    {
        h ^= w[i];
        h *= 0x100000001b3ull;
        h ^= h >> 29;
    }
    return h;
}

/*****************************************************************************/
/* Clip a rect to the frame; returns 0 if nothing is left */
static int
clip_rect(struct xh_rect *r, int width, int height)
{
    int x1 = r->x < 0 ? 0 : r->x;
    int y1 = r->y < 0 ? 0 : r->y;
    int x2 = r->x + r->w > width ? width : r->x + r->w;
    int y2 = r->y + r->h > height ? height : r->y + r->h;

    if (x2 <= x1 || y2 <= y1)
    {
        return 0;
    }
    r->x = x1;
    r->y = y1;
    r->w = x2 - x1;
    r->h = y2 - y1;
    return 1;
}

/*****************************************************************************/
size_t
wlxrdp_cpu_bytes(enum wlxrdp_cpu_layout layout, int width, int height)
{
    switch (layout)
    {
        case WLXRDP_CPU_NV12:
            return (size_t) width * height * 3 / 2;
        case WLXRDP_CPU_YUVALP:
            return (size_t) ((width + 63) & ~63) * 4 * ((height + 63) & ~63);
        case WLXRDP_CPU_XRGB:
            return (size_t) width * height * 4;
    }
    return 0;
}

/*****************************************************************************/
int
wlxrdp_cpu_nv12(const uint8_t *src, int src_stride, int bgr, uint8_t *out,
                int width, int height,
                struct xh_rect *rects, int num_rects)
{
    uint8_t *uv = out + (size_t) width * height;
    int n = 0;
    int i;

    for (i = 0; i < num_rects; i++)
    {
        struct xh_rect r = rects[i];
        int x2;
        int y2;

        /* even-aligned, as NV12 subsamples 2x2 (and xorgxrdp aligns) */
        x2 = r.x + r.w;
        y2 = r.y + r.h;
        r.x &= ~1;
        r.y &= ~1;
        x2 = (x2 + 1) & ~1;
        y2 = (y2 + 1) & ~1;
        r.w = x2 - r.x;
        r.h = y2 - r.y;
        if (!clip_rect(&r, width & ~1, height & ~1))
        {
            continue;
        }
        xrgb_to_nv12_709fr_box(src + (size_t) r.y * src_stride + r.x * 4,
                               src_stride,
                               out + (size_t) r.y * width + r.x, width,
                               uv + (size_t) (r.y / 2) * width + r.x, width,
                               r.w, r.h, bgr);
        rects[n++] = r;
    }
    return n;
}

/*****************************************************************************/
int
wlxrdp_cpu_yuvalp(const uint8_t *src, int src_stride, int bgr, uint8_t *out,
                  int width, int height,
                  struct xh_rect *rects, int num_rects,
                  uint64_t *hashes, int force,
                  struct xh_rect *tiles, int max_tiles)
{
    int tiles_x = (width + 63) / 64;
    int tiles_y = (height + 63) / 64;
    int stride = ((width + 63) & ~63) * 4;
    uint8_t *mark;
    int num_tiles = 0;
    int i;
    int tx;
    int ty;

    /* the tiles any rect touches */
    mark = (uint8_t *) calloc(tiles_x * tiles_y, 1);
    if (mark == NULL)
    {
        return -1;
    }
    for (i = 0; i < num_rects; i++)
    {
        struct xh_rect r = rects[i];

        if (!clip_rect(&r, width, height))
        {
            continue;
        }
        for (ty = r.y / 64; ty <= (r.y + r.h - 1) / 64; ty++)
        {
            for (tx = r.x / 64; tx <= (r.x + r.w - 1) / 64; tx++)
            {
                mark[ty * tiles_x + tx] = 1;
            }
        }
    }
    for (ty = 0; ty < tiles_y; ty++)
    {
        for (tx = 0; tx < tiles_x; tx++)
        {
            int x = tx * 64;
            int y = ty * 64;
            int w = width - x < 64 ? width - x : 64;
            int h = height - y < 64 ? height - y : 64;
            /* tile (tx, ty) as xorgxrdp lays it out */
            uint8_t *tile = out + (size_t) (y << 8) * (stride >> 8) +
                            (x << 8);
            uint64_t hv;

            if (!mark[ty * tiles_x + tx])
            {
                continue;
            }
            if (w < 64 || h < 64)
            {
                memset(tile, 0, WLXRDP_TILE_BYTES); /* the frame's edge */
            }
            xrgb_to_yuvalp_box(src + (size_t) y * src_stride + x * 4,
                               src_stride, tile, w, h, bgr);
            hv = tile_hash(tile);
            if (!force && hashes[ty * tiles_x + tx] == hv)
            {
                continue; /* damaged, but no different */
            }
            hashes[ty * tiles_x + tx] = hv;
            if (num_tiles >= max_tiles)
            {
                free(mark);
                return -1;
            }
            tiles[num_tiles].x = x;
            tiles[num_tiles].y = y;
            tiles[num_tiles].w = 64;
            tiles[num_tiles].h = 64;
            num_tiles++;
        }
    }
    free(mark);
    return num_tiles;
}

/*****************************************************************************/
int
wlxrdp_cpu_xrgb(const uint8_t *src, int src_stride, int bgr, uint8_t *out,
                int width, int height,
                struct xh_rect *rects, int num_rects)
{
    int n = 0;
    int i;
    int j;
    int k;

    for (i = 0; i < num_rects; i++)
    {
        struct xh_rect r = rects[i];

        if (!clip_rect(&r, width, height))
        {
            continue;
        }
        for (j = 0; j < r.h; j++)
        {
            uint8_t *d = out + ((size_t) (r.y + j) * width + r.x) * 4;
            const uint8_t *s = src + (size_t) (r.y + j) * src_stride +
                               r.x * 4;

            if (!bgr)
            {
                memcpy(d, s, (size_t) r.w * 4);
                continue;
            }
            for (k = 0; k < r.w; k++)
            {
                uint32_t p = ((const uint32_t *) s)[k];

                ((uint32_t *) d)[k] = (p & 0xff00ff00) |
                                      ((p >> 16) & 0xff) |
                                      ((p & 0xff) << 16);
            }
        }
        rects[n++] = r;
    }
    return n;
}
