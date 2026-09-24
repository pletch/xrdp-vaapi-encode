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
 * wlxrdp: the xrdp backend for Wayland sessions.
 *
 * Plays xorgxrdp's part on the xup socket: answers xrdp's version / caps /
 * client-info handshake and sends the compositor's outputs as frames. The
 * compositor side is an adapter's (wla.h; wla_wlr.c for wlroots
 * compositors): it captures, and injects input. The GPU work is the
 * xrdp_accel_assist helper's, as for xorgxrdp: we start it (-w) between
 * ourselves and xrdp, hand it the capture buffers' dma-bufs, and send each
 * frame as an EGFX WireToSurface1 it encodes (AVC420 or AVC444) on the way
 * to xrdp. Clients and hosts the helper cannot serve take a CPU path
 * (wlxrdp_cpu.c): capture into shm, and xrdp encodes.
 *
 * Monitors: the client's layout becomes the adapter's monitors, each sent
 * as its own GFX surface, as xorgxrdp sends each RandR monitor. The cursor
 * is sent as an RDP pointer. The keyboard is the core's: the client's
 * layout as an xkb keymap, its state, and spare keys for Unicode input.
 *
 * The clipboard is chansrv's (clipboard_wl.c). Not handled yet: touch.
 *
 * Usage: wlxrdp -s <socket path>   (with WAYLAND_DISPLAY set;
 *        WLXRDP_ADAPTER=<name> picks an adapter instead of the first that
 *        connects)
 */

#include <config_ac.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#include "arch.h"
#include "parse.h"
#include "os_calls.h"
#include "log.h"
#include "xrdp_client_info.h"
#include "xup_client_info.h"
#include "xrdp_constants.h"
#include "ms-rdpbcgr.h"
#include "xrdp_accel_assist.h"
#include "wlxrdp_cpu.h"
#include "wla.h"

#define MAX_RECTS 256
/* monitors, at most */
#define MAX_MONS WLA_MAX_MONITORS
#ifndef XRDP_LIBEXEC_PATH
#define XRDP_LIBEXEC_PATH "/usr/local/libexec/xrdp"
#endif
/* frames in flight, per monitor */
#define MAX_IN_FLIGHT 2
#define IN_MAX (128 * 1024)
/* spare keycodes for characters not on the client's layout */
#define UNI_SLOTS 8
/* pollfds an adapter may use */
#define ADAPTER_FDS 4

struct be;

/* one monitor: its capture buffers as the adapter has them, and its GFX
   surface */
struct mon
{
    struct be *b;
    int index;                  /* surface id, flags >> 28 */
    int width;                  /* its capture size */
    int height;
    int force_idr;

    /* the core's view: the capture buffers as the adapter announced them,
       and the newest one delivered (-1: none yet) */
    struct wla_buffer wb[WLA_MAX_BUFS];
    int num_wb;
    int core_last;
    /* buffers delivered and not yet released, and the frame each last went
       out in (0: none the helper may still read) */
    int held[WLA_MAX_BUFS];
    int sent_id[WLA_MAX_BUFS];
    /* pacing: a want_frame() not yet answered, and when it went */
    int want_out;
    uint32_t want_ms;

    /* CPU path: the frame in the capture code's layout */
    uint8_t *cpu_out;
    size_t cpu_out_bytes;
    uint64_t *cpu_hashes;       /* RFX: one per 64x64 tile */
    int cpu_num_tiles;
};

struct be
{
    /* monitors */
    struct mon mons[MAX_MONS];
    int num_mons;               /* the adapter's, at most */
    /* the core's layout: checked, normalised; the adapter applies it */
    struct wla_monitor lay[MAX_MONS];
    int active;                 /* monitors in the client's layout */
    int total_w;                /* the layout's extent */
    int total_h;
    int frame_interval_ms;      /* the client's h264_frame_interval */

    int codec_id;
    /* CPU path: capture into shm and let xrdp encode, as xorgxrdp does
       without accel-assist */
    int cpu;                    /* this connection uses the CPU path */
    int cpu_code;               /* its capture code */
    enum wlxrdp_cpu_layout cpu_layout;
    int accel_failed;           /* the helper died at start: no VA-API */
    uint32_t helper_start_ms;
    pid_t helper_pid;           /* xrdp_accel_assist, 0 if not running */
    int helper_ready;           /* it has this capture's buffers */
    int buffers_changed;        /* the helper's batch is out of date */

    /* xrdp connection */
    int listen_fd;
    int client_fd;
    char *in;
    int in_len;
    struct xup_client_info ci;
    int have_ci;
    int frame_id;
    int acked_id;
    int suppress;

    /* keyboard */
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;
    char *keymap_text;          /* the client layout's keymap, as uploaded */
    /* characters with no key on the layout (xrdp's WM_KEYBRD_UNICODE):
       each gets a spare keycode in a copy of the keymap */
    struct
    {
        int keycode;
        uint32_t cp;            /* 0: free */
        uint32_t used;          /* for least-recently-used replacement */
    } uni[UNI_SLOTS];
    int num_uni;
    uint32_t uni_clock;

    /* the adapter -> core events; set when the compositor is gone */
    const struct wla_events *ev;
    int lost;
    /* core -> adapter, and the adapter's own state */
    const struct wla_ops *ops;
    void *ad;

    /* the cursor as the compositor last reported it */
    uint32_t *cur_img;
    int cur_w;
    int cur_h;
    int cur_hx;
    int cur_hy;
    int cur_visible;

    /* the cursor the client has, to skip repeats: labwc completes a cursor
       frame on every motion, changed or not */
    uint32_t *sent_image;
    int sent_w;
    int sent_h;
    int sent_hx;
    int sent_hy;
    int sent_visible;           /* -1: nothing sent to this client yet */

    /* stats */
    int frames_sent;
    unsigned int stat_ms;
};

static struct be g_be;
static volatile sig_atomic_t g_term;

static void drop_client(struct be *b);
static void core_release(struct be *b, struct mon *m);
static int core_helper_sync(struct be *b);

/*****************************************************************************/
static uint32_t
now_ms32(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/*****************************************************************************/
/* The client's monitors (inclusive rects, as xrdp passes them) become the
   layout: normalised to start at 0,0, capped at the outputs we have.
   Without monitor data, or for a client without GFX (whose bitmaps xrdp
   places in desktop coordinates), one monitor spans the session.
   Returns non-zero, changing nothing, for a layout no output can take: a
   monitor outside MIN_MON_SIDE..MAX_MON_SIDE a side (MS-RDPEDISP's range,
   which xrdp holds clients to), or all of them beyond MAX_LAYOUT_SIDE. A
   compositor refuses a zero-sized mode with a protocol error, which ends
   our connection to it; labwc takes a tiny one and aborts (wlroots
   asserts on the negative window size it works out). */
#define MIN_MON_SIDE 200
#define MAX_MON_SIDE 8192
#define MAX_LAYOUT_SIDE 16384

/* A monitor's scale, in percent: XRDP_WAYLAND_SCALE, a number (percent,
   or a factor like 1.5), sets it for every monitor; otherwise the client's
   own (each monitor's, or the session's for a client without monitor
   data: mstsc sends the laptop's 200%). A client that sends none gets 200
   on a monitor wider than 2000 pixels, 100 otherwise, unless
   XRDP_WAYLAND_SCALE=client (then 100). */
#define MIN_SCALE 100
#define MAX_SCALE 500
static int
monitor_scale(int client_scale, int width)
{
    const char *env = getenv("XRDP_WAYLAND_SCALE");

    if (env != NULL && env[0] != '\0' && strcmp(env, "auto") != 0 &&
            strcmp(env, "client") != 0)
    {
        char *end;
        double v = strtod(env, &end);
        int pct;

        if (end != env && *end == '\0' && v > 0)
        {
            pct = v < 10 ? (int) (v * 100 + 0.5) : (int) (v + 0.5);
            if (pct >= MIN_SCALE && pct <= MAX_SCALE)
            {
                return pct;
            }
        }
        LOG(LOG_LEVEL_WARNING, "XRDP_WAYLAND_SCALE=%s is not a scale "
            "from 100 to 500 (or 1 to 5); using the client's", env);
    }
    if (client_scale >= MIN_SCALE && client_scale <= MAX_SCALE)
    {
        return client_scale;
    }
    if (env != NULL && strcmp(env, "client") == 0)
    {
        return 100;
    }
    return width > 2000 ? 200 : 100;
}

static int
layout_set(struct be *b, int count, const struct monitor_info *mi,
           int width, int height)
{
    int64_t x[MAX_MONS];
    int64_t y[MAX_MONS];
    int64_t w[MAX_MONS];
    int64_t h[MAX_MONS];
    int64_t min_x = 0;
    int64_t min_y = 0;
    int64_t total_w = 0;
    int64_t total_h = 0;
    int single;
    int no_data = count < 1;
    int i;

    single = count < 1 ||
             (b->cpu && b->cpu_code != CC_GFX_A2 &&
              b->cpu_code != CC_GFX_PRO);
    if (single)
    {
        count = 1;
    }
    if (count > b->ops->max_monitors(b->ad))
    {
        LOG(LOG_LEVEL_WARNING, "the client has %d monitors; driving the "
            "first %d", count, b->ops->max_monitors(b->ad));
        count = b->ops->max_monitors(b->ad);
    }
    if (count < 1)
    {
        return 1;
    }
    if (!single)
    {
        min_x = mi[0].left;
        min_y = mi[0].top;
        for (i = 1; i < count; i++)
        {
            min_x = mi[i].left < min_x ? mi[i].left : min_x;
            min_y = mi[i].top < min_y ? mi[i].top : min_y;
        }
    }
    for (i = 0; i < count; i++)
    {
        if (single)
        {
            x[i] = 0;
            y[i] = 0;
            w[i] = width;
            h[i] = height;
        }
        else
        {
            x[i] = (int64_t) mi[i].left - min_x;
            y[i] = (int64_t) mi[i].top - min_y;
            w[i] = (int64_t) mi[i].right - mi[i].left + 1;
            h[i] = (int64_t) mi[i].bottom - mi[i].top + 1;
        }
        if (w[i] < MIN_MON_SIDE || h[i] < MIN_MON_SIDE ||
                w[i] > MAX_MON_SIDE || h[i] > MAX_MON_SIDE)
        {
            LOG(LOG_LEVEL_ERROR, "monitor %d: %lldx%lld is not a usable size",
                i, (long long) w[i], (long long) h[i]);
            return 1;
        }
        total_w = x[i] + w[i] > total_w ? x[i] + w[i] : total_w;
        total_h = y[i] + h[i] > total_h ? y[i] + h[i] : total_h;
    }
    if (total_w > MAX_LAYOUT_SIDE || total_h > MAX_LAYOUT_SIDE)
    {
        LOG(LOG_LEVEL_ERROR, "a %lldx%lld layout is too big",
            (long long) total_w, (long long) total_h);
        return 1;
    }
    b->active = count;
    b->total_w = (int) total_w;
    b->total_h = (int) total_h;
    for (i = 0; i < count; i++)
    {
        b->lay[i].x = (int) x[i];
        b->lay[i].y = (int) y[i];
        b->lay[i].width = (int) w[i];
        b->lay[i].height = (int) h[i];
        /* a client without monitor data sends one scale for the session;
           xrdp gives monitors it sent without one 100 */
        b->lay[i].scale = monitor_scale(
                              no_data ? b->ci.session_desktop_scale_factor
                              : (int) mi[i].desktop_scale_factor, (int) w[i]);
    }
    return 0;
}

/*****************************************************************************/
/* xrdp connection: framing */

static int
send_all(int fd, const char *data, int len)
{
    while (len > 0)
    {
        int rv = send(fd, data, len, MSG_NOSIGNAL);

        if (rv < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN)
            {
                struct pollfd pfd = { fd, POLLOUT, 0 };

                poll(&pfd, 1, 1000);
                continue;
            }
            return 1;
        }
        data += rv;
        len -= rv;
    }
    return 0;
}

