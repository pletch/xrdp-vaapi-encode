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
 * RemoteApp ([MS-RDPERP]) for Wayland sessions, over sway's IPC.
 *
 * In an X session chansrv is the window manager (rail.c). A Wayland
 * compositor is its own window manager, and the standard protocols let no
 * other program see where windows are or move them; sway's IPC does both.
 * A RemoteApp session therefore runs sway (sway-remoteapp.conf: every
 * window floating, undecorated), and this module:
 *
 *   - follows the windows: sway's window events, and a periodic look at
 *     its tree (a window resizing itself sends no event), become the same
 *     window orders rail.c sends: create/update, title, show, destroy.
 *     A window in the scratchpad is a minimized one.
 *   - carries out the client's requests as sway commands: exec, activate
 *     (focus), move/resize, minimize (scratchpad), maximize (the output's
 *     size), restore, close (kill).
 *
 * The desktop itself is captured whole by wlxrdp, as xorgxrdp captures the
 * X screen; the client shows each window's part of it.
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <json-c/json.h>

#include "arch.h"
#include "parse.h"
#include "os_calls.h"
#include "string_calls.h"
#include "log.h"
#include "chansrv.h"
#include "rail_wl.h"

extern int g_rail_chan_id; /* in chansrv.c */

/* [MS-RDPERP] 2.2.2.1 orders, and 2.2.2.5.1 system commands */
#define TS_RAIL_ORDER_EXEC 0x0001
#define TS_RAIL_ORDER_ACTIVATE 0x0002
#define TS_RAIL_ORDER_SYSPARAM 0x0003
#define TS_RAIL_ORDER_SYSCOMMAND 0x0004
#define TS_RAIL_ORDER_HANDSHAKE 0x0005
#define TS_RAIL_ORDER_WINDOWMOVE 0x0008
#define SC_MINIMIZE 0xF020
#define SC_MAXIMIZE 0xF030
#define SC_CLOSE 0xF060
#define SC_RESTORE 0xF120
#define SPI_SET_WORK_AREA 0x0000002F

/* [MS-RDPERP] 2.2.1.3.1.2.1 show states */
#define SHOW_HIDE 0
#define SHOW_MINIMIZED 2
#define SHOW_MAXIMIZED 3
#define SHOW_NORMAL 5

/* rail.c's styles for an ordinary top-level window */
#define RAIL_STYLE_NORMAL (0x00C00000 | 0x00080000 | 0x00040000 | \
                           0x00010000 | 0x00020000)
#define RAIL_EXT_STYLE_NORMAL (0x00040000)

/* [MS-RDPEGDI] 2.2.1.3.1.2.1 fields, as rail.c uses them */
#define WINDOW_ORDER_TYPE_WINDOW 0x01000000
#define WINDOW_ORDER_STATE_NEW 0x10000000
#define WINDOW_ORDER_FIELD_OWNER 0x00000002
#define WINDOW_ORDER_FIELD_STYLE 0x00000008
#define WINDOW_ORDER_FIELD_SHOW 0x00000010
#define WINDOW_ORDER_FIELD_TITLE 0x00000004
#define WINDOW_ORDER_FIELD_CLIENT_AREA_OFFSET 0x00004000
#define WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE 0x00010000
#define WINDOW_ORDER_FIELD_ROOT_PARENT 0x00040000
#define WINDOW_ORDER_FIELD_WND_OFFSET 0x00000800
#define WINDOW_ORDER_FIELD_WND_CLIENT_DELTA 0x00008000
#define WINDOW_ORDER_FIELD_WND_SIZE 0x00000400
#define WINDOW_ORDER_FIELD_WND_RECTS 0x00000100
#define WINDOW_ORDER_FIELD_VIS_OFFSET 0x00001000
#define WINDOW_ORDER_FIELD_VISIBILITY 0x00000200
#define WINDOW_ORDER_FIELD_DESKTOP_HOOKED 0x00000002
#define WINDOW_ORDER_FIELD_DESKTOP_ARC_COMPLETED 0x00000004
#define WINDOW_ORDER_FIELD_DESKTOP_ARC_BEGAN 0x00000008

