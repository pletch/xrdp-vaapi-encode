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

#ifndef _WLXRDP_CPU_H
#define _WLXRDP_CPU_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "xrdp_accel_assist.h" /* struct xh_rect */

/* the layouts xrdp takes from xorgxrdp, per capture mode */
enum wlxrdp_cpu_layout
{
    WLXRDP_CPU_NV12,    /* CC_GFX_A2 without the helper: xrdp's x264 */
    WLXRDP_CPU_YUVALP,  /* CC_GFX_PRO, CC_SUF_RFX: 64x64 planar YUVA tiles */
    WLXRDP_CPU_XRGB     /* CC_SIMPLE at 24/32 bpp: bitmaps */
};

#define WLXRDP_TILE_BYTES (64 * 64 * 4)

/* size of a frame in a layout */
size_t
wlxrdp_cpu_bytes(enum wlxrdp_cpu_layout layout, int width, int height);

/* Convert the changed rects of an XRGB8888 frame (XBGR8888 with bgr) into
   out (the full frame in that layout, kept between frames). NV12 and XRGB rewrite rects[] as
   sent (clipped; even-aligned for NV12) and return their count. */
int
wlxrdp_cpu_nv12(const uint8_t *src, int src_stride, int bgr, uint8_t *out,
                int width, int height,
                struct xh_rect *rects, int num_rects);
int
wlxrdp_cpu_xrgb(const uint8_t *src, int src_stride, int bgr, uint8_t *out,
                int width, int height,
                struct xh_rect *rects, int num_rects);

/* YUVA: rebuilds every tile the rects touch, and returns in tiles[] the
   ones whose content changed (all of them with force), per hashes[]
   (one per tile, row-major). -1 on error. */
int
wlxrdp_cpu_yuvalp(const uint8_t *src, int src_stride, int bgr, uint8_t *out,
                  int width, int height,
                  struct xh_rect *rects, int num_rects,
                  uint64_t *hashes, int force,
                  struct xh_rect *tiles, int max_tiles);

#endif
