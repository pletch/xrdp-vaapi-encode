/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Jay Sorg 2022-2026
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
 * VA-API hardware H.264 encoder for xrdp_accel_assist.
 *
 * This is the ffmpeg-free path discussed in neutrinolabs/xrdp PR #3774:
 * the RGB->YUV conversion is done by the existing GLSL shaders into an
 * NV12-layout OpenGL texture, that texture is exported as a dma-buf via
 * EGL_MESA_image_dma_buf_export, the dma-buf is imported into libva as an
 * NV12 surface (zero-copy), and libva's hardware encoder produces the
 * H.264 bitstream.
 *
 * Status: scaffold. init + dma-buf export + VA surface import +
 * config/context creation are implemented. The H.264 parameter buffers
 * (sequence/picture/slice) and packed SPS/PPS/slice-header bitstream
 * generation are NOT yet implemented -- that is the bulk of the remaining
 * work (see the TODOs in create_encoder()/encode()). Until then,
 * encode() returns ENCODER_ERROR.
 *
 * Primary target HW: Intel iHD (Gen9+), which exposes H.264 encode only
 * via VAEntrypointEncSliceLP (low-power / VDEnc). Other drivers -- the
 * legacy Intel i965 (Haswell/Broadwell era) and AMD radeonsi -- offer the
 * regular VAEntrypointEncSlice instead, and some only reach Main profile.
 * xrdp_accel_assist_vaapi_init() therefore probes a preference list of
 * profile/entrypoint pairs rather than requiring one; the chosen pair
 * drives profile_idc and the 8x8 transform in the packed SPS/PPS.
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <epoxy/gl.h>
#include <epoxy/egl.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>

#include "arch.h"
#include "os_calls.h"
#include "string_calls.h"
#include "xrdp_accel_assist.h"
#include "xrdp_accel_assist_x11.h"
#include "xrdp_accel_assist_vaapi.h"
#include "log.h"

/* EGL display/context created by the EGL interface backend. The VA-API
   encoder requires the EGL backend (INF_EGL) for dma-buf export. */
extern EGLDisplay g_egl_display; /* in xrdp_accel_assist_egl.c */
extern EGLContext g_egl_context; /* in xrdp_accel_assist_egl.c */

/* fourcc helper, avoids a hard dependency on drm_fourcc.h */
#define XH_FOURCC(a, b, c, d) \
    ((unsigned int) (a) | ((unsigned int) (b) << 8) | \
     ((unsigned int) (c) << 16) | ((unsigned int) (d) << 24))
#define XH_DRM_FORMAT_R8    XH_FOURCC('R', '8', ' ', ' ')
#define XH_DRM_FORMAT_GR88  XH_FOURCC('G', 'R', '8', '8')
/* DRM_FORMAT_MOD_LINEAR, avoids a hard dependency on drm_fourcc.h */
#define XH_DRM_FORMAT_MOD_LINEAR 0ULL

#define XH_VAAPI_DEFAULT_QP     28
/* AVC444 aux Y plane carries chroma data dressed up as luma; H.264's luma
   quantisation is tuned for high-frequency content and adds visible noise
   to the smooth chroma. Lower QP on aux to reduce that noise -- it's the
   noise differential between main and aux streams that produces stripes
   when ChromaV1ToYUV444 interleaves rows from both. */
#define XH_VAAPI_DEFAULT_AUX_QP 18

/* Reconstructed-picture surfaces. AVC444 interleaves two views in one H.264
   sequence and each view predicts from its own previous picture, so each
   view needs a pair to ping-pong across: 0/1 for the main view, 2/3 for the
   auxiliary. AVC420 uses 0/1 only. */
#define XH_VAAPI_NUM_RECON 4

/* Write the slice header ourselves. Required for the long-term reference
   marking; XRDP_AVC444_PACKED_SLICE=0 hands it back to the driver, which
   also disables LTR since the marking would not reach the decoder. */
static int g_packed_slice = 1;

/* render node, can be overridden with XRDP_VAAPI_DEVICE */
static char g_default_dev[] = "/dev/dri/renderD128";

static int g_drm_fd = -1;
static VADisplay g_va_dpy = NULL;

/* H.264 profile/entrypoint chosen by xrdp_accel_assist_vaapi_init(), and the
   two bitstream decisions that follow from it. Defaults match the preferred
   candidate so the values are sane even if init() is bypassed in a test. */
static VAProfile g_va_profile = VAProfileH264High;
static VAEntrypoint g_va_entrypoint = VAEntrypointEncSliceLP;
static int g_profile_idc = 100;     /* 100 = High, 77 = Main */
static int g_transform_8x8 = 1;     /* High profile only */

/* Preference order. High first for the 8x8 integer transform (~5-10%% better
   compression on desktop content), low-power entrypoint first because it is
   the cheaper engine where both exist. Baseline is deliberately absent: the
   stream uses CABAC, which baseline does not allow. */
struct xh_va_candidate
{
    VAProfile profile;
    VAEntrypoint entrypoint;
    int profile_idc;
    int transform_8x8;
    const char *name;
};

static const struct xh_va_candidate g_va_candidates[] =
{
    { VAProfileH264High, VAEntrypointEncSliceLP, 100, 1, "High/EncSliceLP" },
    { VAProfileH264High, VAEntrypointEncSlice,   100, 1, "High/EncSlice"   },
    { VAProfileH264Main, VAEntrypointEncSliceLP,  77, 0, "Main/EncSliceLP" },
    { VAProfileH264Main, VAEntrypointEncSlice,    77, 0, "Main/EncSlice"   }
};
#define XH_VA_NUM_CANDIDATES \
    ((int) (sizeof(g_va_candidates) / sizeof(g_va_candidates[0])))

struct enc_info
{
    int width;
    int height;
    int frameCount;
    int nviews;          /* 1 = AVC420, 2 = AVC444 (main + aux, one sequence) */
    int qp_view[2];      /* per-view QP; [1] is the AVC444 aux view */
    int chroma_qp_offset; /* PPS chroma_qp_index_offset, -12..12 */
    int deblock_view[2]; /* 0 = disable_deblocking_filter_idc 1 */
    int single_ref;      /* 1 = aux pictures are non-reference (fallback) */
    /* Rate control. Constant QP by default, which is what this encoder has
       always done; it gives steady quality but unbounded frame size, so a
       high-motion frame can be an order of magnitude larger than a still
       one and arrives at the client as a stall. XRDP_VAAPI_BITRATE (kbit/s)
       switches to VBR and bounds it. */
    int rc_mode;         /* VA_RC_CQP or VA_RC_VBR */
    int bitrate_kbps;    /* XRDP_VAAPI_BITRATE, 0 = CQP */
    int use_ltr;         /* 1 = aux pictures are long-term references */

    /* GL -> dma-buf export, one per view */
    EGLImageKHR egl_image[2];
    int dmabuf_fd[2];
    int dmabuf_stride[2];
    int dmabuf_offset[2];

    /* VA-API objects */
    VASurfaceID input_surface[2];  /* NV12 sources, backed by the dma-bufs */
    VASurfaceID recon_surfaces[XH_VAAPI_NUM_RECON];
    VAConfigID config;
    VAContextID context;
    VABufferID coded_buf[2];   /* one per view, so both can be in flight */

    /* H.264 bitstream state. One sequence for both views: frame_num runs
       across every picture, and each view references its own previous
       picture (two back when interleaved). */
    int cur_recon;                 /* index into recon_surfaces */
    int frame_num;                 /* H.264 frame_num for the next picture */
    int poc;                       /* picture order count for the next pic */
    int idr_pic_id;                /* increments per IDR */
    int have_ref[2];               /* is ref_pic[view] valid */
    VAPictureH264 ref_pic[2];      /* each view's own previous picture */
    int ndpb;                      /* live entries in dpb[] */
    VAPictureH264 dpb[2];          /* short-term refs, most recent first */
    int cur_recon_aux;             /* 0/1, selects recon_surfaces[2 + n] */
    /* XRDP_VAAPI_TIMING=1: how long the GPU actually spends encoding, so a
       frame-rate ceiling can be attributed to this end of the pipe rather
       than to the client. Accumulated per view and reported periodically. */
    int timing;
    int t_count[2];
    int t_total_us[2];
    int t_max_us[2];
    int t_sync_us[2];
    unsigned int t_begin_ms[2];
    enum encoder_result pending_rv[2];
    int have_ltr;                  /* is ltr_pic valid */
    VAPictureH264 ltr_pic;         /* the previous aux picture, LongTermFrameIdx 0 */
};

/* ------------------------------------------------------------------------ */
/* Minimal H.264 RBSP bit writer (MSB first) for packed SPS/PPS.            */
/* ------------------------------------------------------------------------ */

struct bitstream
{
    unsigned char *p; /* caller-zeroed buffer */
    int bitpos;
};

/*****************************************************************************/
static void
bs_put(struct bitstream *b, int nbits, unsigned int value)
{
    int i;
    int bit;
    int byte;
    int off;

    for (i = nbits - 1; i >= 0; i--)
    {
        bit = (value >> i) & 1;
        byte = b->bitpos >> 3;
        off = 7 - (b->bitpos & 7);
        if (bit)
        {
            b->p[byte] |= (1 << off);
        }
        b->bitpos++;
    }
}

/*****************************************************************************/
/* unsigned Exp-Golomb */
static void
bs_ue(struct bitstream *b, unsigned int value)
{
    int n;
    unsigned int t;

    n = 0;
    t = value + 1;
    while (t >> 1)
    {
        t >>= 1;
        n++;
    }
    bs_put(b, n, 0);            /* n leading zero bits */
    bs_put(b, n + 1, value + 1);
}

/*****************************************************************************/
/* signed Exp-Golomb */
static void
bs_se(struct bitstream *b, int value)
{
    unsigned int u;

    u = (value <= 0) ? (unsigned int) (-2 * value)
                     : (unsigned int) (2 * value - 1);
    bs_ue(b, u);
}