/* sway IPC ([sway-ipc(7)]) */
#define IPC_MAGIC "i3-ipc"
#define IPC_RUN_COMMAND 0
#define IPC_SUBSCRIBE 2
#define IPC_GET_TREE 4
#define IPC_EVENT_BIT 0x80000000u
#define IPC_MAX_PAYLOAD (16 * 1024 * 1024)

/* a window resizing itself sends no event: look again this often */
#define POLL_MS 500
#define MAX_WINDOWS 256

struct wl_win
{
    int id;                 /* sway's con id: the RAIL window id */
    int x;
    int y;
    int w;
    int h;
    int show;               /* SHOW_* sent */
    int title_crc;
    int seen;               /* in the latest tree */
    int max;                /* maximized by us: normal_* holds the rect */
    int normal_x;
    int normal_y;
    int normal_w;
    int normal_h;
};

static int g_up;
static int g_cmd_fd = -1;           /* commands and replies */
static int g_ev_fd = -1;            /* subscribed events */
static char *g_ev_buf;
static int g_ev_len;
static int g_client_ready;          /* sent SPI_SET_WORK_AREA */
static int g_handshake_sent;
static int g_desktop_sent;
static unsigned int g_init_ms;
/* the server's handshake waits this long for the client's channel to be
   open (chansrv hears of the channel before the client has joined it),
   unless the client's own handshake comes first */
#define HANDSHAKE_DELAY_MS 1000
static int g_dirty;                 /* an event since the last look */
static unsigned int g_last_sync_ms;
static struct wl_win g_wins[MAX_WINDOWS];
static int g_num_wins;
/* the output a maximized window fills */
static int g_out_x;
static int g_out_y;
static int g_out_w;
static int g_out_h;

/*****************************************************************************/
/* sway IPC */

