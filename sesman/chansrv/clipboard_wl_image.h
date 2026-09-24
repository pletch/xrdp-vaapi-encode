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
 * Image conversions for the Wayland clipboard: RDP carries CF_DIB (a
 * BITMAPINFO and its pixels), Wayland applications image/bmp (a DIB with a
 * file header) and image/png. Each function returns 0 and a malloc'd
 * *out, or non-zero.
 */

#ifndef _CLIPBOARD_WL_IMAGE_H
#define _CLIPBOARD_WL_IMAGE_H

/* PNG conversions are there (built with libpng) */
int
wl_image_have_png(void);

/* CF_DIB -> image/bmp: prepend the file header */
int
wl_image_dib_to_bmp(const char *dib, int len, char **out, int *out_len);

/* image/bmp -> CF_DIB: drop the file header; a header other than a plain
   BITMAPINFOHEADER, which not every Windows application reads, is rewritten
   as a 32bpp BI_RGB DIB when its pixels are understood */
int
wl_image_bmp_to_dib(const char *bmp, int len, char **out, int *out_len);

/* CF_DIB (24 or 32 bpp) -> image/png */
int
wl_image_dib_to_png(const char *dib, int len, char **out, int *out_len);

/* image/png -> CF_DIB, 32bpp BI_RGB */
int
wl_image_png_to_dib(const char *png, int len, char **out, int *out_len);

#endif