/*****************************************************************************/
static void
bs_trailing(struct bitstream *b)
{
    bs_put(b, 1, 1);
    while (b->bitpos & 7)
    {
        bs_put(b, 1, 0);
    }
}

/*****************************************************************************/
/* Emit start code + NAL header + emulation-prevented RBSP into out.
   Returns the number of bytes written. */
static int
build_nal(unsigned char *out, int nal_ref_idc, int nal_unit_type,
          const unsigned char *rbsp, int rbsp_len)
{
    unsigned char tmp[1024];
    int tlen;
    int o;
    int zeros;
    int i;

    tlen = 0;
    tmp[tlen++] = (unsigned char) ((nal_ref_idc << 5) | nal_unit_type);
    for (i = 0; i < rbsp_len; i++)
    {
        tmp[tlen++] = rbsp[i];
    }
    o = 0;
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 1;
    zeros = 0;
    for (i = 0; i < tlen; i++)
    {
        if ((zeros >= 2) && (tmp[i] <= 3))
        {
            out[o++] = 3;
            zeros = 0;
        }
        out[o++] = tmp[i];
        zeros = (tmp[i] == 0) ? (zeros + 1) : 0;
    }
    return o;
}

/*****************************************************************************/
/* The H.264 level to declare, from the picture size.

   This was a hard-coded 4.1, whose maximum frame size is 8192 macroblocks
   (H.264 Table A-1). That is fine for 1920x944, which is 7080, and wrong for
   anything larger: 2688x1488 is 15624 macroblocks and 3008x2000 is 23500,
   so the stream declared a level it exceeded by two to three times.

   A decoder that honours the declaration sizes its buffers from it and
   refuses the hardware path when the frames do not fit, falling back to
   software. Under AVC444 that is two oversized pictures per frame to decode
   on the CPU, which is why the same client sailed through 1080p and crawled
   at higher resolutions -- a cliff at the level boundary rather than a curve
   in the pixel count.

   Levels also cap macroblocks per second, and that limit is routinely
   exceeded without consequence: 1920x944 at 60 fps needs 4.2 by rate while
   working perfectly as 4.1. Frame size is what decoders enforce, because it
   is what they must allocate for. We satisfy both anyway -- declaring a
   higher level costs nothing but decoder buffer -- assuming the fastest rate
   the capture loop can offer. */
static int
h264_level_for(int width, int height)
{
    /* level_idc, MaxFS in macroblocks, MaxMBPS in macroblocks per second */
    static const int levels[][3] =
    {
        {40,   8192,  245760}, {41,   8192,  245760}, {42,   8704,  522240},
        {50,  22080,  589824}, {51,  36864,  983040}, {52,  36864, 2073600},
        {60, 139264, 4177920}, {61, 139264, 8355840}, {62, 139264, 16711680}
    };
    int mbs = ((width + 15) / 16) * ((height + 15) / 16);
    int mbps = mbs * 60;
    unsigned int index;

    for (index = 0; index < sizeof(levels) / sizeof(levels[0]); index++)
    {
        if (mbs <= levels[index][1] && mbps <= levels[index][2])
        {
            return levels[index][0];
        }
    }
    /* Larger than level 6.2 allows. Declare the highest rather than a value
       we know to be too small; the encoder will have failed long before. */
    return 62;
}

/*****************************************************************************/
static int
build_sps(unsigned char *out, struct enc_info *ei)
{
    int num_ref = (ei->nviews > 1 && !ei->single_ref) ? 2 : 1;

    unsigned char rbsp[256];
    struct bitstream b;
    int w_mbs;
    int h_mbs;
    int crop_r;
    int crop_b;

    w_mbs = (ei->width + 15) / 16;
    h_mbs = (ei->height + 15) / 16;
    crop_r = (w_mbs * 16 - ei->width) / 2; /* 4:2:0 crop units = 2 luma */
    crop_b = (h_mbs * 16 - ei->height) / 2;

    g_memset(rbsp, 0, sizeof(rbsp));
    b.p = rbsp;
    b.bitpos = 0;
    bs_put(&b, 8, g_profile_idc);  /* High (100) where the driver offers it --
                                      enables the 8x8 integer transform in PPS,
                                      ~5-10%% better compression on smooth
                                      desktop content at the same QP -- else
                                      Main (77) */
    bs_put(&b, 8, 0);              /* constraint flags + reserved zero */
    bs_put(&b, 8, h264_level_for(ei->width, ei->height));
    bs_ue(&b, 0);                  /* seq_parameter_set_id */
    if (g_profile_idc >= 100)
    {
        /* High-profile-only header fields (profile_idc >= 100). Order per
           H.264 7.3.2.1.1: chroma_format_idc, bit_depths, then two flags.
           Main must not emit them at all. */
        bs_ue(&b, 1);              /* chroma_format_idc = 4:2:0 */
        bs_ue(&b, 0);              /* bit_depth_luma_minus8 = 8-bit */
        bs_ue(&b, 0);              /* bit_depth_chroma_minus8 = 8-bit */
        bs_put(&b, 1, 0);          /* qpprime_y_zero_transform_bypass_flag */
        bs_put(&b, 1, 0);          /* seq_scaling_matrix_present_flag */
    }
    bs_ue(&b, 0);                  /* log2_max_frame_num_minus4 */
    bs_ue(&b, 2);                  /* pic_order_cnt_type = 2 */
    /* Must match seq.max_num_ref_frames below. Only reference pictures count;
       interleaved AVC444 keeps the previous picture of each view. */
    bs_ue(&b, num_ref);            /* max_num_ref_frames */
    bs_put(&b, 1, 0);              /* gaps_in_frame_num_value_allowed_flag */
    bs_ue(&b, w_mbs - 1);          /* pic_width_in_mbs_minus1 */
    bs_ue(&b, h_mbs - 1);          /* pic_height_in_map_units_minus1 */
    bs_put(&b, 1, 1);              /* frame_mbs_only_flag */
    bs_put(&b, 1, 1);              /* direct_8x8_inference_flag */
    if ((crop_r != 0) || (crop_b != 0))
    {
        bs_put(&b, 1, 1);          /* frame_cropping_flag */
        bs_ue(&b, 0);              /* frame_crop_left_offset */
        bs_ue(&b, crop_r);         /* frame_crop_right_offset */
        bs_ue(&b, 0);              /* frame_crop_top_offset */
        bs_ue(&b, crop_b);         /* frame_crop_bottom_offset */
    }
    else
    {
        bs_put(&b, 1, 0);          /* frame_cropping_flag */
    }
    /* Emit a VUI carrying only the bitstream restriction. The stream is
       strictly I/P with pic_order_cnt_type=2 -- decode order == display
       order, no reordering is possible. But profile_idc=100 (High) does NOT
       get the automatic max_num_reorder_frames=0 inference that
       constrained-baseline streams do, so without this VUI a hardware
       decoder must assume the level-4.1 max DPB (~4-5 frames) and buffers
       that many frames before emitting output. At a low/variable frame rate
       that fixed buffer becomes multi-second latency. Signalling
       max_num_reorder_frames=0 tells the decoder to emit every frame the
       instant it is decoded.

       max_dec_frame_buffering must track max_num_ref_frames: H.264 E.2.1
       requires max_dec_frame_buffering >= max_num_ref_frames. Leaving it
       pinned at 1 while interleaved AVC444 raised max_num_ref_frames to 2
       made the stream non-conforming. Lenient decoders (mstsc, libavcodec)
       ignore the contradiction; a strict one that sizes its DPB from the VUI
       -- Chrome's WebCodecs, which is what the guacd H.264 passthrough feeds
       -- produces no output at all, which renders as a white screen. */
    bs_put(&b, 1, 1);              /* vui_parameters_present_flag = 1 */
    bs_put(&b, 1, 0);              /* aspect_ratio_info_present_flag */
    bs_put(&b, 1, 0);              /* overscan_info_present_flag */
    /* Say what colour space the stream is actually in. The shader converts
       with the full-range BT.709 matrix -- g_rgb2yux_matrix[1] in
       xrdp_accel_assist_x11.c, coefficients 54/183/18 per 256 with a luma
       offset of zero -- so the samples span 0-255, not 16-235.

       Absent this, a decoder is entitled to assume limited range and expands
       16-235 to 0-255 on its way to RGB, which crushes both ends: black
       arrives at 16 and white at 235, chroma unscaled to match, and the
       picture reads flatter than it should. The client cannot infer the
       range from the samples, so it has to be signalled here. */
    bs_put(&b, 1, 1);              /* video_signal_type_present_flag = 1 */
    bs_put(&b, 3, 5);              /* video_format = 5, unspecified */
    bs_put(&b, 1, 1);              /* video_full_range_flag = 1 */
    bs_put(&b, 1, 1);              /* colour_description_present_flag = 1 */
    bs_put(&b, 8, 1);              /* colour_primaries = BT.709 */
    bs_put(&b, 8, 1);              /* transfer_characteristics = BT.709 */
    bs_put(&b, 8, 1);              /* matrix_coefficients = BT.709 */
    bs_put(&b, 1, 0);              /* chroma_loc_info_present_flag */
    bs_put(&b, 1, 0);              /* timing_info_present_flag */
    bs_put(&b, 1, 0);              /* nal_hrd_parameters_present_flag */
    bs_put(&b, 1, 0);              /* vcl_hrd_parameters_present_flag */
    bs_put(&b, 1, 0);              /* pic_struct_present_flag */
    bs_put(&b, 1, 1);              /* bitstream_restriction_flag = 1 */
    bs_put(&b, 1, 1);              /* motion_vectors_over_pic_boundaries_flag */
    bs_ue(&b, 0);                  /* max_bytes_per_pic_denom */
    bs_ue(&b, 0);                  /* max_bits_per_mb_denom */
    bs_ue(&b, 16);                 /* log2_max_mv_length_horizontal */
    bs_ue(&b, 16);                 /* log2_max_mv_length_vertical */
    bs_ue(&b, 0);                  /* max_num_reorder_frames = 0 */
    bs_ue(&b, num_ref);            /* max_dec_frame_buffering */
    bs_trailing(&b);
    return build_nal(out, 3, 7, rbsp, b.bitpos / 8);
}

