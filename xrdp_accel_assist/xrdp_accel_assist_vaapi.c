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
 * VA-API hardware H.264 encoder for xrdp_accel_assist.
 *
 * The shaders convert the bound screen pixmap to an NV12-layout GL texture,
 * which is exported as a dma-buf (EGL_MESA_image_dma_buf_export) and imported
 * into libva as an NV12 surface, zero-copy.
 *
 * libva only encodes slice data. SPS, PPS and slice headers are written here
 * as packed headers, because AVC444 needs control the parameter buffers do
 * not give: two views in one sequence, explicitly named long-term
 * references, MMCO and reference list modification.
 *
 * Drivers differ in entrypoint (EncSliceLP or EncSlice) and profile (High or
 * Main), so init probes a preference list of profile/entrypoint pairs.
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

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

/* EGL display/context from the EGL interface backend; dma-buf export
   needs INF_EGL. */
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
/* The AVC444 aux Y plane carries chroma, which luma quantisation makes
   noisy; a QP gap between the views shows as stripes once the client
   interleaves them. */
#define XH_VAAPI_DEFAULT_AUX_QP 18

/* Reconstructed-picture surfaces: a ping-pong pair per view (0/1 main,
   2/3 aux). AVC420 uses 0/1 only. */
#define XH_VAAPI_NUM_RECON 4

/* render node, can be overridden with XRDP_VAAPI_DEVICE */
static char g_default_dev[] = "/dev/dri/renderD128";

static int g_drm_fd = -1;
static VADisplay g_va_dpy = NULL;

/* H.264 profile/entrypoint chosen by xrdp_accel_assist_vaapi_init(). */
static VAProfile g_va_profile = VAProfileH264High;
static VAEntrypoint g_va_entrypoint = VAEntrypointEncSliceLP;
static int g_profile_idc = 100;     /* 100 = High, 77 = Main */
static int g_transform_8x8 = 1;     /* High profile only */

/* Preference order: High for the 8x8 transform, low-power entrypoint
   where it exists. No Baseline: the stream uses CABAC. */
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
    int deblock_view[2]; /* 0 = disable_deblocking_filter_idc 1 */
    /* Constant QP unless XRDP_VAAPI_BITRATE (kbit/s) selects VBR. */
    int rc_mode;         /* VA_RC_CQP or VA_RC_VBR */
    int bitrate_kbps;    /* XRDP_VAAPI_BITRATE, 0 = CQP */

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

    /* One sequence for both views: frame_num runs across every picture. */
    int cur_recon;                 /* index into recon_surfaces */
    int frame_num;                 /* H.264 frame_num for the next picture */
    int poc;                       /* picture order count for the next pic */
    int idr_pic_id;                /* increments per IDR */
    int have_ref[2];               /* is ref_pic[view] valid */
    VAPictureH264 ref_pic[2];      /* each view's own previous picture */
    int ndpb;                      /* live entries in dpb[] */
    VAPictureH264 dpb[1];          /* the short-term reference, if any */
    int cur_recon_aux;             /* 0/1, selects recon_surfaces[2 + n] */
    /* XRDP_VAAPI_TIMING=1: GPU encode time per view. */
    int timing;
    int t_count[2];
    int t_total_us[2];
    int t_max_us[2];
    int t_sync_us[2];
    unsigned int t_begin_ms[2];
    enum encoder_result pending_rv[2];
    /* Dual-LTR: each view's previous picture; LongTermFrameIdx is the view
       index (main 0, aux 1). */
    int have_ltrv[2];
    VAPictureH264 ltrv[2];
    /* Dual-LTR: MMCO 4 already sent this sequence. */
    int lt_max_sent;
};

/* Minimal H.264 RBSP bit writer (MSB first) */

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
/* The H.264 level to declare, from the picture size and the fastest
   capture rate (H.264 Table A-1). Decoders size their buffers from it and
   may refuse hardware decode when the frames exceed it. */