/*****************************************************************************/
/* caps reply to xrdp's version message */
static int
send_caps(struct be *b)
{
    struct stream *s;
    int rv;

    make_stream(s);
    init_stream(s, 64);
    out_uint16_le(s, 2);        /* caps */
    out_uint16_le(s, 1);        /* count */
    out_uint32_le(s, 8);        /* bytes after the header */
    out_uint16_le(s, 100);      /* version capability */
    out_uint16_le(s, 8);
    out_uint32_le(s, XUP_CLIENT_INFO_CURRENT_VERSION);
    s_mark_end(s);
    rv = send_all(b->client_fd, s->data, (int) (s->end - s->data));
    free_stream(s);
    return rv;
}

/*****************************************************************************/
/* resize done (type 100, memory allocation complete): the whole layout */
static int
send_resize_done(struct be *b)
{
    struct stream *s;
    int rv;

    make_stream(s);
    init_stream(s, 64);
    out_uint16_le(s, 100);
    out_uint16_le(s, 1);
    out_uint32_le(s, 8);
    out_uint16_le(s, 3);        /* memory allocation complete */
    out_uint16_le(s, 8);
    out_uint16_le(s, b->total_w);
    out_uint16_le(s, b->total_h);
    s_mark_end(s);
    rv = send_all(b->client_fd, s->data, (int) (s->end - s->data));
    free_stream(s);
    return rv;
}

/*****************************************************************************/
static void
out_rects(struct stream *s, const struct xh_rect *rects, int num_rects)
{
    int j;

    out_uint16_le(s, num_rects);
    for (j = 0; j < num_rects; j++)
    {
        out_uint16_le(s, rects[j].x);
        out_uint16_le(s, rects[j].y);
        out_uint16_le(s, rects[j].w);
        out_uint16_le(s, rects[j].h);
    }
}

/*****************************************************************************/
/* One EGFX frame for a monitor, as xorgxrdp sends it: begin update, order
   62 (StartFrame, WireToSurface1 or 2, EndFrame), end update; then, when
   shm_fd is given, the frame's data as that fd. cmd is 1 (WireToSurface1:
   H.264) or 2 (WireToSurface2: RFX progressive). The monitor's index is
   its surface id and rides in the flags; rects are the monitor's own. */
static int
send_gfx(struct mon *m, int cmd, int codec_id, int flags,
         const struct xh_rect *drects, int num_drects,
         const struct xh_rect *crects, int num_crects,
         int shm_fd, int shm_bytes)
{
    struct be *b = m->b;
    struct stream *s;
    int wts_bytes;
    int cmd_bytes;
    int order_bytes;

    b->frame_id++;
    wts_bytes = 8 + (cmd == 2 ? 13 : 9) +
                2 + num_drects * 8 + 2 + num_crects * 8 + 8;
    cmd_bytes = 16 + wts_bytes + 12;
    order_bytes = 2 + 2 + 4 + cmd_bytes + 4;

    make_stream(s);
    init_stream(s, order_bytes + 64);
    out_uint16_le(s, 3);                /* order list with lengths */
    out_uint16_le(s, 3);                /* begin, egfx, end */
    out_uint32_le(s, 4 + order_bytes + 4);
    out_uint16_le(s, 1);                /* begin update */
    out_uint16_le(s, 4);
    out_uint16_le(s, 62);               /* egfx with shm fd */
    out_uint16_le(s, order_bytes);
    out_uint32_le(s, cmd_bytes);
    /* XR_RDPGFX_CMDID_STARTFRAME */
    out_uint16_le(s, 0x000B);
    out_uint16_le(s, 0);
    out_uint32_le(s, 16);
    out_uint32_le(s, b->frame_id);
    out_uint32_le(s, 0);                /* timestamp */
    /* XR_RDPGFX_CMDID_WIRETOSURFACE_1 or _2 */
    out_uint16_le(s, cmd);
    out_uint16_le(s, 0);
    out_uint32_le(s, wts_bytes);
    out_uint16_le(s, m->index);         /* surface id: the monitor */
    out_uint16_le(s, codec_id);
    if (cmd == 2)
    {
        out_uint32_le(s, 0);            /* codec context id */
    }
    out_uint8(s, 0x20);                 /* pixel format */
    out_uint32_le(s, flags | ((m->index & 0xF) << 28));
    out_rects(s, drects, num_drects);   /* dirty */
    out_rects(s, crects, num_crects);   /* copied (RFX: tiles) */
    out_uint16_le(s, b->lay[m->index].x);   /* the monitor's place and size */
    out_uint16_le(s, b->lay[m->index].y);
    out_uint16_le(s, m->width);
    out_uint16_le(s, m->height);
    /* XR_RDPGFX_CMDID_ENDFRAME */
    out_uint16_le(s, 0x000C);
    out_uint16_le(s, 0);
    out_uint32_le(s, 12);
    out_uint32_le(s, b->frame_id);
    out_uint32_le(s, shm_fd >= 0 ? shm_bytes : 0);
    out_uint16_le(s, 2);                /* end update */
    out_uint16_le(s, 4);
    s_mark_end(s);

    if (send_all(b->client_fd, s->data, (int) (s->end - s->data)) != 0 ||
            (shm_fd >= 0 &&
             g_sck_send_fd_set(b->client_fd, "int", 4, &shm_fd, 1) < 0))
    {
        free_stream(s);
        return 1;
    }
    free_stream(s);
    b->frames_sent++;
    return 0;
}

/*****************************************************************************/
/* One non-GFX frame (order 64, paint rect with shm fd), as xorgxrdp sends
   it for bitmaps and RFX surface commands. Single monitor only: xrdp
   places these in desktop coordinates. */
