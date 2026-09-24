# The compositor contract

What an xrdp Wayland session needs from its compositor, and what it
promises in return. It covers the three session processes that talk to
the compositor (wlxrdp, chansrv, the session scripts) and the start-up and
shutdown handshake with sesexec. Read it before trying another compositor,
or before writing a wlxrdp adapter for one.

The reference is labwc 0.9.3 on wlroots 0.19 (desktop sessions) and
sway 1.11 (RemoteApp sessions). Anything else is untested.

## Processes

sesexec starts three children per session, as it does for Xorg:

| sesexec's name | Wayland session | Role |
|---|---|---|
| `win_mgr` | `startwayland.sh`, which `exec`s the compositor | Runs the desktop. The session lasts as long as this process does. |
| `x_server` | `wlxrdp` | The backend xrdp connects to over xup, as with xorgxrdp. |
| `chansrv` | `xrdp-chansrv` | Virtual channels: clipboard, drives, audio, RemoteApp. |

The compositor is the session leader. When it exits, sesexec ends the
session and stops the other two.

## Lifecycle

**Start.** sesexec runs `startwayland.sh` as the user with:

- `XRDP_WAYLAND_NAME_FILE`: where to report the display (below).
- `XRDP_WAYLAND_SIZE`: the client's size, `WxH`, for the first output.
- `XRDP_WAYLAND_REMOTEAPP=1` for a RemoteApp session.

`XDG_RUNTIME_DIR` is the user's. sesexec does not set `WAYLAND_DISPLAY`;
the compositor picks its own socket name.

**Readiness.** The session command the compositor runs (`waylandsession.sh`)
writes the name file once the compositor is ready. It writes a temporary
file, then renames it, so sesexec never reads a partial file. The format is:

```
/run/user/1000/wayland-1          <- line 1: the socket, absolute path
/run/user/1000/sway-ipc.1000.sock <- line 2 (optional): the compositor's IPC socket
```

Both lines must be plain paths: letters, digits, `/ - _ .` only. sesexec
waits up to 30 s for the file, and gives up early if the compositor exits.
It then deletes the file and starts wlxrdp and chansrv with
`WAYLAND_DISPLAY` set to the absolute path, so neither needs
`XDG_RUNTIME_DIR`. The socket's base name (`wayland-1`) becomes the
session's display string. Line 2 is exported to chansrv as `SWAYSOCK`.

**End.** To end a session, sesexec sends SIGTERM to the compositor only.
Reaping it then stops wlxrdp and chansrv. The compositor must:

- exit on SIGTERM, taking its clients with it;
- exit when its session command exits (labwc: `-S <command>`), so that
  logging out of the desktop ends the session.

A failed start (no name file, no environment, wlxrdp not running) is
rolled back: SIGTERM to each started child, then SIGKILL after 5 s.

**Backend restarts.** If wlxrdp exits while the compositor lives, sesexec
starts another on the same socket, at most 5 times a minute, then ends
the session. The compositor must accept new clients for its lifetime, and
drop a departed client's virtual input devices and capture sessions.

## Outputs

`startwayland.sh` runs the compositor headless:

```
WLR_BACKENDS=headless
WLR_HEADLESS_OUTPUTS=${XRDP_WAYLAND_MONITORS:-4}
WLR_LIBINPUT_NO_DEVICES=1
WLR_RENDERER=gles2
WLR_RENDER_DRM_DEVICE=${XRDP_WAYLAND_RENDER_NODE:-${XRDP_VAAPI_DEVICE:-/dev/dri/renderD128}}
```

The session starts with the first output at the client's size and the
others disabled (`waylandsession.sh` does this with `wlr-randr`). After
that, wlxrdp owns the layout.

- **Naming.** wlxrdp sorts outputs by name, numerically (`HEADLESS-2`
  before `HEADLESS-10`). Output *i* shows client monitor *i*.
- **Count.** The session can show as many client monitors as the
  compositor has outputs. There are no hot-plugged outputs; a larger
  client layout is cut to that count.
- **Layout.** For each client layout, wlxrdp sends one
  `zwlr_output_manager_v1` configuration. It enables and sizes the
  outputs in use (`set_custom_mode`), places them (`set_position`), scales
  them (`set_scale`), and disables the rest. Positions are in the
  compositor's logical coordinates: a monitor at 200% takes half its
  pixel size. If the compositor rejects the configuration, wlxrdp retries
  once with a fresh serial, then keeps the previous layout.
