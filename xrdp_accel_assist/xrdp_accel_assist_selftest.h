/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) 2026 Klotzi111, all xrdp contributors
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

#ifndef _XRDP_ACCEL_ASSIST_SELFTEST_H
#define _XRDP_ACCEL_ASSIST_SELFTEST_H

/* Exercise the VA-API encoder path end to end on the render node, without
   an X server: GL on a surfaceless EGL display, an encoder with nviews
   views (1 = AVC420, 2 = AVC444), 'frames' frames of a known pattern drawn
   into the encoder's input surfaces, read back through libva and encoded.

   stress: extra full-surface draws queued before each frame's pattern, to
   show whether the encoder waits for GL (0 = none).
   dump_path: if not NULL, the coded pictures are written there in
   submission order, and the expected picture contents to <dump_path>.expect
   (one line per picture: view frame y0 y1 y2 y3 u v), for an external
   decoder to compare against.

   xrdp_accel_assist_vaapi_init() must have succeeded. Returns 0 if every
   step worked. */
int
xrdp_accel_assist_selftest(int width, int height, int nviews, int frames,
                           int stress, const char *dump_path);

#endif