static int
send_paint_rect(struct mon *m, const struct xh_rect *drects, int num_drects,
                const struct xh_rect *crects, int num_crects,
                int shm_fd, int shm_bytes)
{
    struct be *b = m->b;
    struct stream *s;
    int order_bytes;

    b->frame_id++;
    order_bytes = 2 + 2 + 2 + num_drects * 8 + 2 + num_crects * 8 +
                  4 * 4 + 2 * 4;
    make_stream(s);
    init_stream(s, order_bytes + 64);
    out_uint16_le(s, 3);
    out_uint16_le(s, 3);                /* begin, paint, end */
    out_uint32_le(s, 4 + order_bytes + 4);
    out_uint16_le(s, 1);                /* begin update */
    out_uint16_le(s, 4);
    out_uint16_le(s, 64);               /* paint rect with shm fd */
    out_uint16_le(s, order_bytes);
    out_rects(s, drects, num_drects);
    out_rects(s, crects, num_crects);
    out_uint32_le(s, 0);                /* flags: monitor 0 */
    out_uint32_le(s, b->frame_id);
    out_uint32_le(s, shm_bytes);
    out_uint32_le(s, 0);                /* shm offset */
    out_uint16_le(s, 0);                /* left, top, width, height */
    out_uint16_le(s, 0);
    out_uint16_le(s, m->width);
    out_uint16_le(s, m->height);
    out_uint16_le(s, 2);                /* end update */
    out_uint16_le(s, 4);
    s_mark_end(s);

    if (send_all(b->client_fd, s->data, (int) (s->end - s->data)) != 0 ||
            g_sck_send_fd_set(b->client_fd, "int", 4, &shm_fd, 1) < 0)
    {
        free_stream(s);
        return 1;
    }
    free_stream(s);
    b->frames_sent++;
    return 0;
}

/*****************************************************************************/
/* CPU path: convert the capture buffer's changed rects into the layout
   xrdp takes for the client's capture mode, and hand it over in a fresh
   shm (xrdp maps it and encodes asynchronously, so it is not reused). */
static int
cpu_send_frame(struct mon *m, int index, struct xh_rect *rects,
               int num_rects, int force)
{
    struct be *b = m->b;
    const struct wla_buffer *bf;
    struct xh_rect *tiles = NULL;
    int bgr;
    void *addr;
    int shm_fd;
    int n;
    int rv;

    if (index < 0 || index >= m->num_wb || m->cpu_out == NULL ||
            m->wb[index].shm == NULL)
    {
        return 0;
    }
    bf = m->wb + index;
    bgr = bf->fourcc == DRM_FORMAT_XBGR8888;
    switch (b->cpu_layout)
    {
        case WLXRDP_CPU_NV12:
            n = wlxrdp_cpu_nv12(bf->shm, bf->shm_stride, bgr, m->cpu_out,
                                bf->width, bf->height, rects, num_rects);
            break;
        case WLXRDP_CPU_XRGB:
            n = wlxrdp_cpu_xrgb(bf->shm, bf->shm_stride, bgr, m->cpu_out,
                                bf->width, bf->height, rects, num_rects);
            break;
        default:
            tiles = (struct xh_rect *)
                    malloc(sizeof(*tiles) * m->cpu_num_tiles);
            if (tiles == NULL)
            {
                return 0;
            }
            n = wlxrdp_cpu_yuvalp(bf->shm, bf->shm_stride, bgr, m->cpu_out,
                                  bf->width, bf->height, rects, num_rects,
                                  m->cpu_hashes, force,
                                  tiles, m->cpu_num_tiles);
            break;
    }
    if (n <= 0)
    {
        free(tiles);
        return 0; /* nothing changed */
    }
    if (g_alloc_shm_map_fd(&addr, &shm_fd, (int) m->cpu_out_bytes) != 0)
    {
        free(tiles);
        return 0;
    }
    memcpy(addr, m->cpu_out, m->cpu_out_bytes);
    g_munmap(addr, (int) m->cpu_out_bytes);

    switch (b->cpu_code)
    {
        case CC_GFX_A2:     /* not encoded: xrdp's x264 takes the NV12 */
            rv = send_gfx(m, 1, 0x000B, 0, rects, n, rects, n,
                          shm_fd, (int) m->cpu_out_bytes);
            break;
        case CC_GFX_PRO:    /* RFX progressive (RDPGFX_CODECID_CAPROGRESSIVE) */
            rv = send_gfx(m, 2, 0x0009, 0, rects, num_rects, tiles, n,
                          shm_fd, (int) m->cpu_out_bytes);
            break;
        case CC_SUF_RFX:    /* RFX surface commands */
            rv = send_paint_rect(m, rects, num_rects, tiles, n,
                                 shm_fd, (int) m->cpu_out_bytes);
            break;
        default:            /* CC_SIMPLE: bitmaps */
            rv = send_paint_rect(m, rects, n, rects, n,
                                 shm_fd, (int) m->cpu_out_bytes);
            break;
    }
    g_file_close(shm_fd);
    free(tiles);
    return rv;
}

/*****************************************************************************/
/* GPU path: send capture buffer index as one EGFX frame for the helper to
   encode; no shm. The helper encodes from the buffer the flags name,
   attaches the bitstream and marks it encoded, as for xorgxrdp. An IDR
   cannot be asked for here (bit 0 is "encoded"); the helper forces IDRs
   after each buffer batch and on its periodic schedule. */
static int
send_frame(struct mon *m, int index, struct xh_rect *rects, int num_rects)
{
    struct be *b = m->b;

    if (b->helper_pid <= 0 || !b->helper_ready)
    {
        return 0;
    }
    /* AVC444 goes as 0x000E, as from xorgxrdp: the helper picks the layout
       (0x000F for v2) from the caps in its batch */
    return send_gfx(m, 1, b->codec_id == 0x000B ? 0x000B : 0x000E,
                    (index << ACCEL_ASSIST_BUFFER_SHIFT) &
                    ACCEL_ASSIST_BUFFER_MASK,
                    rects, num_rects, rects, num_rects, -1, 0);
}

/*****************************************************************************/
/* Start the helper between us and xrdp, as xorgxrdp does: it gets the xrdp
   socket and one end of a socketpair; we talk to xrdp through the other. */
static int
helper_start(struct be *b)
{
    int spair[2];
    const char *path = getenv("WLXRDP_ACCEL_ASSIST");
    char exe[256];
    pid_t pid;

    if (b->helper_pid > 0)
    {
        return 0;
    }
    snprintf(exe, sizeof(exe), "%s",
             path != NULL ? path : XRDP_LIBEXEC_PATH "/xrdp-accel-assist");
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, spair) != 0)
    {
        return 1;
    }
    pid = fork();
    if (pid < 0)
    {
        close(spair[0]);
        close(spair[1]);
        return 1;
    }
    if (pid == 0)
    {
        char text[32];
        int fd;

        /* the two sockets, stdio on /dev/null, nothing else */
        for (fd = 3; fd < 1024; fd++)
        {
            if (fd != b->client_fd && fd != spair[0])
            {
                close(fd);
            }
        }
        fcntl(b->client_fd, F_SETFD, 0);
        fcntl(spair[0], F_SETFD, 0);
        fd = open("/dev/null", O_RDWR);
        dup2(fd, 0);
        dup2(fd, 1);
        snprintf(text, sizeof(text), "%d", spair[0]);
        setenv("XORGXRDP_XORG_FD", text, 1);
        snprintf(text, sizeof(text), "%d", b->client_fd);
        setenv("XORGXRDP_XRDP_FD", text, 1);
        execl(exe, exe, "-w", (char *) NULL);
        _exit(127);
    }
    LOG(LOG_LEVEL_INFO, "started %s -w, pid %d", exe, (int) pid);
    close(spair[0]);
    close(b->client_fd);
    b->client_fd = spair[1];
    b->helper_pid = pid;
    b->helper_ready = 0;
    return 0;
}

/*****************************************************************************/
static void
helper_stop(struct be *b)
{
    int status;

    if (b->helper_pid <= 0)
    {
        return;
    }
    if (waitpid(b->helper_pid, &status, WNOHANG) == 0)
    {
        kill(b->helper_pid, SIGTERM);
        waitpid(b->helper_pid, &status, 0);
    }
    b->helper_pid = 0;
    b->helper_ready = 0;
}

/*****************************************************************************/
/* The helper's setup batch (type 100): clear, session caps, then each
   monitor's capture buffers as dma-bufs, whose fds follow in order. Resent
   whenever the capture buffers change. */
