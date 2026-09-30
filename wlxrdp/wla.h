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
 * COMPOSITOR.md has the whole contract: what a compositor must offer, and
 * the session's start-up and shutdown handshake with sesexec.
 */

#ifndef _WLA_H
#define _WLA_H

#include <poll.h>
#include <stdint.h>

#include "xrdp_accel_assist.h" /* struct xh_rect */

#define WLA_MAX_BUFS 4          ///< most capture buffers per monitor
#define WLA_MAX_MONITORS 16     ///< most monitors in a layout

/**
 * A capture buffer. It belongs to the adapter; the core only reads it.
 */
struct wla_buffer
{
    int width;
    int height;
    uint32_t fourcc;            ///< DRM_FORMAT_XRGB8888, _XBGR8888 or (shm) _BGR888
    int fd;                     ///< dma-buf, else -1
    uint32_t stride;
    uint32_t offset;
    uint64_t modifier;
    const uint8_t *shm;         ///< mapped memory, else NULL
    int shm_stride;
};

/**
 * One monitor of the layout, as the core has checked it
 *
 * Its place and size are in the client's pixels, with the scale to show it
 * at. The adapter places scaled monitors in the compositor's logical
 * coordinates, and maps pointer positions (in layout pixels) to them.
 */
struct wla_monitor
{
    int x;
    int y;
    int width;
    int height;
    int scale;                  ///< percent, 100-500
};

/**
 * Events from an adapter to the core
 */
struct wla_events
{
    /**
     * Announce the buffers capture uses on a monitor
     *
     * @param core Core object, as passed to create()
     * @param mon Monitor index in the layout
     * @param n Number of buffers, at most WLA_MAX_BUFS; 0 when capture of
     *          the monitor has stopped
     * @param bufs The buffers
     */
    void (*buffers)(void *core, int mon, int n,
                    const struct wla_buffer *bufs);

    /**
     * A buffer holds a complete frame
     *
     * @param core Core object
     * @param mon Monitor index
     * @param buf Buffer index, as announced by buffers()
     * @param damage Changed areas, in the monitor's coordinates
     * @param num_damage Number of areas; 0 means all of the monitor
     */
    void (*frame)(void *core, int mon, int buf,
                  const struct xh_rect *damage, int num_damage);

    /**
     * The pointer's image changed (only with WLA_CAP_CURSOR)
     *
     * @param core Core object
     * @param argb Image, ARGB, rows top-down; NULL when the pointer is
     *             hidden
     * @param w Image width
     * @param h Image height
     * @param hot_x Hotspot x
     * @param hot_y Hotspot y
     */
    void (*cursor)(void *core, const uint32_t *argb, int w, int h,
                   int hot_x, int hot_y);

    /**
     * The compositor is gone; wlxrdp ends
     *
     * @param core Core object
     * @param why Reason, for the log
     */
    void (*lost)(void *core, const char *why);
};

/**
 * Adapter capabilities (wla_ops.caps)
 */
enum wla_caps
{
    WLA_CAP_SET_KEYMAP = 1,     ///< keymap() applies the core's keymap
    WLA_CAP_CURSOR = 2          ///< the adapter sends cursor() events
};

/**
 * Scroll axes
 */
enum wla_axis
{
    WLA_AXIS_VERTICAL = 0,      ///< positive: down
    WLA_AXIS_HORIZONTAL = 1     ///< positive: right
};

/**
 * Capture modes
 */
enum wla_mode
{
    WLA_DMABUF,                 ///< GPU path: dma-bufs for accel-assist
    WLA_SHM                     ///< CPU path: mapped memory
};

/**
 * An adapter: the core's calls into it
 *
 * Every method takes the adapter object create() returned.
 */
struct wla_ops
{
    const char *name;           ///< for WLXRDP_ADAPTER and the log
    int caps;                   ///< enum wla_caps flags

    /**
     * Connect to the compositor
     *
     * @param ev Events the adapter sends to the core
     * @param core Core object, passed back with every event
     * @return adapter object, or NULL if this is not a compositor the
     *         adapter drives
     */
    void *(*create)(const struct wla_events *ev, void *core);

