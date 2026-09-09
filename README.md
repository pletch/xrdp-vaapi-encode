[![Build Status](https://github.com/neutrinolabs/xrdp/actions/workflows/build.yml/badge.svg)](https://github.com/neutrinolabs/xrdp/actions)
[![Gitter](https://badges.gitter.im/Join%20Chat.svg)](https://gitter.im/neutrinolabs/xrdp-questions)
![Apache-License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)

[![Latest Version](https://img.shields.io/github/v/release/neutrinolabs/xrdp.svg?label=Latest%20Version)](https://github.com/neutrinolabs/xrdp/releases)

# xrdp - an open source RDP server

## Fork: Intel VAAPI hardware H.264 encoding (ffmpeg-free)

This is a fork of xrdp that adds an **Intel VA-API hardware H.264 encoder** to the
`xrdp_accel_assist` helper — the ffmpeg-free GPU encoding path suggested by jsorg71
in [neutrinolabs/xrdp#3774](https://github.com/neutrinolabs/xrdp/pull/3774). It fills
the previously-empty `ENC_VA` encoder slot, so EGFX H.264 frames are produced by the
GPU's video-encode engine instead of software libx264.

**Pipeline:** glamor screen pixmap → GL RGB→NV12 shader → `eglExportDMABUFImageMESA`
(dma-buf) → import as a VA NV12 surface (DRM PRIME, zero-copy) → `VAEntrypointEncSliceLP`
H.264. SPS/PPS/slice headers are generated directly — no ffmpeg, no libx264.

### What changed
* `xrdp_accel_assist/xrdp_accel_assist_vaapi.{c,h}` — the VA-API encoder (new)
* `xrdp_accel_assist/xrdp_accel_assist_x11.c` — wire the `ENC_VA` vtable + NV12 layout
* `configure.ac`, `xrdp_accel_assist/Makefile.am` — `--enable-vaapi` build option

**Stream tuning** (later improvements):
* H.264 **High profile + 8×8 integer transform** in the packed SPS/PPS — ~5–10 % better
  compression at the same QP, especially on the smooth solid-color regions and text
  that dominate a typical desktop.
* `glFlush()` instead of `glFinish()` at the encode entry — the dma-buf shared between
  Mesa GL and iHD VAAPI carries implicit DRM_PRIME synchronisation, so the kernel
  sequencer already enforces GL-before-VAAPI ordering on the GPU itself. The hard
  CPU-side stall was double-syncing; the next frame's GL work can now overlap with
  this frame's encode.
* **Even-rect fix** in `out_RFX_AVC420_METABLOCK` — the existing −1/+1 chroma margin
  expansion could produce odd-width rects, which FreeRDP's AVC SSE primitive asserts
  on and crashes the client (and mstsc tolerates but mis-renders as stripes during
  P-frame updates). All four edges are now rounded to even.
* **Hardware-active log line** in `xrdp.log` on the first compressed frame received
  from accel-assist:
  ```
  gfx_wiretosurface1: AVC420 hardware encoding active (accel-assist), first compressed frame received: N bytes
  ```
  Single signal in the daemon log that the VAAPI path is doing the work, without
  having to grep the per-display `xrdp-accel-assist.NN.log`.

### Building / enabling

```
./configure --enable-rfxcodec --enable-x264 --enable-vaapi   # needs libva-dev, libva-drm-dev
```

Enable per session in `sesman.ini` `[SessionVariables]` with `XRDP_USE_ACCEL_ASSIST=1`
(constant-QP override via `XRDP_VAAPI_QP`; see **Tuning** below).

The encoder probes a preference list of H.264 profile/entrypoint pairs at startup and
uses the first one the driver offers, so it is not tied to a single generation:

| pair | typical hardware |
| ---- | ---------------- |
| `High` / `EncSliceLP` | Intel iHD, Gen9+ (Skylake → Arc) — the primary target |
| `High` / `EncSlice`   | AMD radeonsi, other drivers with the regular entrypoint |
| `Main` / `EncSliceLP` | iHD parts without High-profile low-power encode |
| `Main` / `EncSlice`   | legacy Intel i965 (Haswell/Broadwell era) |

Main drops the 8x8 integer transform (slightly larger output at the same QP); the
packed SPS/PPS follow the chosen profile automatically. A pair is preferred only if it
also advertises constant-QP rate control and application-supplied packed headers.
Baseline is not in the list — the stream uses CABAC. The chosen pair is logged:

```
vaapi_init: using H.264 High/EncSliceLP (profile_idc 100, 8x8 transform on)
```

The DRM node defaults to `/dev/dri/renderD128`; on a multi-GPU host set
`XRDP_VAAPI_DEVICE` (and xorgxrdp's matching `DRMDevice` / `XORGXRDP_DRM_DEVICE`) to
the same GPU. **Requires the companion
[pletch/xorgxrdp-glamor-gbm](https://github.com/pletch/xorgxrdp-glamor-gbm) fork** for the
GBM-backed glamor screen pixmap that the dma-buf export depends on.

### AVC444: true 4:4:4 chroma

The encoder also implements **AVC444** (MS-RDPEGFX `RDPGFX_CODECID_AVC444` /
`AVC444v2`), which carries the chroma that 4:2:0 throws away in a second H.264
picture. The client reassembles both into a YUV444 frame. This is what keeps
subpixel-antialiased text crisp — 4:2:0 smears colour across adjacent pixels and
turns ClearType fringes muddy.

Both pictures are **one H.264 sequence**, not two streams. FreeRDP hands the same
decoder context to both views, so they must interleave as a single sequence of
pictures sharing one DPB. Encoding them as two independent sequences produces
persistent corruption that looks like a chroma bug but is not.

* **Capability negotiation** — xrdp derives AVC444 support from the client's
  advertised EGFX capability set (MS-RDPEGFX 2.2.3) and falls back to AVC420 on
  its own, so `XRDP_ACCEL_AVC444=1` is safe to leave enabled for mixed clients.
  The confirmed capability version also picks the chroma layout: 10.2 and later
  get v2, a bare 10.0 client is held to v1. There is no capability flag
  separating the two layouts — the whole set is `THINCLIENT`, `SMALL_CACHE`,
  `AVC420_ENABLED`, `AVC_DISABLED`, `AVC_THINCLIENT`, `SCALEDMAP_DISABLE` — so
  the version is the only signal and the choice is finally the server's.
* **Long-term reference frames** — the auxiliary picture predicts from an LTR
  rather than the previous frame, so the two views do not disturb each other's
  prediction chain and neither needs a periodic IDR to resynchronise. The slice
  header is written directly, because iHD predicts from the LTR but does not emit
  the `dec_ref_pic_marking()` MMCO that tells the decoder about it.
* **Chroma interval** — the auxiliary picture only needs to be sent periodically.
  `XRDP_AVC444_CHROMA_INTERVAL=8` refreshes chroma every eighth frame while luma
  updates every frame, and measured far smoother than every-frame chroma with no
  visible penalty on desktop content. Frames without it carry `LC=1` (luma only).
* **Packed shaders** — the RGB→NV12 conversion writes four destination bytes per
  fragment as RGBA8 over a quarter-width viewport, for both the main and auxiliary
  views.
* **The v2 auxiliary view is rendered over the damage**, not the whole frame,
  accumulating a bounding box between the frames that carry it and falling back
  to full-frame on an IDR or once the box passes half the picture. Measured 2.5x
  less GL time on desktop content; no effect during video, where the damage
  covers most of the frame anyway. It does not change the bitstream — both views
  encode a full picture and the encoder skips unchanged macroblocks — so the
  auxiliary picture's size still follows the age of its reference rather than how
  much was drawn.

Note that the auxiliary view is rendered full-frame and so needs a *complete* source
pixmap, not just the current damage. That constrains the capture side — see the
xorgxrdp fork's README.

* **Chroma is stored as the 2x2 mean**, not the even/even sample. The auxiliary
  view carries three of every four chroma samples; the fourth is never sent, and
  a decoder recovers it as `4*mean - the other three` (FreeRDP does this in
  YUV444 to RGB, `prim_YUV.c`). Storing the sample makes that recovery produce a
  value unrelated to anything wherever the four differ — coloured speckling on
  icon and glyph edges, invisible on flat colour.
* **Level derived from the picture.** `level_idc` was fixed at 4.1, whose maximum
  frame size is 8192 macroblocks: fine for 1920x944 (7080), wrong for 2688x1488
  (15624) or 3008x2000 (23500). A decoder that honours the declaration refuses
  the hardware path and falls back to software, which under AVC444 is two
  oversized pictures per frame on the CPU. The level now follows the size — 4.2,
  5.1 and 5.2 respectively. A client that hardcodes its own decoder level has to
  match; parse it from the SPS rather than assuming.
* **Full-range colour is signalled.** The shaders convert with full-range BT.709,
  so samples span 0-255. Without `video_full_range_flag` a decoder assumes
  limited range and expands 16-235 to 0-255, leaving black at 16 and white at
  235. The VUI now carries it along with BT.709 primaries, transfer and matrix.

Note that AVC444's advantage is largest at 1x rendering. On a HiDPI client sending
physical pixels, 4:2:0's 2x2 chroma block covers a single logical pixel, so much of
what 4:4:4 buys is already recovered by the extra resolution — and the second
picture per frame is often not worth its decode cost there.

### Tuning

All of these are `[SessionVariables]` in `sesman.ini`, documented there as well:

| variable | default | effect |
| -------- | ------- | ------ |
| `XRDP_USE_ACCEL_ASSIST` | off | required for any of the below |
| `XRDP_ACCEL_AVC444` | off | AVC444; negotiated, falls back to AVC420 |
| `XRDP_AVC444_CHROMA_INTERVAL` | 1 | frames between auxiliary (chroma) pictures |
| `XRDP_VAAPI_QP` / `_AUX_QP` | 26 | constant quantiser, 1-51 |
| `XRDP_VAAPI_BITRATE` | 0 (CQP) | kbit/s; switches to VBR |
| `XRDP_SOUND_MAX_LATENCY_MS` | 0 | drop audio above this measured latency |
| `XRDP_VAAPI_LOG_SPS` | off | dump the SPS as hex once per session |

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
decode as fast as the server encodes. Match it to the content — 33 for 30 fps.

### Benchmark: VAAPI hardware vs software x264

Server-side CPU for the full encode pipeline (Xorg capture/convert + xrdp +
xrdp-accel-assist) over an identical 40 s workload — 1920×1080 @ 30 fps full-motion
video, Intel UHD 770 (iHD 26.1.2), hardware CQP 28 vs xrdp's default x264:

| process                          | software (x264) | hardware (VAAPI) |
| -------------------------------- | --------------: | ---------------: |
| xrdp — H.264 encode              |         8.31 s  |  0.55 s (relay)  |
| Xorg — capture + color convert   |         5.92 s  |  1.32 s          |
| xrdp-accel-assist — VAAPI encode |            —    |  0.95 s          |
| **total CPU-seconds / 40 s**     |     **14.23 s** |   **2.82 s**     |
| **% of one CPU core**            |      **35.6 %** |    **7.0 %**     |

**≈80 % less server CPU (about 5× lighter)** for the same 1080p30 motion. Two costs
move off the CPU: the encoder itself (libx264 → GPU video engine) and RGB→NV12 color
conversion (CPU → GPU shader). The gap widens with resolution, frame rate, and the
number of concurrent sessions. *Comparison is default-config, not equal-bitrate (HW uses
CQP 28; software x264 uses xrdp's defaults).*

*The benchmark predates the High-profile / 8×8-transform / `glFlush` tweaks above; the
current build should be at least as efficient (slightly less CPU and ~5–10 % smaller
H.264 output at the same QP), but no fresh end-to-end measurement has been taken.*

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

![demo](https://github.com/neutrinolabs/xrdp/raw/gh-pages/xrdp_demo.gif)

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
├── xrdp ········ main server code
├── xrdpapi ····· virtual channel API
├── xrdpvr ······ API for playing media over RDP
└── xup ········· xorgxrdp client module
```