static int
helper_send_buffers(struct be *b)
{
    struct stream *s;
    int caps;
    int count;
    int i;
    int j;

    if (b->helper_pid <= 0 || b->cpu)
    {
        return 0;
    }
    caps = 0;
    if (b->codec_id != 0x000B)
    {
        caps |= XH_CAPS_AVC444;
        if (b->codec_id == 0x000F)
        {
            caps |= XH_CAPS_AVC444_V2;
        }
    }
    count = 0;
    for (i = 0; i < b->num_mons; i++)
    {
        count += b->mons[i].num_wb;
    }
    make_stream(s);
    init_stream(s, 64 + count * XH_BATCH_DMABUF_BUFFER_BYTES);
    out_uint16_le(s, 100);
    out_uint16_le(s, 2 + count);
    out_uint32_le(s, 4 + 8 + count * XH_BATCH_DMABUF_BUFFER_BYTES);
    out_uint16_le(s, 1);                /* clear monitors */
    out_uint16_le(s, 4);
    out_uint16_le(s, 4);                /* session capabilities */
    out_uint16_le(s, 8);
    out_uint32_le(s, caps);
    for (i = 0; i < b->num_mons; i++)
    {
        struct mon *m = b->mons + i;

        for (j = 0; j < m->num_wb; j++)
        {
            const struct wla_buffer *bf = m->wb + j;

            out_uint16_le(s, XH_BATCH_DMABUF_BUFFER);
            out_uint16_le(s, XH_BATCH_DMABUF_BUFFER_BYTES);
            out_uint16_le(s, bf->width);
            out_uint16_le(s, bf->height);
            out_uint32_le(s, m->index);     /* monitor */
            out_uint32_le(s, j);            /* buffer */
            out_uint32_le(s, bf->fourcc);
            out_uint32_le(s, bf->stride);
            out_uint32_le(s, bf->offset);
            out_uint32_le(s, (uint32_t) (bf->modifier & 0xffffffff));
            out_uint32_le(s, (uint32_t) (bf->modifier >> 32));
        }
    }
    s_mark_end(s);
    if (send_all(b->client_fd, s->data, (int) (s->end - s->data)) != 0)
    {
        free_stream(s);
        return 1;
    }
    free_stream(s);
    for (i = 0; i < b->num_mons; i++)
    {
        struct mon *m = b->mons + i;

        for (j = 0; j < m->num_wb; j++)
        {
            int fd = m->wb[j].fd;

            if (g_sck_send_fd_set(b->client_fd, "int", 4, &fd, 1) < 0)
            {
                return 1;
            }
        }
    }
    b->helper_ready = 1;
    LOG(LOG_LEVEL_INFO, "helper: %d capture buffers, caps 0x%x", count, caps);
    return 0;
}

/*****************************************************************************/
/* each monitor's newest complete capture, full frame */
static int
send_full_frame(struct be *b)
{
    int i;

    for (i = 0; i < b->num_mons; i++)
    {
        struct mon *m = b->mons + i;
        struct xh_rect r;
        int rv;

        if (m->num_wb == 0)
        {
            continue;
        }
        if (m->core_last < 0 || (!b->cpu && !b->helper_ready))
        {
            m->force_idr = 1; /* the first capture will be whole */
            continue;
        }
        r.x = 0;
        r.y = 0;
        r.w = m->wb[m->core_last].width;
        r.h = m->wb[m->core_last].height;
        m->force_idr = 0;
        rv = b->cpu ? cpu_send_frame(m, m->core_last, &r, 1, 1)
             : send_frame(m, m->core_last, &r, 1);
        if (rv != 0)
        {
            return rv;
        }
        if (!b->cpu)
        {
            m->sent_id[m->core_last] = b->frame_id;
        }
    }
    return 0;
}

/* Send the cursor to xrdp: 32bpp ARGB, rows bottom-up, empty AND mask, as
   xorgxrdp does. 32x32 goes as order 51; bigger cursors as a 96x96 order 63
   (shm fd) when the client takes large pointers, else cropped to 32x32. */
static int
cursor_send(struct be *b)
{
    struct stream *s;
    int visible;
    int large;
    int sw;
    int sh;
    int x;
    int y;
    int hx;
    int hy;
    int rv;
    int data_bytes;
    int mask_bytes;
    uint8_t *data;

    if (b->client_fd < 0 || !b->have_ci)
    {
        return 0;
    }
    visible = b->cur_visible && b->cur_img != NULL;
    if (visible == b->sent_visible &&
            (!visible ||
             (b->sent_image != NULL && b->sent_w == b->cur_w &&
              b->sent_h == b->cur_h && b->sent_hx == b->cur_hx &&
              b->sent_hy == b->cur_hy &&
              memcmp(b->sent_image, b->cur_img,
                     b->cur_w * b->cur_h * 4) == 0)))
    {
        return 0; /* the client already has it */
    }
    b->sent_visible = visible;
    if (visible)
    {
        free(b->sent_image);
        b->sent_image = malloc(b->cur_w * b->cur_h * 4);
        if (b->sent_image != NULL)
        {
            memcpy(b->sent_image, b->cur_img, b->cur_w * b->cur_h * 4);
        }
        b->sent_w = b->cur_w;
        b->sent_h = b->cur_h;
        b->sent_hx = b->cur_hx;
        b->sent_hy = b->cur_hy;
    }
    make_stream(s);
    init_stream(s, 32 * 32 * 4 + 32 * 32 / 8 + 64);
    if (!visible)
    {
        /* hidden: the null system pointer */
        out_uint16_le(s, 3);
        out_uint16_le(s, 3);
        out_uint32_le(s, 4 + 8 + 4);
        out_uint16_le(s, 1);
        out_uint16_le(s, 4);
        out_uint16_le(s, 65);       /* set pointer system */
        out_uint16_le(s, 8);
        out_uint32_le(s, SYSPTR_NULL);
        out_uint16_le(s, 2);
        out_uint16_le(s, 4);
        s_mark_end(s);
        rv = send_all(b->client_fd, s->data, (int) (s->end - s->data));
        free_stream(s);
        return rv;
    }
    large = (b->cur_w > 32 || b->cur_h > 32) &&
            (b->ci.large_pointer_support_flags & LARGE_POINTER_FLAG_96x96);
    sw = large ? 96 : 32;
    sh = large ? 96 : 32;
    data_bytes = sw * sh * 4;
    mask_bytes = sw * sh / 8;
    data = calloc(1, data_bytes + mask_bytes); /* mask all zero */
    if (data == NULL)
    {
        free_stream(s);
        return 1;
    }
    for (y = 0; y < sh && y < b->cur_h; y++)
    {
        uint32_t *dst = (uint32_t *) data + (sh - 1 - y) * sw;

        for (x = 0; x < sw && x < b->cur_w; x++)
        {
            dst[x] = b->cur_img[y * b->cur_w + x];
        }
    }
    hx = b->cur_hx < 0 ? 0 : (b->cur_hx >= sw ? sw - 1 : b->cur_hx);
    hy = b->cur_hy < 0 ? 0 : (b->cur_hy >= sh ? sh - 1 : b->cur_hy);

    out_uint16_le(s, 3);
    out_uint16_le(s, 3);
    if (!large)
    {
        int order_bytes = 2 + 2 + 2 + 2 + 2 + data_bytes + mask_bytes;

        out_uint32_le(s, 4 + order_bytes + 4);
        out_uint16_le(s, 1);
        out_uint16_le(s, 4);
        out_uint16_le(s, 51);       /* set pointer ex */
        out_uint16_le(s, order_bytes);
        out_uint16_le(s, hx);
        out_uint16_le(s, hy);
        out_uint16_le(s, 32);       /* bpp */
        out_uint8a(s, data, data_bytes);
        out_uint8a(s, data + data_bytes, mask_bytes);
        out_uint16_le(s, 2);
        out_uint16_le(s, 4);
        s_mark_end(s);
        rv = send_all(b->client_fd, s->data, (int) (s->end - s->data));
    }
    else
    {
        void *addr;
        int fd;

        rv = 1;
        if (g_alloc_shm_map_fd(&addr, &fd, data_bytes + mask_bytes) == 0)
        {
            memcpy(addr, data, data_bytes + mask_bytes);
            out_uint32_le(s, 4 + 14 + 4);
            out_uint16_le(s, 1);
            out_uint16_le(s, 4);
            out_uint16_le(s, 63);   /* set pointer shmfd */
            out_uint16_le(s, 14);
            out_uint16_le(s, hx);
            out_uint16_le(s, hy);
            out_uint16_le(s, 32);
            out_uint16_le(s, sw);
            out_uint16_le(s, sh);
            out_uint16_le(s, 2);
            out_uint16_le(s, 4);
            s_mark_end(s);
            rv = send_all(b->client_fd, s->data, (int) (s->end - s->data));
            if (rv == 0 &&
                    g_sck_send_fd_set(b->client_fd, "int", 4, &fd, 1) < 0)
            {
                rv = 1;
            }
            g_munmap(addr, data_bytes + mask_bytes);
            g_file_close(fd);
        }
    }
    LOG(LOG_LEVEL_DEBUG, "cursor %dx%d hotspot %d,%d sent as %dx%d",
        b->cur_w, b->cur_h, b->cur_hx, b->cur_hy, sw, sh);
    free(data);
    free_stream(s);
    return rv;
}

/*****************************************************************************/
/* input: the core's side */

static void
kbd_send_modifiers(struct be *b)
{
    if (b->xkb_state == NULL)
    {
        return;
    }
    b->ops->modifiers(
        b->ad,
        xkb_state_serialize_mods(b->xkb_state, XKB_STATE_MODS_DEPRESSED),
        xkb_state_serialize_mods(b->xkb_state, XKB_STATE_MODS_LATCHED),
        xkb_state_serialize_mods(b->xkb_state, XKB_STATE_MODS_LOCKED),
        xkb_state_serialize_layout(b->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE));
}

/*****************************************************************************/
static void
kbd_key(struct be *b, int x11_keycode, int down)
{
    if (b->xkb_state == NULL || x11_keycode < 8)
    {
        return;
    }
    b->ops->key(b->ad, x11_keycode - 8, down);
    xkb_state_update_key(b->xkb_state, x11_keycode,
                         down ? XKB_KEY_DOWN : XKB_KEY_UP);
    kbd_send_modifiers(b);
}

/*****************************************************************************/
/* hand the adapter a keymap, if it takes ours */
static void
kbd_upload(struct be *b, const char *text)
{
    if (b->ops->caps & WLA_CAP_SET_KEYMAP)
    {
        b->ops->keymap(b->ad, text);
    }
}