- **Budget.** Layouts over `XRDP_WAYLAND_MAX_PIXELS` (default 40 million
  pixels, all monitors together) are refused before reaching the
  compositor.

## Protocols

Bound by wlxrdp's wlroots adapter (`wla_wlr.c`), by chansrv, and by the
session scripts. "Required" means that component refuses to run without
it.

| Protocol | Version | Used by | Status | For |
|---|---|---|---|---|
| `wl_output` | 4 | wlxrdp | required (≥ 1 output) | output names |
| `wl_seat` | 1 | wlxrdp, chansrv | required | virtual devices; the clipboard's seat |
| `wl_shm` | 1 | wlxrdp | CPU path, cursor | shared-memory frames; cursor images |
| `ext_output_image_capture_source_manager_v1` | 1 | wlxrdp | required | a capture source per output |
| `ext_image_copy_capture_manager_v1` | 1 | wlxrdp | required | frame capture; cursor sessions |
| `zwp_linux_dmabuf_v1` | 3 | wlxrdp | required | dma-buf capture buffers |
| `zwlr_output_manager_v1` | 1–4 | wlxrdp, `wlr-randr` | required | the monitor layout |
| `zwlr_virtual_pointer_manager_v1` | 1 | wlxrdp | required | pointer input |
| `zwp_virtual_keyboard_manager_v1` | 1 | wlxrdp | required | keyboard input |
| `ext_data_control_manager_v1` | 1 | chansrv | optional | the clipboard; without it, no clipboard |
| sway IPC (i3 protocol) | — | chansrv | RemoteApp only | following and placing windows |

If any required protocol is missing, wlxrdp logs
`wlr: compositor lacks a required protocol` and exits.

### Capture

- **Sessions.** One `ext_image_copy_capture` session per enabled output,
  created with cursor painting off. The cursor goes to the client
  separately, as an RDP pointer.
- **Device.** On the GPU path wlxrdp allocates its buffers with GBM on
  the render node the capture session names (`dmabuf_device`), so the
  compositor can always write to them. `WLXRDP_DRM` overrides it; a
  compositor that names none gets the encoder's device
  (`XRDP_VAAPI_DEVICE`). The encoder imports the same buffers, so it must
  be on that GPU too. wlxrdp logs a warning naming both devices when it
  is not. The CPU path opens no device.
- **Formats.** On the GPU path wlxrdp takes only `DRM_FORMAT_XRGB8888`
  dma-bufs. It allocates them with GBM on the render node, preferring
  Intel Y-tiled, then X-tiled, then linear, then any other modifier the
  compositor offers. On the CPU path it takes `wl_shm` XRGB8888 or
  XBGR8888.
- **Buffers.** Three per output. Each capture goes into one the core has
  released, and marks the whole buffer damaged. The compositor must copy a full frame
  into it, not only what changed: AVC444's second pass reads the whole
  buffer.
- **Damage.** The compositor's reported damage goes to the encoder as the
  frame's changed region.
- **Pacing.** wlxrdp asks for the next frame only when the client has
  acknowledged enough of the previous ones. A frame the compositor holds
  back until something changes is the expected, idle case.
- **Failure.** If a session stops, wlxrdp restarts it after 1 s, up to 5
  times, then ends; sesexec then restarts the backend (above).
