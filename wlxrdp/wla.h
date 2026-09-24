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

/**
 * wla: wlxrdp's compositor adapter interface.
 *
 * wlxrdp's core knows RDP: the xup protocol with xrdp, frame pacing and
 * acks, the accel-assist helper, the CPU encode path, the RDP pointer, the
 * client's keymap and layout checks. An adapter knows one family of
 * compositors: how it exposes outputs, frames, the cursor and input.
 *
 *   - the core paces: want_frame() says it can take a frame now; a pull
 *     adapter (ext-image-copy-capture) captures one, a push adapter
 *     (PipeWire) hands over the newest it holds.
 *   - buffers are the adapter's: it announces a monitor's set with
 *     buffers(), fills them, reports each complete one with frame(), and
 *     reuses one only after the core has release()d it (once a newer frame
 *     of that monitor has been acknowledged).
 *   - coordinates are the layout's (all monitors, from 0,0); keys and
 *     buttons are evdev codes.
 *
 * Adapters are compiled in, and tried in order; WLXRDP_ADAPTER names one.
 */

#ifndef _WLA_H
#define _WLA_H

#include <poll.h>
#include <stdint.h>

#include "xrdp_accel_assist.h" /* struct xh_rect */

#define WLA_MAX_BUFS 4
#define WLA_MAX_MONITORS 16

/* a capture buffer: the adapter's; the core only reads it */
struct wla_buffer
{
    int width;
    int height;
    uint32_t fourcc;            /* DRM_FORMAT_XRGB8888 or _XBGR8888 */
    int fd;                     /* dma-buf, else -1 */
    uint32_t stride;
    uint32_t offset;
    uint64_t modifier;
    const uint8_t *shm;         /* mapped memory, else NULL */
    int shm_stride;
};

/* one monitor of the layout (checked by the core): its place and size in
   the client's pixels, and the scale to show it at, in percent (100-500).
   The adapter places scaled monitors in the compositor's logical
   coordinates, and maps pointer positions (in layout pixels) to them. */
struct wla_monitor
{
    int x;
    int y;
    int width;
    int height;
    int scale;
};

/* adapter -> core */
struct wla_events
{
    /* the buffers capture uses on monitor mon (n 0: none, capture off) */
    void (*buffers)(void *core, int mon, int n,
                    const struct wla_buffer *bufs);
    /* buffer buf of monitor mon holds a complete frame; damage in the
       monitor's coordinates (none: all of it) */
    void (*frame)(void *core, int mon, int buf,
                  const struct xh_rect *damage, int num_damage);
    /* the pointer's image, ARGB, rows top-down (argb NULL: hidden) */
    void (*cursor)(void *core, const uint32_t *argb, int w, int h,
                   int hot_x, int hot_y);
    /* the compositor is gone: wlxrdp ends */
    void (*lost)(void *core, const char *why);
};

enum wla_caps
{
    WLA_CAP_SET_KEYMAP = 1,     /* keymap(): the core's keymap is used */
    WLA_CAP_CURSOR = 2          /* cursor() events come */
};

enum wla_axis
{
    WLA_AXIS_VERTICAL = 0,      /* positive: down */
    WLA_AXIS_HORIZONTAL = 1     /* positive: right */
};

enum wla_mode
{
    WLA_DMABUF,                 /* GPU path: dma-bufs for accel-assist */
    WLA_SHM                     /* CPU path: mapped memory */
};

/* core -> adapter */
struct wla_ops
{
    const char *name;
    int caps;

    /* connect; NULL if this is not a compositor the adapter drives */
    void *(*create)(const struct wla_events *ev, void *core);
    void (*destroy)(void *a);

    /* monitors: how many it can drive, and the layout (0: in place) */
    int (*max_monitors)(void *a);
    int (*set_layout)(void *a, const struct wla_monitor *m, int n);

    /* capture of the layout's monitors */
    int (*start)(void *a, enum wla_mode mode);
    void (*stop)(void *a);
    void (*want_frame)(void *a, int mon);
    void (*release)(void *a, int mon, int buf);

    /* input */
    int (*keymap)(void *a, const char *xkb_text);
    void (*key)(void *a, int evdev, int down);
    void (*modifiers)(void *a, uint32_t depressed, uint32_t latched,
                      uint32_t locked, uint32_t group);
    void (*motion)(void *a, int x, int y, int width, int height);
    void (*button)(void *a, int evdev_button, int down);
    /* discrete: wheel notches (0: a continuous amount); value in surface
       units */
    void (*scroll)(void *a, enum wla_axis axis, int discrete, double value);

    /* event loop: fds() fills up to max pollfds (and may lower
       *timeout_ms, -1: none); after every poll, dispatch() gets them back
       with their revents. Negative: the compositor is gone. */
    int (*fds)(void *a, struct pollfd *p, int max, int *timeout_ms);
    int (*dispatch)(void *a, const struct pollfd *p, int n);
};

/* the adapters */
extern const struct wla_ops wla_wlr;

#endif