/*****************************************************************************/
/* An XKB rules/model/layout/variant/options name, as keymaps spell them.
   Anything else (a path: "/...", "../...") goes nowhere near xkbcommon's
   file lookup, which even asserts on an absolute rules name. */
static int
xkb_name_ok(const char *name)
{
    for (; *name != '\0'; name++)
    {
        if (!((*name >= 'a' && *name <= 'z') || (*name >= 'A' && *name <= 'Z') ||
                (*name >= '0' && *name <= '9') ||
                strchr("_-,:()+", *name) != NULL))
        {
            return 0;
        }
    }
    return 1;
}

/*****************************************************************************/
/* Keymap from the client's layout, as xorgxrdp loads it into X. */
static void
kbd_load_keymap(struct be *b)
{
    struct xkb_rule_names names;
    struct xkb_keymap *keymap;
    xkb_keycode_t kc;

    names.rules = b->ci.xkb_rules[0] ? b->ci.xkb_rules : "evdev";
    names.model = b->ci.model;
    names.layout = b->ci.layout;
    names.variant = b->ci.variant;
    names.options = b->ci.options;
    if (!xkb_name_ok(names.rules) || !xkb_name_ok(names.model) ||
            !xkb_name_ok(names.layout) || !xkb_name_ok(names.variant) ||
            !xkb_name_ok(names.options))
    {
        LOG(LOG_LEVEL_WARNING, "the client's keymap names are not XKB "
            "names; using evdev/us");
        names.rules = "evdev";
        names.model = "";
        names.layout = "us";
        names.variant = "";
        names.options = "";
    }
    LOG(LOG_LEVEL_INFO, "keymap: rules '%s' model '%s' layout '%s' "
        "variant '%s' options '%s'", names.rules, names.model, names.layout,
        names.variant, names.options);
    keymap = xkb_keymap_new_from_names(b->xkb_ctx, &names, 0);
    if (keymap == NULL)
    {
        LOG(LOG_LEVEL_WARNING, "keymap failed, falling back to evdev/us");
        names.rules = "evdev";
        names.model = "";
        names.layout = "us";
        names.variant = "";
        names.options = "";
        keymap = xkb_keymap_new_from_names(b->xkb_ctx, &names, 0);
    }
    if (keymap == NULL)
    {
        /* keep whatever keymap there was */
        LOG(LOG_LEVEL_ERROR, "no usable keymap");
        return;
    }
    if (b->keymap != NULL)
    {
        xkb_state_unref(b->xkb_state);
        xkb_keymap_unref(b->keymap);
    }
    b->keymap = keymap;
    b->xkb_state = xkb_state_new(b->keymap);
    free(b->keymap_text);
    b->keymap_text = xkb_keymap_get_as_string(b->keymap,
                     XKB_KEYMAP_FORMAT_TEXT_V1);
    kbd_upload(b, b->keymap_text);

    /* Spare keycodes, from the top: keys the keymap names but gives no
       symbols. At most 255, the X11 limit: Xwayland drops higher ones (evdev
       keymaps name every keycode up to there, and some beyond). */
    b->num_uni = 0;
    for (kc = xkb_keymap_max_keycode(b->keymap) < 255
              ? xkb_keymap_max_keycode(b->keymap) : 255;
            kc >= xkb_keymap_min_keycode(b->keymap) && kc > 8 &&
            b->num_uni < UNI_SLOTS; kc--)
    {
        if (xkb_keymap_key_get_name(b->keymap, kc) != NULL &&
                xkb_keymap_num_layouts_for_key(b->keymap, kc) == 0)
        {
            b->uni[b->num_uni].keycode = kc;
            b->uni[b->num_uni].cp = 0;
            b->uni[b->num_uni].used = 0;
            b->num_uni++;
        }
    }
}

/*****************************************************************************/
/* text with ins inserted before the "};" that ends the section named sect */
static char *
keymap_insert(const char *text, const char *sect, const char *ins)
{
    const char *start = strstr(text, sect);
    const char *end = start != NULL ? strstr(start, "\n};") : NULL;
    size_t at;
    size_t ins_len = strlen(ins);
    size_t len = strlen(text);
    char *out;

    if (end == NULL)
    {
        return NULL;
    }
    at = (size_t) (end - text) + 1; /* after the newline */
    out = (char *) malloc(len + ins_len + 1);
    if (out == NULL)
    {
        return NULL;
    }
    memcpy(out, text, at);
    memcpy(out + at, ins, ins_len);
    memcpy(out + at + ins_len, text + at, len - at + 1);
    return out;
}

/*****************************************************************************/
/* The client keymap plus, on each spare key, the character it holds */
static char *
keymap_with_unicode(struct be *b)
{
    char syms[UNI_SLOTS * 128 + 1] = "";
    int i;

    for (i = 0; i < b->num_uni; i++)
    {
        char name[64];

        if (b->uni[i].cp == 0 ||
                xkb_keysym_get_name(xkb_utf32_to_keysym(b->uni[i].cp),
                                    name, sizeof(name)) <= 0)
        {
            continue;
        }
        snprintf(syms + strlen(syms), sizeof(syms) - strlen(syms),
                 "\tkey <%s> { [ %s ] };\n",
                 xkb_keymap_key_get_name(b->keymap, b->uni[i].keycode), name);
    }
    return keymap_insert(b->keymap_text, "xkb_symbols", syms);
}

/*****************************************************************************/
/* Type a character the client's layout has no key for: give it a spare
   keycode (the least recently used), and press that. */
static void
kbd_unicode(struct be *b, uint32_t cp)
{
    struct xkb_keymap *check;
    char *text;
    int slot = -1;
    int i;

    if (!(b->ops->caps & WLA_CAP_SET_KEYMAP) || b->keymap_text == NULL ||
            b->num_uni == 0 || xkb_utf32_to_keysym(cp) == XKB_KEY_NoSymbol)
    {
        return;
    }
    for (i = 0; i < b->num_uni && slot < 0; i++)
    {
        if (b->uni[i].cp == cp)
        {
            slot = i;
        }
    }
    if (slot < 0)
    {
        slot = 0;
        for (i = 1; i < b->num_uni; i++)
        {
            if (b->uni[i].used < b->uni[slot].used)
            {
                slot = i;
            }
        }
        b->uni[slot].cp = cp;
        text = keymap_with_unicode(b);
        check = text != NULL ? xkb_keymap_new_from_string(
                    b->xkb_ctx, text, XKB_KEYMAP_FORMAT_TEXT_V1, 0) : NULL;
        if (check == NULL)
        {
            LOG(LOG_LEVEL_WARNING, "keymap for U+%04X failed", cp);
            b->uni[slot].cp = 0;
            free(text);
            return;
        }
        xkb_keymap_unref(check);
        kbd_upload(b, text);
        free(text);
        kbd_send_modifiers(b); /* a new keymap resets them */
    }
    b->uni[slot].used = ++b->uni_clock;
    b->ops->key(b->ad, b->uni[slot].keycode - 8, 1);
    b->ops->key(b->ad, b->uni[slot].keycode - 8, 0);
}

/*****************************************************************************/
/* RDP synchronize: bring the lock keys to the client's state */
static void
kbd_sync(struct be *b, int flags)
{
    struct
    {
        int sync_flag;
        const char *led;
        int keycode;
    } locks[] =
    {
        { TS_SYNC_CAPS_LOCK, XKB_LED_NAME_CAPS, b->ci.x11_keycode_caps_lock },
        { TS_SYNC_NUM_LOCK, XKB_LED_NAME_NUM, b->ci.x11_keycode_num_lock },
        {
            TS_SYNC_SCROLL_LOCK, XKB_LED_NAME_SCROLL,
            b->ci.x11_keycode_scroll_lock
        }
    };
    int i;

    if (b->xkb_state == NULL)
    {
        return;
    }
    for (i = 0; i < 3; i++)
    {
        int on = xkb_state_led_name_is_active(b->xkb_state, locks[i].led) > 0;
        int want = (flags & locks[i].sync_flag) != 0;

        if (on != want && locks[i].keycode >= 8)
        {
            kbd_key(b, locks[i].keycode, 1);
            kbd_key(b, locks[i].keycode, 0);
        }
    }
}

/*****************************************************************************/
static void
ptr_button(struct be *b, int button, int down)
{
    b->ops->button(b->ad, button, down);
}

/*****************************************************************************/
/* wheel notches: 15 surface units each, as Wayland wheels scroll */
static void
ptr_wheel(struct be *b, enum wla_axis axis, int steps)
{
    b->ops->scroll(b->ad, axis, steps, 15.0 * steps);
}

/*****************************************************************************/
/* High-resolution scrolling (xrdp's WM_TOUCH_VSCROLL/HSCROLL): the client's
   wheel delta, 120 a notch, positive up or right. Whole notches stay wheel
   clicks; anything finer (precision touchpads, hi-res wheels) scrolls
   smoothly, 15 surface units a notch as a wheel click scrolls. */