/*****************************************************************************/
static int
build_pps(unsigned char *out, struct enc_info *ei)
{
    unsigned char rbsp[64];
    struct bitstream b;

    g_memset(rbsp, 0, sizeof(rbsp));
    b.p = rbsp;
    b.bitpos = 0;
    bs_ue(&b, 0);                  /* pic_parameter_set_id */
    bs_ue(&b, 0);                  /* seq_parameter_set_id */
    bs_put(&b, 1, 1);              /* entropy_coding_mode_flag = CABAC */
    bs_put(&b, 1, 0);              /* bottom_field_pic_order_present_flag */
    bs_ue(&b, 0);                  /* num_slice_groups_minus1 */
    bs_ue(&b, 0);                  /* num_ref_idx_l0_default_active_minus1 */
    bs_ue(&b, 0);                  /* num_ref_idx_l1_default_active_minus1 */
    bs_put(&b, 1, 0);              /* weighted_pred_flag */
    bs_put(&b, 2, 0);              /* weighted_bipred_idc */
    /* One PPS serves the whole sequence, so its pic_init_qp is the main
       view's QP; a picture that wants a different QP (the AVC444 aux view)
       carries the difference in slice_qp_delta. */
    bs_se(&b, ei->qp_view[0] - 26); /* pic_init_qp_minus26 */
    bs_se(&b, 0);                  /* pic_init_qs_minus26 */
    bs_se(&b, ei->chroma_qp_offset); /* chroma_qp_index_offset */
    bs_put(&b, 1, 1);              /* deblocking_filter_control_present_flag */
    bs_put(&b, 1, 0);              /* constrained_intra_pred_flag */
    bs_put(&b, 1, 0);              /* redundant_pic_cnt_present_flag */
    if (g_transform_8x8)
    {
        /* High-profile PPS extension: signal that 8x8 transforms may appear
           in the bitstream. Decoders that only understand Main would see
           this as extra trailing data and reject it -- so it is emitted only
           when the SPS carries profile_idc=100, and any decoder reaching
           this point understands High. */
        bs_put(&b, 1, 1);          /* transform_8x8_mode_flag = 1 */
        bs_put(&b, 1, 0);          /* pic_scaling_matrix_present_flag = 0 */
        bs_se(&b, 0);              /* second_chroma_qp_index_offset = 0 */
    }
    bs_trailing(&b);
    return build_nal(out, 3, 8, rbsp, b.bitpos / 8);
}

/*****************************************************************************/
/* Build the slice NAL header and slice header, per H.264 7.3.3.

   We write this ourselves rather than let the driver generate it, for one
   reason: dec_ref_pic_marking() lives here, and iHD will predict from a
   long-term reference when asked but does not emit the MMCO commands that
   tell the decoder to mark it. The encoder's DPB and the decoder's then
   diverge -- the aux view compressed beautifully, 1.8 KB against 79 KB, and
   the picture corrupted. Writing the header ourselves closes that gap.

   Everything else must reproduce exactly what the driver encoded, since it
   appends the slice data to this header; a mismatch corrupts silently. The
   fields mirror the VAEncSliceParameterBufferH264 the caller fills in, and
   the SPS/PPS built above:

     log2_max_frame_num_minus4 = 0   -> frame_num is 4 bits
     pic_order_cnt_type        = 2   -> no POC syntax in the slice header
     frame_mbs_only_flag       = 1   -> no field syntax
     redundant_pic_cnt_present = 0   -> no redundant_pic_cnt
     weighted_pred_flag        = 0   -> no pred_weight_table
     entropy_coding_mode_flag  = 1   -> cabac_init_idc present on P slices
     deblocking_filter_control_present_flag = 1
     num_slice_groups_minus1   = 0

   The reference list stays in default order -- short-term by descending
   PicNum, then long-term by ascending LongTermPicNum -- which is already
   what the caller passes as RefPicList0, so
   ref_pic_list_modification_flag_l0 is 0. That order puts each view's own
   previous picture where it needs it: index 0 for the main view, index 1
   for the aux.

   Returns the length in BITS. A slice header does not end byte-aligned and
   carries no rbsp_trailing_bits; the driver continues from here. */
static int
build_slice_header(unsigned char *out, struct enc_info *ei, int is_idr,
                   int is_ref, int is_ltr, int slice_type, int nref,
                   int qp, int deblock_idc, int idr_pic_id)
{
    unsigned char rbsp[64];
    struct bitstream b;
    int nbytes;
    int i;
    int o;

    g_memset(rbsp, 0, sizeof(rbsp));
    b.p = rbsp;
    b.bitpos = 0;

    bs_ue(&b, 0);                      /* first_mb_in_slice */
    bs_ue(&b, slice_type);             /* 0 = P, 2 = I */
    bs_ue(&b, 0);                      /* pic_parameter_set_id */
    bs_put(&b, 4, ei->frame_num & 15); /* frame_num */
    if (is_idr)
    {
        bs_ue(&b, idr_pic_id);
    }
    if (slice_type == 0)               /* P */
    {
        bs_put(&b, 1, 1);              /* num_ref_idx_active_override_flag */
        bs_ue(&b, (nref > 0 ? nref : 1) - 1);
        bs_put(&b, 1, 0);              /* ref_pic_list_modification_flag_l0 */
    }
    if (is_ref)
    {
        if (is_idr)
        {
            bs_put(&b, 1, 0);          /* no_output_of_prior_pics_flag */
            bs_put(&b, 1, 0);          /* long_term_reference_flag */
        }
        else if (is_ltr)
        {
            /* Mark this picture long-term with LongTermFrameIdx 0, replacing
               whichever aux held it. MMCO 4 comes first because every IDR
               resets MaxLongTermFrameIdx to "none" and MMCO 6 is only legal
               once it is set. Re-sending it on every aux is idempotent: it
               evicts long-term indices above 0, and 0 is the only one used. */
            bs_put(&b, 1, 1);          /* adaptive_ref_pic_marking_mode_flag */
            bs_ue(&b, 4);              /* MMCO 4 */
            bs_ue(&b, 1);              /* max_long_term_frame_idx_plus1 = 1 */
            bs_ue(&b, 6);              /* MMCO 6: mark current long-term */
            bs_ue(&b, 0);              /* long_term_frame_idx = 0 */
            bs_ue(&b, 0);              /* MMCO 0: end of list */
        }
        else
        {
            bs_put(&b, 1, 0);          /* sliding window */
        }
    }
    if (slice_type != 2)               /* CABAC, non-I */
    {
        bs_ue(&b, 0);                  /* cabac_init_idc */
    }
    bs_se(&b, qp - ei->qp_view[0]);    /* slice_qp_delta */
    bs_ue(&b, deblock_idc);            /* disable_deblocking_filter_idc */
    if (deblock_idc != 1)
    {
        bs_se(&b, 0);                  /* slice_alpha_c0_offset_div2 */
        bs_se(&b, 0);                  /* slice_beta_offset_div2 */
    }

    /* Start code and NAL header, then the slice header bits. Emulation
       prevention is left to the driver (has_emulation_bytes = 0), which is
       what it expects for a header that runs on into the slice data. */
    o = 0;
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 1;
    out[o++] = (unsigned char) (((is_ref ? 3 : 0) << 5) | (is_idr ? 5 : 1));
    nbytes = (b.bitpos + 7) / 8;
    for (i = 0; i < nbytes; i++)
    {
        out[o + i] = rbsp[i];
    }
    return o * 8 + b.bitpos;
}

/*****************************************************************************/
/* Does the driver advertise this profile/entrypoint pair? Returns 1 if the
   pair exists at all, and sets *attribs_ok when the pair additionally
   supports the config attributes create_encoder() asks for: constant-QP
   rate control, and application-supplied packed SPS/PPS/slice headers. A pair that exists
   but lacks those is kept only as a last resort -- see vaapi_init(). */
static int
xrdp_accel_assist_vaapi_probe(VAProfile profile, VAEntrypoint entrypoint,
                              int *attribs_ok)
{
    VAEntrypoint *entrypoints;
    VAConfigAttrib attrib[2];
    int num_entrypoints;
    int max_entrypoints;
    int index;
    int found;

    *attribs_ok = 0;
    max_entrypoints = vaMaxNumEntrypoints(g_va_dpy);
    entrypoints = g_new(VAEntrypoint, max_entrypoints);
    if (entrypoints == NULL)
    {
        return 0;
    }
    found = 0;
    if (vaQueryConfigEntrypoints(g_va_dpy, profile, entrypoints,
                                 &num_entrypoints) == VA_STATUS_SUCCESS)
    {
        for (index = 0; index < num_entrypoints; index++)
        {
            if (entrypoints[index] == entrypoint)
            {
                found = 1;
                break;
            }
        }
    }
    g_free(entrypoints);
    if (!found)
    {
        return 0;
    }
    g_memset(attrib, 0, sizeof(attrib));
    attrib[0].type = VAConfigAttribRateControl;
    attrib[1].type = VAConfigAttribEncPackedHeaders;
    if (vaGetConfigAttributes(g_va_dpy, profile, entrypoint, attrib, 2)
            == VA_STATUS_SUCCESS)
    {
        if ((attrib[0].value != VA_ATTRIB_NOT_SUPPORTED) &&
                ((attrib[0].value & VA_RC_CQP) != 0) &&
                (attrib[1].value != VA_ATTRIB_NOT_SUPPORTED) &&
                ((attrib[1].value & VA_ENC_PACKED_HEADER_SEQUENCE) != 0) &&
                ((attrib[1].value & VA_ENC_PACKED_HEADER_PICTURE) != 0))
        {
            *attribs_ok = 1;
        }
    }
    return 1;
}