static int
h264_level_for(int width, int height)
{
    /* level_idc, MaxFS (macroblocks), MaxMBPS */
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
    /* Beyond level 6.2: declare the highest. */
    return 62;
}

/*****************************************************************************/
/* Reference frames to declare: AVC420 one; AVC444 one per view, plus one
   more than it uses. A client that drops the aux view leaves frame_num gaps,
   and the decoder must infer a short-term frame for each (H.264 8.2.5.2).
   With both slots long-term there is nothing for the sliding window to
   evict, and decoding fails. The spare slot is never filled. */
static int
xrdp_accel_assist_vaapi_num_ref(struct enc_info *ei)
{
    if (ei->nviews < 2)
    {
        return 1;
    }
    return 3;
}

/*****************************************************************************/
static int
build_sps(unsigned char *out, struct enc_info *ei)
{
    int num_ref = xrdp_accel_assist_vaapi_num_ref(ei);

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
    bs_put(&b, 8, g_profile_idc);  /* High (100), else Main (77) */
    bs_put(&b, 8, 0);              /* constraint flags + reserved zero */
    bs_put(&b, 8, h264_level_for(ei->width, ei->height));
    bs_ue(&b, 0);                  /* seq_parameter_set_id */
    if (g_profile_idc >= 100)
    {
        /* High-only fields, H.264 7.3.2.1.1; Main must not emit them. */
        bs_ue(&b, 1);              /* chroma_format_idc = 4:2:0 */
        bs_ue(&b, 0);              /* bit_depth_luma_minus8 = 8-bit */
        bs_ue(&b, 0);              /* bit_depth_chroma_minus8 = 8-bit */
        bs_put(&b, 1, 0);          /* qpprime_y_zero_transform_bypass_flag */
        bs_put(&b, 1, 0);          /* seq_scaling_matrix_present_flag */
    }
    bs_ue(&b, 0);                  /* log2_max_frame_num_minus4 */
    bs_ue(&b, 2);                  /* pic_order_cnt_type = 2 */
    /* Must match seq.max_num_ref_frames. */
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
    /* VUI with bitstream restriction only. The stream is I/P with
       pic_order_cnt_type 2, so max_num_reorder_frames=0 lets a High profile
       decoder output each frame at once instead of filling its DPB first.
       max_dec_frame_buffering must be >= max_num_ref_frames (E.2.1); strict
       decoders (e.g. WebCodecs) output nothing otherwise. */
    bs_put(&b, 1, 1);              /* vui_parameters_present_flag = 1 */
    bs_put(&b, 1, 0);              /* aspect_ratio_info_present_flag */
    bs_put(&b, 1, 0);              /* overscan_info_present_flag */
    /* Full-range BT.709, matching the shader matrix
       (g_rgb2yux_matrix[1] in xrdp_accel_assist_x11.c). Unsignalled, a
       decoder may assume limited range and crush black and white. */
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
    /* pic_init_qp is the main view's QP; the aux view uses
       slice_qp_delta. */
    bs_se(&b, ei->qp_view[0] - 26); /* pic_init_qp_minus26 */
    bs_se(&b, 0);                  /* pic_init_qs_minus26 */
    bs_se(&b, 0);                  /* chroma_qp_index_offset */
    bs_put(&b, 1, 1);              /* deblocking_filter_control_present_flag */
    bs_put(&b, 1, 0);              /* constrained_intra_pred_flag */
    bs_put(&b, 1, 0);              /* redundant_pic_cnt_present_flag */
    if (g_transform_8x8)
    {
        /* High profile only: 8x8 transform flag. */
        bs_put(&b, 1, 1);          /* transform_8x8_mode_flag = 1 */
        bs_put(&b, 1, 0);          /* pic_scaling_matrix_present_flag = 0 */
        bs_se(&b, 0);              /* second_chroma_qp_index_offset = 0 */
    }
    bs_trailing(&b);
    return build_nal(out, 3, 8, rbsp, b.bitpos / 8);
}