    /**
     * Disconnect and free the adapter object
     *
     * @param a Adapter object
     */
    void (*destroy)(void *a);

    /**
     * @param a Adapter object
     * @return how many monitors the adapter can drive
     */
    int (*max_monitors)(void *a);

    /**
     * Apply a layout
     *
     * @param a Adapter object
     * @param m Monitors, in the client's pixels, with their scales
     * @param n Number of monitors, at most max_monitors()
     * @return 0 once applied, or if the layout is already in place;
     *         non-zero if the compositor refuses it
     */
    int (*set_layout)(void *a, const struct wla_monitor *m, int n);

    /**
     * Start capture of the layout's monitors
     *
     * The adapter announces each monitor's buffers with buffers().
     * @param a Adapter object
     * @param mode GPU (dma-buf) or CPU (mapped memory) buffers
     * @return 0 on success
     */
    int (*start)(void *a, enum wla_mode mode);

    /**
     * Stop capture
     *
     * @param a Adapter object
     */
    void (*stop)(void *a);

    /**
     * The core can take a frame of a monitor now
     *
     * A pull adapter (ext-image-copy-capture) captures one; a push adapter
     * (PipeWire) hands over the newest it holds. Either reports it with
     * frame().
     * @param a Adapter object
     * @param mon Monitor index
     */
    void (*want_frame)(void *a, int mon);

    /**
     * The core is done with a buffer
     *
     * The adapter reuses a buffer only once it has been released: when a
     * newer frame of that monitor has been acknowledged.
     * @param a Adapter object
     * @param mon Monitor index
     * @param buf Buffer index
     */
    void (*release)(void *a, int mon, int buf);

    /**
     * Use a keymap for the keyboard (only with WLA_CAP_SET_KEYMAP)
     *
     * @param a Adapter object
     * @param xkb_text Keymap, in XKB text format
     * @return 0 on success
     */
    int (*keymap)(void *a, const char *xkb_text);

    /**
     * @param a Adapter object
     * @param evdev Key, as an evdev code
     * @param down non-zero for a press
     */
    void (*key)(void *a, int evdev, int down);

    /**
     * Set the keyboard's modifier and lock state (xkb masks)
     *
     * @param a Adapter object
     * @param depressed Depressed modifiers
     * @param latched Latched modifiers
     * @param locked Locked modifiers
     * @param group Layout group
     */
    void (*modifiers)(void *a, uint32_t depressed, uint32_t latched,
                      uint32_t locked, uint32_t group);

    /**
     * Move the pointer
     *
     * @param a Adapter object
     * @param x Position in the layout, in pixels
     * @param y Position in the layout, in pixels
     * @param width The layout's total width
     * @param height The layout's total height
     */
    void (*motion)(void *a, int x, int y, int width, int height);

    /**
     * @param a Adapter object
     * @param evdev_button Button, as an evdev code
     * @param down non-zero for a press
     */
    void (*button)(void *a, int evdev_button, int down);

    /**
     * @param a Adapter object
     * @param axis Axis
     * @param discrete Wheel notches; 0 for a continuous amount
     * @param value Distance, in surface units
     */
    void (*scroll)(void *a, enum wla_axis axis, int discrete, double value);

    /**
     * The adapter's part of the core's poll loop, before each poll
     *
     * @param a Adapter object
     * @param p pollfds to fill
     * @param max Size of p
     * @param timeout_ms Poll timeout in ms (-1: none); the adapter may
     *                   lower it
     * @return number of pollfds filled
     */
    int (*fds)(void *a, struct pollfd *p, int max, int *timeout_ms);

    /**
     * The adapter's part of the core's poll loop, after each poll
     *
     * @param a Adapter object
     * @param p The pollfds fds() filled, with their revents
     * @param n Number of them
     * @return negative if the compositor is gone
     */
    int (*dispatch)(void *a, const struct pollfd *p, int n);
};

/** The adapters, tried in order */
extern const struct wla_ops wla_wlr;

#endif