/*****************************************************************************/
int
xrdp_accel_assist_vaapi_init(void)
{
    char *dev;
    int major;
    int minor;
    int index;
    int best;
    int fallback;
    int attribs_ok;
    VAStatus va_status;

    dev = g_getenv("XRDP_VAAPI_DEVICE");
    if (dev == NULL)
    {
        dev = g_default_dev;
    }
    g_drm_fd = open(dev, O_RDWR);
    if (g_drm_fd < 0)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi_init: open %s failed", dev);
        return 1;
    }
    g_va_dpy = vaGetDisplayDRM(g_drm_fd);
    if (g_va_dpy == NULL)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi_init: vaGetDisplayDRM failed");
        close(g_drm_fd);
        g_drm_fd = -1;
        return 1;
    }
    va_status = vaInitialize(g_va_dpy, &major, &minor);
    if (va_status != VA_STATUS_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi_init: vaInitialize failed %d", va_status);
        vaTerminate(g_va_dpy);
        g_va_dpy = NULL;
        close(g_drm_fd);
        g_drm_fd = -1;
        return 1;
    }
    LOG(LOG_LEVEL_INFO, "vaapi_init: VA-API %d.%d driver '%s'",
        major, minor, vaQueryVendorString(g_va_dpy));

    /* Pick the first profile/entrypoint pair this driver actually offers.
       Intel iHD lands on High/EncSliceLP, the legacy i965 driver and AMD
       radeonsi on one of the regular EncSlice pairs. */
    best = -1;
    fallback = -1;
    for (index = 0; index < XH_VA_NUM_CANDIDATES; index++)
    {
        if (xrdp_accel_assist_vaapi_probe(g_va_candidates[index].profile,
                                          g_va_candidates[index].entrypoint,
                                          &attribs_ok))
        {
            if (attribs_ok)
            {
                best = index;
                break;
            }
            if (fallback < 0)
            {
                fallback = index;
            }
        }
    }
    if (best < 0)
    {
        best = fallback;
        if (best >= 0)
        {
            LOG(LOG_LEVEL_WARNING, "vaapi_init: %s does not advertise "
                "constant-QP rate control and packed SPS/PPS; trying it "
                "anyway", g_va_candidates[best].name);
        }
    }
    if (best < 0)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi_init: no usable H.264 encode "
            "profile/entrypoint (tried High and Main on both EncSliceLP "
            "and EncSlice) on this driver");
        vaTerminate(g_va_dpy);
        g_va_dpy = NULL;
        close(g_drm_fd);
        g_drm_fd = -1;
        return 1;
    }
    g_va_profile = g_va_candidates[best].profile;
    g_va_entrypoint = g_va_candidates[best].entrypoint;
    g_profile_idc = g_va_candidates[best].profile_idc;
    g_transform_8x8 = g_va_candidates[best].transform_8x8;
    LOG(LOG_LEVEL_INFO, "vaapi_init: using H.264 %s (profile_idc %d, "
        "8x8 transform %s)", g_va_candidates[best].name, g_profile_idc,
        g_transform_8x8 ? "on" : "off");
    return 0;
}

/*****************************************************************************/
/* Export the NV12-layout GL texture as a dma-buf via
   EGL_MESA_image_dma_buf_export. The shader renders into a single R8
   texture of height * 3 / 2: plane 0 (Y) is width x height, plane 1 (UV
   interleaved) is width x height / 2 immediately after it. */
static int
xrdp_accel_assist_vaapi_export_dmabuf(struct enc_info *lei, int view, int tex)
{
    int fourcc;
    int num_planes;
    EGLuint64KHR modifiers;
    int fds;
    EGLint strides;
    EGLint offsets;

    lei->egl_image[view] = eglCreateImageKHR(g_egl_display, g_egl_context,
                                       EGL_GL_TEXTURE_2D,
                                       (EGLClientBuffer) (intptr_t) tex, NULL);
    if (lei->egl_image[view] == EGL_NO_IMAGE_KHR)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi: eglCreateImageKHR failed");
        return 1;
    }
    if (!eglExportDMABUFImageQueryMESA(g_egl_display, lei->egl_image[view],
                                       &fourcc, &num_planes, &modifiers))
    {
        LOG(LOG_LEVEL_ERROR, "vaapi: eglExportDMABUFImageQueryMESA failed");
        eglDestroyImageKHR(g_egl_display, lei->egl_image[view]);
        lei->egl_image[view] = EGL_NO_IMAGE_KHR;
        return 1;
    }
    LOG(LOG_LEVEL_INFO, "vaapi: exported dmabuf fourcc 0x%8.8x num_planes %d "
        "modifier 0x%llx", fourcc, num_planes,
        (unsigned long long) modifiers);
    if (modifiers != XH_DRM_FORMAT_MOD_LINEAR)
    {
        /* The import below describes the buffer to VAAPI as linear. A tiled
           export would decode as garbage rather than fail, so say so. */
        LOG(LOG_LEVEL_ERROR, "vaapi: exported dmabuf is not linear "
            "(modifier 0x%llx); the NV12 import assumes linear and the "
            "picture will be wrong", (unsigned long long) modifiers);
    }
    if (!eglExportDMABUFImageMESA(g_egl_display, lei->egl_image[view],
                                  &fds, &strides, &offsets))
    {
        LOG(LOG_LEVEL_ERROR, "vaapi: eglExportDMABUFImageMESA failed");
        eglDestroyImageKHR(g_egl_display, lei->egl_image[view]);
        lei->egl_image[view] = EGL_NO_IMAGE_KHR;
        return 1;
    }
    lei->dmabuf_fd[view] = fds;
    lei->dmabuf_stride[view] = strides;
    lei->dmabuf_offset[view] = offsets;
    LOG(LOG_LEVEL_INFO, "vaapi: dmabuf fd %d stride %d offset %d",
        fds, strides, offsets);
    return 0;
}

/*****************************************************************************/
/* Import the exported single-plane R8 dma-buf as an NV12 VA surface.
   The Y and UV planes share the one dma-buf object at different offsets. */
static int
xrdp_accel_assist_vaapi_import_surface(struct enc_info *lei, int view)
{
    VADRMPRIMESurfaceDescriptor desc;
    VASurfaceAttrib attribs[2];
    int stride;
    VAStatus va_status;

    stride = lei->dmabuf_stride[view];

    g_memset(&desc, 0, sizeof(desc));
    desc.fourcc = VA_FOURCC_NV12;
    desc.width = lei->width;
    desc.height = lei->height;
    desc.num_objects = 1;
    desc.objects[0].fd = lei->dmabuf_fd[view];
    desc.objects[0].size = stride * lei->height * 3 / 2;
    desc.objects[0].drm_format_modifier = XH_DRM_FORMAT_MOD_LINEAR;
    desc.num_layers = 2;
    /* Y plane */
    desc.layers[0].drm_format = XH_DRM_FORMAT_R8;
    desc.layers[0].num_planes = 1;
    desc.layers[0].object_index[0] = 0;
    desc.layers[0].offset[0] = lei->dmabuf_offset[view];
    desc.layers[0].pitch[0] = stride;
    /* interleaved UV plane */
    desc.layers[1].drm_format = XH_DRM_FORMAT_GR88;
    desc.layers[1].num_planes = 1;
    desc.layers[1].object_index[0] = 0;
    desc.layers[1].offset[0] = lei->dmabuf_offset[view] +
                               stride * lei->height;
    desc.layers[1].pitch[0] = stride;

    g_memset(attribs, 0, sizeof(attribs));
    attribs[0].type = VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[1].value.type = VAGenericValueTypePointer;
    attribs[1].value.value.p = &desc;

    va_status = vaCreateSurfaces(g_va_dpy, VA_RT_FORMAT_YUV420,
                                 lei->width, lei->height,
                                 &lei->input_surface[view], 1,
                                 attribs, 2);
    if (va_status != VA_STATUS_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi: vaCreateSurfaces(import) failed %d",
            va_status);
        return 1;
    }
    return 0;
}

/*****************************************************************************/
int
xrdp_accel_assist_vaapi_create_encoder(int width, int height, int tex,
                                       int tex_aux, int tex_format,
                                       struct enc_info **ei)
{
    struct enc_info *lei;
    VAStatus va_status;
    VAConfigAttrib config_attrib[2];
    VASurfaceID render_targets[2 + XH_VAAPI_NUM_RECON];
    char *qp_str;
    int qp_int;
    int view;
    int nrt;