/*****************************************************************************/
/* Build the slice NAL header and slice header, H.264 7.3.3.

   Written here because iHD predicts from a long-term reference when asked
   but does not emit the MMCO that marks it, so the decoder's DPB would
   diverge. Everything else must match what the driver encodes, given the
   SPS/PPS above: 4-bit frame_num, POC type 2, frames only, CABAC, no
   weighted prediction, deblocking control present.

   ref_lt_idx >= 0 (dual-LTR) names that long-term picture as the only
   list entry; for frame coding LongTermPicNum == LongTermFrameIdx
   (8.2.4.1). ref_lt_idx < 0 keeps the default list order. cur_lt_idx is
   the index this picture claims when is_ltr.

   Returns the length in bits; the header is not byte-aligned. */
static int
build_slice_header(unsigned char *out, struct enc_info *ei, int is_idr,
                   int is_ltr, int slice_type, int nref,
                   int qp, int deblock_idc, int idr_pic_id,
                   int ref_lt_idx, int cur_lt_idx, int send_lt_max)
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
        if (ref_lt_idx >= 0)
        {
            /* One active entry, so nothing else (including a decoder-
               inferred frame) is reachable from this slice. */
            bs_put(&b, 1, 1);          /* ref_pic_list_modification_flag_l0 */
            bs_ue(&b, 2);              /* idc 2: long_term_pic_num follows */
            bs_ue(&b, ref_lt_idx);     /* LongTermPicNum */
            bs_ue(&b, 3);              /* idc 3: end of the list */
        }
        else
        {
            bs_put(&b, 1, 0);          /* ref_pic_list_modification_flag_l0 */
        }
    }
    if (is_idr)
    {
        bs_put(&b, 1, 0);          /* no_output_of_prior_pics_flag */
        /* An IDR marks itself long-term with index 0, the head of the
           main chain. */
        bs_put(&b, 1, is_ltr ? 1 : 0); /* long_term_reference_flag */
    }
    else if (is_ltr)
    {
        /* Mark long-term at cur_lt_idx. MMCO 4 raises
           MaxLongTermFrameIdx, which MMCO 6 needs and every IDR resets;
           nothing lowers it again, so it is sent once per
           sequence (send_lt_max). The ceiling covers both views' indexes
           so one view's marking never evicts the other's chain. */
        bs_put(&b, 1, 1);          /* adaptive_ref_pic_marking_mode_flag */
        if (send_lt_max)
        {
            bs_ue(&b, 4);          /* MMCO 4 */
            bs_ue(&b, 2);          /* max_long_term_frame_idx_plus1 */
        }
        bs_ue(&b, 6);              /* MMCO 6: mark current long-term */
        bs_ue(&b, cur_lt_idx);     /* long_term_frame_idx */
        bs_ue(&b, 0);              /* MMCO 0: end of list */
    }
    else
    {
        bs_put(&b, 1, 0);          /* sliding window */
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

    /* Emulation prevention is left to the driver (has_emulation_bytes 0),
       since the header runs on into the slice data. */
    o = 0;
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 1;
    out[o++] = (unsigned char) ((3 << 5) | (is_idr ? 5 : 1));
    nbytes = (b.bitpos + 7) / 8;
    for (i = 0; i < nbytes; i++)
    {
        out[o + i] = rbsp[i];
    }
    return o * 8 + b.bitpos;
}