static void
ptr_scroll(struct be *b, enum wla_axis axis, int delta)
{
    int value;

    /* RDP's wheel rotation is 9 bits, sign included */
    if (delta == 0 || delta < -0x1FF || delta > 0x1FF)
    {
        return;
    }
    /* the adapter's vertical axis runs down, RDP's up */
    value = axis == WLA_AXIS_VERTICAL ? -delta : delta;
    if (value % 120 == 0)
    {
        ptr_wheel(b, axis, value / 120);
        return;
    }
    b->ops->scroll(b->ad, axis, 0, value * 15.0 / 120.0);
}

/*****************************************************************************/
/* A new layout: stop capture, apply it, capture again. Non-zero, with the
   old layout still running, if the layout is unusable. */
static int
relayout(struct be *b, int count, const struct monitor_info *mi,
         int width, int height)
{
    if (b->ops->max_monitors(b->ad) < 1 ||
            layout_set(b, count, mi, width, height) != 0)
    {
        return 1; /* the running layout stays */
    }
    b->ops->stop(b->ad);
    b->ops->set_layout(b->ad, b->lay, b->active);
    b->ops->start(b->ad, b->cpu ? WLA_SHM : WLA_DMABUF);
    if (core_helper_sync(b) != 0)
    {
        LOG(LOG_LEVEL_ERROR, "the helper did not take the new buffers");
    }
    return 0;
}

/*****************************************************************************/
static void
handle_input(struct be *b, int msg, int p1, int p2, int p3, int p4)
{
    switch (msg)
    {
        case WM_KEYDOWN:
        case WM_KEYUP:
            kbd_key(b, p1, msg == WM_KEYDOWN);
            break;
        case WM_KEYBRD_UNICODE:
            if (p2)
            {
                kbd_unicode(b, (uint32_t) p1);
            }
            break;
        case WM_KEYBRD_SYNC:
            kbd_sync(b, p1);
            break;
        case WM_MOUSEMOVE:
            /* client coordinates span the layout, as the output layout
               does: absolute motion maps onto its whole extent */
            if (b->total_w > 0)
            {
                int x = p1 < 0 ? 0 : p1 >= b->total_w ? b->total_w - 1 : p1;
                int y = p2 < 0 ? 0 : p2 >= b->total_h ? b->total_h - 1 : p2;

                b->ops->motion(b->ad, x, y, b->total_w, b->total_h);
            }
            break;
        case WM_LBUTTONUP:
        case WM_LBUTTONDOWN:
            ptr_button(b, BTN_LEFT, msg == WM_LBUTTONDOWN);
            break;
        case WM_RBUTTONUP:
        case WM_RBUTTONDOWN:
            ptr_button(b, BTN_RIGHT, msg == WM_RBUTTONDOWN);
            break;
        case WM_BUTTON3UP:
        case WM_BUTTON3DOWN:
            ptr_button(b, BTN_MIDDLE, msg == WM_BUTTON3DOWN);
            break;
        /* wheel notches arrive as down/up pairs; act on the down */
        case WM_BUTTON4DOWN:
            ptr_wheel(b, WLA_AXIS_VERTICAL, -1);
            break;
        case WM_BUTTON5DOWN:
            ptr_wheel(b, WLA_AXIS_VERTICAL, 1);
            break;
        case WM_BUTTON6DOWN:
            ptr_wheel(b, WLA_AXIS_HORIZONTAL, -1);
            break;
        case WM_BUTTON7DOWN:
            ptr_wheel(b, WLA_AXIS_HORIZONTAL, 1);
            break;
        case WM_TOUCH_VSCROLL:
            ptr_scroll(b, WLA_AXIS_VERTICAL, p3);
            break;
        case WM_TOUCH_HSCROLL:
            ptr_scroll(b, WLA_AXIS_HORIZONTAL, p3);
            break;
        case WM_BUTTON8UP:
        case WM_BUTTON8DOWN:
            ptr_button(b, BTN_SIDE, msg == WM_BUTTON8DOWN);
            break;
        case WM_BUTTON9UP:
        case WM_BUTTON9DOWN:
            ptr_button(b, BTN_EXTRA, msg == WM_BUTTON9DOWN);
            break;
        case WM_INVALIDATE:
            LOG(LOG_LEVEL_INFO, "invalidate");
            send_full_frame(b);
            break;
        case 301: /* version */
            LOG(LOG_LEVEL_INFO, "xrdp version message, sending caps");
            send_caps(b);
            break;
        default:
            break;
    }
    (void) p4;
}

/*****************************************************************************/
static void
handle_client_info(struct be *b, const char *data, int bytes)
{
    const struct display_size_description *ds;

    memset(&b->ci, 0, sizeof(b->ci));
    memcpy(&b->ci, data, bytes < (int) sizeof(b->ci) ? bytes
           : (int) sizeof(b->ci));
    /* the keymap names are C strings to us: make sure they end */
    b->ci.xkb_rules[sizeof(b->ci.xkb_rules) - 1] = '\0';
    b->ci.model[sizeof(b->ci.model) - 1] = '\0';
    b->ci.layout[sizeof(b->ci.layout) - 1] = '\0';
    b->ci.variant[sizeof(b->ci.variant) - 1] = '\0';
    b->ci.options[sizeof(b->ci.options) - 1] = '\0';
    b->have_ci = 1;
    ds = &b->ci.display_sizes;
    LOG(LOG_LEVEL_INFO, "client info: version %d, %dx%d, %d monitor(s), "
        "capture_code %d, avc444 level %d", b->ci.version, ds->session_width,
        ds->session_height, ds->monitorCount, b->ci.capture_code,
        b->ci.gfx_avc444);
    b->codec_id = 0x000B;
    if (b->ci.gfx_avc444 > 0 && getenv("WLXRDP_AVC420") == NULL)
    {
        b->codec_id = (b->ci.gfx_avc444 >= 2) ? 0x000F : 0x000E;
    }

    /* GFX H.264 goes through the helper's GPU encoder unless it has failed
       here before (no working VA-API) or WLXRDP_ACCEL=0. Everything else,
       and that, takes the CPU path: xrdp encodes, as for xorgxrdp without
       accel-assist. */
    b->cpu_code = b->ci.capture_code;
    b->cpu = !(b->ci.capture_code == CC_GFX_A2 && !b->accel_failed &&
               (getenv("WLXRDP_ACCEL") == NULL ||
                strcmp(getenv("WLXRDP_ACCEL"), "0") != 0));
    if (b->cpu)
    {
        switch (b->ci.capture_code)
        {
            case CC_GFX_A2:
                b->cpu_layout = WLXRDP_CPU_NV12;
                b->codec_id = 0x000B; /* x264 encodes 4:2:0 */
                break;
            case CC_GFX_PRO:
            case CC_SUF_RFX:
                b->cpu_layout = WLXRDP_CPU_YUVALP;
                break;
            default:
                b->cpu_layout = WLXRDP_CPU_XRGB;
                if (b->ci.capture_code != CC_SIMPLE ||
                        (b->ci.bpp != 24 && b->ci.bpp != 32))
                {
                    LOG(LOG_LEVEL_ERROR, "capture code %d at %d bpp is not "
                        "supported; the client will see nothing",
                        b->ci.capture_code, b->ci.bpp);
                }
                break;
        }
        LOG(LOG_LEVEL_INFO, "CPU path: capture code %d, xrdp encodes",
            b->ci.capture_code);
    }
    else
    {
        LOG(LOG_LEVEL_INFO, "GPU path: codec id 0x%4.4x", b->codec_id);
    }

    kbd_load_keymap(b);
    b->sent_visible = -1; /* replay the current cursor to this client */

    /* at most one capture per frame interval, as xorgxrdp paces capture */
    b->frame_interval_ms = b->ci.h264_frame_interval > 0
                           ? b->ci.h264_frame_interval : 0;
    LOG(LOG_LEVEL_INFO, "frame interval %d ms", b->frame_interval_ms);

    if (relayout(b, (int) ds->monitorCount, ds->minfo_wm,
                 (int) ds->session_width, (int) ds->session_height) != 0)
    {
        /* xrdp checks the client's layout; this one did not come from
           there. Something to show rather than nothing. */
        LOG(LOG_LEVEL_ERROR, "unusable connect-time layout; using "
            "1024x768");
        relayout(b, 0, NULL, 1024, 768);
    }
    /* the helper encodes GFX H.264 only, as for xorgxrdp */
    if (!b->cpu && helper_start(b) == 0)
    {
        b->helper_start_ms = now_ms32();
        b->buffers_changed = 1;
        core_helper_sync(b);
    }
    /* The connect-time layout is in place. xrdp counts it as an
       outstanding resize until told, as xorgxrdp tells it after client
       info, and holds every later resize (dynamic resolution) behind it. */
    send_resize_done(b);
    if (cursor_send(b) != 0)
    {
        drop_client(b);
    }
}