    if (tex_format != XH_YUV420)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi: only XH_YUV420 (NV12) supported, got %d",
            tex_format);
        return 1;
    }

    lei = g_new0(struct enc_info, 1);
    if (lei == NULL)
    {
        return 1;
    }
    lei->width = width;
    lei->height = height;
    lei->nviews = (tex_aux != 0) ? 2 : 1;
    for (view = 0; view < 2; view++)
    {
        lei->egl_image[view] = EGL_NO_IMAGE_KHR;
        lei->dmabuf_fd[view] = -1;
        lei->input_surface[view] = VA_INVALID_ID;
    }
    for (view = 0; view < XH_VAAPI_NUM_RECON; view++)
    {
        lei->recon_surfaces[view] = VA_INVALID_ID;
    }
    lei->config = VA_INVALID_ID;
    lei->context = VA_INVALID_ID;
    lei->coded_buf[0] = VA_INVALID_ID;
    lei->coded_buf[1] = VA_INVALID_ID;

    /* Constant QP, overridable via sesman SessionVariables. The AVC444 aux
       view gets its own QP: its Y plane carries chroma data, and H.264's
       luma quantisation is not tuned for that. Both views live in one
       sequence, so this is applied per picture via pic_init_qp. */
    lei->qp_view[0] = XH_VAAPI_DEFAULT_QP;
    qp_str = g_getenv("XRDP_VAAPI_QP");
    if (qp_str != NULL)
    {
        qp_int = g_atoi(qp_str);
        if ((qp_int >= 1) && (qp_int <= 51))
        {
            lei->qp_view[0] = qp_int;
        }
    }
    lei->qp_view[1] = XH_VAAPI_DEFAULT_AUX_QP;
    qp_str = g_getenv("XRDP_VAAPI_AUX_QP");
    if (qp_str != NULL)
    {
        qp_int = g_atoi(qp_str);
        if ((qp_int >= 1) && (qp_int <= 51))
        {
            lei->qp_view[1] = qp_int;
        }
    }

    /* Rate control mode. CQP unless a bitrate is given, so the default
       behaviour is unchanged. Under VBR the QPs above become the initial
       and maximum quantiser rather than a fixed one. */
    lei->rc_mode = VA_RC_CQP;
    lei->bitrate_kbps = 0;
    qp_str = g_getenv("XRDP_VAAPI_BITRATE");
    if (qp_str != NULL)
    {
        qp_int = g_atoi(qp_str);
        if (qp_int > 0)
        {
            lei->bitrate_kbps = qp_int;
            lei->rc_mode = VA_RC_VBR;
        }
    }

    /* AVC444 chroma-source quality matching. The client builds each YUV444
       chroma plane from two sources: 1/4 of the samples from the main
       view's subsampled U/V planes (at the main picture's *chroma* QP) and
       3/4 from the aux view's B4-B7 blocks, which ride in the aux picture's
       *luma* plane at the aux QP. chroma_qp_index_offset pulls the main
       picture's chroma QP away from its luma QP so the two can be matched;
       with the defaults (main 28, aux 18) that offset is -10. Default 0 so
       AVC420 is unchanged. Range -12..12 per the H.264 spec. */
    lei->chroma_qp_offset = 0;
    qp_str = g_getenv("XRDP_VAAPI_CHROMA_QP_OFFSET");
    if (qp_str != NULL)
    {
        qp_int = g_atoi(qp_str);
        if ((qp_int >= -12) && (qp_int <= 12))
        {
            lei->chroma_qp_offset = qp_int;
        }
    }

    /* In-loop deblocking. The main view is an image, so it always helps.

       For the aux view it depends on the layout. v1's B4/B5 plane is tiled
       in 16-row groups, so vertically adjacent rows hold unrelated chroma
       and the filter smooths across seams that are not image edges -- pure
       loss. v2's plane is the left half carrying U and the right half V, and
       within each half neighbouring samples really are adjacent chroma: a
       coherent, horizontally subsampled chroma image that deblocks like any
       other. Only the single column at the midline is artificial.

       So default off for v1, on for v2. Leaving it off under v2 loses real
       smoothing at every macroblock edge and shows as blockiness and
       haloing around sharp edges. XRDP_VAAPI_AUX_DEBLOCK overrides either
       way. */
    lei->deblock_view[0] = 1;
    qp_str = g_getenv("XRDP_VAAPI_AUX_DEBLOCK");
    if (qp_str != NULL)
    {
        lei->deblock_view[1] = (g_atoi(qp_str) != 0);
    }
    else
    {
        lei->deblock_view[1] = xrdp_accel_assist_x11_avc444_v2();
    }

    /* Reference structure for the interleaved AVC444 sequence.

       Each view has to predict from its own previous picture: the two views
       hold completely different data (luma+subsampled chroma vs packed
       chroma tiles), so predicting across them is worthless -- it made the
       aux stream ~79KB per frame on a static desktop while the main stream,
       predicting from itself, sat at ~500 bytes.

       Reaching the right picture by naming it in RefPicList0 does not work
       on iHD; it ignores the reordering. So do it without any
       ref_pic_list_modification at all, by keeping BOTH views as reference
       pictures and handing the encoder a two-entry L0 list in the default
       order. The default P list is short-term references by descending
       PicNum, most recent first, so with max_num_ref_frames = 2 the DPB
       holds exactly the previous picture of each view and a view's own
       previous picture is always at index 1:

         main(N): list = [aux(N-1), main(N-1)]
         aux(N):  list = [main(N),  aux(N-1)]

       That is ordinary multi-reference P encoding -- the hardware's motion
       search picks index 1 for essentially every macroblock on its own, and
       the slice header needs no modification because the list is already in
       default order.

       XRDP_AVC444_SINGLE_REF=1 falls back to non-reference aux pictures:
       one reference, aux predicting from the main picture of the same
       frame. Correct, needs no multi-reference support, compresses badly. */
    qp_str = g_getenv("XRDP_AVC444_SINGLE_REF");
    lei->single_ref = (qp_str != NULL && g_atoi(qp_str) != 0);

    /* Long-term reference for the auxiliary view.

       The aux has to predict from the previous aux; predicting from the main
       picture costs 18x (77.3 KB against 4.3 KB), because the main picture
       is luma and the aux is packed chroma. As a short-term reference the
       previous aux is evicted by the sliding window as soon as any chroma
       frames are skipped, which is what made XRDP_AVC444_CHROMA_INTERVAL > 1
       so expensive. A long-term reference is not subject to the sliding
       window, so it survives however many main pictures pass in between.

       The default L0 order for a P slice is short-term by descending PicNum
       and then long-term by ascending LongTermPicNum, so with one of each
       the main picture is index 0 and the aux is index 1 -- no
       ref_pic_list_modification, which iHD ignores. Same two-entry trick
       that already works for the short-term case.

       XRDP_AVC444_LTR=0 falls back: the aux is then a short-term reference,
       and a chroma interval above 1 additionally has to make it
       non-reference, since a short-term reference that gets skipped leaves
       the next aux predicting from a picture the decoder has evicted. */
    qp_str = g_getenv("XRDP_VAAPI_TIMING");
    lei->timing = (qp_str != NULL && g_atoi(qp_str) != 0);

    qp_str = g_getenv("XRDP_AVC444_PACKED_SLICE");
    g_packed_slice = !(qp_str != NULL && g_atoi(qp_str) == 0);
    qp_str = g_getenv("XRDP_AVC444_LTR");
    lei->use_ltr = !(qp_str != NULL && g_atoi(qp_str) == 0);
    if (lei->use_ltr && !g_packed_slice)
    {
        /* Without our own slice header the MMCO never reaches the decoder,
           so the marking exists only in the encoder and the two DPBs
           diverge. Refuse the combination rather than corrupt the picture. */
        lei->use_ltr = 0;
        LOG(LOG_LEVEL_INFO, "vaapi: packed slice headers off, disabling LTR");
    }
    if (!lei->use_ltr)
    {
        qp_str = g_getenv("XRDP_AVC444_CHROMA_INTERVAL");
        if (qp_str != NULL && g_atoi(qp_str) > 1 && !lei->single_ref)
        {
            lei->single_ref = 1;
            LOG(LOG_LEVEL_INFO, "vaapi: LTR off and chroma interval > 1, "
                "coding the aux view as non-reference");
        }
    }

    LOG(LOG_LEVEL_INFO, "vaapi: encoder %dx%d, %d view(s), QP %d/%d, "
        "chroma_qp_index_offset %d, deblock %d/%d, single_ref %d, ltr %d, "
        "packed_slice %d, rc %s, bitrate %d kbit/s, level %d",
        width, height, lei->nviews, lei->qp_view[0], lei->qp_view[1],
        lei->chroma_qp_offset, lei->deblock_view[0], lei->deblock_view[1],
        lei->single_ref, lei->use_ltr, g_packed_slice,
        lei->rc_mode == VA_RC_CQP ? "CQP" : "VBR",
        lei->bitrate_kbps, h264_level_for(width, height));

    if (xrdp_accel_assist_vaapi_export_dmabuf(lei, 0, tex) != 0)
    {
        g_free(lei);
        return 1;
    }
    if (xrdp_accel_assist_vaapi_import_surface(lei, 0) != 0)
    {
        xrdp_accel_assist_vaapi_delete_encoder(lei);
        return 1;
    }
    if (lei->nviews > 1)
    {
        if (xrdp_accel_assist_vaapi_export_dmabuf(lei, 1, tex_aux) != 0 ||
                xrdp_accel_assist_vaapi_import_surface(lei, 1) != 0)
        {
            xrdp_accel_assist_vaapi_delete_encoder(lei);
            return 1;
        }
    }

    /* reconstructed / reference surfaces, used as a ring */
    va_status = vaCreateSurfaces(g_va_dpy, VA_RT_FORMAT_YUV420, width, height,
                                 lei->recon_surfaces, XH_VAAPI_NUM_RECON,
                                 NULL, 0);
    if (va_status != VA_STATUS_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi: vaCreateSurfaces(recon) failed %d",
            va_status);
        xrdp_accel_assist_vaapi_delete_encoder(lei);
        return 1;
    }

    /* create the H.264 low-power encode config: constant QP, and tell the
       driver we will supply packed SPS/PPS ourselves */
    g_memset(config_attrib, 0, sizeof(config_attrib));
    config_attrib[0].type = VAConfigAttribRateControl;
    config_attrib[0].value = lei->rc_mode;
    config_attrib[1].type = VAConfigAttribEncPackedHeaders;
    /* SLICE as well as SEQUENCE and PICTURE: we write the slice header
       ourselves so that dec_ref_pic_marking() carries the long-term
       reference MMCO, which iHD acts on internally but does not emit. */
    config_attrib[1].value = VA_ENC_PACKED_HEADER_SEQUENCE |
                             VA_ENC_PACKED_HEADER_PICTURE |
                             VA_ENC_PACKED_HEADER_SLICE;
    va_status = vaCreateConfig(g_va_dpy, g_va_profile, g_va_entrypoint,
                               config_attrib, 2, &lei->config);
    if (va_status != VA_STATUS_SUCCESS && lei->rc_mode != VA_RC_CQP)
    {
        /* Rather than fail the session outright, fall back to the constant-QP
           path this encoder has always used. Frame sizes go back to being
           unbounded, so say so plainly. */
        LOG(LOG_LEVEL_WARNING, "vaapi: vaCreateConfig with VBR failed %d, "
            "falling back to constant QP -- frame size will be unbounded",
            va_status);
        lei->rc_mode = VA_RC_CQP;
        lei->bitrate_kbps = 0;
        config_attrib[0].value = lei->rc_mode;
        va_status = vaCreateConfig(g_va_dpy, g_va_profile, g_va_entrypoint,
                                   config_attrib, 2, &lei->config);
    }
    if (va_status != VA_STATUS_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi: vaCreateConfig failed %d", va_status);
        xrdp_accel_assist_vaapi_delete_encoder(lei);
        return 1;
    }
    nrt = 0;
    for (view = 0; view < lei->nviews; view++)
    {
        render_targets[nrt++] = lei->input_surface[view];
    }
    for (view = 0; view < XH_VAAPI_NUM_RECON; view++)
    {
        render_targets[nrt++] = lei->recon_surfaces[view];
    }
    va_status = vaCreateContext(g_va_dpy, lei->config, width, height,
                                VA_PROGRESSIVE, render_targets, nrt,
                                &lei->context);
    if (va_status != VA_STATUS_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi: vaCreateContext failed %d", va_status);
        xrdp_accel_assist_vaapi_delete_encoder(lei);
        return 1;
    }
    /* coded (output bitstream) buffer, generously sized */
    /* One coded buffer per view: with both views submitted before either is
       waited on, two pictures are in flight at once and cannot share it. */
    for (view = 0; view < lei->nviews; view++)
    {
        va_status = vaCreateBuffer(g_va_dpy, lei->context, VAEncCodedBufferType,
                                   width * height * 3 / 2, 1, NULL,
                                   &lei->coded_buf[view]);
        if (va_status != VA_STATUS_SUCCESS)
        {
            LOG(LOG_LEVEL_ERROR, "vaapi: vaCreateBuffer(coded) failed %d",
                va_status);
            xrdp_accel_assist_vaapi_delete_encoder(lei);
            return 1;
        }
    }

    *ei = lei;
    return 0;
}

