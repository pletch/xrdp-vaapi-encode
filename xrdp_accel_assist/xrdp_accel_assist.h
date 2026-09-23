/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Jay Sorg 2020-2024
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

#ifndef _XRDP_ACCEL_ASSIST_H
#define _XRDP_ACCEL_ASSIST_H

#define XH_YUV420   1
#define XH_YUV422   2
#define XH_YUV444   3

#define XH_BT601    0
#define XH_BT709FR  1
#define XH_BTRFX    2

struct xh_rect
{
    short x;
    short y;
    short w;
    short h;
};

#define XH_ENC_FLAGS_FORCEIDR (1 << 0)
/* AVC444: encode the auxiliary (chroma) view. Both views are pictures of
   one H.264 sequence. */
#define XH_ENC_FLAGS_AUXVIEW  (1 << 1)

/* Optional trailer on the AVC444 payload
   ([len1][stream1][len2][stream2]): for a v2 aux view, the rect the aux
   was rendered over (accumulated damage since the last aux), for xrdp to
   declare in the aux metablock. Without it xrdp declares the full frame.
   Optional in both directions.

   Layout: magic, then x1, y1, x2, y2 as signed 32-bit little-endian. */
#define XH_AVC444_AUX_RECT_MAGIC 0x52584141  /* "AAXR" */
#define XH_AVC444_AUX_RECT_BYTES 20

/* Session capability bits from xorgxrdp's control batch. */
#define XH_CAPS_AVC444        (1 << 0)
#define XH_CAPS_AVC444_V2     (1 << 1)

enum encoder_result
{
    INCREMENTAL_FRAME_ENCODED,  /* P frame */
    KEY_FRAME_ENCODED,          /* IDR frame */
    ENCODER_ERROR
};

#endif
