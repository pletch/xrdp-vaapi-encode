
#ifndef _XRDP_ENCODER_H
#define _XRDP_ENCODER_H

#include "arch.h"
#include "fifo.h"
#include "xrdp_client_info.h"

#define ENC_IS_BIT_SET(_flags, _bit) (((_flags) & (1 << (_bit))) != 0)
#define ENC_SET_BIT(_flags, _bit) do { _flags |= (1 << (_bit)); } while (0)
#define ENC_CLR_BIT(_flags, _bit) do { _flags &= ~(1 << (_bit)); } while (0)
#define ENC_SET_BITS(_flags, _mask, _bits) \
    do { _flags &= ~(_mask); _flags |= (_bits) & (_mask); } while (0)

struct xrdp_enc_data;

typedef void *(*xrdp_encoder_h264_create_proc)(void);
typedef int (*xrdp_encoder_h264_delete_proc)(void *handle);
typedef int (*xrdp_encoder_h264_encode_proc)(
    void *handle, int session, int left, int top,
    int width, int height, int twidth, int theight,
    int format, const char *data,
    short *crects, int num_crects,
    char *cdata, int *cdata_bytes,
    int connection_type, int *flags_ptr);

/* for codec mode operations */
struct xrdp_encoder
{
    struct xrdp_mm *mm;
    int in_codec_mode;
    int codec_id;
    int codec_quality;
    int max_compressed_bytes;
    /* XRDP_GFX_FRAME_LOG=1: log one line per encoded frame, for both AVC420
       and AVC444, so the two can be compared on the same workload.

       Read from the xrdp process's own environment, like the other
       XRDP_GFX_* knobs beside it -- NOT from sesman.ini [SessionVariables],
       which only reaches the session (Xorg, xorgxrdp, accel-assist) and
       never xrdp. Set it in an xrdp.service systemd drop-in. The
       XRDP_ACCEL_AVC444 / XRDP_AVC444_* / XRDP_VAAPI_* knobs are the other
       way round: those are read in the session and do belong in
       sesman.ini. */
    int frame_log;
    tbus xrdp_encoder_event_to_proc;
    tbus xrdp_encoder_event_processed;
    tbus xrdp_encoder_term_request;
    tbus xrdp_encoder_term_done;
    struct fifo *fifo_to_proc;
    struct fifo *fifo_processed;
    tbus mutex;
    int (*process_enc)(struct xrdp_encoder *self, struct xrdp_enc_data *enc);
    void *codec_handle_rfx;
    void *codec_handle_jpg;
    void *codec_handle_h264;
    void *codec_handle_prfx_gfx[16];
    void *codec_handle_h264_gfx[16];
    int frame_id_client; /* last frame id received from client */
    int frame_id_server; /* last frame id received from Xorg */
    int frame_id_server_sent;
    /* XRDP_GFX_FRAME_LOG: how long the client takes to acknowledge a frame,
       and how deep its own queue is when it does. The client reports its
       queue depth in every ack, which is the one direct measure of whether
       it is keeping up -- the encode timings only cover our end of the pipe.
       Sent times are a ring indexed by frame id. */
    unsigned int frame_sent_ms[64];
    /* Most recent client acknowledgement round trip, milliseconds. Passed
       down to the module so it can pace capture by the client's real
       latency rather than by a figure that includes its own pacing. */
    int last_rtt_ms;
    int ack_count;
    int ack_rtt_total_ms;
    int ack_rtt_max_ms;
    int ack_qdepth_total;
    int ack_qdepth_max;
    int ack_inflight_total;
    int frames_in_flight;
    int gfx;
    int gfx_ack_off;
    const char *quants;
    int num_quants;
    int quant_idx_y;
    int quant_idx_u;
    int quant_idx_v;
    int pad0;
    xrdp_encoder_h264_create_proc xrdp_encoder_h264_create;
    xrdp_encoder_h264_delete_proc xrdp_encoder_h264_delete;
    xrdp_encoder_h264_encode_proc xrdp_encoder_h264_encode;
    int hw_accel_announced;     /* 1 once we've logged that accel-assist is
                                   feeding pre-encoded H.264 -- proves the
                                   VAAPI hardware path is active without
                                   needing to grep the xorgxrdp/accel-assist
                                   logs */
};

/* cmd_id = 0 */
struct xrdp_enc_surface_command
{
    struct xrdp_mod *mod;
    int num_drects;
    int pad0;
    short *drects;  /* 4 * num_drects */
    int num_crects;
    int pad1;
    short *crects;  /* 4 * num_crects */
    char *data;
    int left;
    int top;
    int width;
    int height;
    int flags;
    int frame_id;
};

struct xrdp_enc_gfx_cmd
{
    char *cmd;
    char *data;
    int cmd_bytes;
    int data_bytes;
};

typedef struct xrdp_enc_data XRDP_ENC_DATA;

#define ENC_DONE_FLAGS_GFX_BIT      0
#define ENC_DONE_FLAGS_FRAME_ID_BIT 1

/* used when scheduling tasks from xrdp_encoder.c */
struct xrdp_enc_data_done
{
    int comp_bytes;
    int pad_bytes;
    char *comp_pad_data;
    struct xrdp_enc_data *enc;
    int last; /* true is this is last message for enc */
    int continuation; /* true if this isn't the start of a frame */
    int x;
    int y;
    int cx;
    int cy;
    int flags; /* ENC_DONE_FLAGS_* */
    int frame_id;
};

#define ENC_FLAGS_GFX_BIT   0

/* used when scheduling tasks in xrdp_encoder.c */
struct xrdp_enc_data
{
    struct xrdp_mod *mod;
    int flags; /* ENC_FLAGS_* */
    int pad0;
    void *shmem_ptr;
    int shmem_bytes;
    int pad1;
    union _u
    {
        struct xrdp_enc_surface_command sc;
        struct xrdp_enc_gfx_cmd gfx;
    } u;
};

typedef struct xrdp_enc_data_done XRDP_ENC_DATA_DONE;

struct xrdp_encoder *
xrdp_encoder_create(struct xrdp_mm *mm);
void
xrdp_encoder_delete(struct xrdp_encoder *self);
THREAD_RV THREAD_CC
proc_enc_msg(void *arg);

#endif