- **Cursor.** One pointer cursor session (`create_pointer_cursor_session`
  on the seat's pointer, into `wl_shm` buffers) supplies the cursor image
  and hotspot.

### Input

- **Keyboard.** wlxrdp builds an XKB keymap from the client's layout and
  loads it into its virtual keyboard (`keymap`, XKB v1, from a memfd). It
  then sends evdev key codes (`key`) and the client's lock state
  (`modifiers`). The compositor must use the virtual keyboard's keymap for
  that keyboard's events.
- **Pointer.** Positions go through `motion_absolute`, against the whole
  layout's extent in logical coordinates. The compositor must map that
  extent across all enabled outputs, as wlroots does. Buttons are evdev
  codes. Wheel notches use `axis_discrete` (15 surface units per notch,
  source `wheel`); smooth scrolling uses `axis` at the same rate. Every
  event ends with `frame`.
- **Not yet.** Touch and pen input (RDPEI) are not forwarded.

### Clipboard

chansrv's `clipboard_wl.c` holds an `ext_data_control` device on the seat.
It offers the client's formats as a data source and reads the
compositor's selection when it changes. It covers text, images (PNG, BMP)
and file lists (`text/uri-list`, served from the FUSE drive mount). The
compositor must send selection events to data-control clients whether or
not they have focus; that is the protocol's purpose.

### RemoteApp

A RemoteApp login starts sway instead of labwc (`sway-remoteapp.conf`:
floating windows, no borders, no desktop). chansrv needs sway's IPC socket
(line 2 of the name file), built with json-c. It uses:

- `SUBSCRIBE ["window"]`: window events, each followed by a fresh look
  at the tree.
- `GET_TREE`: window geometry, titles and state. chansrv also reads it
  every 500 ms, because a window that resizes itself sends no event. A
  window in the scratchpad is a minimised one.
- `RUN_COMMAND`:
  - `[con_id=N] focus`
  - `[con_id=N] resize set W H, move position X Y` (also maximise, at the
    output's size)
  - `[con_id=N] move scratchpad` (minimise)
  - `[con_id=N] scratchpad show, floating enable` (restore)
  - `[con_id=N] kill`
  - `exec sh <script>` (start the client's program)

Program arguments never reach sway's command parser. chansrv writes them,
shell-quoted, into a private temporary script, which deletes itself and
`exec`s the program.

labwc has no equivalent interface, so RemoteApp on another compositor
needs either sway's IPC or a new chansrv backend for that compositor's
window interface (for example `ext-foreign-toplevel-list` plus a control
protocol).

## Writing an adapter

wlxrdp's core (`wlxrdp.c`) is compositor-agnostic. Everything above under
Outputs, Capture and Input is the wlroots adapter's side of
[`wla.h`](wla.h). An adapter for another family (Mutter or KWin, through
PipeWire capture and libei input) implements `struct wla_ops` and keeps
these promises to the core:

- **`create`** connects, and returns NULL if this is not a compositor it
  drives. Adapters are tried in order, and `WLXRDP_ADAPTER` picks one.
- **`max_monitors` / `set_layout`** apply a layout in client pixels,
  including each monitor's scale in percent. The adapter converts to the
  compositor's coordinates. It returns non-zero if the layout is refused;
  the core then keeps the previous one.
- **`start(WLA_DMABUF | WLA_SHM)`** begins capture of the layout's
  monitors. The adapter announces each monitor's buffers with the
  `buffers` event (up to `WLA_MAX_BUFS`).
- **`want_frame`** means one frame can be taken now. A pull adapter
  captures one; a push adapter hands over the newest it holds. Each
  complete frame is reported with `frame`, and every buffer must hold a
  whole frame.
- **`release`** returns a buffer. The adapter reuses a buffer only after
  it has been released.
- **Input** comes in the layout's pixel coordinates, with evdev key and
  button codes. `keymap` arrives only if the adapter sets
  `WLA_CAP_SET_KEYMAP`.
- **`cursor`** events come only with `WLA_CAP_CURSOR`. Without it, the
  client shows its default pointer.
- **`fds` / `dispatch`** plug into the core's poll loop, with no threads.
  A negative return, or the `lost` event, ends wlxrdp; sesexec decides
  whether to restart it.

The session scripts are part of the contract too. A new compositor needs
its own branch in `startwayland.sh` (how to start it headless, how to make
it exit with the session command) and a way for `waylandsession.sh` to
set the first output's size.

## Known gaps

- **One GPU.** The compositor, the capture buffers and the encoder must
  share a GPU on the GPU path: the helper does not import frames across
  devices. wlxrdp warns when they differ.
- **Format.** Dma-buf capture is XRGB8888 only. A compositor that offers
  no XRGB8888 dma-buf cannot be captured on the GPU path; there is no
  automatic fallback to shared memory.
- **wlroots only.** `wla_wlr.c` is the one adapter so far. Wayfire (on
  wlroots 0.19+) should fit the same contract but is untested.