/*****************************************************************************/
/* One message from xrdp: [u32 len incl. this][u16 type]... */
static int
handle_xrdp_msg(struct be *b, const char *data, int len)
{
    struct stream ls;
    struct stream *s = &ls;
    int type;

    memset(s, 0, sizeof(*s));
    s->data = (char *) data;
    s->p = s->data;
    s->end = s->data + len;
    s->size = len;
    in_uint8s(s, 4);
    in_uint16_le(s, type);
    switch (type)
    {
        case 103: /* client input / control */
        {
            int msg, p1, p2, p3, p4;

            if (!s_check_rem(s, 20))
            {
                return 1;
            }
            in_uint32_le(s, msg);
            in_uint32_le(s, p1);
            in_uint32_le(s, p2);
            in_uint32_le(s, p3);
            in_uint32_le(s, p4);
            if (msg == 302)
            {
                /* monitor update: width, height, count, 0, then the
                   monitors */
                struct monitor_info mi[CLIENT_MONITOR_DATA_MAXIMUM_MONITORS];
                int count = p3;

                if (count < 0 || count > CLIENT_MONITOR_DATA_MAXIMUM_MONITORS ||
                        !s_check_rem(s, count * (int) sizeof(mi[0])))
                {
                    return 1;
                }
                in_uint8a(s, mi, count * sizeof(mi[0]));
                LOG(LOG_LEVEL_INFO, "monitor update %dx%d, %d monitor(s)",
                    p1, p2, count);
                if (relayout(b, count, mi, p1, p2) != 0)
                {
                    LOG(LOG_LEVEL_ERROR, "unusable monitor update ignored");
                }
                /* either way, or xrdp holds every later resize */
                send_resize_done(b);
                break;
            }
            handle_input(b, msg, p1, p2, p3, p4);
            break;
        }
        case 104: /* client info */
            handle_client_info(b, s->p, (int) (s->end - s->p));
            break;
        case 106: /* frame ack: flags, frame_id[, rtt] */
        {
            int flags, id;

            if (!s_check_rem(s, 8))
            {
                return 1;
            }
            in_uint32_le(s, flags);
            in_uint32_le(s, id);
            (void) flags;
            /* INT_MAX: "everything is acked". Acks only move forward, and
               never past the last frame sent: anything else would make the
               count in flight nonsense (and stall capture). */
            if (id == 0x7fffffff || id > b->frame_id)
            {
                id = b->frame_id;
            }
            if (id > b->acked_id)
            {
                int i;

                b->acked_id = id;
                for (i = 0; i < b->num_mons; i++)
                {
                    core_release(b, b->mons + i);
                }
            }
            break;
        }
        case 108: /* suppress output */
        {
            int suppress;

            if (!s_check_rem(s, 4))
            {
                return 1;
            }
            in_uint32_le(s, suppress);
            LOG(LOG_LEVEL_INFO, "suppress output %d", suppress);
            b->suppress = suppress != 0;
            if (!suppress)
            {
                send_full_frame(b);
            }
            break;
        }
        default:
            break;
    }
    return 0;
}

/*****************************************************************************/
static int
read_xrdp(struct be *b)
{
    int rv;
    int len;

    rv = recv(b->client_fd, b->in + b->in_len, IN_MAX - b->in_len, 0);
    if (rv <= 0)
    {
        return (rv < 0 && (errno == EINTR || errno == EAGAIN)) ? 0 : 1;
    }
    b->in_len += rv;
    while (b->in_len >= 4)
    {
        uint32_t ulen = (uint32_t) (unsigned char) b->in[0] |
                        ((uint32_t) (unsigned char) b->in[1] << 8) |
                        ((uint32_t) (unsigned char) b->in[2] << 16) |
                        ((uint32_t) (unsigned char) b->in[3] << 24);

        if (ulen < 6 || ulen > IN_MAX)
        {
            LOG(LOG_LEVEL_ERROR, "bad message length %u", ulen);
            return 1;
        }
        len = (int) ulen;
        if (b->in_len < len)
        {
            break;
        }
        if (handle_xrdp_msg(b, b->in, len) != 0)
        {
            return 1;
        }
        memmove(b->in, b->in + len, b->in_len - len);
        b->in_len -= len;
    }
    return 0;
}

/*****************************************************************************/
/* The core's side of the adapter interface (wla.h): what an adapter
   reports, and what the core does with it. */

/* a monitor's capture buffers, as the adapter has them (n 0: none) */
static void
core_buffers(void *core, int mon, int n, const struct wla_buffer *bufs)
{
    struct be *b = core;
    struct mon *m;

    if (mon < 0 || mon >= MAX_MONS)
    {
        return;
    }
    m = b->mons + mon;
    n = n < 0 ? 0 : n > WLA_MAX_BUFS ? WLA_MAX_BUFS : n;
    if (n > 0)
    {
        memcpy(m->wb, bufs, sizeof(bufs[0]) * n);
    }
    m->num_wb = n;
    m->width = n > 0 ? m->wb[0].width : 0;
    m->height = n > 0 ? m->wb[0].height : 0;
    m->core_last = -1;
    m->force_idr = 1;
    memset(m->held, 0, sizeof(m->held));
    memset(m->sent_id, 0, sizeof(m->sent_id));
    m->want_out = 0;
    /* the helper's images are of the old set until the next batch */
    b->buffers_changed = 1;
    b->helper_ready = 0;
    free(m->cpu_out);
    m->cpu_out = NULL;
    free(m->cpu_hashes);
    m->cpu_hashes = NULL;
    if (n > 0 && b->cpu)
    {
        /* CPU path: the frame in the capture code's layout */
        int w = m->wb[0].width;
        int h = m->wb[0].height;

        m->cpu_out_bytes = wlxrdp_cpu_bytes(b->cpu_layout, w, h);
        m->cpu_out = (uint8_t *) calloc(1, m->cpu_out_bytes);
        m->cpu_num_tiles = ((w + 63) / 64) * ((h + 63) / 64);
        m->cpu_hashes = (uint64_t *) calloc(m->cpu_num_tiles,
                                            sizeof(uint64_t));
        if (m->cpu_out == NULL || m->cpu_hashes == NULL)
        {
            LOG(LOG_LEVEL_ERROR, "monitor %d: out of memory", mon);
            m->num_wb = 0;
        }
    }
}

/* Give back the buffers the core no longer needs: all but the newest,
   once the frame each last went out in has been acknowledged (the helper
   reads a GPU-path buffer when it encodes, which may be after the send). */
static void
core_release(struct be *b, struct mon *m)
{
    int i;

    for (i = 0; i < m->num_wb; i++)
    {
        if (m->held[i] && i != m->core_last && m->sent_id[i] <= b->acked_id)
        {
            m->held[i] = 0;
            b->ops->release(b->ad, m->index, i);
        }
    }
}

/* a complete frame in buffer buf: send it */
static void
core_frame(void *core, int mon, int buf, const struct xh_rect *damage,
           int num_damage)
{
    struct be *b = core;
    struct mon *m;
    struct xh_rect rects[MAX_RECTS];
    int num_rects;
    int before;
    int rv;

    if (mon < 0 || mon >= MAX_MONS || buf < 0 || buf >= b->mons[mon].num_wb)
    {
        return;
    }
    m = b->mons + mon;
    m->core_last = buf;
    m->held[buf] = 1;
    m->sent_id[buf] = 0;
    m->want_out = 0;
    num_rects = num_damage < 0 ? 0 : num_damage > MAX_RECTS ? MAX_RECTS
                : num_damage;
    if (num_rects > 0)
    {
        memcpy(rects, damage, sizeof(rects[0]) * num_rects);
    }
    if (num_rects == 0 || m->force_idr)
    {
        /* all of it: none reported, or a full frame is due */
        rects[0].x = 0;
        rects[0].y = 0;
        rects[0].w = m->wb[buf].width;
        rects[0].h = m->wb[buf].height;
        num_rects = 1;
    }
    if (b->client_fd < 0 || !b->have_ci)
    {
        core_release(b, m);
        return;
    }
    before = b->frame_id;
    rv = b->cpu ? cpu_send_frame(m, buf, rects, num_rects, m->force_idr)
         : send_frame(m, buf, rects, num_rects);
    if (rv != 0)
    {
        drop_client(b);
        return;
    }
    if (b->frame_id != before)
    {
        /* sent: a full frame, if one was due, has gone; the helper may read
           a GPU-path buffer until this frame's ack (the CPU path copied it) */
        m->force_idr = 0;
        if (!b->cpu)
        {
            m->sent_id[buf] = b->frame_id;
        }
    }
    core_release(b, m);
}

/* the pointer's image changed (argb NULL: hidden): keep it, send it */
static void
core_cursor(void *core, const uint32_t *argb, int w, int h,
            int hot_x, int hot_y)
{
    struct be *b = core;

    b->cur_visible = 0;
    if (argb != NULL && w > 0 && h > 0 && w <= 512 && h <= 512)
    {
        uint32_t *img = malloc((size_t) w * h * 4);

        if (img != NULL)
        {
            memcpy(img, argb, (size_t) w * h * 4);
            free(b->cur_img);
            b->cur_img = img;
            b->cur_w = w;
            b->cur_h = h;
            b->cur_hx = hot_x;
            b->cur_hy = hot_y;
            b->cur_visible = 1;
        }
    }
    if (b->client_fd >= 0 && b->have_ci && cursor_send(b) != 0)
    {
        drop_client(b);
    }
}

/* the compositor is gone */
static void
core_lost(void *core, const char *why)
{
    struct be *b = core;

    LOG(LOG_LEVEL_ERROR, "%s", why);
    b->lost = 1;
}

