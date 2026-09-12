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
/* AVC444: encode the auxiliary (chroma) view rather than the main view.
   Both views are pictures of the SAME H.264 sequence -- see the comment on
   xrdp_accel_assist_vaapi_encode. */
#define XH_ENC_FLAGS_AUXVIEW  (1 << 1)

/* Optional trailer on the AVC444 shared-memory payload.

   The payload is framed [len1][stream1][len2][stream2]. When the auxiliary
   view is present and its layout is v2, accel-assist appends the rectangle
   it actually rendered the aux over -- the damage accumulated since the last
   aux picture, not this frame's damage, because under
   XRDP_AVC444_CHROMA_INTERVAL > 1 the aux carries the frames it skipped.
   xrdp declares that rectangle in the aux metablock; without it xrdp has to
   assume the whole frame, which at interval 8 makes every aux copy a
   full-plane copy on the client.

   The trailer is optional in both directions. An accel-assist that does not
   append it leaves xrdp on the full-frame fallback, which is conservative
   but correct; an xrdp that does not look for it ignores the extra bytes.
   Neither half has to match the other's version.

   Layout: magic, then x1, y1, x2, y2 as signed 32-bit little-endian. */
#define XH_AVC444_AUX_RECT_MAGIC 0x52584141  /* "AAXR" */
#define XH_AVC444_AUX_RECT_BYTES 20

/* Session capability bits, sent by xorgxrdp as message type 3 of the
   accel-assist control batch. */
#define XH_CAPS_AVC444        (1 << 0)
#define XH_CAPS_AVC444_V2     (1 << 1)

enum encoder_result
{
    INCREMENTAL_FRAME_ENCODED,  /* P frame */
    KEY_FRAME_ENCODED,          /* IDR frame */
    ENCODER_ERROR
};

#endif