/*****************************************************************************/
/* Returns 1 if the driver offers this profile/entrypoint pair, and sets
   *attribs_ok if it also supports CQP and packed SPS/PPS/slice headers. */
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
    g_drm_fd = g_file_open_ex(dev, 1, 1, 0, 0);
    if (g_drm_fd < 0)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi_init: open %s failed", dev);
        return 1;
    }
    g_va_dpy = vaGetDisplayDRM(g_drm_fd);
    if (g_va_dpy == NULL)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi_init: vaGetDisplayDRM failed");
        g_file_close(g_drm_fd);
        g_drm_fd = -1;
        return 1;
    }
    va_status = vaInitialize(g_va_dpy, &major, &minor);
    if (va_status != VA_STATUS_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi_init: vaInitialize failed %d", va_status);
        vaTerminate(g_va_dpy);
        g_va_dpy = NULL;
        g_file_close(g_drm_fd);
        g_drm_fd = -1;
        return 1;
    }
    LOG(LOG_LEVEL_INFO, "vaapi_init: VA-API %d.%d driver '%s'",
        major, minor, vaQueryVendorString(g_va_dpy));

    /* First profile/entrypoint pair the driver offers. */
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
        g_file_close(g_drm_fd);
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
/* Export the NV12-layout GL texture as a dma-buf. The shader renders a
   single R8 texture of height * 3 / 2: Y, then interleaved UV. */
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
        /* The import assumes linear; a tiled export would not fail, it
           would decode as garbage. */
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
/* Import the single-plane R8 dma-buf as an NV12 VA surface, both planes
   in one object at different offsets. */
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

    /* Per-view QP; applied per picture since both views share a
       sequence. */
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

    /* Under VBR the QPs become the initial and maximum quantiser. */
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

    /* Deblocking always helps the main view. For the aux view: v1 tiles
       chroma in 16-row groups, so filtering smooths across false edges;
       v2 holds a coherent U|V chroma image, which deblocks normally. */
    lei->deblock_view[0] = 1;
    lei->deblock_view[1] = xrdp_accel_assist_x11_avc444_v2();

    qp_str = g_getenv("XRDP_VAAPI_TIMING");
    lei->timing = (qp_str != NULL && g_atoi(qp_str) != 0);

    /* Reference structure: each AVC444 view predicts from its own previous
       picture, both long-term (main on LongTermFrameIdx 0, aux on 1, so the
       aux survives the main pictures of skipped chroma frames), and every
       slice names its single reference with ref_pic_list_modification.

       This makes the aux view droppable by a client that does not use it
       (rustguac). With the shared default list, a main macroblock could pick
       an aux reference, and a dropped aux leaves frame_num gaps whose
       inferred short-term frames (8.2.5.2) shift the list. Naming the
       reference by long_term_pic_num avoids both. Droppability also needs
       the spare reference slot; see xrdp_accel_assist_vaapi_num_ref(). */

    LOG(LOG_LEVEL_INFO, "vaapi: encoder %dx%d, %d view(s), QP %d/%d, "
        "deblock %d/%d, rc %s, bitrate %d kbit/s, level %d",
        width, height, lei->nviews, lei->qp_view[0], lei->qp_view[1],
        lei->deblock_view[0], lei->deblock_view[1],
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

    /* H.264 encode config: rate control and packed headers */
    g_memset(config_attrib, 0, sizeof(config_attrib));
    config_attrib[0].type = VAConfigAttribRateControl;
    config_attrib[0].value = lei->rc_mode;
    config_attrib[1].type = VAConfigAttribEncPackedHeaders;
    /* Packed slice header as well, to carry the long-term MMCO. */
    config_attrib[1].value = VA_ENC_PACKED_HEADER_SEQUENCE |
                             VA_ENC_PACKED_HEADER_PICTURE |
                             VA_ENC_PACKED_HEADER_SLICE;
    va_status = vaCreateConfig(g_va_dpy, g_va_profile, g_va_entrypoint,
                               config_attrib, 2, &lei->config);
    if (va_status != VA_STATUS_SUCCESS && lei->rc_mode != VA_RC_CQP)
    {
        /* Fall back to constant QP rather than fail the session. */
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
    /* One coded buffer per view: both can be in flight. */
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
/* Create a buffer, queue it, and remember its id for cleanup. */
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
/* Queue one VAEncMiscParameter payload (common header + type struct). */
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
/* Queue a packed header (parameter buffer + data buffer). */
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
/* SPS/PPS are whole NAL units with emulation bytes already inserted. */
static int
render_packed(VAContextID ctx, int htype, unsigned char *data, int bytes,
              VABufferID *track, int *ntrack)
{
    return render_packed_bits(ctx, htype, data, bytes * 8, 1, track, ntrack);
}

/*****************************************************************************/
/* XRDP_VAAPI_DUMP_STREAM=<prefix>: diagnostic capture of the encoded
   stream. Keeps the most recent stream in two ring files of 64 MB
   each, plus an unrotated <prefix>.index
   with one line per access unit:

     <seq> <elapsed ms> <view> <idr> <bytes> <ring file> <offset> */
static void
xrdp_accel_assist_vaapi_dump(int view, const void *cdata, int total)
{
    static const char *prefix = NULL;
    static int checked = 0;
    static int cap_bytes = 0;
    static int ring = 0;
    static int ring_bytes = 0;
    static int seq = 0;
    static unsigned int t0 = 0;
    const unsigned char *p = (const unsigned char *) cdata;
    char filename[512];
    char line[256];
    int is_idr;
    int off;
    int fd;
    int i;

    if (!checked)
    {
        checked = 1;
        prefix = g_getenv("XRDP_VAAPI_DUMP_STREAM");
        cap_bytes = 64 * 1024 * 1024;
        t0 = g_get_elapsed_ms();
        if (prefix != NULL)
        {
            LOG(LOG_LEVEL_INFO, "vaapi: dumping the stream to %s.{0,1}.h264, "
                "%d MB a ring file, index in %s.index",
                prefix, cap_bytes / (1024 * 1024), prefix);
        }
    }
    if (prefix == NULL || total <= 0)
    {
        return;
    }

    /* Start codes are always four bytes here. */
    is_idr = 0;
    for (i = 0; i + 4 < total; i++)
    {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1)
        {
            if ((p[i + 4] & 0x1f) == 5)
            {
                is_idr = 1;
                break;
            }
            i += 3;
        }
    }

    /* Roll over before writing, so a picture is never split. */
    if (ring_bytes > 0 && ring_bytes + total > cap_bytes)
    {
        ring ^= 1;
        ring_bytes = 0;
    }
    g_snprintf(filename, sizeof(filename) - 1, "%s.%d.h264", prefix, ring);
    /* Truncate on the first write of each pass over a ring file. */
    fd = g_file_open_ex(filename, 0, 1, 1, ring_bytes == 0);
    if (fd < 0)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi: cannot open %s", filename);
        return;
    }
    if (ring_bytes > 0)
    {
        g_file_seek_end(fd, 0);
    }
    off = ring_bytes;
    if (g_file_write(fd, (const char *) cdata, total) != total)
    {
        LOG(LOG_LEVEL_ERROR, "vaapi: short write on %s", filename);
    }
    else
    {
        ring_bytes += total;
    }
    g_file_close(fd);

    g_snprintf(filename, sizeof(filename) - 1, "%s.index", prefix);
    fd = g_file_open_ex(filename, 0, 1, 1, 0);
    if (fd >= 0)
    {
        g_file_seek_end(fd, 0);
        g_snprintf(line, sizeof(line) - 1, "%d %u %d %d %d %d %d\n",
                   seq, g_get_elapsed_ms() - t0, view, is_idr, total,
                   ring, off);
        g_file_write(fd, line, g_strlen(line));
        g_file_close(fd);
    }
    seq++;
}