static const struct wla_events g_core_events =
{
    core_buffers, core_frame, core_cursor, core_lost
};

/*****************************************************************************/
/* The core's pacing: ask for a frame from each capturing monitor at most
   once a frame interval, while the client can take frames. Returns the ms
   until the next ask is due, -1 if nothing waits on time (an ack or a
   frame will wake the loop). */
static int
core_pace(struct be *b)
{
    uint32_t now = now_ms32();
    int wait = -1;
    int i;

    if (b->client_fd < 0 || !b->have_ci || b->suppress ||
            b->frame_id - b->acked_id >= MAX_IN_FLIGHT * b->active)
    {
        return -1;
    }
    for (i = 0; i < b->num_mons; i++)
    {
        struct mon *m = b->mons + i;
        uint32_t since = now - m->want_ms;

        if (m->num_wb == 0 || m->want_out)
        {
            continue;
        }
        if (since >= (uint32_t) b->frame_interval_ms)
        {
            m->want_out = 1;
            m->want_ms = now;
            b->ops->want_frame(b->ad, i);
        }
        else
        {
            int left = b->frame_interval_ms - (int) since;

            wait = wait < 0 || left < wait ? left : wait;
        }
    }
    return wait;
}

/*****************************************************************************/
/* the helper's batch, after the capture buffers changed */
static int
core_helper_sync(struct be *b)
{
    if (!b->buffers_changed || b->client_fd < 0 || b->helper_pid <= 0 ||
            b->cpu)
    {
        return 0;
    }
    b->buffers_changed = 0;
    return helper_send_buffers(b);
}

/*****************************************************************************/
static void
drop_client(struct be *b)
{
    LOG(LOG_LEVEL_INFO, "xrdp disconnected after %d frames", b->frames_sent);
    b->ops->stop(b->ad);
    close(b->client_fd);
    if (b->helper_pid > 0 && waitpid(b->helper_pid, NULL, WNOHANG) > 0 &&
            now_ms32() - b->helper_start_ms < 10000)
    {
        /* the helper exited soon after starting: its GPU encoder is not
           usable here. Later connections take the CPU path. */
        LOG(LOG_LEVEL_WARNING, "the accel-assist helper exited at start; "
            "using the CPU path from now on");
        b->accel_failed = 1;
        b->helper_pid = 0;
    }
    helper_stop(b);
    b->client_fd = -1;
    b->in_len = 0;
    b->have_ci = 0;
    b->frame_id = 0;
    b->acked_id = 0;
    b->suppress = 0;
    b->frames_sent = 0;
}

/*****************************************************************************/
static int
listen_unix(const char *path)
{
    struct sockaddr_un sa;
    int fd;

    if (strlen(path) >= sizeof(sa.sun_path))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) != 0 ||
            listen(fd, 2) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

/*****************************************************************************/
static void
on_signal(int sig)
{
    g_term = 1;
}

/*****************************************************************************/
/* the adapters, in the order tried */
static const struct wla_ops *const g_adapters[] =
{
    &wla_wlr,
    NULL
};

/* WLXRDP_ADAPTER names one; else the first that connects */
static int
adapter_open(struct be *b)
{
    const char *want = getenv("WLXRDP_ADAPTER");
    int i;

    for (i = 0; g_adapters[i] != NULL; i++)
    {
        const struct wla_ops *ops = g_adapters[i];

        if (want != NULL && want[0] != '\0' && strcmp(want, ops->name) != 0)
        {
            continue;
        }
        b->ad = ops->create(b->ev, b);
        if (b->ad != NULL)
        {
            b->ops = ops;
            b->num_mons = ops->max_monitors(b->ad);
            if (b->num_mons > MAX_MONS)
            {
                b->num_mons = MAX_MONS;
            }
            LOG(LOG_LEVEL_INFO, "adapter %s: %d monitor(s) at most",
                ops->name, b->num_mons);
            if (b->num_mons >= 1)
            {
                return 0;
            }
            ops->destroy(b->ad);
            b->ad = NULL;
            b->ops = NULL;
        }
    }
    if (want != NULL && want[0] != '\0')
    {
        LOG(LOG_LEVEL_ERROR, "adapter %s: not built in, or cannot drive "
            "this compositor", want);
    }
    else
    {
        LOG(LOG_LEVEL_ERROR, "no adapter can drive this compositor");
    }
    return 1;
}

/*****************************************************************************/
int
main(int argc, char **argv)
{
    struct be *b = &g_be;
    const char *sock_path = NULL;
    int opt;
    int i;

    while ((opt = getopt(argc, argv, "s:")) != -1)
    {
        if (opt == 's')
        {
            sock_path = optarg;
        }
    }
    if (sock_path == NULL)
    {
        fprintf(stderr, "usage: wlxrdp -s <socket path>\n");
        return 1;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    memset(b, 0, sizeof(*b));
    b->ev = &g_core_events;
    b->client_fd = -1;
    b->sent_visible = -1;
    for (i = 0; i < MAX_MONS; i++)
    {
        b->mons[i].b = b;
        b->mons[i].index = i;
        b->mons[i].core_last = -1;
    }
    b->in = malloc(IN_MAX);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    g_init("wlxrdp");
    log_start_from_param(log_config_init_for_console(
                             getenv("WLXRDP_DEBUG") ? LOG_LEVEL_DEBUG
                             : LOG_LEVEL_INFO, NULL));

    if (adapter_open(b) != 0)
    {
        return 1;
    }
    b->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    b->listen_fd = listen_unix(sock_path);
    if (b->listen_fd < 0)
    {
        LOG(LOG_LEVEL_ERROR, "cannot listen on %s: %s", sock_path,
            strerror(errno));
        return 1;
    }
    LOG(LOG_LEVEL_INFO, "listening on %s", sock_path);

    while (!g_term && !b->lost)
    {
        struct pollfd pfd[ADAPTER_FDS + 2];
        int timeout;
        int pace;
        int na;
        int n;
        int ls_i;
        int cl_i = -1;

        /* frames the client can take now; when the next is due */
        pace = core_pace(b);
        /* the adapter's fds, and when it wants waking */
        timeout = -1;
        na = b->ops->fds(b->ad, pfd, ADAPTER_FDS, &timeout);
        na = na < 0 ? 0 : na > ADAPTER_FDS ? ADAPTER_FDS : na;
        n = na;
        ls_i = n;
        pfd[n].fd = b->listen_fd;
        pfd[n].events = POLLIN;
        pfd[n++].revents = 0;
        if (b->client_fd >= 0)
        {
            cl_i = n;
            pfd[n].fd = b->client_fd;
            pfd[n].events = POLLIN;
            pfd[n++].revents = 0;
        }
        /* wake at least once a second, and when the frame interval next
           allows a capture */
        if (timeout < 0 || timeout > 1000)
        {
            timeout = 1000;
        }
        if (pace >= 0 && pace < timeout)
        {
            timeout = pace;
        }
        if (poll(pfd, n, timeout) < 0)
        {
            int err = errno;

            /* the adapter gets its fds back whatever happened */
            for (i = 0; i < n; i++)
            {
                pfd[i].revents = 0;
            }
            b->ops->dispatch(b->ad, pfd, na);
            if (err == EINTR)
            {
                continue;
            }
            break;
        }
        if (b->ops->dispatch(b->ad, pfd, na) < 0 || b->lost)
        {
            break;
        }
        if (core_helper_sync(b) != 0)
        {
            drop_client(b);
        }

        if (pfd[ls_i].revents & POLLIN)
        {
            int fd = accept4(b->listen_fd, NULL, NULL, SOCK_CLOEXEC);

            if (fd >= 0 && b->client_fd >= 0)
            {
                LOG(LOG_LEVEL_WARNING, "already serving a client, "
                    "replacing it");
                drop_client(b);
            }
            if (fd >= 0)
            {
                LOG(LOG_LEVEL_INFO, "xrdp connected");
                b->client_fd = fd;
                b->in_len = 0;
                continue;
            }
        }
        if (cl_i >= 0 && (pfd[cl_i].revents & (POLLIN | POLLHUP | POLLERR)))
        {
            if (read_xrdp(b) != 0)
            {
                drop_client(b);
            }
        }
        if (b->client_fd >= 0 && now_ms32() - b->stat_ms >= 5000)
        {
            static int last_sent;
            uint32_t now = now_ms32();

            if (b->frames_sent < last_sent)
            {
                last_sent = 0; /* a new connection */
            }
            if (b->frames_sent != last_sent)
            {
                LOG(LOG_LEVEL_INFO, "%.1f fps over %.1f s (%d frames total, "
                    "%d in flight, %d monitor(s))",
                    (b->frames_sent - last_sent) * 1000.0 /
                    (now - b->stat_ms), (now - b->stat_ms) / 1000.0,
                    b->frames_sent, b->frame_id - b->acked_id, b->active);
            }
            last_sent = b->frames_sent;
            b->stat_ms = now;
        }
    }
    if (b->client_fd >= 0)
    {
        drop_client(b);
    }
    b->ops->destroy(b->ad);
    unlink(sock_path);
    return 0;
}
