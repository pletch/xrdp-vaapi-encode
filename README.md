[![Build Status](https://github.com/neutrinolabs/xrdp/actions/workflows/build.yml/badge.svg)](https://github.com/neutrinolabs/xrdp/actions)
[![Gitter](https://badges.gitter.im/Join%20Chat.svg)](https://gitter.im/neutrinolabs/xrdp-questions)
![Apache-License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)

[![Latest Version](https://img.shields.io/github/v/release/neutrinolabs/xrdp.svg?label=Latest%20Version)](https://github.com/neutrinolabs/xrdp/releases)

# xrdp - an open source RDP server

> **Wayland sessions:** the child branch
> [`feature/wayland`](https://github.com/pletch/xrdp-vaapi-encode/tree/feature/wayland)
> builds on this one and adds experimental Wayland sessions - a desktop on a
> Wayland compositor instead of Xorg, encoded by the same VA-API helper.

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

```
./configure --enable-rfxcodec --enable-x264 --enable-vaapi   # needs libva-dev, libva-drm-dev
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
the same GPU. **Requires the companion
[pletch/xorgxrdp-glamor-gbm](https://github.com/pletch/xorgxrdp-glamor-gbm) fork**: it
carries the xorgxrdp side of the AVC444 negotiation, which has to match this branch
(xorgxrdp decides from the client info xrdp sends whether AVC444 is in effect), and
FlyGoat's glamor/DRI3 fixes that make glamor work on the Intel `xe` kernel driver
([neutrinolabs/xorgxrdp#423](https://github.com/neutrinolabs/xorgxrdp/pull/423)).

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
* **Chroma interval**: the auxiliary picture only needs to be sent periodically.
  The default of 4 refreshes chroma every fourth frame while luma updates every
  frame, and measures far smoother than every-frame chroma with no visible penalty
  on desktop content. Frames without it carry `LC=1` (luma only). Larger intervals
  work - 8 was the previous default - but chroma then lags luma by up to
  interval-1 frames, and the auxiliary view's accumulated damage box reaches its
  full-frame fallback sooner, so the saving falls off.
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

* **The auxiliary view declares the rect the helper actually rendered.** Under
  `XRDP_AVC444_CHROMA_INTERVAL > 1` the auxiliary picture carries the damage
  accumulated across the frames it skipped, which is more than the current frame's
  rects; declaring only those would leave a region that changed during the skipped
  frames holding its odd-row chroma from the last auxiliary frame. accel-assist has
  that rectangle - it is what the v2 shader pass was scissored to - and appends it
  to the shared-memory payload as an optional 20-byte trailer after
  `[len1][stream1][len2][stream2]`. The trailer is optional in both directions, so a
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
| `XRDP_AVC444_CHROMA_INTERVAL` | 4 | frames between auxiliary (chroma) pictures |
| `XRDP_AVC444_CHROMA_MAX_MS` | 200 | upper bound on chroma staleness; `0` for frame counting only |
| `XRDP_AVC444_IDR_MS` | 10000 | synchronised IDR on both views at most this often; `0` off |
| `XRDP_AVC444_IDR_MIN_KB` | 100 | ...and only after this much has been sent since the last; `0` purely timed |
| `XRDP_AVC444_IDR_PERIOD` | unset | overrides the two above with an IDR every Nth frame, ungated (A/B testing) |
| `XRDP_VAAPI_QP` / `_AUX_QP` | 28 / 18 | constant quantiser, 1-51; 26 is a reasonable desktop value for main |
| `XRDP_VAAPI_BITRATE` | 0 (CQP) | kbit/s; switches to VBR |
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
the auxiliary view goes out every fourth frame by default
(`XRDP_AVC444_CHROMA_INTERVAL`), so AVC444 costs about 1.25 pictures a frame.
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