/*****************************************************************************/
int
xrdp_accel_assist_vaapi_delete_encoder(struct enc_info *ei)
{
    int view;

    if (ei == NULL)
    {
        return 0;
    }
    for (view = 0; view < 2; view++)
    {
        if (ei->coded_buf[view] != VA_INVALID_ID)
        {
            vaDestroyBuffer(g_va_dpy, ei->coded_buf[view]);
        }
    }
    if (ei->context != VA_INVALID_ID)
    {
        vaDestroyContext(g_va_dpy, ei->context);
    }
    if (ei->config != VA_INVALID_ID)
    {
        vaDestroyConfig(g_va_dpy, ei->config);
    }
    if (ei->recon_surfaces[0] != VA_INVALID_ID)
    {
        vaDestroySurfaces(g_va_dpy, ei->recon_surfaces, XH_VAAPI_NUM_RECON);
    }
    for (view = 0; view < 2; view++)
    {
        if (ei->input_surface[view] != VA_INVALID_ID)
        {
            vaDestroySurfaces(g_va_dpy, &ei->input_surface[view], 1);
        }
        if (ei->egl_image[view] != EGL_NO_IMAGE_KHR)
        {
            eglDestroyImageKHR(g_egl_display, ei->egl_image[view]);
        }
    }
    /* VA takes ownership of the dma-buf fds on import (PRIME_2), so do not
       close ei->dmabuf_fd[] here. */
    g_free(ei);
    return 0;
}

/*****************************************************************************/
/* Helper: create a buffer, queue it, and remember its id for cleanup. */
static int
render_buffer(VAContextID ctx, VABufferType type, unsigned int size,
              void *data, VABufferID *track, int *ntrack)
{
    VABufferID buf;
    VAStatus va_status;

    va_status = vaCreateBuffer(g_va_dpy, ctx, type, size, 1, data, &buf);
    if (va_status != VA_STATUS_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi: vaCreateBuffer(type %d) failed %d",
            type, va_status);
        return 1;
    }
    track[*ntrack] = buf;
    (*ntrack)++;
    va_status = vaRenderPicture(g_va_dpy, ctx, &buf, 1);
    if (va_status != VA_STATUS_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi: vaRenderPicture(type %d) failed %d",
            type, va_status);
        return 1;
    }
    return 0;
}

/*****************************************************************************/
/* Helper: queue one VAEncMiscParameter payload. The buffer is the common
   header followed by the type-specific struct. */
static int
render_misc(VAContextID ctx, VAEncMiscParameterType type,
            const void *payload, unsigned int payload_size,
            VABufferID *track, int *ntrack)
{
    unsigned char buf[sizeof(VAEncMiscParameterBuffer) + 128];
    VAEncMiscParameterBuffer *hdr;

    if (payload_size > 128)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi: misc payload %u too large", payload_size);
        return 1;
    }
    g_memset(buf, 0, sizeof(buf));
    hdr = (VAEncMiscParameterBuffer *) buf;
    hdr->type = type;
    g_memcpy(hdr->data, payload, payload_size);
    return render_buffer(ctx, VAEncMiscParameterBufferType,
                         sizeof(VAEncMiscParameterBuffer) + payload_size,
                         buf, track, ntrack);
}

/*****************************************************************************/
/* Helper: queue a packed header (parameter buffer + data buffer). */
static int
render_packed_bits(VAContextID ctx, int htype, unsigned char *data,
                   int bit_length, int has_epb,
                   VABufferID *track, int *ntrack)
{
    VAEncPackedHeaderParameterBuffer ph;
    int bytes = (bit_length + 7) / 8;

    g_memset(&ph, 0, sizeof(ph));
    ph.type = htype;
    ph.bit_length = bit_length;
    ph.has_emulation_bytes = has_epb;
    if (render_buffer(ctx, VAEncPackedHeaderParameterBufferType, sizeof(ph),
                      &ph, track, ntrack) != 0)
    {
        return 1;
    }
    return render_buffer(ctx, VAEncPackedHeaderDataBufferType, bytes, data,
                         track, ntrack);
}

/*****************************************************************************/
/* SPS and PPS are whole NAL units: byte-aligned, emulation bytes already
   inserted by build_nal(). */
static int
render_packed(VAContextID ctx, int htype, unsigned char *data, int bytes,
              VABufferID *track, int *ntrack)
{
    return render_packed_bits(ctx, htype, data, bytes * 8, 1, track, ntrack);
}

/*****************************************************************************/
/* Second half of an encode: wait for the GPU and copy the bitstream out.
   Split from the submission so both AVC444 views can be in flight at once --
   serialising them cost a full CPU round-trip to the GPU per chroma frame,
   and those frames were the ones exceeding the frame budget. */
static enum encoder_result
vaapi_finish(struct enc_info *ei, int flags, void *cdata, int *cdata_bytes)
{
    int view = (flags & XH_ENC_FLAGS_AUXVIEW) ? 1 : 0;
    VAStatus va_status;
    VACodedBufferSegment *seg;
    int total;
    int t_submit = 0;
    enum encoder_result rv = ei->pending_rv[view];

    if (ei->timing)
    {
        t_submit = g_get_elapsed_ms();
    }
    va_status = vaSyncSurface(g_va_dpy, ei->input_surface[view]);
    if (va_status != VA_STATUS_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi: vaSyncSurface failed %d", va_status);
        return ENCODER_ERROR;
    }
    if (ei->timing)
    {
        int t_end = g_get_elapsed_ms();
        int us = (t_end - ei->t_begin_ms[view]) * 1000;

        ei->t_count[view]++;
        ei->t_total_us[view] += us;
        ei->t_sync_us[view] += (t_end - t_submit) * 1000;
        if (us > ei->t_max_us[view])
        {
            ei->t_max_us[view] = us;
        }
        if (ei->t_count[view] >= 100)
        {
            /* g_get_elapsed_ms() has millisecond resolution, so a single
               frame's figure is coarse; the mean over 100 of them is not. */
            LOG(LOG_LEVEL_INFO, "vaapi: %s encode over %d frames: mean %d us, "
                "max %d ms, of which vaSyncSurface wait mean %d us",
                view ? "aux " : "main", ei->t_count[view],
                ei->t_total_us[view] / ei->t_count[view],
                ei->t_max_us[view] / 1000,
                ei->t_sync_us[view] / ei->t_count[view]);
            ei->t_count[view] = 0;
            ei->t_total_us[view] = 0;
            ei->t_max_us[view] = 0;
            ei->t_sync_us[view] = 0;
        }
    }
    seg = NULL;
    va_status = vaMapBuffer(g_va_dpy, ei->coded_buf[view], (void **) &seg);
    if (va_status != VA_STATUS_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi: vaMapBuffer(coded) failed %d", va_status);
        return ENCODER_ERROR;
    }
    total = 0;
    while (seg != NULL)
    {
        if (total + (int) seg->size > *cdata_bytes)
        {
            LOG(LOG_LEVEL_ERROR, "vaapi: coded data too big %d > %d",
                total + (int) seg->size, *cdata_bytes);
            rv = ENCODER_ERROR;
            break;
        }
        g_memcpy((char *) cdata + total, seg->buf, seg->size);
        total += seg->size;
        seg = (VACodedBufferSegment *) seg->next;
    }
    vaUnmapBuffer(g_va_dpy, ei->coded_buf[view]);
    if (rv == ENCODER_ERROR)
    {
        return rv;
    }
    *cdata_bytes = total;
    return rv;
}

