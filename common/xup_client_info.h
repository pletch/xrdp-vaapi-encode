/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Jay Sorg 2004-2025
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

/**
 * @file    common/xup_client_info.h
 * @brief   Data shared with xorgxrdp
 */

#if !defined(XUP_CLIENT_INFO_H)
#define XUP_CLIENT_INFO_H

#include "xrdp_client_info.h"

/**
 * Information about the xrdp client which is passed to xorgxrdp
 *
 * This is a subset of 'struct xrdp_client_info'
 *
 * @note If you change this structure, you MUST bump the
 *       XUP_CLIENT_INFO_CURRENT_VERSION number so that the mismatch
 *       can be detected.
 */
struct xup_client_info
{
    int size; /* bytes for this structure */
    int version; /* Should be XUP_CLIENT_INFO_CURRENT_VERSION */
    int bpp;
    int jpeg; /* non standard bitmap cache v2 cap */
    int offscreen_support_level;
    int offscreen_cache_size;
    int offscreen_cache_entries;

    char orders[XR_PRIMARY_ORDER_COUNT];
    int order_flags_ex;
    int pointer_flags; /* 0 color, 1 new, 2 no new */
    int large_pointer_support_flags;

    struct display_size_description display_sizes;

    enum xrdp_capture_code capture_code;
    int capture_format;

    /* X11 keyboard layout - inferred from keyboard type/subtype */
    char model[CI_KBD_MODEL_SIZE];
    char layout[CI_KBD_LAYOUT_SIZE];
    char variant[CI_KBD_VARIANT_SIZE];
    char options[CI_KBD_OPTIONS_SIZE];
    char xkb_rules[CI_KBD_XKB_RULES_SIZE];
    // A few x11 keycodes are needed by the xup module
    int x11_keycode_caps_lock;
    int x11_keycode_num_lock;
    int x11_keycode_scroll_lock;

    /* xorgxrdp: frame capture interval (milliseconds) */
    int rfx_frame_interval;
    int h264_frame_interval;
    int normal_frame_interval;

    /* The EGFX capability set xrdp confirmed to the client permits
       RDPGFX_CODECID_AVC444 (0x000E), the dual-view 4:4:4 chroma encoding,
       and not merely AVC420. Set alongside capture_code, so it arrives by
       the same route and at the same time. xorgxrdp uses it to pick the
       codec id it asks for and to tell the accel-assist helper how to size
       its encoder. Zero unless EGFX H.264 was negotiated at all. */
    /* 0 none, 1 AVC444 v1 chroma layout, 2 v2 */
    int gfx_avc444;
};

/* yyyymmdd of last incompatible change to xup_client_info */
/* gfx_avc444 carries an AVC444 LEVEL (0 none, 1 v1, 2 v2) from this
   version on; before it, any non-zero value meant only "AVC444 supported".
   A consumer that treats "not 2" as "v1" would silently downgrade every
   session served by an older xrdp, so it must check the version first. */
#define XUP_CLIENT_INFO_LEVEL_AVC444_VERSION 20260908
#define XUP_CLIENT_INFO_CURRENT_VERSION 20260908

#endif // XUP_CLIENT_INFO_H