/*****************************************************************************/
/* Second half of an encode: wait for the GPU and copy the bitstream out,
   split from submission so both AVC444 views can be in flight. */
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
    xrdp_accel_assist_vaapi_dump(view, cdata, total);
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
    int sh_bits;
    int is_ltr;
    int cur_lt_idx;  /* LongTermFrameIdx this picture claims, if is_ltr */
    int ref_lt_idx;  /* LongTermFrameIdx it predicts from, -1 = default list */
    int force_intra; /* code as a non-IDR I picture: no reference list at all */
    int send_lt_max; /* emit MMCO 4, raising MaxLongTermFrameIdx for the sequence */

    /* Both AVC444 views share one H.264 sequence because the client decodes
       them with a single decoder context. */
    view = (flags & XH_ENC_FLAGS_AUXVIEW) ? 1 : 0;
    qp = ei->qp_view[view];

    /* glFlush, not glFinish: the dma-buf's implicit sync orders the GL
       writes before the VA-API encode. */
    glFlush();

    w_mbs = (ei->width + 15) / 16;
    h_mbs = (ei->height + 15) / 16;
    is_idr = (flags & XH_ENC_FLAGS_FORCEIDR) || (ei->frameCount == 0);
    ntrack = 0;

    /* An IDR resets the sequence, so only the first view of a frame can be
       one. */
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
        ei->have_ltrv[0] = 0;
        ei->have_ltrv[1] = 0;
        ei->lt_max_sent = 0;
    }
    idr_id_used = ei->idr_pic_id;

    /* AVC444: both views long-term, each on its own index (the view), the
       short-term DPB unused. AVC420 is short-term. */
    is_ltr = ei->nviews > 1;
    cur_lt_idx = view;
    /* MMCO 4 once per sequence, set only after a successful submit. */
    send_lt_max = !ei->lt_max_sent;
    /* This view's own long-term picture; before it exists, the IDR. */
    ref_lt_idx = -1;
    force_intra = 0;
    if (is_ltr && !is_idr)
    {
        if (ei->have_ltrv[view])
        {
            ref_lt_idx = view;
        }
        else
        {
            /* First aux picture of a sequence: code it intra rather than
               predict from the main IDR, so the two chains never overlap.
               A plain I slice, not an IDR. */
            force_intra = 1;
        }
    }

    g_memset(&curr, 0, sizeof(curr));
    if (view == 1 && ei->nviews > 1)
    {
        curr.picture_id = ei->recon_surfaces[2 + ei->cur_recon_aux];
    }
    else
    {
        curr.picture_id = ei->recon_surfaces[ei->cur_recon];
    }
    /* For a long-term reference frame_idx is LongTermFrameIdx: the
       view. */
    curr.frame_idx = is_ltr ? cur_lt_idx : ei->frame_num;
    curr.flags = is_ltr ? VA_PICTURE_H264_LONG_TERM_REFERENCE
                 : VA_PICTURE_H264_SHORT_TERM_REFERENCE;
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
        seq.max_num_ref_frames = xrdp_accel_assist_vaapi_num_ref(ei);
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

            /* VBR: headroom between ceiling and target lets a busy frame
               borrow bits. Frame skip stays off: it would break the AVC444
               view pairing. */
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
            /* One second of coded picture buffer. */
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
        /* The SPS as sent, once per session, at debug level. */
        {
            static int logged = 0;

            if (!logged)
            {
                char hex[256];
                int i;

                logged = 1;
                hex[0] = 0;
                for (i = 0; i < sps_len && i < 60; i++)
                {
                    g_snprintf(hex + i * 3, 4, "%2.2x ", sps[i]);
                }
                LOG(LOG_LEVEL_DEBUG, "vaapi: SPS %d bytes: %s", sps_len, hex);
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
    /* The DPB as the driver models it: short-term references most recent
       first (AVC420), or the two views' long-term ones (AVC444). */
    nref = 0;
    for (i = 0; i < ei->ndpb; i++)
    {
        pic.ReferenceFrames[nref++] = ei->dpb[i];
    }
    for (i = 0; ei->nviews > 1 && i < 2; i++)
    {
        if (ei->have_ltrv[i])
        {
            pic.ReferenceFrames[nref++] = ei->ltrv[i];
        }
    }
    pic.coded_buf = ei->coded_buf[view];
    pic.pic_parameter_set_id = 0;
    pic.seq_parameter_set_id = 0;
    pic.last_picture = 0;
    pic.frame_num = ei->frame_num;
    /* Must equal the packed PPS pic_init_qp. */
    pic.pic_init_qp = ei->qp_view[0];
    pic.num_ref_idx_l0_active_minus1 = 0;
    pic.num_ref_idx_l1_active_minus1 = 0;
    /* Must match the packed PPS. */
    pic.chroma_qp_index_offset = 0;
    pic.second_chroma_qp_index_offset = 0;
    pic.pic_fields.bits.idr_pic_flag = is_idr ? 1 : 0;
    pic.pic_fields.bits.reference_pic_flag = 1;
    pic.pic_fields.bits.entropy_coding_mode_flag = 1; /* CABAC */
    pic.pic_fields.bits.deblocking_filter_control_present_flag = 1;
    /* High profile only; Main forbids 8x8 transforms. */
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
    slice.slice_type = (is_idr || force_intra) ? 2 : 0; /* I : P */
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
    nref = 0;
    if (ref_lt_idx >= 0)
    {
        /* AVC444: one entry, this view's long-term picture (or the IDR),
           named the same way in the packed slice header. */
        slice.RefPicList0[nref++] = ei->ltrv[ref_lt_idx];
    }
    else if (!is_idr)
    {
        /* AVC420: short-term by descending PicNum, the default order */
        for (i = 0; i < ei->ndpb; i++)
        {
            slice.RefPicList0[nref++] = ei->dpb[i];
        }
    }
    if (nref == 0 && slice.slice_type != 2)
    {
        LOG(LOG_LEVEL_WARNING, "vaapi: P picture with no reference, "
            "forcing intra");
        slice.slice_type = 2;
    }
    /* One entry for the first aux after an IDR, two thereafter. */
    slice.num_ref_idx_active_override_flag = 1;
    slice.num_ref_idx_l0_active_minus1 = (nref > 0) ? nref - 1 : 0;
    pic.num_ref_idx_l0_active_minus1 = slice.num_ref_idx_l0_active_minus1;
    slice.cabac_init_idc = 0;
    slice.slice_qp_delta = qp - ei->qp_view[0];
    slice.disable_deblocking_filter_idc = ei->deblock_view[view] ? 0 : 1;
    slice.slice_alpha_c0_offset_div2 = 0;
    slice.slice_beta_offset_div2 = 0;
    slice.direct_spatial_mv_pred_flag = 0;
    /* Must be rendered before the slice parameter buffer. */
    sh_bits = build_slice_header(sh, ei, is_idr, is_ltr, slice.slice_type,
                                 nref, qp, slice.disable_deblocking_filter_idc,
                                 idr_id_used, ref_lt_idx, cur_lt_idx,
                                 send_lt_max);
    if (render_packed_bits(ei->context, VAEncPackedHeaderSlice, sh,
                           sh_bits, 0, track, &ntrack) != 0)
    {
        goto cleanup_err;
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

    rv = is_idr ? KEY_FRAME_ENCODED : INCREMENTAL_FRAME_ENCODED;

    if (is_ltr)
    {
        /* Long-term: exempt from the sliding window, but frame_num still
           advances. */
        ei->ref_pic[view] = curr;
        ei->have_ref[view] = 1;
        ei->ltrv[cur_lt_idx] = curr;
        ei->have_ltrv[cur_lt_idx] = 1;
        /* An IDR cannot carry MMCO 4 and leaves the ceiling at 0, so it
           must not consume the flag. */
        if (!is_idr && send_lt_max)
        {
            ei->lt_max_sent = 1;
        }
        /* Write to the surface this view's previous picture does not
           hold. */
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
    else
    {
        /* Short-term: the one previous picture of this chain. */
        ei->ref_pic[view] = curr;
        ei->have_ref[view] = 1;
        ei->dpb[0] = curr;
        ei->ndpb = 1;
        /* Ping-pong 0/1 for main, 2/3 for aux, so a view never overwrites
           the reference it is about to use. */
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
/* Submit both AVC444 views before waiting on either; they are
   independent within a frame, so the GPU can overlap them. */
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
        /* Drain the submitted main view, then fail the frame. */
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