/*****************************************************************************/
static enum encoder_result
vaapi_submit(struct enc_info *ei, void *cdata, int *cdata_bytes,
             int flags, int defer)
{
    VAEncSequenceParameterBufferH264 seq;
    VAEncPictureParameterBufferH264 pic;
    VAEncSliceParameterBufferH264 slice;
    VAPictureH264 curr;
    VABufferID track[12];
    int ntrack;
    int is_idr;
    int w_mbs;
    int h_mbs;
    int i;
    int idr_id_used;
    unsigned char sps[256];
    unsigned char pps[64];
    unsigned char sh[64];
    int sps_len;
    int pps_len;
    enum encoder_result rv;

    int view;
    int qp;
    int nref;
    int is_ref;
    int is_ltr;

    /* AVC444 puts both views in ONE H.264 sequence, because the client
       decodes both bitstreams with a single decoder: FreeRDP's
       avc444_decompress passes the same H264_CONTEXT to both
       avc444_process_rects calls, and that context owns one
       codecDecoderContext. Two independent encoders -- separate SPS/PPS,
       separate IDR sequences, separate frame_num, separate DPB -- produce
       two individually valid streams that corrupt each other the moment
       they are interleaved into one decoder, because every P picture then
       predicts from whichever view happened to be decoded last. That is
       what the horizontal banding was. */
    view = (flags & XH_ENC_FLAGS_AUXVIEW) ? 1 : 0;
    qp = ei->qp_view[view];

    /* Kick GL commands down to the GPU without waiting. The dma-buf shared
       with VAAPI uses implicit DRM_PRIME synchronisation -- the kernel will
       hold the iHD encode submission back until Mesa's GL writes to the
       shared NV12 surface have signalled completion through the underlying
       sync_file/dma_resv attached to the buffer. We were calling glFinish()
       here for safety, but that's a hard CPU-side stall that double-syncs:
       once for GL, then again for VAAPI in vaSyncSurface below. glFlush()
       lets the GL commands queue, returns immediately, and the kernel
       sequencer enforces GL-before-VAAPI ordering on the GPU itself. Net
       win: encode_pixmap no longer blocks for the GL pipeline drain, so
       the next frame's GL work can overlap with this frame's encode. */
    glFlush();

    w_mbs = (ei->width + 15) / 16;
    h_mbs = (ei->height + 15) / 16;
    is_idr = (flags & XH_ENC_FLAGS_FORCEIDR) || (ei->frameCount == 0);
    ntrack = 0;

    /* An IDR resets the whole sequence, so it can only be issued on the
       first view of a frame -- the aux picture that follows must be able to
       reference it. */
    if (is_idr && view != 0)
    {
        is_idr = 0;
    }
    if (is_idr)
    {
        ei->frame_num = 0;
        ei->poc = 0;
        ei->have_ref[0] = 0;
        ei->have_ref[1] = 0;
        ei->ndpb = 0;
        ei->have_ltr = 0;
    }
    idr_id_used = ei->idr_pic_id;

    /* Both views are reference pictures by default -- see the comment on
       single_ref in create_encoder. Under XRDP_AVC444_SINGLE_REF the aux
       picture is non-reference; it still needs somewhere to be
       reconstructed into, but must not land on a surface the DPB holds, so
       it takes the third surface as scratch while the reference pictures
       ping-pong across the other two. */
    is_ref = (view == 0) || (ei->nviews < 2) || !ei->single_ref;
    /* The aux view is the long-term reference; the main view stays
       short-term so its own previous picture is the head of the default L0
       list. */
    is_ltr = is_ref && (view == 1) && (ei->nviews > 1) && ei->use_ltr;

    g_memset(&curr, 0, sizeof(curr));
    if (!is_ref)
    {
        /* non-reference: a scratch surface, so it cannot land on one the
           DPB still holds */
        curr.picture_id = ei->recon_surfaces[XH_VAAPI_NUM_RECON - 1];
    }
    else if (view == 1 && ei->nviews > 1)
    {
        curr.picture_id = ei->recon_surfaces[2 + ei->cur_recon_aux];
    }
    else
    {
        curr.picture_id = ei->recon_surfaces[ei->cur_recon];
    }
    /* For a long-term reference frame_idx carries LongTermFrameIdx, not
       frame_num. One long-term index is enough: each new aux replaces the
       previous one. */
    curr.frame_idx = is_ltr ? 0 : ei->frame_num;
    curr.flags = is_ltr ? VA_PICTURE_H264_LONG_TERM_REFERENCE
                        : (is_ref ? VA_PICTURE_H264_SHORT_TERM_REFERENCE : 0);
    curr.TopFieldOrderCnt = ei->poc;
    curr.BottomFieldOrderCnt = ei->poc;

    if (ei->timing)
    {
        ei->t_begin_ms[view] = g_get_elapsed_ms();
    }
    if (vaBeginPicture(g_va_dpy, ei->context, ei->input_surface[view])
            != VA_STATUS_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi: vaBeginPicture failed");
        return ENCODER_ERROR;
    }

    /* sequence parameters + packed SPS (IDR only) */
    if (is_idr)
    {
        g_memset(&seq, 0, sizeof(seq));
        seq.seq_parameter_set_id = 0;
        seq.level_idc = h264_level_for(ei->width, ei->height);
        seq.intra_period = 0;       /* IDR cadence driven by FORCEIDR flag */
        seq.intra_idr_period = 0;
        seq.ip_period = 1;          /* IPPP, no B frames */
        seq.bits_per_second = ei->bitrate_kbps * 1000; /* 0 under CQP */
        seq.max_num_ref_frames =
            (ei->nviews > 1 && !ei->single_ref) ? 2 : 1;
        seq.picture_width_in_mbs = w_mbs;
        seq.picture_height_in_mbs = h_mbs;
        seq.seq_fields.bits.chroma_format_idc = 1;
        seq.seq_fields.bits.frame_mbs_only_flag = 1;
        seq.seq_fields.bits.direct_8x8_inference_flag = 1;
        seq.seq_fields.bits.log2_max_frame_num_minus4 = 0;
        seq.seq_fields.bits.pic_order_cnt_type = 2;
        if ((w_mbs * 16 != ei->width) || (h_mbs * 16 != ei->height))
        {
            seq.frame_cropping_flag = 1;
            seq.frame_crop_right_offset = (w_mbs * 16 - ei->width) / 2;
            seq.frame_crop_bottom_offset = (h_mbs * 16 - ei->height) / 2;
        }
        if (render_buffer(ei->context, VAEncSequenceParameterBufferType,
                          sizeof(seq), &seq, track, &ntrack) != 0)
        {
            goto cleanup_err;
        }
        if (ei->rc_mode != VA_RC_CQP)
        {
            VAEncMiscParameterRateControl rc;
            VAEncMiscParameterHRD hrd;

            /* VBR: bits_per_second is the ceiling and target_percentage the
               steady-state target, so leaving headroom between them is what
               lets a busy frame borrow bits instead of blowing the buffer.
               The configured QPs become the initial and maximum quantiser --
               quality still degrades gracefully under motion rather than the
               frame growing without limit. Frame skip stays disabled: dropping
               a frame here would desynchronise the AVC444 view pairing. */
            g_memset(&rc, 0, sizeof(rc));
            rc.bits_per_second = ei->bitrate_kbps * 1000;
            rc.target_percentage = 80;
            rc.window_size = 1000;
            rc.initial_qp = ei->qp_view[0];
            rc.min_qp = 1;
            rc.max_qp = 51;
            rc.rc_flags.bits.disable_frame_skip = 1;
            if (render_misc(ei->context, VAEncMiscParameterTypeRateControl,
                            &rc, sizeof(rc), track, &ntrack) != 0)
            {
                goto cleanup_err;
            }
            /* One second of coded picture buffer. Larger smooths quality
               across a scene change; smaller bounds how far ahead of the
               client's decode the stream may run. */
            g_memset(&hrd, 0, sizeof(hrd));
            hrd.buffer_size = ei->bitrate_kbps * 1000;
            hrd.initial_buffer_fullness = hrd.buffer_size / 2;
            if (render_misc(ei->context, VAEncMiscParameterTypeHRD,
                            &hrd, sizeof(hrd), track, &ntrack) != 0)
            {
                goto cleanup_err;
            }
        }
        sps_len = build_sps(sps, ei);
        /* The sequence parameter set as it goes on the wire, once per
           session. It carries the colour signalling, the level and the
           reference counts, all of which a client acts on and none of which
           are visible anywhere else -- a decoder reporting a colour range we
           did not intend is otherwise indistinguishable from a decoder
           ignoring what we sent. XRDP_VAAPI_LOG_SPS=1 to enable. */
        {
            static int logged = 0;
            const char *env = g_getenv("XRDP_VAAPI_LOG_SPS");

            if (!logged && env != NULL && g_atoi(env) != 0)
            {
                char hex[256];
                int i;

                logged = 1;
                hex[0] = 0;
                for (i = 0; i < sps_len && i < 60; i++)
                {
                    g_snprintf(hex + i * 3, 4, "%2.2x ", sps[i]);
                }
                LOG(LOG_LEVEL_INFO, "vaapi: SPS %d bytes: %s", sps_len, hex);
            }
        }
        if (render_packed(ei->context, VAEncPackedHeaderSequence,
                          sps, sps_len, track, &ntrack) != 0)
        {
            goto cleanup_err;
        }
    }

    /* picture parameters */
    g_memset(&pic, 0, sizeof(pic));
    pic.CurrPic = curr;
    for (i = 0; i < 16; i++)
    {
        pic.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        pic.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    /* The DPB as the driver must model it: every short-term reference still
       live, most recent first. Distinct from RefPicList0 below, which picks
       which one this picture actually predicts from. */
    nref = 0;
    for (i = 0; i < ei->ndpb; i++)
    {
        pic.ReferenceFrames[nref++] = ei->dpb[i];
    }
    if (ei->have_ltr)
    {
        pic.ReferenceFrames[nref++] = ei->ltr_pic;
    }
    pic.coded_buf = ei->coded_buf[view];
    pic.pic_parameter_set_id = 0;
    pic.seq_parameter_set_id = 0;
    pic.last_picture = 0;
    pic.frame_num = ei->frame_num;
    /* Must equal the packed PPS's pic_init_qp -- the driver derives the
       slice header's slice_qp_delta from it. */
    pic.pic_init_qp = ei->qp_view[0];
    pic.num_ref_idx_l0_active_minus1 = 0;
    pic.num_ref_idx_l1_active_minus1 = 0;
    /* Must match the packed PPS above bit for bit -- the client parses the
       packed PPS, the driver reconstructs from these. */
    pic.chroma_qp_index_offset = ei->chroma_qp_offset;
    pic.second_chroma_qp_index_offset = ei->chroma_qp_offset;
    pic.pic_fields.bits.idr_pic_flag = is_idr ? 1 : 0;
    pic.pic_fields.bits.reference_pic_flag = is_ref;
    pic.pic_fields.bits.entropy_coding_mode_flag = 1; /* CABAC */
    pic.pic_fields.bits.deblocking_filter_control_present_flag = 1;
    /* High profile only -- let the encoder use 8x8 transforms where it finds
       them beneficial. Must stay 0 on Main, which forbids them. */
    pic.pic_fields.bits.transform_8x8_mode_flag = g_transform_8x8;
    if (render_buffer(ei->context, VAEncPictureParameterBufferType,
                      sizeof(pic), &pic, track, &ntrack) != 0)
    {
        goto cleanup_err;
    }
    if (is_idr)
    {
        pps_len = build_pps(pps, ei);
        if (render_packed(ei->context, VAEncPackedHeaderPicture,
                          pps, pps_len, track, &ntrack) != 0)
        {
            goto cleanup_err;
        }
    }

    /* one slice covering the whole frame */
    g_memset(&slice, 0, sizeof(slice));
    slice.macroblock_address = 0;
    slice.num_macroblocks = w_mbs * h_mbs;
    slice.macroblock_info = VA_INVALID_ID;
    slice.slice_type = is_idr ? 2 : 0; /* I : P */
    slice.pic_parameter_set_id = 0;
    slice.idr_pic_id = idr_id_used;
    slice.pic_order_cnt_lsb = 0;       /* pic_order_cnt_type 2: unused */
    slice.num_ref_idx_l1_active_minus1 = 0;
    for (i = 0; i < 32; i++)
    {
        slice.RefPicList0[i].picture_id = VA_INVALID_SURFACE;
        slice.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
        slice.RefPicList1[i].picture_id = VA_INVALID_SURFACE;
        slice.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
    }
    /* Which picture this one predicts from. */
    nref = 0;
    if (!is_idr)
    {
        /* Default L0 order for a P slice: short-term references by
           descending PicNum, then long-term by ascending LongTermPicNum.
           dpb[] is kept most-recent-first, so writing it and then the
           long-term aux reproduces that order exactly and the driver has no
           reordering to emit. The main view then finds its own previous
           picture at index 0 and the aux finds its own at index 1. */
        for (i = 0; i < ei->ndpb; i++)
        {
            slice.RefPicList0[nref++] = ei->dpb[i];
        }
        if (ei->have_ltr)
        {
            slice.RefPicList0[nref++] = ei->ltr_pic;
        }
    }
    if (nref == 0 && !is_idr)
    {
        LOG(LOG_LEVEL_WARNING, "vaapi: P picture with no reference, "
            "forcing intra");
        slice.slice_type = 2;
    }
    /* Override rather than lean on the PPS default: the list is one entry
       for the first aux picture after an IDR and two thereafter. */
    slice.num_ref_idx_active_override_flag = 1;
    slice.num_ref_idx_l0_active_minus1 = (nref > 0) ? nref - 1 : 0;
    pic.num_ref_idx_l0_active_minus1 = slice.num_ref_idx_l0_active_minus1;
    slice.cabac_init_idc = 0;
    slice.slice_qp_delta = qp - ei->qp_view[0];
    /* The AVC444 aux picture is not an image: it is a tiling of B4/B5/B6/B7
       chroma blocks packed into a luma plane. The in-loop deblocking filter
       would smooth across the tile seams, which are not real image edges --
       pure loss, and a contributor to visible banding once
       ChromaV1ToYUV444 unpacks the tiles back into chroma rows. Leave it on
       for the main view, where the content really is an image. */
    slice.disable_deblocking_filter_idc = ei->deblock_view[view] ? 0 : 1;
    slice.slice_alpha_c0_offset_div2 = 0;
    slice.slice_beta_offset_div2 = 0;
    slice.direct_spatial_mv_pred_flag = 0;
    /* Our own slice header, in place of the driver's. Must be rendered
       before the slice parameter buffer. */
    if (g_packed_slice)
    {
        int sh_bits = build_slice_header(sh, ei, is_idr, is_ref, is_ltr,
                                         slice.slice_type, nref, qp,
                                         slice.disable_deblocking_filter_idc,
                                         idr_id_used);
        if (render_packed_bits(ei->context, VAEncPackedHeaderSlice, sh,
                               sh_bits, 0, track, &ntrack) != 0)
        {
            goto cleanup_err;
        }
    }
    if (render_buffer(ei->context, VAEncSliceParameterBufferType,
                      sizeof(slice), &slice, track, &ntrack) != 0)
    {
        goto cleanup_err;
    }

    if (vaEndPicture(g_va_dpy, ei->context) != VA_STATUS_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi: vaEndPicture failed");
        goto cleanup_err;
    }
    for (i = 0; i < ntrack; i++)
    {
        vaDestroyBuffer(g_va_dpy, track[i]);
    }
    ntrack = 0;

    /* The picture is submitted; its result is known now, and vaapi_finish()
       only downgrades it if the copy-out fails. */
    rv = is_idr ? KEY_FRAME_ENCODED : INCREMENTAL_FRAME_ENCODED;

    /* A non-reference picture changes nothing: it does not enter the DPB,
       and per H.264 7.4.3 it does not advance PrevRefFrameNum, so the next
       reference picture reuses this frame_num. */
    if (is_ltr)
    {
        /* Replaces the previous long-term aux. It does not enter the
           short-term DPB -- long-term references are exempt from the sliding
           window, which is the whole point -- but frame_num still advances,
           since this is a reference picture. */
        ei->ref_pic[view] = curr;
        ei->have_ref[view] = 1;
        ei->ltr_pic = curr;
        ei->have_ltr = 1;
        ei->cur_recon_aux ^= 1;
        ei->frame_num = (ei->frame_num + 1) & 15;
    }
    else if (is_ref)
    {
        ei->ref_pic[view] = curr;
        ei->have_ref[view] = 1;
        if (is_idr)
        {
            ei->ndpb = 0;
        }
        nref = (ei->nviews > 1 && !ei->single_ref && !ei->use_ltr) ? 2 : 1;
        for (i = (ei->ndpb < nref ? ei->ndpb : nref - 1); i > 0; i--)
        {
            ei->dpb[i] = ei->dpb[i - 1];
        }
        ei->dpb[0] = curr;
        if (ei->ndpb < nref)
        {
            ei->ndpb++;
        }
        /* Surfaces 0/1 ping-pong for the main view, 2/3 for the aux. Each
           view needs its own pair: it predicts from its own previous
           picture, so writing the new one must not land on the reference it
           is about to use. A non-reference aux (LTR off, chroma interval
           above 1) takes surface 3 as scratch instead, which cannot collide
           because cur_recon_aux is unused in that mode. */
        if ((view == 1) && (ei->nviews > 1))
        {
            ei->cur_recon_aux ^= 1;
        }
        else
        {
            ei->cur_recon ^= 1;
        }
        ei->frame_num = (ei->frame_num + 1) & 15;
    }
    ei->poc += 2;
    if (is_idr)
    {
        ei->idr_pic_id = (ei->idr_pic_id + 1) & 0xffff;
    }
    ei->frameCount++;

    /* Submission is done. With two views the caller submits both before
       waiting on either, so the GPU can overlap them; the wait and the
       copy-out then happen in a second pass. */
    if (defer)
    {
        ei->pending_rv[view] = rv;
        return rv;
    }
    return vaapi_finish(ei, flags, cdata, cdata_bytes);

cleanup_err:
    for (i = 0; i < ntrack; i++)
    {
        vaDestroyBuffer(g_va_dpy, track[i]);
    }
    vaEndPicture(g_va_dpy, ei->context);
    return ENCODER_ERROR;
}

/*****************************************************************************/
enum encoder_result
xrdp_accel_assist_vaapi_encode(struct enc_info *ei, int tex,
                               void *cdata, int *cdata_bytes,
                               int flags)
{
    (void) tex;
    return vaapi_submit(ei, cdata, cdata_bytes, flags, 0);
}

/*****************************************************************************/
/* Encode both AVC444 views for one frame, submitting each before waiting on
   either. The two are independent within a frame -- with a long-term
   reference the main predicts from the previous main and the aux from the
   previous aux -- so the only ordering that matters is submission order to
   the context, which the driver preserves. Waiting on each in turn instead
   cost a full CPU round-trip to the GPU on every chroma frame, and those are
   the frames that overrun the budget: 14.3 ms main plus 18.7 ms aux against
   a 43 ms period at 3008x2000, worse at the tail. */
enum encoder_result
xrdp_accel_assist_vaapi_encode_dual(struct enc_info *ei,
                                    void *cdata1, int *cdata1_bytes,
                                    void *cdata2, int *cdata2_bytes,
                                    int flags)
{
    enum encoder_result rv;
    int aux_flags = (flags & ~XH_ENC_FLAGS_FORCEIDR) | XH_ENC_FLAGS_AUXVIEW;

    rv = vaapi_submit(ei, cdata1, cdata1_bytes,
                      flags & ~XH_ENC_FLAGS_AUXVIEW, 1);
    if (rv == ENCODER_ERROR)
    {
        return ENCODER_ERROR;
    }
    if (vaapi_submit(ei, cdata2, cdata2_bytes, aux_flags, 1) == ENCODER_ERROR)
    {
        /* The main view is already submitted; drain it so the coded buffer
           and the surface do not stay busy, then fail the frame. */
        vaapi_finish(ei, flags & ~XH_ENC_FLAGS_AUXVIEW, cdata1, cdata1_bytes);
        return ENCODER_ERROR;
    }
    rv = vaapi_finish(ei, flags & ~XH_ENC_FLAGS_AUXVIEW, cdata1, cdata1_bytes);
    if (rv == ENCODER_ERROR)
    {
        vaapi_finish(ei, aux_flags, cdata2, cdata2_bytes);
        return ENCODER_ERROR;
    }
    if (vaapi_finish(ei, aux_flags, cdata2, cdata2_bytes) == ENCODER_ERROR)
    {
        return ENCODER_ERROR;
    }
    return rv;
}
