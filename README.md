[![Build Status](https://github.com/neutrinolabs/xrdp/actions/workflows/build.yml/badge.svg)](https://github.com/neutrinolabs/xrdp/actions)
[![Gitter](https://badges.gitter.im/Join%20Chat.svg)](https://gitter.im/neutrinolabs/xrdp-questions)
![Apache-License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)

[![Latest Version](https://img.shields.io/github/v/release/neutrinolabs/xrdp.svg?label=Latest%20Version)](https://github.com/neutrinolabs/xrdp/releases)

# xrdp - an open source RDP server

## This branch: VA-API encoding and Wayland sessions

`feature/wayland` is this fork's VA-API branch (`feature/vaapi-accel-assist`)
plus experimental **Wayland sessions**:

* **[Wayland sessions](#fork-wayland-sessions-experimental)** - log in to a
  desktop on a Wayland compositor (labwc) instead of Xorg, through the new
  `wlxrdp` backend: GPU-encoded AVC420/AVC444 via the same helper, multi-monitor,
  the client's display scale, clipboard with images and files, drives, audio,
  and RemoteApp (over sway). Built with `--enable-wayland`; Xorg sessions are
  unchanged.
* **[VA-API hardware H.264 encoding](#fork-intel-vaapi-hardware-h264-encoding-ffmpeg-free)**
  with AVC444 - the `xrdp_accel_assist` encoder both session types use; on
  NVIDIA, the same helper encodes with NVENC, AVC444 included.

The VA-API work alone is on `feature/vaapi-accel-assist`.

## Fork: Wayland sessions (experimental)

This branch (`feature/wayland`) adds **Wayland sessions** on top of the VA-API
encoder described below: log in and get a desktop running on a Wayland
compositor instead of Xorg, encoded by the same `xrdp_accel_assist` helper
(AVC420 or AVC444; VA-API, or NVENC on NVIDIA). It is experimental and off unless built with
`--enable-wayland`; Xorg sessions are unchanged.

**Pipeline:** a headless [labwc](https://labwc.github.io/) compositor (wlroots)
runs the desktop (XFCE, or any session you choose). `wlxrdp`, a new backend,
plays xorgxrdp's part on the xup socket: it captures each output with
`ext-image-copy-capture` into GBM dma-bufs and hands them to accel-assist in its
headless mode (`-w`), which runs the same RGB->NV12 / AVC444 shaders and encoder
(VA-API, or NVENC on NVIDIA) as for Xorg. Input goes back through the compositor's virtual keyboard
and pointer. X11 applications run under Xwayland.

```
labwc output --ext-image-copy-capture--> GBM dma-buf --> xrdp_accel_assist -w
  (shader, VA-API or NVENC) --> xrdp --> client
client input --> xrdp --> wlxrdp --> zwp_virtual_keyboard / zwlr_virtual_pointer --> labwc
```

### What works

| feature | notes |
| ------- | ----- |
| GPU encoding | AVC420 and AVC444 through accel-assist, as for Xorg (`XRDP_USE_ACCEL_ASSIST=1`) |
| CPU encoding | for clients or hosts the helper cannot serve: x264 H.264, RemoteFX, plain bitmaps (xrdp encodes) |
| multi-monitor | each client monitor is a compositor output and a GFX surface; dynamic resize and layout changes |
| display scale | follows the client's own scale: mstsc on a 200% laptop gets scale 2, per monitor with multiple monitors |
| keyboard | the client's layout as an xkb keymap; characters it lacks (Unicode input) are typed through spare keys |
| pointer | cursor shapes as RDP pointers; high-resolution wheel and touchpad scrolling |
| clipboard | text, images and files, both ways; files into the session need `--enable-fuse` |
| drive redirection | through FUSE (`--enable-fuse`), as for Xorg |
| audio | the PulseAudio/PipeWire xrdp modules, as for Xorg |
| RemoteApp | over sway (below) |
| reconnect | to the running session; `reconnectwm.sh` runs with `WAYLAND_DISPLAY` set |

Not yet: multitouch (xrdp has no MS-RDPEI; a client may still turn touch into
mouse input on its own), GNOME (Mutter) or KDE (KWin) sessions, and Wayfire (its
0.11 release has the capture protocol but needs wlroots 0.20).

Known issue (XFCE 4.20 on labwc): rebooting or shutting down from XFCE's
logout dialog asks for a password, but the prompt takes no typing. The
logout dialog seems to keep the keyboard, and cancelling ends the session,
since XFCE has already begun to close it. `xfce4-session-logout --reboot`
from a terminal, which skips the dialog, or `sudo systemctl reboot`, works.

**Compositor adapters.** wlxrdp's core knows RDP (the xup protocol, pacing, the
helper, the keyboard); an adapter (`wlxrdp/wla.h`) knows one family of
compositors. `wla_wlr.c` drives wlroots compositors through standard protocols:
output management, image copy capture, virtual keyboard and pointer. Other
families (Mutter, KWin: PipeWire capture, libei input) would be further adapters.
[`wlxrdp/COMPOSITOR.md`](wlxrdp/COMPOSITOR.md) is the compositor contract: the
protocols and versions each part needs, the start-up and shutdown handshake with
sesexec, and what a new adapter must provide.

**RemoteApp.** labwc offers no way for another program to follow and place
windows, so a RemoteApp login gets a separate session on
[sway](https://swaywm.org/) (`sway-remoteapp.conf`, no desktop), whose IPC
chansrv uses to map windows to RAIL window orders and carry out the client's
moves, resizes and activations. The RAIL fixes that came with it (below) apply
to Xorg RemoteApp sessions too.

### Requirements

Tested on Ubuntu 26.04 with labwc 0.9.3 (wlroots 0.19), sway 1.11, XFCE 4.20 and
wayland-protocols 1.47. Needed to build: `wayland-client`, `wayland-protocols`
>= 1.39 (`ext-image-copy-capture`, `ext-data-control`), `wayland-scanner`, `gbm`,
`libdrm`, `xkbcommon`; optional: `json-c` (RemoteApp), `libpng` (PNG clipboard
images), `libfuse3` (files and drives), `libxcursor` (a scaled pointer where
the cursor can't be captured; see NVIDIA below). At run time: `labwc`, `wlr-randr`, and
`sway` for RemoteApp. The compositor must offer `ext-image-copy-capture`; otherwise
wlxrdp refuses to start and logs the protocols it needs.

### Building / enabling

```
./configure --enable-rfxcodec --enable-x264 --enable-vaapi --enable-wayland --enable-fuse
```

Uncomment the `[Wayland]` section in `xrdp.ini` (it is installed commented out),
and choose it at the login screen. A client can pick it without the login
screen by giving the section name as the domain: in mstsc, user name
`Wayland\you`. Windows saves one set of credentials per host name, so to keep
both an Xorg and a Wayland shortcut, reach the server by two names (host name
and IP address). A RemoteApp connection to the `[Wayland]` section gets the sway
session.

The GPU path uses the same `[SessionVariables]` as Xorg (`XRDP_USE_ACCEL_ASSIST=1`,
the AVC444 and QP settings in **Tuning** below). Wayland adds:

| variable | default | effect |
| -------- | ------- | ------ |
| `XRDP_WAYLAND_MONITORS` | 4 | client monitors a session can show (headless outputs, all but the first off until used) |
| `XRDP_WAYLAND_SCALE` | auto | `auto`: the client's scale, else 200% on a monitor over 2000 pixels wide; `client`: the client's, else 100%; or a number for every monitor (`150` or `1.5`) |
| `XRDP_WAYLAND_MAX_PIXELS` | 40000000 | all monitors together, in pixels; a larger layout is refused (1024x768 at connect, the current layout on a resize). Each pixel costs 12 bytes of capture buffers |
| `XRDP_WAYLAND_PRIVATE_BUS` | off | `1` gives the desktop its own D-Bus session bus (no keyring); by default it uses the user's, unless another desktop session is already on it |
| `XRDP_WAYLAND_RENDER_NODE` | `XRDP_VAAPI_DEVICE`, else `/dev/dri/renderD128` | the compositor's GPU. The GPU path needs it to be the encoder's (`XRDP_VAAPI_DEVICE`); wlxrdp warns if they differ |
| `WLXRDP_ACCEL` | on | `0` takes the CPU path (xrdp encodes) |
| `WLXRDP_AVC420` | unset | set: AVC420 even for AVC444 clients (half the decode work; see the mstsc known issue) |
| `WLXRDP_DRM` | the compositor's device | overrides the GBM device for the capture buffers |
| `WLXRDP_PAINT_CURSOR` | unset | `1`: where the cursor can't be captured, the compositor paints it into the frames (the session's shapes, but moving at the frame rate) instead of the client drawing the theme's arrow |
| `WLXRDP_DEBUG` | unset | debug logging |

A user's own desktop command goes in `~/.config/xrdp/waylandsession`
(executable); without it the session runs `xfce4-session`, or a terminal. The
session scripts are `startwayland.sh` (the compositor) and `waylandsession.sh`
(the desktop) in `/etc/xrdp`. wlxrdp logs to the journal (as `xrdp-sesman`, in
the session's scope).

### Changes outside Wayland

The Wayland work needed a few fixes that apply to every session:

* **RemoteApp with FreeRDP 3.31 and whole windows.** FreeRDP 3.31 starts a
  RemoteApp session only after an Actively Monitored Desktop order with
  `ARC_COMPLETED`, and waits for the server's handshake first; chansrv now sends
  both. RAIL window orders carry the client area offset in screen coordinates
  (0,0 made FreeRDP paint windows only from twice their offset), and a window the
  client moves is reported back with its new offsets.
* **The client's display scale** (`desktopScaleFactor` in the core data, sent by
  mstsc for a single monitor) is read and passed to backends; xorgxrdp ignores it.
* **chansrv without an X display** no longer crashes on a RemoteApp handshake or
  the X11 clipboard.
* **accel-assist** keeps up to four capture buffers per monitor (bits 4-5 of the
  frame flags); xorgxrdp's two are unchanged.

## Fork: Intel VAAPI hardware H.264 encoding (ffmpeg-free)

This is a fork of xrdp that adds an **Intel VA-API hardware H.264 encoder** to the
`xrdp_accel_assist` helper - the ffmpeg-free GPU encoding path suggested by jsorg71
in [neutrinolabs/xrdp#3774](https://github.com/neutrinolabs/xrdp/pull/3774). It fills
the previously-empty `ENC_VA` encoder slot, so EGFX H.264 frames are produced by the
GPU's video-encode engine instead of software libx264.

**Pipeline:** glamor screen pixmap, bound through the upstream X pixmap handshake ->
GL RGB->NV12 shader -> `eglExportDMABUFImageMESA` (dma-buf) -> import as a VA NV12 surface
(DRM PRIME, zero-copy) -> `VAEntrypointEncSliceLP`, or `EncSlice` where the driver has no
low-power entry point, H.264. The dma-buf is the shader's output, inside the helper;
xorgxrdp passes none. Mesa implements the pixmap binding with DRI3, which shares the
screen's buffer underneath, but that is the graphics stack's business, not xrdp's.
SPS/PPS/slice headers are generated directly - no ffmpeg, no libx264.

### What changed
* `xrdp_accel_assist/xrdp_accel_assist_vaapi.{c,h}`: the VA-API encoder (new)
* `xrdp_accel_assist/xrdp_accel_assist_x11.c`: wire the `ENC_VA` vtable + NV12 layout
* `configure.ac`, `xrdp_accel_assist/Makefile.am`: `--enable-vaapi` build option

**Stream tuning** (later improvements):
* H.264 **High profile + 8x8 integer transform** in the packed SPS/PPS - ~5-10 % better
  compression at the same QP, especially on the smooth solid-color regions and text
  that dominate a typical desktop.
* `glFlush()` instead of `glFinish()` at the encode entry - the dma-buf shared between
  Mesa GL and iHD VAAPI carries implicit DRM_PRIME synchronisation, so the kernel
  sequencer already enforces GL-before-VAAPI ordering on the GPU itself. The hard
  CPU-side stall was double-syncing; the next frame's GL work can now overlap with
  this frame's encode.
* **Metablock rect alignment** in `out_RFX_AVC420_METABLOCK` - the -1/+1 chroma
  margin expansion could produce odd-width rects, which FreeRDP's AVC SSE primitive
  asserts on and crashes the client (and mstsc tolerates but mis-renders as stripes
  during P-frame updates). Every rect is now rounded outward to a per-codec grid,
  2x2 for AVC420 and more for the two AVC444 layouts - see **AVC444** below.
* **Hardware-active log line** in `xrdp.log` on the first compressed frame received
  from accel-assist:
  ```
  gfx_wiretosurface1: AVC420 hardware encoding active (accel-assist), first compressed frame received: N bytes
  ```
  Single signal in the daemon log that the VAAPI path is doing the work, without
  having to grep the per-display `xrdp-accel-assist.NN.log`.

### Building / enabling

**Install the companion xorgxrdp fork as well,**
[pletch/xorgxrdp-glamor-gbm](https://github.com/pletch/xorgxrdp-glamor-gbm)
(branch `feature/gbm-dmabuf-hwencode`). Upstream xorgxrdp is not enough for most of
what this branch does, and where it falls short, it does so quietly:

| you want | with upstream xorgxrdp |
| -------- | ---------------------- |
| AVC444 | never negotiated: the session runs AVC420 with no error, even for an AVC444 client |
| Intel `xe` kernel driver (the default from Lunar Lake and Battlemage on) | glamor does not start, so there is no GPU path ([neutrinolabs/xorgxrdp#423](https://github.com/neutrinolabs/xorgxrdp/pull/423)) |
| NVIDIA | no RandR 1.5 and no start size: the XFCE desktop stays at 640x480 or the first client's size (see **Which setup for which GPU** below) |
| AVC420 on `i915` or AMD | not tested here; use the fork |

```
./configure --enable-rfxcodec --enable-x264 --enable-vaapi   # needs libva-dev, libva-drm-dev
```

and in the xorgxrdp fork:

```
git clone -b feature/gbm-dmabuf-hwencode https://github.com/pletch/xorgxrdp-glamor-gbm
cd xorgxrdp-glamor-gbm && ./bootstrap && ./configure --enable-glamor && make && sudo make install
```

Enable per session in `sesman.ini` `[SessionVariables]` with `XRDP_USE_ACCEL_ASSIST=1`
(constant-QP override via `XRDP_VAAPI_QP`; see **Tuning** below).

The encoder probes a preference list of H.264 profile/entrypoint pairs at startup and
uses the first one the driver offers, so it is not tied to a single generation:

| pair | typical hardware |
| ---- | ---------------- |
| `High` / `EncSliceLP` | Intel iHD, Gen9+ (Skylake -> Arc) - the primary target |
| `High` / `EncSlice`   | AMD radeonsi, other drivers with the regular entrypoint |
| `Main` / `EncSliceLP` | iHD parts without High-profile low-power encode |
| `Main` / `EncSlice`   | legacy Intel i965 (Haswell/Broadwell era) |

Main drops the 8x8 integer transform (slightly larger output at the same QP); the
packed SPS/PPS follow the chosen profile automatically. A pair is preferred only if it
also advertises constant-QP rate control and application-supplied packed headers.
Baseline is not in the list - the stream uses CABAC. The chosen pair is logged:

```
vaapi_init: using H.264 High/EncSliceLP (profile_idc 100, 8x8 transform on)
```

The DRM node defaults to `/dev/dri/renderD128`; on a multi-GPU host set
`XRDP_VAAPI_DEVICE` (and xorgxrdp's matching `DRMDevice` / `XORGXRDP_DRM_DEVICE`) to
the same GPU.

Why the xorgxrdp fork is needed: it carries the xorgxrdp side of the AVC444
negotiation, which has to match this branch (xorgxrdp decides from the client info
xrdp sends whether AVC444 is in effect, and tells the helper), and FlyGoat's
glamor/DRI3 fixes that make glamor work on the Intel `xe` kernel driver
([neutrinolabs/xorgxrdp#423](https://github.com/neutrinolabs/xorgxrdp/pull/423)).

### Installing from source: layout and common snags

These notes come from bringing the fork up on a fresh Xubuntu machine. Package
names are Debian/Ubuntu; Fedora has the same libraries as `-devel` packages.

**Remove the distro packages first** (`sudo apt remove xrdp xorgxrdp`). Their
binaries, service files and config would otherwise shadow or clash with the
source build.

**Dependencies**, for both xrdp and the xorgxrdp fork:

```
sudo apt install build-essential git autoconf automake libtool pkg-config nasm \
  libssl-dev libpam0g-dev libx11-dev libxfixes-dev libxrandr-dev libjpeg-dev \
  libfuse3-dev libx264-dev libopus-dev libpixman-1-dev systemd-dev \
  xserver-xorg-dev libdrm-dev libgbm-dev libepoxy-dev libegl-dev libva-dev
```

`systemd-dev` is easy to miss: without it configure silently skips the unit
files, and `systemctl enable xrdp` then fails with *Unit xrdp.service does not
exist*. On older releases, where `systemd.pc` is in `libsystemd-dev`, or to be
explicit, pass `--with-systemdsystemunitdir=/usr/lib/systemd/system`.

**Where things install.** Programs go under the prefix, `/usr/local` by
default; configuration does not:

| what | where |
| ---- | ----- |
| `xrdp`, `xrdp-sesman` | `/usr/local/sbin` |
| `xrdp-accel-assist`, `xrdp-sesexec`, ... | `/usr/local/libexec/xrdp` |
| `xrdp.ini`, `sesman.ini`, `startwm.sh` | `/etc/xrdp` (configure always uses `/etc`) |
| xorgxrdp modules | the Xorg module directory, e.g. `/usr/lib/xorg/modules` |
| `xorg.conf`, `xorg_nvidia.conf` | `/etc/X11/xrdp` |

**`make install` overwrites the configuration.** Every xrdp install replaces
`/etc/xrdp/*.ini` and `startwm.sh`, and every xorgxrdp install replaces the
files in `/etc/X11/xrdp`. Keep your edits somewhere else and reapply them, or
put them in a script:

* copy the Xorg config to a name the install doesn't know (for example
  `xorg_nvidia_local.conf`) and point `sesman.ini` at the copy;
* when only the helper changed, install only the helper, which leaves
  `/etc/xrdp` alone: `sudo make -C xrdp_accel_assist install`.

**`sesman.ini` changes needed on Debian/Ubuntu:**

* `[Xorg]` `param=Xorg` runs the `Xorg.wrap` wrapper, which refuses users who
  are not at the console (*Only console users are allowed to run the X
  server*). Point it at the server itself: `param=/usr/lib/xorg/Xorg`.
* The GPU path is opt-in: add `XRDP_USE_ACCEL_ASSIST=1` under
  `[SessionVariables]`. xrdp must also be built with x264 or OpenH264, or it
  never selects H.264 and the GPU encoder goes unused (fork issue #3).
* `sesman.ini` is read when a session starts. After a change, **log out** of
  the session; a disconnect and reconnect keeps the old settings.

**Same user logged in at the console.** If you also use the machine locally,
the xrdp session shares the user's D-Bus session bus. `xfce4-session` then finds
a session manager already running and exits at once (*Session failed
immediately*, *window manager exited quickly*). Give the xrdp session its own
bus with a `~/startwm.sh`, which sesman runs in place of `/etc/xrdp/startwm.sh`:

```sh
#!/bin/sh
unset DBUS_SESSION_BUS_ADDRESS
exec dbus-run-session -- startxfce4
```

**Intel `xe` in a VM with GPU passthrough: turn off runtime power
management.** Otherwise the card can be suspended while the session is using
it, and sessions turn sluggish or freeze. A udev rule keeps it powered:

```
ACTION=="add|bind", SUBSYSTEM=="pci", DRIVER=="xe", ATTR{power/control}="on"
```

**Which setup for which GPU:**

| | Intel / AMD (VA-API) | NVIDIA (NVENC) |
| --- | --- | --- |
| xrdp configure | `--enable-vaapi` | `--enable-nvenc` (the SDK header is bundled; `libnvidia-encode` is loaded at run time) |
| xorgxrdp configure | `--enable-glamor` | `--enable-glamor --enable-lrandr` |
| Xorg config | `xrdp/xorg.conf` (xrdpdev) | a copy of `xrdp/xorg_nvidia.conf` with your `BusID` |
| AVC420 / AVC444 | both | both (AVC444 needs two long-term reference frames, which the helper checks for) |
| Wayland sessions (`feature/wayland`) | GPU encoding | GPU encoding (NVENC; see below) |

For NVIDIA:

* The default `xorg.conf` does not work: its `DRMAllowList` leaves out
  `nvidia-drm`, so xrdpdev falls back to software rendering and the GPU path
  never starts.
* `xorg_nvidia.conf` ships with `BusID "PCI:3:0:0"`. Take yours from
  `nvidia-smi` (Bus-Id `00000000:01:00.0` is `PCI:1:0:0`; the fields are
  decimal). A wrong one gives *No devices detected*, then a fatal error about
  `/dev/tty0`.
* Without `--enable-lrandr`, the NVIDIA driver exposes no RandR outputs and the
  session fails with *waitforx: Unable to find any RandR outputs*.
* The screen starts at 640x480 and is resized to the client, and resized
  again when a client of another size reconnects. Two fixes in the xorgxrdp
  fork keep the desktop in step: the client's size
  (`XRDP_START_WIDTH`/`HEIGHT`) is applied at startup, as xrdpdev does, and
  the local RandR (`--enable-lrandr`) implements RandR 1.5.
  libxfce4windowing, which xfdesktop 4.20 uses, needs 1.5; below it,
  xfdesktop takes the screen size once and never follows a resize. With
  upstream xorgxrdp the wallpaper stays in a 640x480 corner, or at the first
  client's size, with window trails across the rest. The workaround there is
  to restart xfdesktop from the reconnect script, and to wait for the
  resize in `~/startwm.sh` before the `exec`:

  ```sh
  i=0
  while [ $i -lt 50 ] && xdpyinfo 2>/dev/null | grep -q "dimensions: *640x480"; do
      sleep 0.1; i=$((i+1))
  done
  ```

Wayland sessions on NVIDIA (`--enable-nvenc --enable-wayland`):

* The helper picks NVENC by the render node's driver, and takes the GL
  texture as on Xorg. labwc and its capture work on the proprietary driver
  (tested with 595 and labwc 0.9.3).
* Keep labwc on its OpenGL renderer (the default). With `WLR_RENDERER=vulkan`
  it gives a captured cursor, but doesn't report all the damage: parts of the
  screen stay stale until something redraws them.
* On OpenGL, NVIDIA's driver reads pixels back only as BGR888, with no alpha:
  the CPU path widens it, but the cursor can't be captured. wlxrdp then sends
  the cursor theme's arrow at the session's scale (with `libxcursor`), which
  the client draws: it moves at once, but stays an arrow. `WLXRDP_PAINT_CURSOR=1`
  trades that for the real shapes, painted into the frames.

**Checking that the GPU is doing the work:**

* `sudo grep "hardware encoding active" /var/log/xrdp.log` (the log needs
  root). The line appears on the first GPU-encoded frame.
* The helper's own log: `~/.local/share/xrdp/xrdp-accel-assist.<display>.log`.
* `intel_gpu_top` (the Video engine) or `nvidia-smi dmon -s u` (the `enc`
  column) while something on screen is moving.

**Error messages and their usual causes:**

| message | cause |
| ------- | ----- |
| *Unit xrdp-sesman.service does not exist* | `systemd-dev` missing at configure time |
| *Only console users are allowed to run the X server* | `param=Xorg` in `sesman.ini`; use `/usr/lib/xorg/Xorg` |
| *X server could not be started*, no `~/.xorgxrdp.*.log` | as above, or a `make install` reset `sesman.ini` |
| *Session failed immediately* / *window manager exited quickly* | the same user logged in locally: separate D-Bus bus |
| sessions sluggish or frozen (Intel `xe`, VM passthrough) | GPU runtime-suspended: set `power/control=on` (udev rule above) |
| *waitforx: Unable to find any RandR outputs* | NVIDIA driver without xorgxrdp `--enable-lrandr` |
| *No devices detected*, then `/dev/tty0` fatal | wrong `BusID` in the NVIDIA Xorg config |
| wallpaper only in a 640x480 corner, or doesn't follow a reconnect at another size | NVIDIA path with upstream xorgxrdp (RandR 1.3): update the xorgxrdp fork, or restart xfdesktop |
| AVC444 client, but the session runs AVC420 | upstream xorgxrdp instead of the fork (no *(AVC444 ... negotiated)* line in `~/.xorgxrdp.*.log`), a client that doesn't advertise AVC444 (FreeRDP without `/gfx:AVC444`), or `XRDP_ACCEL_AVC444=0` |
| no *hardware encoding active* line | `XRDP_USE_ACCEL_ASSIST` unset, no x264/OpenH264 in the build, or (NVIDIA) the xrdpdev `xorg.conf` |
| bash: *!dev: event not found* when pasting a command | history expansion on `!` inside double quotes; use single quotes |

### AVC444: true 4:4:4 chroma

The encoder also implements **AVC444** (MS-RDPEGFX `RDPGFX_CODECID_AVC444` /
`AVC444v2`), which carries the chroma that 4:2:0 throws away in a second H.264
picture. The client reassembles both into a YUV444 frame. This is what keeps
subpixel-antialiased text crisp - 4:2:0 smears colour across adjacent pixels and
turns ClearType fringes muddy.

Both pictures are **one H.264 sequence**, not two streams. FreeRDP hands the same
decoder context to both views, so they must interleave as a single sequence of
pictures sharing one DPB. Encoding them as two independent sequences produces
persistent corruption that looks like a chroma bug but is not.

* **Capability negotiation**: xrdp derives AVC444 support from the client's
  advertised EGFX capability set (MS-RDPEGFX 2.2.3) and falls back to AVC420 on
  its own, so `XRDP_ACCEL_AVC444=1` is safe to leave enabled for mixed clients.
  AVC444 also needs the xorgxrdp fork, which passes the negotiated result to the
  helper; with upstream xorgxrdp the session stays on AVC420. The fork logs the
  outcome in `~/.xorgxrdp.<display>.log`: *rdpSendAccelAssistMonitors:
  capabilities ... (AVC444 v2 negotiated)*, or *not negotiated*. The client has
  to advertise it as well: current mstsc does by default, FreeRDP with
  `/gfx:AVC444`.
  The confirmed capability version also picks the chroma layout: 10.2 and later
  get v2, a bare 10.0 client is held to v1. There is no capability flag
  separating the two layouts - the whole set is `THINCLIENT`, `SMALL_CACHE`,
  `AVC420_ENABLED`, `AVC_DISABLED`, `AVC_THINCLIENT`, `SCALEDMAP_DISABLE` - so
  the version is the only signal and the choice is finally the server's.
* **Long-term reference frames, one chain per view**: each view predicts only
  from its own previous picture, held long-term on its own `LongTermFrameIdx`
  (main 0, auxiliary 1), and every slice names that picture with a
  `ref_pic_list_modification` instead of inheriting the default reference list.
  Neither view disturbs the other's prediction chain and neither needs a periodic
  IDR to resynchronise - the periodic IDR described under *Known issue* below
  is for clients' decoders, not the encoder. The slice header is written directly, because iHD will
  predict from a long-term reference but does not emit the
  `dec_ref_pic_marking()` MMCO that tells the decoder about it - and it ignores
  reordering syntax when it generates the header itself, so the header has to be
  ours for the naming to reach the client at all. The first auxiliary picture of
  a sequence has no chain of its own yet and is coded intra, which also keeps the
  two chains disjoint from the first picture of each.

* **A downstream proxy can discard the auxiliary view and leave the main view
  decodable.** This is what the per-view chains are for. A client that paints 4:2:0
  anyway - a browser that will not recombine the two views - otherwise pays for the
  chroma half twice, in bandwidth and in a second decode, and then throws it away.
  Two properties make shedding it safe, and both are in the bitstream:

  1. **Nothing surviving can refer to a dropped picture.** Each slice activates one
     list-0 entry and names it by `long_term_pic_num`, so a main macroblock has no
     second index to reach the auxiliary view with, and no inferred picture can be
     named either.
  2. **The stream declares a reference slot for the pictures the dropper removes.**
     Dropping leaves holes in `frame_num`, so the proxy must set
     `gaps_in_frame_num_value_allowed_flag`, which obliges the decoder to infer a
     frame per hole and hold it as a *short-term* reference (8.2.5.2). The sliding
     window (8.2.5.3) evicts only short-term pictures, so with both slots long-term
     under a ceiling of two there is nothing to evict and nowhere to put the
     inferred frame - the decoder fails outright rather than degrading. Under
     dual-LTR `max_num_ref_frames` is therefore 3, with `max_dec_frame_buffering`
     tracking it as E.2.1 requires. The third slot is declared and never filled;
     the encoder's own DPB is unchanged, and a client that does not drop pays
     nothing for it.

  Verified on a 2992x1648 capture: every frame of the stream with the auxiliary
  views removed is bit-identical to the corresponding main-view frame of the full
  stream. Note that a hardware decoder will paint straight through a broken
  reference chain without reporting an error - a deliberately corrupted control
  stream decoded clean on Chrome's hardware path - so "it looks right" is not
  evidence here; comparing decoded pictures is.
* **Damage detection**: an application that repaints without changing anything
  (a hover redraw, a blinking caret's whole line) still reports damage. The helper
  compares each damaged 16x16 cell with the source as last encoded, on the GPU (a
  per-pixel mask, then a per-cell maximum, then one small readback), and passes on
  only the cells that changed; a frame where nothing did is not sent at all. About
  1 ms a frame. `XRDP_AVC444_DAMAGE_DETECT=0` turns it off, for diagnosis only.
* **The auxiliary picture goes only where a change needs it.** The same pass
  marks the cells whose chroma is off by more than `XRDP_AVC444_AUX_THRESHOLD` (30,
  the threshold MS-RDPEGFX gives the client's reverse filter) from the 2x2 mean the
  main view carries. Grey and flat content goes without; coloured detail and
  subpixel-antialiased text get it on the frame they change. Frames without it
  carry `LC=1` (luma only).
* **Moving areas get it once they settle.** A cell that changes again within
  150 ms is moving (video, scrolling, a drag): it goes luma-only and owes the
  auxiliary picture, which it gets once it has been still for 120 ms. Motion is
  judged over the 3x3 cells around each one: inside a video, a cell that changes
  only now and then is moving too, and none settles until the area has (on a test
  video, 60% fewer auxiliary bytes). Typing stays a run of one-off changes and
  gets 4:4:4 at once. When captures stop altogether,
  the capture side sends one more 150 ms later (xorgxrdp and wlxrdp both, as a
  16x16 corner) so the owed cells go out instead of waiting for the next change.
  This replaced a fixed interval (every fourth frame, plus a 200 ms deadline),
  which both cost more and, because the deadline was only checked on the next
  frame, could leave a one-off change at 4:2:0 indefinitely: the hover flicker.
  Measured at 1728x1024 v2 on FreeRDP 3.32, against the interval of 4: video 4.40
  against 5.03 Mbit/s, coloured scrolling 2.80 against 3.18, with 4:4:4 back within
  0.3-0.5 s of motion stopping, and no change in frame rate. Without damage
  detection the auxiliary picture goes with every frame.
* **Packed shaders**: the RGB->NV12 conversion writes four destination bytes per
  fragment as RGBA8 over a quarter-width viewport, for both the main and auxiliary
  views.
* **The v2 auxiliary view is rendered over the damage**, not the whole frame,
  accumulating a bounding box between the frames that carry it and falling back
  to full-frame on an IDR or once the box passes half the picture. Measured 2.5x
  less GL time on desktop content; no effect during video, where the damage
  covers most of the frame anyway. It does not change the bitstream - both views
  encode a full picture and the encoder skips unchanged macroblocks - so the
  auxiliary picture's size still follows the age of its reference rather than how
  much was drawn.

* **The metablocks declare the damage rects**, not a single full-frame rect. A view's
  metablock is what the client copies out of that view's decoded picture, so a
  full-frame declaration turns every picture into a full-plane copy on the client
  regardless of how little changed. Each view rounds its rects outward to its own
  grid, because the two chroma layouts address differently:

  | view | grid (x by y) | why |
  | ---- | ------------ | --- |
  | AVC420 | 2 x 2 | 4:2:0 chroma is half resolution on both axes |
  | AVC444 v1 | 4 x 16 | `ChromaV1ToYUV444` walks the B4/B5 tiles *relative to the rect*, while the packing shader anchors its 16-row tiling at frame row 0; the two coincide only when the rect's top is a multiple of 16 |
  | AVC444 v2 | 4 x 2 | `ChromaV2ToYUV444` is absolutely addressed and needs only an even top, but its `roi->left / 4` addressing and 4x+0 / 4x+2 destination phases need left on a multiple of 4 |

  An unaligned v1 top mis-maps the whole of B4/B5 - every column of the odd chroma
  rows - which reads as horizontal banding wherever the screen changed. The
  `aa444map` round-trip harness puts 99.2% of B4/B5 samples wrong at MAE 85.3 for a
  top of 200, and bit-exact at 192 or 208. `pixman`'s union only cuts bands at
  coordinates present in its inputs, so a region built from aligned rects stays
  aligned. This is metadata only - both views still encode a full picture - so it
  changes what the client copies, not the bitrate or the quality.

* **The auxiliary view declares the rects the helper actually rendered.** After
  motion the auxiliary picture carries cells that changed during the luma-only
  frames before it, which are not the current frame's rects; declaring only those
  would leave the settled cells holding their odd-row chroma from the last
  auxiliary frame. accel-assist has those rects - they are what the shader pass
  was scissored to - and appends them to the shared-memory payload after
  `[len1][stream1][len2][stream2]`: the changed cells as a `RECT` list (both
  views), the auxiliary cells as an `AUXR` list, and failing those the rendered
  box as a 20-byte trailer. The trailer is optional in both directions, so a
  mismatched pair of binaries falls back to declaring the whole frame rather than to
  stale chroma. Measured on a 2992x1648 desktop at interval 8, the auxiliary view
  went from a full-plane copy every time (29.3 ms a picture) to 37% of the plane
  (12.6 ms), with client throughput rising from 30.4 to 40.8 fps.

Note that the v1 auxiliary view is rendered full-frame, and the v2 view's accumulated
box grows to the whole picture often enough, so either way the auxiliary pass needs a
*complete* source pixmap rather than only the current damage. That constrains the
capture side - see the xorgxrdp fork's README.

* **Chroma is stored as the 2x2 mean**, not the even/even sample. The auxiliary
  view carries three of every four chroma samples; the fourth is never sent, and
  a decoder recovers it as `4*mean - the other three` (FreeRDP does this in
  YUV444 to RGB, `prim_YUV.c`). Storing the sample makes that recovery produce a
  value unrelated to anything wherever the four differ - coloured speckling on
  icon and glyph edges, invisible on flat colour.
* **Level derived from the picture.** `level_idc` was fixed at 4.1, whose maximum
  frame size is 8192 macroblocks: fine for 1920x944 (7080), wrong for 2688x1488
  (15624) or 3008x2000 (23500). A decoder that honours the declaration refuses
  the hardware path and falls back to software, which under AVC444 is two
  oversized pictures per frame on the CPU. The level now follows the size - 4.2,
  5.1 and 5.2 respectively. A client that hardcodes its own decoder level has to
  match; parse it from the SPS rather than assuming.
* **Full-range colour is signalled.** The shaders convert with full-range BT.709,
  so samples span 0-255. Without `video_full_range_flag` a decoder assumes
  limited range and expands 16-235 to 0-255, leaving black at 16 and white at
  235. The VUI now carries it along with BT.709 primaries, transfer and matrix.

Note that AVC444's advantage is largest at 1x rendering. On a HiDPI client sending
physical pixels, 4:2:0's 2x2 chroma block covers a single logical pixel, so much of
what 4:4:4 buys is already recovered by the extra resolution - and the second
picture per frame is often not worth its decode cost there.

#### Known issue: mstsc with Intel hardware decoding

When mstsc - the Windows Remote Desktop client - decodes AVC444 on an **Intel
GPU**, it can intermittently combine the two views the wrong way round: patches
of the screen turn pink and green, and windows appear ghosted at double size. It
shows up under load, typically within minutes of video playback on an affected
client, and without an IDR it persists until the client reconnects.

The stream is not the cause. It decodes bit-exactly in ffmpeg, the same
configuration stayed clean on an NVIDIA client, and on the affected Intel Iris
Xe client it disappeared entirely once the client's hardware decoding was
switched off. Microsoft's own hosts are reported to trigger the same symptom in
mstsc.

**What xrdp does about it.** The corruption clears at the next IDR, so a
synchronised IDR goes out on both views periodically: at most every
`XRDP_AVC444_IDR_MS` (default 10 s), riding on the next frame, and only once
`XRDP_AVC444_IDR_MIN_KB` (default 100) has been sent since the last one. A
glitch therefore lasts about one period at most. It costs ~5% during video and
nothing on a desktop that merely ticks.

**Workarounds**, if even a brief glitch is unacceptable:

* **Turn off hardware decoding on the client.** This avoids the fault entirely,
  for a few percent of CPU. Set the policy *Do not allow hardware accelerated
  decoding* (Computer Configuration > Administrative Templates > Windows
  Components > Remote Desktop Services > Remote Desktop Connection Client) to
  Enabled. On Windows Home, which has no Group Policy editor, create the DWORD
  `EnableHardwareMode` = `0` under
  `HKLM\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services\Client`.
  Either way, restart mstsc afterwards.
* **Shorten the period on the server.** A smaller `XRDP_AVC444_IDR_MS` makes each
  glitch shorter, at proportionally more bandwidth.
* **Fall back to AVC420 on the server** with `XRDP_ACCEL_AVC444=0`. With only
  one view there is nothing to combine wrongly; it's the equivalent of
  Microsoft's own workaround for its hosts, which is to disable AVC444. Not
  tested here, and it gives up 4:4:4 chroma for every client.

### Tuning

All of these are `[SessionVariables]` in `sesman.ini`, documented there as well:

| variable | default | effect |
| -------- | ------- | ------ |
| `XRDP_USE_ACCEL_ASSIST` | off | required for any of the below |
| `XRDP_ACCEL_AVC444` | negotiated | `0` forces AVC420; otherwise follows what the client advertised |
| `XRDP_AVC444_AUX_THRESHOLD` | 30 | chroma error (0-255) beyond which a change gets the auxiliary picture |
| `XRDP_AVC444_IDR_MS` | 10000 | synchronised IDR on both views at most this often; `0` off |
| `XRDP_AVC444_IDR_MIN_KB` | 100 | ...and only after this much has been sent since the last; `0` purely timed |
| `XRDP_AVC444_IDR_PERIOD` | unset | overrides the two above with an IDR every Nth frame, ungated (A/B testing) |
| `XRDP_VAAPI_QP` / `_AUX_QP` | 28 / 18 | constant quantiser, 1-51; 26 is a reasonable desktop value for main |
| `XRDP_VAAPI_BITRATE` | 0 (CQP) | kbit/s; switches to VBR |
| `XRDP_NVENC_QP` / `XRDP_NVENC_AUX_QP` | 28 / 18 | NVENC constant quantiser (with `XRDP_NVENC_RATE_CONTROL_MODE=NV_ENC_PARAMS_RC_CONSTQP`); the aux QP applies to AVC444's chroma view |
| `XRDP_NVENC_DUMP_STREAM` | unset | `<prefix>`: write the NVENC stream to `<prefix>.h264` and a per-picture index, for offline checks |
| `XRDP_SOUND_MAX_LATENCY_MS` | 0 | drop audio above this measured latency |

These are read by the **xrdp process itself**, so they go in an
`xrdp.service` systemd drop-in (`Environment=...`), not in `sesman.ini` --
`[SessionVariables]` only reaches the session (Xorg, xorgxrdp, accel-assist)
and never xrdp:

| variable | default | effect |
| -------- | ------- | ------ |
| `XRDP_GFX_FRAME_LOG` | off | one log line per encoded frame |
| `XRDP_GFX_FRAMES_IN_FLIGHT` | 2 | encoder queue depth |

These are read by xorgxrdp rather than the encoder, and are documented in the
[xorgxrdp fork](https://github.com/pletch/xorgxrdp-glamor-gbm):

| variable | default | effect |
| -------- | ------- | ------ |
| `XORGXRDP_ADAPTIVE_PACE` | off | steer the capture interval per client |
| `XORGXRDP_PACE_MIN_MS` / `_MAX_MS` | 20 / 100 | bounds for the above |
| `XORGXRDP_CAPTURE_DEPTH` | 1 | 2 captures ahead of the acknowledgement |
| `XORGXRDP_VFREQ` | 50 | refresh rate the virtual output advertises |

Together, adaptive pacing and capture depth 2 took a native client from 26 to
51 fps on this host. The pacing loop needs the client round trip xrdp measures,
which is why `mod_frame_ack` carries it down to the module.

`h264_frame_interval` in `xrdp.ini` `[Xorg]` is the minimum spacing between
captures in milliseconds, and matters more than it looks: too high aliases against
the content's own frame rate and judders, too low floods a client that cannot
decode as fast as the server encodes. Match it to the content - 33 for 30 fps.

LibreOffice's default gtk3 plugin is expensive under glamor: one instance being
typed into can take ~44 % of the render engine, and a few of them stall every
session on the GPU. `SAL_USE_VCLPLUGIN=qt6` (or `gen`) in the session
environment brings it to 0.2-1 %
([xorgxrdp#438](https://github.com/neutrinolabs/xorgxrdp/issues/438)).

### Benchmark: VAAPI hardware vs software x264

Server-side CPU for the full encode pipeline (Xorg capture/convert + xrdp +
xrdp-accel-assist) over an identical 40 s workload - 1920x1080 @ 30 fps full-motion
video, Intel UHD 770 (iHD 26.1.2), hardware CQP 28 vs xrdp's default x264:

| process                          | software (x264) | hardware (VAAPI) |
| -------------------------------- | --------------: | ---------------: |
| xrdp - H.264 encode              |         8.31 s  |  0.55 s (relay)  |
| Xorg - capture + color convert   |         5.92 s  |  1.32 s          |
| xrdp-accel-assist - VAAPI encode |            -    |  0.95 s          |
| **total CPU-seconds / 40 s**     |     **14.23 s** |   **2.82 s**     |
| **% of one CPU core**            |      **35.6 %** |    **7.0 %**     |

**~80 % less server CPU (about 5x lighter)** for the same 1080p30 motion. Two costs
move off the CPU: the encoder itself (libx264 -> GPU video engine) and RGB->NV12 color
conversion (CPU -> GPU shader). The gap widens with resolution, frame rate, and the
number of concurrent sessions. *Comparison is default-config, not equal-bitrate (HW uses
CQP 28; software x264 uses xrdp's defaults).*

*The benchmark predates the High-profile / 8x8-transform / `glFlush` tweaks above; the
current build should be at least as efficient (slightly less CPU and ~5-10 % smaller
H.264 output at the same QP), but no fresh end-to-end measurement has been taken.*

### Capacity: many sessions on one GPU

A tester's run on an Intel Arc Pro B50 (`xe`, Debian 13 VM, GPU passed through;
[details](https://github.com/pletch/xrdp-vaapi-encode/issues/2)), with real mstsc
clients at 1920x1080, AVC444, each "video" session playing 1080p30 H.264
full-screen in Firefox with VA-API decoding:

| load | fps per session | video engines | render |
| ---- | --------------: | ------------: | -----: |
| 8 video sessions | 30.5 | 40 % | 20 % |
| 16 video sessions | 31.3 | 75 % | 24 % |
| 20 video sessions | 29.8 | 99 % | 26 % |
| 24 video sessions | 21.3 | 99 % | - |
| 36 mixed: 10 video + 26 office-like browser pages | video 32.8, office ~18 | 95 % | 32 % |

The video engines are the limit, at about 20 full-screen video sessions; a
video session costs ~5 % of them (encoder and Firefox's decoder together, the
encoder about two thirds) and an office session ~1.5 %. Render and CPU (12-36 % of the VM) had
headroom, and at 36 sessions memory (~0.8 GB per session) runs out before the
GPU does. An office page's ~18 fps is its own update rate.

AVC420 (`XRDP_ACCEL_AVC444=0`) is only about 20 % cheaper to encode, not half:
these figures were taken with the auxiliary view on every fourth frame (about
1.25 pictures a frame); it now goes out during video only where the picture
settles, which costs less again.
It moves the full-screen video limit from about 20 sessions to about 23, while
the decoding share stays the same - a knob for servers that are truly
video-bound, not a reason to give up 4:4:4 text.

## Overview

**xrdp** provides a graphical login to remote machines using Microsoft
Remote Desktop Protocol (RDP). xrdp accepts connections from a variety of RDP clients:
  * FreeRDP
  * rdesktop
  * KRDC
  * NeutrinoRDP
  * Windows MSTSC (Microsoft Terminal Services Client, aka `mstsc.exe`)
  * Microsoft Remote Desktop (found on Microsoft Store, which is distinct from MSTSC)

Many of these work on some or all of Windows, Mac OS, iOS, and/or Android.

RDP transport is encrypted using TLS by default.

![Modern xrdp login followed by a successful connection to an Xfce desktop](docs/images/lvgl-demo.gif)

The demo shows the [optional LVGL login interface](docs/lvgl.md).
The legacy login remains the default.

## Features

### Remote Desktop Access

 * Connect to a Linux desktop using RDP from anywhere (requires
   [xorgxrdp](https://github.com/neutrinolabs/xorgxrdp) Xorg module)
 * Reconnect to an existing session
 * Session resizing (both on-connect and on-the-fly)
 * RDP/VNC proxy (connect to another RDP/VNC server via xrdp)

### Access to Remote Resources
 * Two-way clipboard transfer (text, bitmap, file)
 * Audio redirection ([requires to build additional modules](https://github.com/neutrinolabs/xrdp/wiki/How-to-set-up-audio-redirection))
 * Microphone redirection ([requires to build additional modules](https://github.com/neutrinolabs/xrdp/wiki/How-to-set-up-audio-redirection))
 * Drive redirection (mount local client drives on remote machine)

## Supported Platforms

**xrdp** primarily targets GNU/Linux operating system. x86 (including x86-64)
and ARM processors are most mature architecture to run xrdp on.
See also [Platform Support Tier](https://github.com/neutrinolabs/xrdp/wiki/Platform-Support-Tier).

Some components such as xorgxrdp and RemoteFX codec have special optimization
for x86 using SIMD instructions. So running xrdp on x86 processors will get
fully accelerated experience.

## Quick Start

Most Linux distributions should distribute the latest release of xrdp in their
repository. You would need xrdp and xorgxrdp packages for the best
experience. It is recommended that xrdp depends on xorgxrdp, so it should
be sufficient to install xrdp. If xorgxrdp is not provided, use Xvnc
server.

xrdp listens on 3389/tcp. Make sure your firewall accepts connection to
3389/tcp from where you want to access.

### Ubuntu / Debian
```bash
apt install xrdp
```

### Fedora, RHEL and derivatives

If you're not running Fedora, make sure to enable EPEL packages first.

```bash
dnf install epel-release
```

(All systems) Install xrdp with:-

```bash
dnf install xrdp
```

## Compiling

See also https://github.com/neutrinolabs/xrdp/wiki#building-from-sources

### Prerequisites

To compile xrdp from the packaged sources, you need basic build tools - a
compiler (**gcc** or **clang**) and the **make** program.  Additionally,
you would need **openssl-devel**, **pam-devel**, **libX11-devel**,
**libXfixes-devel**, **libXrandr-devel**. More additional software would
be needed depending on your configuration.

To compile xrdp from a checked out git repository, you would additionally
need **autoconf**, **automake**, **libtool** and **pkg-config**.

### Get the source and build it

If compiling from the packaged source, unpack the tarball and change to the
resulting directory.

If compiling from a checked out repository, please make sure you've got the submodules
cloned too (use `git clone --recursive https://github.com/neutrinolabs/xrdp`)

Then run following commands to compile and install xrdp:
```bash
./bootstrap
./configure
make
sudo make install
```

If you want to use audio redirection, you need to build and install additional
pulseaudio modules. The build instructions can be found at wiki.

* [How to set up audio redirection](https://github.com/neutrinolabs/xrdp/wiki/How-to-set-up-audio-redirection)

## Directory Structure

```
xrdp
├── common ······ common code
├── docs ········ documentation
├── fontutils ··· font handling utilities
├── genkeymap ··· keymap generator
├── instfiles ··· installable data file
├── keygen ······ xrdp RSA key pair generator
├── libpainter ·· painter library
├── librfxcodec · RFX codec library
├── libxrdp ····· core RDP protocol implementation
├── m4 ·········· Autoconf macros
├── mc ·········· media center module
├── neutrinordp · RDP client module for proxying RDP connections using NeutrinoRDP
├── pkgconfig ··· pkg-config configuration
├── scripts ····· build scripts
├┬─ sesman ······ session manager for xrdp
|├── chansrv ···· channel server for xrdp
|├── libsesman ·· Code common to sesman and its related executables
|└── tools ······ session management tools for sys admins
├── tests ······· tests for the code
├┬─ tools ······· tools
|└┬─ devel ······ development tools
| ├── gtcp_proxy  GTK app that forwards TCP connections to a remote host
| └── tcp_proxy · CLI app that forwards TCP connections to a remote host
├── vnc ········· VNC client module for xrdp
├── vrplayer ···· QT player redirecting video/audio to clients over xrdpvr channel
├── wlxrdp ······ Wayland session backend (xup), with compositor adapters
├── xrdp ········ main server code
├── xrdp_accel_assist  GPU encoding helper (VA-API H.264, AVC444)
├── xrdpapi ····· virtual channel API
├── xrdpvr ······ API for playing media over RDP
└── xup ········· xorgxrdp client module
```

An optional modern pre-session interface is available with the system LVGL
library. See [LVGL login UI](docs/lvgl.md) for build requirements and configuration.