static int
ipc_connect(const char *path)
{
    struct sockaddr_un sa;
    int fd;

    if (g_strlen(path) >= (int) sizeof(sa.sun_path))
    {
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        return -1;
    }
    g_memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    g_strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    if (connect(fd, (struct sockaddr *) &sa, sizeof(sa)) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static int
write_all(int fd, const char *p, int len)
{
    while (len > 0)
    {
        ssize_t n = write(fd, p, len);

        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n <= 0)
        {
            return 1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

static int
read_all(int fd, char *p, int len)
{
    while (len > 0)
    {
        ssize_t n = read(fd, p, len);

        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n <= 0)
        {
            return 1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

static int
ipc_send(int fd, uint32_t type, const char *payload)
{
    char hdr[14];
    uint32_t len = (uint32_t) g_strlen(payload);

    g_memcpy(hdr, IPC_MAGIC, 6);
    g_memcpy(hdr + 6, &len, 4);         /* host byte order, as sway reads */
    g_memcpy(hdr + 10, &type, 4);
    return write_all(fd, hdr, 14) || write_all(fd, payload, (int) len);
}

/* a reply on the command socket: its payload, NUL-terminated, to g_free */
static char *
ipc_reply(int fd, uint32_t want_type)
{
    char hdr[14];
    uint32_t len;
    uint32_t type;
    char *payload;

    if (read_all(fd, hdr, 14) != 0 || g_memcmp(hdr, IPC_MAGIC, 6) != 0)
    {
        return NULL;
    }
    g_memcpy(&len, hdr + 6, 4);
    g_memcpy(&type, hdr + 10, 4);
    if (len > IPC_MAX_PAYLOAD)
    {
        return NULL;
    }
    payload = g_new(char, len + 1);
    if (payload == NULL || read_all(fd, payload, (int) len) != 0 ||
            type != want_type)
    {
        g_free(payload);
        return NULL;
    }
    payload[len] = '\0';
    return payload;
}

/* run sway commands; logs a failure */
static int
sway_command(const char *cmd)
{
    char *reply;
    int ok = 0;

    LOG(LOG_LEVEL_DEBUG, "rail_wl: sway: %s", cmd);
    if (g_cmd_fd < 0 || ipc_send(g_cmd_fd, IPC_RUN_COMMAND, cmd) != 0 ||
            (reply = ipc_reply(g_cmd_fd, IPC_RUN_COMMAND)) == NULL)
    {
        LOG(LOG_LEVEL_ERROR, "rail_wl: sway IPC failed for '%s'", cmd);
        return 1;
    }
    ok = g_strstr(reply, "\"success\": false") == NULL &&
         g_strstr(reply, "\"success\":false") == NULL;
    if (!ok)
    {
        LOG(LOG_LEVEL_WARNING, "rail_wl: sway refused '%s': %s", cmd, reply);
    }
    g_free(reply);
    return ok ? 0 : 1;
}

/*****************************************************************************/
/* window orders, to xrdp (the formats rail.c sends) */

static int
string_crc(const char *text)
{
    /* FNV-1a: only compared with itself */
    uint32_t h = 2166136261u;

    for (; *text != '\0'; text++)
    {
        h = (h ^ (unsigned char) text[0]) * 16777619u;
    }
    return (int) h;
}

static void
send_window(struct wl_win *w, int is_new, const char *title)
{
    struct stream *s;
    int title_len = g_strlen(title);
    int flags;

    if (title_len > 0xFFFF)
    {
        title_len = 0xFFFF;
    }
    flags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_FIELD_OWNER |
            WINDOW_ORDER_FIELD_STYLE | WINDOW_ORDER_FIELD_SHOW |
            WINDOW_ORDER_FIELD_TITLE | WINDOW_ORDER_FIELD_CLIENT_AREA_OFFSET |
            WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE |
            WINDOW_ORDER_FIELD_ROOT_PARENT | WINDOW_ORDER_FIELD_WND_OFFSET |
            WINDOW_ORDER_FIELD_WND_CLIENT_DELTA | WINDOW_ORDER_FIELD_WND_SIZE |
            WINDOW_ORDER_FIELD_WND_RECTS | WINDOW_ORDER_FIELD_VIS_OFFSET |
            WINDOW_ORDER_FIELD_VISIBILITY;
    if (is_new)
    {
        flags |= WINDOW_ORDER_STATE_NEW;
    }
    make_stream(s);
    init_stream(s, title_len + 256);
    out_uint32_le(s, 2);                /* create/update window */
    out_uint32_le(s, w->id);
    out_uint32_le(s, 0);                /* owner */
    out_uint32_le(s, RAIL_STYLE_NORMAL);
    out_uint32_le(s, RAIL_EXT_STYLE_NORMAL);
    out_uint32_le(s, w->show);
    out_uint16_le(s, title_len);
    out_uint8a(s, title, title_len);
    /* client area offset: on the screen, like the window's ([MS-RDPERP]
       2.2.1.3.1.2.1); the client area is the whole window. FreeRDP places
       the visible region at VisibleOffset - (ClientOffset -
       WindowClientDelta), so 0 here shifted it by the window's offset. */
    out_uint32_le(s, w->x);
    out_uint32_le(s, w->y);
    out_uint32_le(s, w->w);             /* client area size */
    out_uint32_le(s, w->h);
    out_uint32_le(s, 0);                /* rp_content */
    out_uint32_le(s, 0);                /* root parent */
    out_uint32_le(s, w->x);             /* window offset */
    out_uint32_le(s, w->y);
    out_uint32_le(s, 0);                /* window-client delta */
    out_uint32_le(s, 0);
    out_uint32_le(s, w->w);             /* window size */
    out_uint32_le(s, w->h);
    out_uint16_le(s, 1);                /* window rects */
    out_uint16_le(s, 0);
    out_uint16_le(s, 0);
    out_uint16_le(s, w->w);
    out_uint16_le(s, w->h);
    out_uint32_le(s, w->x);             /* visible offset */
    out_uint32_le(s, w->y);
    out_uint16_le(s, 1);                /* visibility rects */
    out_uint16_le(s, 0);
    out_uint16_le(s, 0);
    out_uint16_le(s, w->w);
    out_uint16_le(s, w->h);
    out_uint32_le(s, flags);
    s_mark_end(s);
    send_rail_drawing_orders(s->data, (int) (s->end - s->data));
    free_stream(s);
}

static void
send_title(struct wl_win *w, const char *title)
{
    struct stream *s;
    int len = g_strlen(title);

    make_stream(s);
    init_stream(s, len + 64);
    out_uint32_le(s, 8);                /* update title */
    out_uint32_le(s, w->id);
    out_uint32_le(s, WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_FIELD_TITLE);
    out_uint32_le(s, len);
    out_uint8a(s, title, len);
    s_mark_end(s);
    send_rail_drawing_orders(s->data, (int) (s->end - s->data));
    free_stream(s);
}

static void
send_show(struct wl_win *w)
{
    struct stream *s;

    make_stream(s);
    init_stream(s, 64);
    out_uint32_le(s, 6);                /* show window */
    out_uint32_le(s, w->id);
    out_uint32_le(s, WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_FIELD_SHOW);
    out_uint32_le(s, w->show);
    s_mark_end(s);
    send_rail_drawing_orders(s->data, (int) (s->end - s->data));
    free_stream(s);
}

static void
send_destroy(int id)
{
    struct stream *s;

    make_stream(s);
    init_stream(s, 64);
    out_uint32_le(s, 4);                /* destroy window */
    out_uint32_le(s, id);
    s_mark_end(s);
    send_rail_drawing_orders(s->data, (int) (s->end - s->data));
    free_stream(s);
}

/* The Actively Monitored Desktop ([MS-RDPERP] 2.2.1.3.3): the desktop is
   hooked, then the arc of initial window orders begins and completes. A
   Windows host opens a RemoteApp session this way, and FreeRDP 3.31's X11
   client waits for ARC_COMPLETED before it starts the application. Our
   windows follow as ordinary window orders once the client is ready. */
static void
send_desktop(int flags)
{
    struct stream *s;

    make_stream(s);
    init_stream(s, 64);
    out_uint32_le(s, 12);               /* monitored desktop */
    out_uint32_le(s, flags);
    out_uint32_le(s, 0);                /* active window: none */
    out_uint32_le(s, 0);                /* no z-order */
    s_mark_end(s);
    send_rail_drawing_orders(s->data, (int) (s->end - s->data));
    free_stream(s);
}

static void
send_desktop_arc(void)
{
    if (g_desktop_sent)
    {
        return;
    }
    g_desktop_sent = 1;
    send_desktop(WINDOW_ORDER_FIELD_DESKTOP_HOOKED |
                 WINDOW_ORDER_FIELD_DESKTOP_ARC_BEGAN);
    send_desktop(WINDOW_ORDER_FIELD_DESKTOP_ARC_COMPLETED);
}

/*****************************************************************************/
/* the tree */

static struct wl_win *
find_win(int id)
{
    int i;

    for (i = 0; i < g_num_wins; i++)
    {
        if (g_wins[i].id == id)
        {
            return g_wins + i;
        }
    }
    return NULL;
}

static int
jint(struct json_object *o, const char *key, int dflt)
{
    struct json_object *v;

    return json_object_object_get_ex(o, key, &v) ? json_object_get_int(v)
           : dflt;
}

static const char *
jstr(struct json_object *o, const char *key)
{
    struct json_object *v;

    if (!json_object_object_get_ex(o, key, &v) ||
            !json_object_is_type(v, json_type_string))
    {
        return NULL;
    }
    return json_object_get_string(v);
}

/* One application window, as sway has it: bring the client up to date. */
static void
sync_window(struct json_object *node, int in_scratchpad)
{
    struct json_object *rect;
    struct wl_win *w;
    const char *title = jstr(node, "name");
    int id = jint(node, "id", 0);
    int x;
    int y;
    int width;
    int height;
    int show;
    int is_new = 0;

    if (id <= 0 || !json_object_object_get_ex(node, "rect", &rect))
    {
        return;
    }
    x = jint(rect, "x", 0);
    y = jint(rect, "y", 0);
    width = jint(rect, "width", 0);
    height = jint(rect, "height", 0);
    if (title == NULL || title[0] == '\0')
    {
        title = jstr(node, "app_id");
    }
    if (title == NULL)
    {
        title = "";
    }
    show = in_scratchpad ? SHOW_MINIMIZED : SHOW_NORMAL;
    w = find_win(id);
    if (w == NULL)
    {
        if (g_num_wins >= MAX_WINDOWS)
        {
            return;
        }
        w = g_wins + g_num_wins++;
        g_memset(w, 0, sizeof(*w));
        w->id = id;
        is_new = 1;
    }
    w->seen = 1;
    if (w->max)
    {
        show = in_scratchpad ? SHOW_MINIMIZED : SHOW_MAXIMIZED;
    }
    if (is_new || (!in_scratchpad &&
                   (x != w->x || y != w->y || width != w->w || height != w->h)))
    {
        /* a minimized window keeps its last place for the client */
        if (!in_scratchpad || is_new)
        {
            w->x = x;
            w->y = y;
            w->w = width;
            w->h = height;
        }
        w->show = show;
        w->title_crc = string_crc(title);
        send_window(w, is_new, title);
        return;
    }
    if (string_crc(title) != w->title_crc)
    {
        w->title_crc = string_crc(title);
        send_title(w, title);
    }
    if (show != w->show)
    {
        w->show = show;
        send_show(w);
    }
}

/* Walk a node's children and floating children for windows: nodes with
   neither, and a pid, are application windows. */
static void
walk(struct json_object *node, int in_scratchpad)
{
    static const char *const lists[] = { "nodes", "floating_nodes" };
    const char *type = jstr(node, "type");
    const char *name = jstr(node, "name");
    int children = 0;
    unsigned int l;

    if (type != NULL && g_strcmp(type, "workspace") == 0)
    {
        in_scratchpad = name != NULL && g_strcmp(name, "__i3_scratch") == 0;
    }
    if (type != NULL && g_strcmp(type, "output") == 0 &&
            (name == NULL || g_strcmp(name, "__i3") != 0))
    {
        struct json_object *rect;

        /* the first output: RemoteApp sessions have one */
        if (g_out_w == 0 && json_object_object_get_ex(node, "rect", &rect))
        {
            g_out_x = jint(rect, "x", 0);
            g_out_y = jint(rect, "y", 0);
            g_out_w = jint(rect, "width", 0);
            g_out_h = jint(rect, "height", 0);
        }
    }
    for (l = 0; l < 2; l++)
    {
        struct json_object *arr;
        size_t i;

        if (!json_object_object_get_ex(node, lists[l], &arr) ||
                !json_object_is_type(arr, json_type_array))
        {
            continue;
        }
        for (i = 0; i < json_object_array_length(arr); i++)
        {
            walk(json_object_array_get_idx(arr, i), in_scratchpad);
            children++;
        }
    }
    if (children == 0 && jint(node, "pid", 0) > 0 && type != NULL &&
            (g_strcmp(type, "con") == 0 ||
             g_strcmp(type, "floating_con") == 0))
    {
        sync_window(node, in_scratchpad);
    }
}

/* Bring the client's windows up to date with sway's tree. */
static void
sync_tree(void)
{
    struct json_object *tree;
    char *reply;
    int i;
    int n;

    g_dirty = 0;
    g_last_sync_ms = g_get_elapsed_ms();
    if (!g_client_ready || g_cmd_fd < 0)
    {
        return;
    }
    if (ipc_send(g_cmd_fd, IPC_GET_TREE, "") != 0 ||
            (reply = ipc_reply(g_cmd_fd, IPC_GET_TREE)) == NULL)
    {
        LOG(LOG_LEVEL_ERROR, "rail_wl: sway IPC failed reading the tree");
        return;
    }
    tree = json_tokener_parse(reply);
    g_free(reply);
    if (tree == NULL)
    {
        return;
    }
    for (i = 0; i < g_num_wins; i++)
    {
        g_wins[i].seen = 0;
    }
    g_out_w = 0;
    walk(tree, 0);
    json_object_put(tree);
    /* gone from the tree: gone from the client */
    for (i = 0, n = 0; i < g_num_wins; i++)
    {
        if (!g_wins[i].seen)
        {
            send_destroy(g_wins[i].id);
            continue;
        }
        g_wins[n++] = g_wins[i];
    }
    g_num_wins = n;
}

/*****************************************************************************/
/* the client's requests */

static void
send_handshake(void)
{
    struct stream *s;

    if (g_handshake_sent)
    {
        return;
    }
    g_handshake_sent = 1;
    LOG(LOG_LEVEL_INFO, "rail_wl: sending the server handshake");

    make_stream(s);
    init_stream(s, 64);
    out_uint16_le(s, TS_RAIL_ORDER_HANDSHAKE);
    out_uint16_le(s, 8);                /* orderLength */
    out_uint32_le(s, 1);                /* buildNumber */
    s_mark_end(s);
    send_channel_data(g_rail_chan_id, s->data, (int) (s->end - s->data));
    free_stream(s);
}

static char *
read_uni(struct stream *s, unsigned int num_bytes)
{
    unsigned int num_words = num_bytes / 2;
    unsigned int utf8len;
    char *rv;

    if (!s_check_rem(s, num_bytes))
    {
        return NULL;
    }
    utf8len = in_utf16_le_fixed_as_utf8_length(s, num_words);
    rv = g_new(char, utf8len + 1);
    if (rv != NULL)
    {
        rv[utf8len] = '\0';
        in_utf16_le_fixed_as_utf8(s, num_words, rv, utf8len);
        if ((num_bytes % 2) != 0)
        {
            in_uint8s(s, 1);
        }
    }
    return rv;
}

/* [MS-RDPERP] 2.2.2.3.1: run the program, with its arguments, via sway */
static void
process_exec(struct stream *s)
{
    unsigned int flags;
    unsigned int exe_len;
    unsigned int dir_len;
    unsigned int args_len;
    char *exe;
    char *dir;
    char *args;

    if (!s_check_rem(s, 8))
    {
        return;
    }
    in_uint16_le(s, flags);
    in_uint16_le(s, exe_len);
    in_uint16_le(s, dir_len);
    in_uint16_le(s, args_len);
    (void) flags;
    if (exe_len == 0 || exe_len > 520 || dir_len > 520 || args_len > 16000)
    {
        LOG(LOG_LEVEL_ERROR, "rail_wl: bad exec lengths %u %u %u",
            exe_len, dir_len, args_len);
        return;
    }
    exe = read_uni(s, exe_len);
    dir = read_uni(s, dir_len);
    args = read_uni(s, args_len);
    if (exe != NULL && dir != NULL && args != NULL)
    {
        /* the program single-quoted; the arguments as a command line, as
           Windows passes them */
        int size = 16 + g_strlen(exe) * 4 + g_strlen(args);
        char *cmd = g_new(char, size);
        char *p = cmd;
        const char *q;

        if (cmd != NULL)
        {
            p += g_snprintf(p, size, "exec '");
            for (q = exe; *q != '\0'; q++)
            {
                if (*q == '\'')
                {
                    g_strcpy(p, "'\\''");
                    p += 4;
                }
                else
                {
                    *p++ = *q;
                }
            }
            *p++ = '\'';
            if (args[0] != '\0')
            {
                *p++ = ' ';
                g_strcpy(p, args);
                p += g_strlen(args);
            }
            *p = '\0';
            LOG(LOG_LEVEL_INFO, "rail_wl: the client runs [%s] [%s]", exe,
                args);
            sway_command(cmd);
            g_free(cmd);
        }
    }
    g_free(exe);
    g_free(dir);
    g_free(args);
    g_client_ready = 1; /* the client is up, whatever it sent before */
    g_dirty = 1;
}

static void
process_activate(struct stream *s)
{
    int id;
    int enabled;
    char cmd[64];

    if (!s_check_rem(s, 5))
    {
        return;
    }
    in_uint32_le(s, id);
    in_uint8(s, enabled);
    if (enabled && find_win(id) != NULL)
    {
        g_snprintf(cmd, sizeof(cmd), "[con_id=%d] focus", id);
        sway_command(cmd);
    }
}

static void
move_resize(int id, int x, int y, int w, int h)
{
    char cmd[128];

    /* resize first: sway resizes a floating window about its centre */
    g_snprintf(cmd, sizeof(cmd), "[con_id=%d] resize set %d %d, "
               "move position %d %d", id, w, h, x, y);
    sway_command(cmd);
}

static void
process_syscommand(struct stream *s)
{
    struct wl_win *w;
    int id;
    int command;
    char cmd[96];

    if (!s_check_rem(s, 6))
    {
        return;
    }
    in_uint32_le(s, id);
    in_uint16_le(s, command);
    w = find_win(id);
    if (w == NULL)
    {
        return;
    }
    switch (command)
    {
        case SC_MINIMIZE:
            g_snprintf(cmd, sizeof(cmd), "[con_id=%d] move scratchpad", id);
            sway_command(cmd);
            break;
        case SC_MAXIMIZE:
            if (!w->max && g_out_w > 0)
            {
                w->normal_x = w->x;
                w->normal_y = w->y;
                w->normal_w = w->w;
                w->normal_h = w->h;
                w->max = 1;
                move_resize(id, g_out_x, g_out_y, g_out_w, g_out_h);
            }
            break;
        case SC_RESTORE:
            if (w->show == SHOW_MINIMIZED)
            {
                /* out of the scratchpad, floating where it was */
                g_snprintf(cmd, sizeof(cmd), "[con_id=%d] scratchpad show, "
                           "floating enable", id);
                sway_command(cmd);
                if (!w->max)
                {
                    move_resize(id, w->x, w->y, w->w, w->h);
                }
            }
            else if (w->max)
            {
                w->max = 0;
                move_resize(id, w->normal_x, w->normal_y,
                            w->normal_w, w->normal_h);
            }
            break;
        case SC_CLOSE:
            g_snprintf(cmd, sizeof(cmd), "[con_id=%d] kill", id);
            sway_command(cmd);
            break;
        default:
            break;
    }
    g_dirty = 1;
}

static void
process_window_move(struct stream *s)
{
    struct wl_win *w;
    int id;
    tsi16 left;
    tsi16 top;
    tsi16 right;
    tsi16 bottom;

    if (!s_check_rem(s, 12))
    {
        return;
    }
    in_uint32_le(s, id);
    in_uint16_le(s, left);
    in_uint16_le(s, top);
    in_uint16_le(s, right);
    in_uint16_le(s, bottom);
    w = find_win(id);
    if (w == NULL || right <= left || bottom <= top)
    {
        return;
    }
    w->max = 0;
    move_resize(id, left, top, right - left, bottom - top);
    g_dirty = 1;
}

/*****************************************************************************/
int
rail_wl_enabled(void)
{
    return g_getenv("SWAYSOCK") != NULL && g_getenv("WAYLAND_DISPLAY") != NULL;
}

/*****************************************************************************/
int
rail_wl_init(void)
{
    const char *path = g_getenv("SWAYSOCK");
    char *reply;

    if (g_up)
    {
        return 0;
    }
    g_cmd_fd = ipc_connect(path);
    g_ev_fd = ipc_connect(path);
    if (g_cmd_fd < 0 || g_ev_fd < 0 ||
            ipc_send(g_ev_fd, IPC_SUBSCRIBE, "[\"window\"]") != 0 ||
            (reply = ipc_reply(g_ev_fd, IPC_SUBSCRIBE)) == NULL)
    {
        LOG(LOG_LEVEL_ERROR, "rail_wl: cannot reach sway's IPC at %s: "
            "RemoteApp windows will not be managed", path);
        rail_wl_deinit();
        return 1;
    }
    g_free(reply);
    fcntl(g_ev_fd, F_SETFL, O_NONBLOCK);
    g_ev_buf = g_new(char, 65536);
    g_ev_len = 0;
    g_num_wins = 0;
    g_client_ready = 0;
    g_handshake_sent = 0;
    g_desktop_sent = 0;
    g_init_ms = g_get_elapsed_ms();
    g_up = 1;
    LOG(LOG_LEVEL_INFO, "rail_wl: RemoteApp over sway's IPC (%s)", path);
    /* [MS-RDPERP] 3.2.5.1: the server opens with its handshake, once the
       client can hear it (rail_wl_check_wait_objs) */
    return 0;
}

/*****************************************************************************/
int
rail_wl_deinit(void)
{
    if (g_cmd_fd >= 0)
    {
        close(g_cmd_fd);
        g_cmd_fd = -1;
    }
    if (g_ev_fd >= 0)
    {
        close(g_ev_fd);
        g_ev_fd = -1;
    }
    g_free(g_ev_buf);
    g_ev_buf = NULL;
    g_num_wins = 0;
    g_up = 0;
    return 0;
}

/*****************************************************************************/
int
rail_wl_data_in(struct stream *s, int chan_id, int chan_flags, int length,
                int total_length)
{
    int code;
    int size;

    if (!g_up || !s_check_rem(s, 4))
    {
        return 0;
    }
    in_uint16_le(s, code);
    in_uint16_le(s, size);
    (void) size;
    LOG(LOG_LEVEL_DEBUG, "rail_wl: client order 0x%4.4x", code);
    switch (code)
    {
        case TS_RAIL_ORDER_HANDSHAKE:
            send_handshake(); /* if the client went first: answer */
            send_desktop_arc();
            break;
        case TS_RAIL_ORDER_SYSPARAM:
            if (s_check_rem(s, 4))
            {
                int param;

                in_uint32_le(s, param);
                if (param == SPI_SET_WORK_AREA)
                {
                    LOG(LOG_LEVEL_INFO, "rail_wl: the client is ready "
                        "for windows");
                    /* the client is ready for windows (as in rail.c) */
                    g_client_ready = 1;
                    g_num_wins = 0; /* all new to this client */
                    g_dirty = 1;
                }
            }
            break;
        case TS_RAIL_ORDER_EXEC:
            process_exec(s);
            break;
        case TS_RAIL_ORDER_ACTIVATE:
            process_activate(s);
            break;
        case TS_RAIL_ORDER_SYSCOMMAND:
            process_syscommand(s);
            break;
        case TS_RAIL_ORDER_WINDOWMOVE:
            process_window_move(s);
            break;
        default:
            break;
    }
    if (g_dirty)
    {
        sync_tree();
    }
    return 0;
}

/*****************************************************************************/
int
rail_wl_get_wait_objs(tbus *objs, int *count, int *timeout)
{
    if (!g_up)
    {
        return 0;
    }
    objs[(*count)++] = g_ev_fd;
    if ((g_client_ready || !g_handshake_sent) &&
            (*timeout < 0 || *timeout > POLL_MS))
    {
        *timeout = POLL_MS;
    }
    return 0;
}

/*****************************************************************************/
/* events: any window event means a new look at the tree */
static void
read_events(void)
{
    for (;;)
    {
        ssize_t n = read(g_ev_fd, g_ev_buf + g_ev_len, 65536 - g_ev_len);

        if (n > 0)
        {
            g_ev_len += n;
        }
        else if (n == 0 || (errno != EAGAIN && errno != EINTR))
        {
            LOG(LOG_LEVEL_ERROR, "rail_wl: sway's IPC closed");
            rail_wl_deinit();
            return;
        }
        /* drop whole messages; only that they came matters */
        while (g_ev_len >= 14)
        {
            uint32_t len;

            g_memcpy(&len, g_ev_buf + 6, 4);
            if (g_memcmp(g_ev_buf, IPC_MAGIC, 6) != 0 ||
                    len > 65536 - 14)
            {
                /* a huge event (a window with an enormous title): skip it
                   by resynchronising on the next message */
                g_ev_len = 0;
                g_dirty = 1;
                break;
            }
            if (g_ev_len < 14 + (int) len)
            {
                break;
            }
            g_memmove(g_ev_buf, g_ev_buf + 14 + len, g_ev_len - 14 - len);
            g_ev_len -= 14 + len;
            g_dirty = 1;
        }
        if (n <= 0)
        {
            return;
        }
    }
}

/*****************************************************************************/
int
rail_wl_check_wait_objs(void)
{
    struct pollfd pfd;

    if (!g_up)
    {
        return 0;
    }
    pfd.fd = g_ev_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) > 0)
    {
        read_events();
        if (!g_up)
        {
            return 0;
        }
    }
    if (!g_handshake_sent &&
            g_get_elapsed_ms() - g_init_ms >= HANDSHAKE_DELAY_MS)
    {
        send_handshake();
    }
    if (g_client_ready &&
            (g_dirty || g_get_elapsed_ms() - g_last_sync_ms >= POLL_MS))
    {
        sync_tree();
    }
    return 0;
}
