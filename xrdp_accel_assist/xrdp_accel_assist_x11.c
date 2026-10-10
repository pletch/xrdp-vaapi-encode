/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Jay Sorg 2020-2024
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

/* Currently nvenc requires GLX because NVidia's EGL does not have
 * EGL_NOK_texture_from_pixmap extension but NVidia's GLX does have
 * GLX_EXT_texture_from_pixmap.  We require one if those,
 * also, va required EGL because it used dma bufs.
 * I do not think any vendor's GLX support dma bufs */
/* Things like render on one GPU and encode with another is possible
 * but not supported now. */
/* One suggestion about dma bufs and GLX, one can use the DRI3
 * extension to get dma buffs for pixmaps */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <X11/Xlib.h>

#include <epoxy/gl.h>

#include "arch.h"
#include "os_calls.h"
#include "string_calls.h"
#include "xrdp_client_info.h"
#include "xrdp_accel_assist.h"
#include "xrdp_accel_assist_x11.h"
#include "xrdp_accel_assist_glx.h"
#include "xrdp_accel_assist_egl.h"
#include "log.h"

/* set to 1 to dump bmp files into /tmp */
#define XR_DUMP_FRAMEBUFFER 0

/* set to 1 to dump bmp files into /tmp */
#define XR_DUMP_PIXMAP 0

#if defined(XRDP_NVENC)
#include "xrdp_accel_assist_nvenc.h"
#endif

#if defined(XRDP_VAAPI)
#include "xrdp_accel_assist_vaapi.h"
#endif

/* X11 */
Display *g_display = NULL;
static int g_x_socket = 0;
int g_screen_num = 0;
static Screen *g_screen = NULL;
Window g_root_window = None;
static Visual *g_vis = NULL;
static GC g_gc;

/* encoders: nvenc or va */
struct enc_funcs
{
    int (*init)(void);
    /* NVENC encodes GL textures it is given. */
    int (*create_enc)(int width, int height, int tex, int tex_aux,
                      int tex_format, struct enc_info **ei);
    /* VA-API allocates its own surfaces and hands back GL targets over
       them, one per view. One of the two create calls is NULL. */
    int (*create_enc_targets)(int width, int height, int nviews,
                              struct enc_info **ei,
                              struct xh_enc_target *targets);
    int (*destroy_enc)(struct enc_info *ei);
    /* idr_pic_id: the stream's id for the picture if it is an IDR. The
       count lives here, per monitor, so it survives an encoder rebuild. */
    enum encoder_result (*encode)(struct enc_info *ei, int tex,
                                  void *cdata, int *cdata_bytes,
                                  int flags, int idr_pic_id);
    /* Optional: encode both AVC444 views, submitting each before waiting
       on either. NULL falls back to two encode() calls. */
    enum encoder_result (*encode_dual)(struct enc_info *ei,
                                       void *cdata1, int *cdata1_bytes,
                                       void *cdata2, int *cdata2_bytes,
                                       int flags, int idr_pic_id);
};

static struct enc_funcs g_enc_funcs[] =
{
    {
#if defined(XRDP_VAAPI)
        xrdp_accel_assist_vaapi_init,
        NULL,
        xrdp_accel_assist_vaapi_create_encoder,
        xrdp_accel_assist_vaapi_delete_encoder,
        xrdp_accel_assist_vaapi_encode,
        xrdp_accel_assist_vaapi_encode_dual
#else
        NULL, NULL, NULL, NULL, NULL, NULL
#endif
    },
    {
#if defined(XRDP_NVENC)
        xrdp_accel_assist_nvenc_init,
        xrdp_accel_assist_nvenc_create_encoder,
        NULL,
        xrdp_accel_assist_nvenc_delete_encoder,
        xrdp_accel_assist_nvenc_encode,
        xrdp_accel_assist_nvenc_encode_dual
#else
        NULL, NULL, NULL, NULL, NULL, NULL
#endif
    }
};

/* GL interface: EGL or GLX */
struct inf_funcs
{
    int (*init)(void);
    int (*create_image)(Pixmap pixmap, inf_image_t *inf_image);
    int (*destroy_image)(inf_image_t inf_image);
    int (*bind_tex_image)(inf_image_t inf_image);
    int (*release_tex_image)(inf_image_t inf_image);
};

static struct inf_funcs g_inf_funcs[] =
{
    {
        xrdp_accel_assist_inf_egl_init,
        xrdp_accel_assist_inf_egl_create_image,
        xrdp_accel_assist_inf_egl_destroy_image,
        xrdp_accel_assist_inf_egl_bind_tex_image,
        xrdp_accel_assist_inf_egl_release_tex_image
    },
    {
        xrdp_accel_assist_inf_glx_init,
        xrdp_accel_assist_inf_glx_create_image,
        xrdp_accel_assist_inf_glx_destroy_image,
        xrdp_accel_assist_inf_glx_bind_tex_image,
        xrdp_accel_assist_inf_glx_release_tex_image
    }
};

/* 0 = EGL, 1 = GLX */
/* 0 = va, 1 = nvenc */
#define INF_EGL     0
#define INF_GLX     1
#define ENC_VA      0
#define ENC_NVENC   1
static int g_inf = INF_EGL;
static int g_enc = ENC_VA;

struct mon_info
{
    int width;
    int height;
    /* Two capture buffers, so xorgxrdp can fill one while we read the
       other. xorgxrdp keeps both complete, which the full-frame AVC444 aux
       pass relies on. */
    Pixmap pixmap[2];
    inf_image_t inf_image[2];
    GLuint bmp_texture[2];
    /* What the shaders draw into, per view: [0] main, [1] AVC444 aux.
       Owned by the encoder when it allocated them (tgt_from_enc). */
    struct xh_enc_target tgt[2];
    int tgt_from_enc;
    /* The main view's target is blank or stale (a new encoder-owned
       surface): draw the next one whole, not just its damage. */
    int full_pending;
    int cur_buf;                  /* capture buffer this frame used */
    /* Damage since the aux view was last rendered, as a bounding box. A
       box, not a region: it only has to be a superset, and one that grows
       past half the frame means render everything. */
    int aux_dirty;                /* 0 = nothing accumulated */
    int aux_x1, aux_y1, aux_x2, aux_y2;
    /* When the aux view last went out (monotonic ms). */
    /* When the last IDR went out, for any reason (monotonic ms). Set at
       creation, since the first picture is an IDR. */
    unsigned int idr_last_ms;
    /* Bytes sent since the last IDR, excluding it. */
    unsigned int idr_bytes_since;
    /* A frame failed to encode (e.g. too big for the shared buffer): the
       client lacks a picture the next ones may refer to, so the next frame
       is an IDR. */
    int idr_pending;
    int idr_seq;                  /* next idr_pic_id; outlives encoders */
    int tex_format;
    GLfloat *(*get_vertices)(GLuint *vertices_bytes,
                             GLuint *vertices_pointes,
                             int num_crects, struct xh_rect *crects,
                             int left, int top, int width, int height);
    struct xh_rect viewport;
    struct enc_info *ei;
    int avc444;                   /* both views live in mi->ei, one sequence */
    int avc444_v2;                /* ChromaV2 aux layout (codec id 0x000F) */
    int enc_w;                    /* encode width; 16-aligned for v2 */
    int enc_w4;                   /* enc_w / 4: the packed viewport width */
    int pad_h;                    /* encode height; == height for v2 */
    int buf_h;                    /* the target's Y plane rows: UV starts
                                     here. VA-API: pad_h 16-aligned */
    int avc444_frame_count;
    /* Damage detection (see damage_detect): the source as last encoded,
       a cell map the compare pass renders, and its CPU copy. */
    GLuint dd_prev_texture;       /* RGBA8, dd_w x dd_h */
    GLuint dd_mask_texture;       /* RG8, dd_w x dd_h: changed, needs aux */
    GLuint dd_cell_texture;       /* RGBA8, one texel per 16x16 cell */
    GLuint dd_m4_texture;         /* RG8, one texel per 4x4 of the mask */
    int dd_w;
    int dd_h;
    int dd_cells_w;
    int dd_cells_h;
    unsigned char *dd_map;        /* dd_cells_w x dd_cells_h RGBA */
    unsigned char *dd_cand;       /* cells the damage touches */
    int dd_prev_valid;            /* 0: dd_prev_texture holds nothing */
    int force_aux;                /* send the aux view with the next frame */
    int aux_only;                 /* this frame is a catch-up: aux view only */
    /* From damage detection, for this frame only: whether its cells say
       which changed areas need the aux view, and their bounding box. */
    int dd_need_valid;
    int dd_need_any;
    int dd_need_x1, dd_need_y1, dd_need_x2, dd_need_y2;
    /* Per cell: frames in a row it changed, whether it still owes the aux
       view (it needed it while moving), and whether it gets it now. */
    unsigned char *dd_streak;     /* 1 once the cell has changed */
    unsigned int *dd_changed_ms;  /* when the cell last changed */
    unsigned int *dd_moving_ms;   /* when it last moved on its own */
    unsigned char *dd_owe;
    unsigned char *dd_auxc;
    /* The cells getting the aux view this frame as rects; -1: too many,
       use the dd_need box */
    struct xh_rect *dd_aux_rects;
    int dd_aux_n;
    int dd_aux_declare;           /* the aux view went out over that list */
};

#define MAX_MON 16

/* RDPGFX codec ids (see xrdp_egfx.h) */
#define XH_CODECID_AVC420     0x000B
#define XH_CODECID_AVC444     0x000E
#define XH_CODECID_AVC444V2   0x000F
static struct mon_info g_mons[MAX_MON];

static GLuint g_quad_vao = 0;
static GLuint g_fb = 0;

#define XH_SHADERCOPY           0
#define XH_SHADERRGB2YUV420     1
#define XH_SHADERRGB2YUV422     2
#define XH_SHADERRGB2YUV444     3
#define XH_SHADERRGB2YUV420MV   4
#define XH_SHADERRGB2YUV420AV   5
#define XH_SHADERRGB2YUV420AVV2 6
#define XH_SHADERMASKDIFF       7
#define XH_SHADERCELLMAX        8

#define XH_NUM_SHADERS 9

struct shader_info
{
    GLuint vertex_shader;
    GLuint fragment_shader;
    GLuint program;
    GLint tex_loc;
    GLint tex_size_loc;
    GLint pad_h_loc;       /* only present in the AV (aux) shader; -1 elsewhere */
    GLint bpf_loc;         /* bytes per fragment, NV12 shaders only */
    GLint y_off_loc;       /* row offset of the plane drawn, NV12 only */
    GLint prev_loc;        /* only present in the mask diff shader */
    GLint thr_loc;         /* only present in the mask diff shader */
    GLint ymath_loc;
    GLint umath_loc;
    GLint vmath_loc;
    int current_matrix;
};
static struct shader_info g_si[XH_NUM_SHADERS];

/* *INDENT-OFF* */
static const GLfloat g_vertices[] =
{
    -1.0f,  1.0f,
    -1.0f, -1.0f,
     1.0f,  1.0f,
     1.0f, -1.0f
};
/* *INDENT-ON* */

struct rgb2yuv_matrix
{
    GLfloat ymath[4];
    GLfloat umath[4];
    GLfloat vmath[4];
};

static struct rgb2yuv_matrix g_rgb2yux_matrix[3] =
{
    {
        /* yuv bt601 lagecy */
        {  66.0 / 256.0,  129.0 / 256.0,   25.0 / 256.0,   16.0 / 256.0 },
        { -38.0 / 256.0,  -74.0 / 256.0,  112.0 / 256.0,  128.0 / 256.0 },
        { 112.0 / 256.0,  -94.0 / 256.0,  -18.0 / 256.0,  128.0 / 256.0 }
    },
    {
        /* yuv bt709 full range, used in gfx h264 */
        {  54.0 / 256.0,  183.0 / 256.0,   18.0 / 256.0,    0.0 / 256.0 },
        { -29.0 / 256.0,  -99.0 / 256.0,  128.0 / 256.0,  128.0 / 256.0 },
        { 128.0 / 256.0, -116.0 / 256.0,  -12.0 / 256.0,  128.0 / 256.0 }
    },
    {
        /* yuv remotefx and gfx progressive remotefx */
        {   0.299000,       0.587000,       0.114000,       0.0 },
        {  -0.168935,      -0.331665,       0.500590,       0.5 },
        {   0.499830,      -0.418531,      -0.081282,       0.5 }
    }
};

#include "xrdp_accel_assist_shaders.c"

/*****************************************************************************/
int
xrdp_accel_assist_x11_init(void)
{
    const GLchar *vsource[XH_NUM_SHADERS];
    const GLchar *fsource[XH_NUM_SHADERS];
    GLint linked;
    GLint compiled;
    GLint vlength;
    GLint flength;
    GLuint quad_vbo;
    int index;
    int gl_ver;
    int major_opcode, first_event, first_error;

    /* x11 */
    g_display = XOpenDisplay(0);
    if (g_display == NULL)
    {
        return 1;
    }
    g_x_socket = XConnectionNumber(g_display);
    g_screen_num = DefaultScreen(g_display);
    g_screen = ScreenOfDisplay(g_display, g_screen_num);
    g_root_window = RootWindowOfScreen(g_screen);
    g_vis = XDefaultVisual(g_display, g_screen_num);
    g_gc = DefaultGC(g_display, 0);
    if (XQueryExtension(g_display, "NV-CONTROL", &major_opcode, &first_event,
                        &first_error))
    {
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_init: "
            "detected NVIDIA XServer");
        g_inf = INF_GLX;
        g_enc = ENC_NVENC;
        if (g_inf_funcs[g_inf].init() != 0)
        {
            LOG(LOG_LEVEL_ERROR, "xrdp_accel_assist_x11_init: "
                "GLX init failed");
            return 1;
        }
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_init: using GLX");
    }
    else
    {
        g_inf = INF_EGL;
        g_enc = ENC_VA;
        if (g_inf_funcs[g_inf].init() != 0)
        {
            LOG(LOG_LEVEL_ERROR, "xrdp_accel_assist_x11_init: "
                "EGL init failed");
            return 1;
        }
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_init: using EGL");
    }
    gl_ver = epoxy_gl_version();
    LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_init: gl_ver %d", gl_ver);
    if (gl_ver < 30)
    {
        LOG(LOG_LEVEL_ERROR, "xrdp_accel_assist_x11_init: "
            "gl_ver too old %d", gl_ver);
        return 1;
    }
    LOG(LOG_LEVEL_INFO, "vendor: %s",
        (const char *) glGetString(GL_VENDOR));
    LOG(LOG_LEVEL_INFO, "version: %s",
        (const char *) glGetString(GL_VERSION));
    /* create vertex array */
    glGenVertexArrays(1, &g_quad_vao);
    glBindVertexArray(g_quad_vao);
    glGenBuffers(1, &quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(g_vertices), g_vertices,
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, NULL);
    glGenFramebuffers(1, &g_fb);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glDeleteBuffers(1, &quad_vbo);

    /* create copy shader */
    vsource[XH_SHADERCOPY] = g_vs;
    fsource[XH_SHADERCOPY] = g_fs_copy;
    /* create rgb2yuv shader */
    vsource[XH_SHADERRGB2YUV420] = g_vs;
    fsource[XH_SHADERRGB2YUV420] = g_fs_rgb_to_yuv420;
    /* create rgb2yuv shader */
    vsource[XH_SHADERRGB2YUV422] = g_vs;
    fsource[XH_SHADERRGB2YUV422] = g_fs_rgb_to_yuv422;
    /* create rgb2yuv shader */
    vsource[XH_SHADERRGB2YUV444] = g_vs;
    fsource[XH_SHADERRGB2YUV444] = g_fs_rgb_to_yuv444;

    vsource[XH_SHADERRGB2YUV420MV] = g_vs;
    fsource[XH_SHADERRGB2YUV420MV] = g_fs_rgb_to_yuv420_mv;

    vsource[XH_SHADERRGB2YUV420AV] = g_vs;
    fsource[XH_SHADERRGB2YUV420AV] = g_fs_rgb_to_yuv420_av;

    vsource[XH_SHADERRGB2YUV420AVV2] = g_vs;
    fsource[XH_SHADERRGB2YUV420AVV2] = g_fs_rgb_to_yuv420_av_v2;

    vsource[XH_SHADERMASKDIFF] = g_vs;
    fsource[XH_SHADERMASKDIFF] = g_fs_mask_diff;

    vsource[XH_SHADERCELLMAX] = g_vs;
    fsource[XH_SHADERCELLMAX] = g_fs_cell_max;

    for (index = 0; index < XH_NUM_SHADERS; index++)
    {
        g_si[index].vertex_shader = glCreateShader(GL_VERTEX_SHADER);
        g_si[index].fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
        vlength = g_strlen(vsource[index]);
        flength = g_strlen(fsource[index]);
        glShaderSource(g_si[index].vertex_shader, 1,
                       &(vsource[index]), &vlength);
        glShaderSource(g_si[index].fragment_shader, 1,
                       &(fsource[index]), &flength);
        glCompileShader(g_si[index].vertex_shader);
        glGetShaderiv(g_si[index].vertex_shader, GL_COMPILE_STATUS,
                      &compiled);
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_init: "
            "vertex_shader compiled %d", compiled);
        glCompileShader(g_si[index].fragment_shader);
        glGetShaderiv(g_si[index].fragment_shader, GL_COMPILE_STATUS,
                      &compiled);
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_init: "
            "fragment_shader compiled %d", compiled);
        g_si[index].program = glCreateProgram();
        glAttachShader(g_si[index].program, g_si[index].vertex_shader);
        glAttachShader(g_si[index].program, g_si[index].fragment_shader);
        glLinkProgram(g_si[index].program);
        glGetProgramiv(g_si[index].program, GL_LINK_STATUS, &linked);
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_init: linked %d", linked);
        g_si[index].tex_loc =
            glGetUniformLocation(g_si[index].program, "tex");
        g_si[index].tex_size_loc =
            glGetUniformLocation(g_si[index].program, "tex_size");
        g_si[index].pad_h_loc =
            glGetUniformLocation(g_si[index].program, "pad_h");
        g_si[index].bpf_loc =
            glGetUniformLocation(g_si[index].program, "bpf");
        g_si[index].y_off_loc =
            glGetUniformLocation(g_si[index].program, "y_off");
        g_si[index].prev_loc =
            glGetUniformLocation(g_si[index].program, "prev");
        g_si[index].thr_loc =
            glGetUniformLocation(g_si[index].program, "thr");
        g_si[index].ymath_loc =
            glGetUniformLocation(g_si[index].program, "ymath");
        g_si[index].umath_loc =
            glGetUniformLocation(g_si[index].program, "umath");
        g_si[index].vmath_loc =
            glGetUniformLocation(g_si[index].program, "vmath");
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_init: tex_loc %d "
            "tex_size_loc %d ymath_loc %d umath_loc %d vmath_loc %d",
            g_si[index].tex_loc, g_si[index].tex_size_loc,
            g_si[index].ymath_loc, g_si[index].umath_loc,
            g_si[index].vmath_loc);
        /* set default matrix */
        glUseProgram(g_si[index].program);
        if (g_si[index].ymath_loc >= 0)
        {
            glUniform4fv(g_si[index].ymath_loc, 1, g_rgb2yux_matrix[1].ymath);
        }
        if (g_si[index].umath_loc >= 0)
        {
            glUniform4fv(g_si[index].umath_loc, 1, g_rgb2yux_matrix[1].umath);
        }
        if (g_si[index].vmath_loc >= 0)
        {
            glUniform4fv(g_si[index].vmath_loc, 1, g_rgb2yux_matrix[1].vmath);
        }
        glUseProgram(0);
    }
    g_memset(g_mons, 0, sizeof(g_mons));
    if (g_enc_funcs[g_enc].init() != 0)
    {
        LOG(LOG_LEVEL_ERROR, "xrdp_accel_assist_x11_init: "
            "encoder init failed");
        return 1;
    }
    return 0;
}

/*****************************************************************************/
int
xrdp_accel_assist_x11_get_wait_objs(intptr_t *objs, int *obj_count)
{
    objs[*obj_count] = g_x_socket;
    (*obj_count)++;
    return 0;
}

/*****************************************************************************/
int
xrdp_accel_assist_x11_check_wait_objs(void)
{
    XEvent xevent;

    while (XPending(g_display) > 0)
    {
        LOG_DEVEL(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_check_wait_objs: "
                  "loop");
        XNextEvent(g_display, &xevent);
    }
    return 0;
}

/*****************************************************************************/
int
xrdp_accel_assist_x11_delete_all_pixmaps(void)
{
    int index;
    struct mon_info *mi;

    for (index = 0; index < MAX_MON; index++)
    {
        mi = g_mons + index;
        if (mi->pixmap[0] != 0)
        {
            int buf;

            g_enc_funcs[g_enc].destroy_enc(mi->ei);
            mi->ei = NULL;
            if (!mi->tgt_from_enc)
            {
                for (buf = 0; buf < 2; buf++)
                {
                    if (mi->tgt[buf].tex[0] != 0)
                    {
                        glDeleteTextures(1, &(mi->tgt[buf].tex[0]));
                    }
                }
            }
            g_memset(mi->tgt, 0, sizeof(mi->tgt));
            for (buf = 0; buf < 2; buf++)
            {
                glDeleteTextures(1, &(mi->bmp_texture[buf]));
                g_inf_funcs[g_inf].destroy_image(mi->inf_image[buf]);
                XFreePixmap(g_display, mi->pixmap[buf]);
                mi->pixmap[buf] = 0;
            }
        }
        if (mi->dd_prev_texture != 0)
        {
            glDeleteTextures(1, &(mi->dd_prev_texture));
            glDeleteTextures(1, &(mi->dd_mask_texture));
            glDeleteTextures(1, &(mi->dd_cell_texture));
            glDeleteTextures(1, &(mi->dd_m4_texture));
            mi->dd_m4_texture = 0;
            mi->dd_prev_texture = 0;
            mi->dd_mask_texture = 0;
            mi->dd_cell_texture = 0;
        }
        g_free(mi->dd_map);
        g_free(mi->dd_cand);
        mi->dd_map = NULL;
        mi->dd_cand = NULL;
        g_free(mi->dd_streak);
        g_free(mi->dd_changed_ms);
        g_free(mi->dd_moving_ms);
        mi->dd_moving_ms = NULL;
        g_free(mi->dd_owe);
        g_free(mi->dd_auxc);
        g_free(mi->dd_aux_rects);
        mi->dd_streak = NULL;
        mi->dd_changed_ms = NULL;
        mi->dd_owe = NULL;
        mi->dd_auxc = NULL;
        mi->dd_aux_rects = NULL;
        mi->dd_w = 0;
        mi->dd_h = 0;
        mi->dd_prev_valid = 0;
    }
    return 0;
}

/*****************************************************************************/
static GLfloat *
get_vertices_all(GLuint *vertices_bytes, GLuint *vertices_pointes,
                 int num_crects, struct xh_rect *crects,
                 int left, int top, int width, int height)
{
    GLfloat *vertices;

    (void)num_crects;
    (void)crects;
    (void)width;
    (void)height;

    vertices = g_new(GLfloat, 12);
    if (vertices == NULL)
    {
        return NULL;
    }
    vertices[0]  = -1;
    vertices[1]  =  1;
    vertices[2]  = -1;
    vertices[3]  = -1;
    vertices[4]  =  1;
    vertices[5]  =  1;
    vertices[6]  = -1;
    vertices[7]  = -1;
    vertices[8]  =  1;
    vertices[9]  =  1;
    vertices[10] =  1;
    vertices[11] = -1;
    *vertices_bytes = sizeof(GLfloat) * 12;
    *vertices_pointes = 6;
    return vertices;
}

/*****************************************************************************/
static GLfloat *
get_vertices420(GLuint *vertices_bytes, GLuint *vertices_pointes,
                int num_crects, struct xh_rect *crects,
                int left, int top, int width, int height)
{
    GLfloat *vertices;
    GLfloat *vert;
    GLfloat x1;
    GLfloat x2;
    GLfloat y1;
    GLfloat y2;
    int index;
    int rx1;
    int rx2;
    GLfloat fwidth;
    GLfloat fheight;
    const GLfloat fac13 = 1.0 / 3.0;
    const GLfloat fac23 = 2.0 / 3.0;
    const GLfloat fac43 = 4.0 / 3.0;
    struct xh_rect *crect;

    if (num_crects < 1)
    {
        return get_vertices_all(vertices_bytes, vertices_pointes,
                                num_crects, crects, left, top, width, height);
    }
    vertices = g_new(GLfloat, num_crects * 24);
    if (vertices == NULL)
    {
        return NULL;
    }
    fwidth = width  / 2.0;
    fheight = height / 2.0;
    for (index = 0; index < num_crects; index++)
    {
        crect = crects + index;
        LOG_DEVEL(LOG_LEVEL_INFO, "get_vertices420: "
                  "rect index %d x %d y %d w %d h %d",
                  index, crect->x, crect->y, crect->w, crect->h);
        /* A fragment covers four destination bytes: round the rect edges
           out to a multiple of four. */
        rx1 = (crect->x - left) & ~3;
        rx2 = ((crect->x - left) + crect->w + 3) & ~3;
        x1 = rx1 / fwidth;
        y1 = (crect->y - top) / fheight;
        x2 = rx2 / fwidth;
        y2 = ((crect->y - top) + crect->h) / fheight;
        vert = vertices + index * 24;
        /* y box */
        vert[0]  =  x1 - 1.0;
        vert[1]  =  y1 * fac23 - 1.0;
        vert[2]  =  x1 - 1.0;
        vert[3]  =  y2 * fac23 - 1.0;
        vert[4]  =  x2 - 1.0;
        vert[5]  =  y1 * fac23 - 1.0;
        vert[6]  =  x1 - 1.0;
        vert[7]  =  y2 * fac23 - 1.0;
        vert[8]  =  x2 - 1.0;
        vert[9]  =  y1 * fac23 - 1.0;
        vert[10] =  x2 - 1.0;
        vert[11] =  y2 * fac23 - 1.0;
        /* uv box */
        vert[12] =  x1 - 1.0;
        vert[13] = (y1 * fac13 + fac43) - 1.0;
        vert[14] =  x1 - 1.0;
        vert[15] = (y2 * fac13 + fac43) - 1.0;
        vert[16] =  x2 - 1.0;
        vert[17] = (y1 * fac13 + fac43) - 1.0;
        vert[18] =  x1  - 1.0;
        vert[19] = (y2 * fac13 + fac43) - 1.0;
        vert[20] =  x2 - 1.0;
        vert[21] = (y1 * fac13 + fac43) - 1.0;
        vert[22] =  x2 - 1.0;
        vert[23] = (y2 * fac13 + fac43) - 1.0;
    }
    *vertices_bytes = sizeof(GLfloat) * num_crects * 24;
    *vertices_pointes = num_crects * 12;
    return vertices;
}

/*****************************************************************************/
/* Destination quads for the v2 aux view from source damage rects. v2's
   mapping is affine, so a source rect covers one column range in each of
   the U and V halves over two row ranges. With x1 = ceil(W/16)*8,
   destination byte column c, row y reads source column 2c+1 (c < x1) or
   2(c-x1)+1 (c >= x1) at row y in the luma region, and 2c or 2(c-x1) at row
   2(y-H)+1 in the chroma region. Bounds round outward. This only saves
   shader work; the encoded picture is the same either way. */
static GLfloat *
get_vertices_av_v2(struct mon_info *mi, GLuint *vertices_bytes,
                   GLuint *vertices_pointes, int num_crects,
                   struct xh_rect *crects)
{
    GLfloat *vertices;
    GLfloat *vert;
    int x1;
    int index;
    int quad;
    int nquads;
    GLfloat fw;
    GLfloat fh;

    if (num_crects < 1)
    {
        return NULL;
    }
    /* Four quads per rect, six vertices of two floats each. */
    vertices = g_new(GLfloat, num_crects * 4 * 12);
    if (vertices == NULL)
    {
        return NULL;
    }
    x1 = ((mi->width + 15) / 16) * 8;
    fw = mi->enc_w4;
    fh = mi->buf_h * 3 / 2;
    nquads = 0;
    for (index = 0; index < num_crects; index++)
    {
        struct xh_rect *r = crects + index;
        int c1 = r->x / 2;
        int c2 = (r->x + r->w + 1) / 2 + 1;
        int rows[2][2];
        int cols[2];

        rows[0][0] = r->y;
        rows[0][1] = r->y + r->h;
        rows[1][0] = mi->buf_h + r->y / 2;
        rows[1][1] = mi->buf_h + (r->y + r->h + 1) / 2 + 1;
        cols[0] = 0;
        cols[1] = x1;
        for (quad = 0; quad < 4; quad++)
        {
            int band = quad >> 1;          /* 0 luma region, 1 chroma region */
            int half = quad & 1;           /* 0 U half, 1 V half */
            /* Four bytes to a fragment. */
            GLfloat fx1 = ((c1 + cols[half]) / 4) / fw * 2.0f - 1.0f;
            GLfloat fx2 = ((c2 + cols[half] + 3) / 4) / fw * 2.0f - 1.0f;
            GLfloat fy1 = rows[band][0] / fh * 2.0f - 1.0f;
            GLfloat fy2 = rows[band][1] / fh * 2.0f - 1.0f;

            vert = vertices + nquads * 12;
            vert[0]  = fx1;
            vert[1]  = fy1;
            vert[2]  = fx1;
            vert[3]  = fy2;
            vert[4]  = fx2;
            vert[5]  = fy1;
            vert[6]  = fx1;
            vert[7]  = fy2;
            vert[8]  = fx2;
            vert[9]  = fy1;
            vert[10] = fx2;
            vert[11] = fy2;
            nquads++;
        }
    }
    *vertices_bytes = sizeof(GLfloat) * nquads * 12;
    *vertices_pointes = nquads * 6;
    return vertices;
}

/*****************************************************************************/
static GLfloat *
get_vertices444(GLuint *vertices_bytes, GLuint *vertices_pointes,
                int num_crects, struct xh_rect *crects,
                int left, int top, int width, int height)
{
    GLfloat *vertices;
    GLfloat *vert;
    GLfloat x1;
    GLfloat x2;
    GLfloat y1;
    GLfloat y2;
    int index;
    GLfloat fwidth;
    GLfloat fheight;
    struct xh_rect *crect;

    if (num_crects < 1)
    {
        return get_vertices_all(vertices_bytes, vertices_pointes,
                                num_crects, crects, left, top, width, height);
    }
    vertices = g_new(GLfloat, num_crects * 12);
    if (vertices == NULL)
    {
        return NULL;
    }
    fwidth = width  / 2.0;
    fheight = height / 2.0;
    for (index = 0; index < num_crects; index++)
    {
        crect = crects + index;
        x1 = (crect->x - left) / fwidth;
        y1 = (crect->y - top) / fheight;
        x2 = ((crect->x - left) + crect->w) / fwidth;
        y2 = ((crect->y - top) + crect->h) / fheight;
        vert = vertices + index * 12;
        vert[0]  = x1 - 1.0;
        vert[1]  = y1 - 1.0;
        vert[2]  = x1 - 1.0;
        vert[3]  = y2 - 1.0;
        vert[4]  = x2 - 1.0;
        vert[5]  = y1 - 1.0;
        vert[6]  = x1 - 1.0;
        vert[7]  = y2 - 1.0;
        vert[8]  = x2 - 1.0;
        vert[9]  = y1 - 1.0;
        vert[10] = x2 - 1.0;
        vert[11] = y2 - 1.0;
    }
    *vertices_bytes = sizeof(GLfloat) * num_crects * 12;
    *vertices_pointes = num_crects * 6;
    return vertices;
}

/* Session capabilities from xorgxrdp (message type 4), zero until sent.
   XH_CAPS_AVC444: the client's EGFX capabilities permit AVC444.
   XH_CAPS_AVC444_V2: the v2 chroma layout (codec id 0x000F) may be used. */
static int g_session_caps = 0;

/*****************************************************************************/
void
xrdp_accel_assist_x11_set_caps(int caps)
{
    g_session_caps = caps;
}

/*****************************************************************************/
/* Whether this session uses AVC444. Decided at pixmap creation: both
   views share one sequence, so the main surface needs the 16-aligned
   height too.

   Follows the client's negotiation. XRDP_ACCEL_AVC444 overrides: unset
   follows the client, "0" forces off, anything else forces on. xorgxrdp
   applies the same rule to the same variable. */
static int
xrdp_accel_assist_x11_avc444_enabled(void)
{
    const char *env = g_getenv("XRDP_ACCEL_AVC444");

    if (env != NULL)
    {
        return g_strcmp(env, "0") != 0;
    }
    return (g_session_caps & XH_CAPS_AVC444) != 0;
}

/*****************************************************************************/
/* The layout of the encoder being created (set before each create_enc). */
static int g_create_v2 = 0;

int
xrdp_accel_assist_x11_encoder_avc444_v2(void)
{
    return g_create_v2;
}

/*****************************************************************************/
/* Whether a monitor's surface uses the v2 layout: the session's choice,
   unless its width rules v2 out (see create_encode_surface). */
int
xrdp_accel_assist_x11_mon_avc444_v2(int mon_id)
{
    return g_mons[mon_id % MAX_MON].avc444_v2;
}

/*****************************************************************************/
/* ChromaV1 (0x000E) or ChromaV2 (0x000F) for the aux view. v2 compresses
   better: its row mapping is the identity, where v1 alternates U and V
   rows in groups of eight.

   MS-RDPEGFX has no capability flag for the layout, so xrdp decides from
   the confirmed capability version (a bare 10.0 client gets v1) and sends
   XH_CAPS_AVC444_V2. XRDP_ACCEL_AVC444_V2 overrides: "0" forces v1,
   anything else v2. xorgxrdp applies the same rule. */
int
xrdp_accel_assist_x11_avc444_v2(void)
{
    const char *env = g_getenv("XRDP_ACCEL_AVC444_V2");

    if (env != NULL)
    {
        return g_strcmp(env, "0") != 0;
    }
    return (g_session_caps & XH_CAPS_AVC444_V2) != 0;
}

/*****************************************************************************/
/* Period of a synchronised IDR on both views, in ms. Some clients'
   decoders get stuck in a bad state (mstsc with hardware decoding on some
   Intel drivers combines the views wrongly until the next IDR). Timed, not
   counted, because frames are damage-driven; it rides on the next frame
   after the period. Also gated on bytes sent; see
   xrdp_accel_assist_x11_idr_min_bytes().

   XRDP_AVC444_IDR_MS, default 10000; 0 disables. XRDP_AVC444_IDR_PERIOD
   (frames) overrides it and is not gated. */
static int
xrdp_accel_assist_x11_idr_ms(void)
{
    static int idr_ms = -1;
    const char *env;

    if (idr_ms < 0)
    {
        env = g_getenv("XRDP_AVC444_IDR_MS");
        idr_ms = (env != NULL) ? g_atoi(env) : 10000;
        if (idr_ms < 0)
        {
            idr_ms = 0;
        }
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11: AVC444 periodic IDR "
            "every %d ms (0 = off)", idr_ms);
    }
    return idr_ms;
}

/*****************************************************************************/
/* Bytes that must have been sent since the last IDR before the periodic
   one goes out, so a desktop with only a ticking clock does not pay for
   it. XRDP_AVC444_IDR_MIN_KB, default 100; 0 removes the gate. */
static unsigned int
xrdp_accel_assist_x11_idr_min_bytes(void)
{
    static int min_kb = -1;
    const char *env;

    if (min_kb < 0)
    {
        env = g_getenv("XRDP_AVC444_IDR_MIN_KB");
        min_kb = (env != NULL) ? g_atoi(env) : 100;
        if (min_kb < 0)
        {
            min_kb = 0;
        }
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11: AVC444 periodic IDR "
            "needs %d KB sent since the last one (0 = no minimum)", min_kb);
    }
    return (unsigned int) min_kb * 1024;
}

/*****************************************************************************/
int
xrdp_accel_assist_x11_create_pixmap(int width, int height, int magic,
                                    int con_id, int mon_id)
{
    struct mon_info *mi;
    XImage *ximage;
    int img[64];
    GLuint enc_texture;
    GLuint enc_texture_aux;
    int gl_alloc;
    int buf;

    mi = g_mons + mon_id % MAX_MON;
    if (mi->pixmap[0] != 0)
    {
        LOG(LOG_LEVEL_ERROR, "xrdp_accel_assist_x11_create_pixmap: "
            "error already setup");
        return 1;
    }
    LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_create_pixmap: "
        "width %d height %d, magic 0x%8.8x, con_id %d mod_id %d",
        width, height, magic, con_id, mon_id);

    /* Two capture buffers, registered separately; the buffer index rides
       in the high bits of mon_id. */
    for (buf = 0; buf < 2; buf++)
    {
        mi->pixmap[buf] = XCreatePixmap(g_display, g_root_window,
                                        width, height, 24);
        LOG(LOG_LEVEL_INFO, "pixmap[%d] %d", buf, (int) mi->pixmap[buf]);
        if (g_inf_funcs[g_inf].create_image(mi->pixmap[buf],
                                            &(mi->inf_image[buf])) != 0)
        {
            return 1;
        }
        LOG(LOG_LEVEL_INFO, "inf_image[%d] %p", buf,
            (void *) mi->inf_image[buf]);
        g_memset(img, 0, sizeof(img));
        img[0] = magic;
        img[1] = con_id;
        img[2] = mon_id | (buf << 8);
        ximage = XCreateImage(g_display, g_vis, 24, ZPixmap, 0, (char *) img,
                              4, 4, 32, 0);
        XPutImage(g_display, mi->pixmap[buf], g_gc, ximage, 0, 0, 0, 0, 4, 4);
        XFree(ximage);
    }

    glEnable(GL_TEXTURE_2D);
    /* The texture that gets encoded, unless the encoder allocates its own
       surfaces and gives us targets over them. */
    gl_alloc = (g_enc_funcs[g_enc].create_enc_targets == NULL);
    enc_texture = 0;
    enc_texture_aux = 0;
    if (gl_alloc)
    {
        glGenTextures(1, &enc_texture);
        glBindTexture(GL_TEXTURE_2D, enc_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    if ((g_enc == ENC_NVENC) || (g_enc == ENC_VA))
    {
        /* NV12: Y (width x height), then interleaved UV (width x
           height / 2), in one texture or in the encoder's two layers. */
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_create_pixmap: "
            "using XH_YUV420");
        mi->tex_format = XH_YUV420;
        /* A new surface invalidates the accumulated damage box. */
        mi->aux_dirty = 0;
        mi->idr_last_ms = g_get_elapsed_ms();
        mi->idr_bytes_since = 0;
        mi->idr_pending = 0;
        mi->avc444 = xrdp_accel_assist_x11_avc444_enabled();
        mi->avc444_v2 = mi->avc444 && xrdp_accel_assist_x11_avc444_v2();
        /* v2 splits the aux view into U and V halves. At a 16-aligned
           width of an odd number of macroblocks the clients disagree on
           where: FreeRDP at half that width, mstsc at half the coded width
           (and it drops a picture whose width is an odd number of
           macroblocks). v1 has no split, so it serves such a surface. */
        if (mi->avc444_v2 && (((width + 15) / 16) & 1) != 0)
        {
            LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_create_pixmap: "
                "width %d is an odd number of macroblocks; AVC444v1 for this "
                "monitor", width);
            mi->avc444_v2 = 0;
        }
        /* v2 splits the aux plane into U and V halves at half the
           16-aligned width (FreeRDP's nTotalWidth; mstsc agrees), so encode
           at the aligned width. Clients copy only the surface width. */
        mi->enc_w = mi->avc444_v2 ? ((width + 15) & ~15) : width;
        /* v1's tiled layout needs the aux padded to 16 rows (MS-RDPEGFX
           2.2.4.4.2); v2 needs none. */
        mi->pad_h = (mi->avc444 && !mi->avc444_v2)
                    ? ((height + 15) & ~15) : height;
        /* The UV plane at a 16-aligned row: VA-API allocates its input
           surfaces at the 16-aligned size, and the shaders' Y/UV boundary
           must be the surface's. The encoder still codes pad_h rows and
           the SPS crops to them. */
        mi->buf_h = (g_enc == ENC_VA) ? ((mi->pad_h + 15) & ~15) : mi->pad_h;
        /* The viewport's width in fragments of four bytes; a plane written
           fewer bytes a fragment widens it (see run_shader). */
        mi->enc_w4 = (mi->enc_w + 3) / 4;
        if (gl_alloc)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mi->enc_w4,
                         mi->buf_h * 3 / 2, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        }
        mi->get_vertices = get_vertices420;
        mi->viewport.x = 0;
        mi->viewport.y = 0;
        mi->viewport.w = mi->enc_w4;
        mi->viewport.h = mi->buf_h * 3 / 2;
    }
    else
    {
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_create_pixmap: "
            "using XH_YUV444");
        mi->tex_format = XH_YUV444;
        mi->enc_w = width;
        mi->enc_w4 = width;
        mi->pad_h = height;
        mi->buf_h = height;
        if (gl_alloc)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                         GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, NULL);
        }
        mi->get_vertices = get_vertices444;
        mi->viewport.x = 0;
        mi->viewport.y = 0;
        mi->viewport.w = width;
        mi->viewport.h = height;
    }
    /* one source texture per capture buffer */
    for (buf = 0; buf < 2; buf++)
    {
        glGenTextures(1, &(mi->bmp_texture[buf]));
        glBindTexture(GL_TEXTURE_2D, mi->bmp_texture[buf]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    /* AVC444 aux view: same NV12 geometry as the main view. */
    if (mi->avc444)
    {
        if (gl_alloc)
        {
            glGenTextures(1, &enc_texture_aux);
            glBindTexture(GL_TEXTURE_2D, enc_texture_aux);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            /* Packed four bytes to a fragment, like the main view. */
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mi->enc_w4,
                         mi->buf_h * 3 / 2, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_create_pixmap: "
            "AVC444%s enabled, encoding both views at %dx%d (desktop %dx%d)",
            mi->avc444_v2 ? "v2" : "v1", mi->enc_w, mi->pad_h, width, height);
    }

    /* Before the encoder: if it fails, encode_pixmap retries it on the
       next frame, which needs the texture and the size. Set after, a
       failed first create left the size at 0, so every frame failed the
       size check even once a retry succeeded, and the retry was handed
       texture 0. */
    mi->width = width;
    mi->height = height;

    g_create_v2 = mi->avc444_v2;
    g_memset(mi->tgt, 0, sizeof(mi->tgt));
    mi->tgt_from_enc = !gl_alloc;
    /* A new target holds nothing yet. */
    mi->full_pending = 1;
    if (gl_alloc)
    {
        mi->tgt[0].tex[0] = enc_texture;
        mi->tgt[0].bpf[0] = 4;
        mi->tgt[1].tex[0] = enc_texture_aux;
        mi->tgt[1].bpf[0] = 4;
        if (g_enc_funcs[g_enc].create_enc(mi->enc_w, mi->pad_h,
                                          enc_texture, enc_texture_aux,
                                          mi->tex_format,
                                          &(mi->ei)) != 0)
        {
            mi->ei = NULL;
            return 1;
        }
    }
    else if (g_enc_funcs[g_enc].create_enc_targets(mi->enc_w, mi->pad_h,
             mi->avc444 ? 2 : 1,
             &(mi->ei), mi->tgt) != 0)
    {
        mi->ei = NULL;
        return 1;
    }

    return 0;
}

#if XR_DUMP_FRAMEBUFFER

static int g_framebuffer_file_index = 0;

/*****************************************************************************/
static int
save_fb_to_file(int width, int height)
{
    char *pixels;
    char filename[256];

    pixels = (char *) g_malloc(width * height * 4, 0);
    if (pixels != NULL)
    {
        glReadPixels(0, 0, width / 2, height, GL_BGRA,
                     GL_UNSIGNED_INT_8_8_8_8_REV, pixels);
        snprintf(filename, 255, "/tmp/gl_surface%8.8x.bmp",
                 g_framebuffer_file_index++);
        g_save_to_bmp(filename, pixels, width * 2, width / 2, height, 24, 32);
        g_free(pixels);
    }
    return 0;
}

#endif

/*****************************************************************************/
/* XRDP_AVC444_DUMP_DIR=<dir>: dump the encode textures raw, as
   <dir>/<tag>_<frame>_<w>x<h>.nv12 (h includes the UV plane), for
   checking the shader's packing offline. One frame is dumped. */
static int
xrdp_accel_assist_x11_dump_plane(const char *tag, int frame,
                                 struct xh_enc_target *tgt,
                                 int width, int height, int packed)
{
    static const char *dump_dir = NULL;
    static int checked = 0;
    char filename[512];
    char *pixels;
    int fd;
    int len;

    if (!checked)
    {
        checked = 1;
        dump_dir = g_getenv("XRDP_AVC444_DUMP_DIR");
    }
    if (dump_dir == NULL)
    {
        return 0;
    }
    /* Read a packed RGBA8 target back as RGBA: the same bytes the encoder
       sees. */
    len = width * height * (packed ? 4 : 1);
    pixels = (char *) g_malloc(len, 0);
    if (pixels == NULL)
    {
        return 1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_fb);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (tgt->tex[1] == 0 || !packed)
    {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tgt->tex[0], 0);
        glReadPixels(0, 0, width, height, packed ? GL_RGBA : GL_RED,
                     GL_UNSIGNED_BYTE, pixels);
    }
    else
    {
        /* Two planes: read each in its own format into the stacked
           layout, rows of width * 4 bytes, Y rows then UV rows. */
        static const GLenum fmt[5] = { 0, GL_RED, GL_RG, 0, GL_RGBA };
        int y_rows = height * 2 / 3;
        int plane;
        int rows;
        int bpf;

        for (plane = 0; plane < 2; plane++)
        {
            bpf = tgt->bpf[plane];
            rows = (plane == 0) ? y_rows : height - y_rows;
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, tgt->tex[plane], 0);
            glReadPixels(0, 0, width * 4 / bpf, rows, fmt[bpf <= 4 ? bpf : 4],
                         GL_UNSIGNED_BYTE,
                         pixels + (plane == 0 ? 0 : y_rows * width * 4));
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_snprintf(filename, sizeof(filename) - 1, "%s/%s_%4.4d_%dx%d.nv12",
               dump_dir, tag, frame, packed ? width * 4 : width, height);
    fd = g_file_open_ex(filename, 0, 1, 1, 1);
    if (fd < 0)
    {
        LOG(LOG_LEVEL_ERROR, "xrdp_accel_assist_x11_dump_plane: "
            "cannot open %s", filename);
        g_free(pixels);
        return 1;
    }
    if (g_file_write(fd, pixels, len) != len)
    {
        LOG(LOG_LEVEL_ERROR, "xrdp_accel_assist_x11_dump_plane: "
            "short write on %s", filename);
    }
    else
    {
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_dump_plane: wrote %s "
            "(%d bytes)", filename, len);
    }
    g_file_close(fd);
    g_free(pixels);
    return 0;
}

/*****************************************************************************/
/* XRDP_VAAPI_TIMING: drain and time the GL pass. A stall; measurement
   only. */
static int g_gl_timing = -1;
static int g_gl_copy_us = 0;
static int g_gl_shader_us = 0;
static int g_gl_count = 0;

static int
xrdp_accel_assist_x11_gl_timing(void)
{
    if (g_gl_timing < 0)
    {
        const char *env = g_getenv("XRDP_VAAPI_TIMING");
        g_gl_timing = (env != NULL && g_atoi(env) != 0);
    }
    return g_gl_timing;
}

/* Drain before the shader so xorgxrdp's copy is excluded from the time. */
static unsigned int
xrdp_accel_assist_x11_time_copy(void)
{
    unsigned int t0;

    if (!xrdp_accel_assist_x11_gl_timing())
    {
        return 0;
    }
    t0 = g_get_elapsed_ms();
    glFinish();
    g_gl_copy_us += (int) (g_get_elapsed_ms() - t0) * 1000;
    return g_get_elapsed_ms();
}

static void
xrdp_accel_assist_x11_time_gl(unsigned int t_after_copy)
{
    if (!xrdp_accel_assist_x11_gl_timing())
    {
        return;
    }
    glFinish();
    g_gl_shader_us += (int) (g_get_elapsed_ms() - t_after_copy) * 1000;
    g_gl_count++;
    if (g_gl_count >= 100)
    {
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11: over %d frames: waiting "
            "for the xorgxrdp copy mean %d us, our RGB->NV12 shader mean "
            "%d us", g_gl_count, g_gl_copy_us / g_gl_count,
            g_gl_shader_us / g_gl_count);
        g_gl_count = 0;
        g_gl_copy_us = 0;
        g_gl_shader_us = 0;
    }
}

/*****************************************************************************/
/* Frame number whose source texture the next run_shader dumps; -1 =
   none. One-shot. */
static int g_dump_src_frame = -1;

/*****************************************************************************/
/* Dump the BGRA source texture; call from run_shader while it is bound. */
static int
xrdp_accel_assist_x11_dump_src(int frame, GLuint tex, int width, int height)
{
    static const char *dump_dir = NULL;
    static int checked = 0;
    char filename[512];
    char *pixels;
    int fd;
    int len;

    if (!checked)
    {
        checked = 1;
        dump_dir = g_getenv("XRDP_AVC444_DUMP_DIR");
    }
    if (dump_dir == NULL)
    {
        return 0;
    }
    len = width * height * 4;
    pixels = (char *) g_malloc(len, 0);
    if (pixels == NULL)
    {
        return 1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    g_snprintf(filename, sizeof(filename) - 1, "%s/src_%4.4d_%dx%d.bgra",
               dump_dir, frame, width, height);
    fd = g_file_open_ex(filename, 0, 1, 1, 1);
    if (fd < 0)
    {
        LOG(LOG_LEVEL_ERROR, "xrdp_accel_assist_x11_dump_src: cannot open %s",
            filename);
        g_free(pixels);
        return 1;
    }
    if (g_file_write(fd, pixels, len) != len)
    {
        LOG(LOG_LEVEL_ERROR, "xrdp_accel_assist_x11_dump_src: short write %s",
            filename);
    }
    else
    {
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_dump_src: wrote %s "
            "(%d bytes)", filename, len);
    }
    g_file_close(fd);
    g_free(pixels);
    return 0;
}

/*****************************************************************************/
/* pad_h: the target's Y/UV boundary row (mi->buf_h), 0 for mi->height.
   The vertices and the shaders see one target of pad_h * 3 / 2 rows, Y
   then UV. A target split into two planes gets two draws over the same
   vertices: the Y plane, then the UV plane with the viewport moved down
   pad_h rows, so the UV rows land at its top, and y_off restoring the
   row the shader expects. Each draw's other rows fall outside its plane
   and are clipped. */
static void
xrdp_accel_assist_x11_run_shader(int left, int top, int width, int height,
                                 struct mon_info *mi,
                                 struct shader_info *si,
                                 struct xh_enc_target *tgt,
                                 int num_crects, struct xh_rect *crects,
                                 int pad_h, int vp_w, int aux_v2)
{
    GLuint vao;
    GLuint vbo;
    GLfloat *vertices;
    GLuint vertices_bytes;
    GLuint vertices_pointes;
    int nplanes;
    int plane;
    int vp_h;
    int bpf;

    /* rgb to yuv */
    glEnable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mi->bmp_texture[mi->cur_buf]);
    g_inf_funcs[g_inf].bind_tex_image(mi->inf_image[mi->cur_buf]);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fb);
    glUseProgram(si->program);
    /* setup vertices from crects */
    /* The v2 aux view builds its own quads from the damage rects. */
    if (aux_v2)
    {
        vertices = get_vertices_av_v2(mi, &vertices_bytes, &vertices_pointes,
                                      num_crects, crects);
    }
    else
    {
        /* In the target's geometry, not the surface's: AVC444 v2 encodes
           at the 16-aligned width and v1 at the 16-aligned height, so
           scaling by the surface size drew each rect up to 8 pixels off
           its place towards the right edge, leaving columns of it stale. */
        vertices = mi->get_vertices(&vertices_bytes, &vertices_pointes,
                                    num_crects, crects, left, top,
                                    mi->enc_w > 0 ? mi->enc_w : width,
                                    pad_h > 0 ? pad_h : height);
    }
    if (vertices == NULL)
    {
        LOG(LOG_LEVEL_ERROR, "xrdp_accel_assist_x11_run_shader: "
            "error get_vertices failed num_crects %d",
            num_crects);
        return;
    }
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices_bytes, vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, NULL);
    /* uniforms */
    glUniform2f(si->tex_size_loc, mi->width, mi->height);
    if (si->pad_h_loc >= 0)
    {
        glUniform1f(si->pad_h_loc,
                    (float) (pad_h > 0 ? pad_h : mi->height));
    }
    /* viewport and draw */
    /* vp_w is a quarter of the byte width; a parameter because the planes
       differ in byte width under v1. A plane written fewer than four bytes
       to a fragment is that many times wider in fragments. */
    vp_h = (pad_h > 0) ? pad_h * 3 / 2 : mi->viewport.h;
    nplanes = (tgt->tex[1] != 0 && pad_h > 0) ? 2 : 1;
    for (plane = 0; plane < nplanes; plane++)
    {
        bpf = (tgt->bpf[plane] > 0) ? tgt->bpf[plane] : 4;
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tgt->tex[plane], 0);
        if (si->bpf_loc >= 0)
        {
            glUniform1f(si->bpf_loc, (float) bpf);
        }
        if (si->y_off_loc >= 0)
        {
            glUniform1f(si->y_off_loc, (float) (plane == 0 ? 0 : pad_h));
        }
        glViewport(mi->viewport.x,
                   mi->viewport.y - (plane == 0 ? 0 : pad_h),
                   vp_w * 4 / bpf, vp_h);
        glDrawArrays(GL_TRIANGLES, 0, vertices_pointes);
    }
    /* cleanup */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    g_free(vertices);
#if XR_DUMP_FRAMEBUFFER
    save_fb_to_file(width, height);
#endif
    if (g_dump_src_frame >= 0)
    {
        xrdp_accel_assist_x11_dump_src(g_dump_src_frame,
                                       mi->bmp_texture[mi->cur_buf],
                                       mi->width, mi->height);
        g_dump_src_frame = -1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_inf_funcs[g_inf].release_tex_image(mi->inf_image[mi->cur_buf]);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

#if XR_DUMP_PIXMAP

static int g_pixmap_file_index = 0;

/*****************************************************************************/
static int
save_pixmap_to_file(Pixmap pix, int width, int height)
{
    XImage *image;
    char filename[256];

    image = XGetImage(g_display, pix, 0, 0, width, height, AllPlanes, ZPixmap);
    if (image != NULL)
    {
        snprintf(filename, 255, "/tmp/pixmap%8.8x.bmp", g_pixmap_file_index++);
        g_save_to_bmp(filename, image->data, width * 4, width, height, 24, 32);
        XFree(image);
    }
    return 0;
}
#endif

/*****************************************************************************/
/* With XRDP_VAAPI_TIMING, every DD_STATS_MS one log line: frames checked,
   the average time a check took (GPU wait included), and what became of
   the frames. */
#define DD_STATS_MS 5000
static struct
{
    unsigned int start_ms;
    unsigned long long us;
    int checked;
    int skipped;
    int filtered;
    int passed;
    int aux_owed;
    int aux_sent;                 /* frames that carried the aux view */
    int frames;                   /* AVC444 frames encoded */
} g_dd_stats;

static enum encoder_result
encode_pixmap(int left, int top, int width, int height,
              int mon_id, int num_crects, struct xh_rect *crects,
              void *cdata, int *cdata_bytes, int codec_id, int flags)
{
    struct mon_info *mi;
    struct shader_info *si;
    enum encoder_result rv;
    struct xh_rect whole;

    mi = g_mons + mon_id % MAX_MON;
    /* Which capture buffer xorgxrdp used for this frame. */
    mi->cur_buf = (flags & ACCEL_ASSIST_BUFFER_1) ? 1 : 0;
    LOG_DEVEL(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_encode_pixmap: "
              "left %d top %d width %d height %d mon_id %d",
              left, top, width, height, mon_id);
    if ((width != mi->width) || (height != mi->height))
    {
        LOG(LOG_LEVEL_ERROR, "xrdp_accel_assist_x11_encode_pixmap: "
            "error width %d should be %d "
            "height %d should be %d",
            width, mi->width, height, mi->height);
        return ENCODER_ERROR;
    }
#if XR_DUMP_PIXMAP
    save_pixmap_to_file(mi->pixmap[mi->cur_buf], width, height);
#endif
    if (mi->full_pending)
    {
        whole.x = 0;
        whole.y = 0;
        whole.w = width;
        whole.h = height;
        crects = &whole;
        num_crects = 1;
        mi->full_pending = 0;
    }
    if (codec_id == XH_CODECID_AVC444 || codec_id == XH_CODECID_AVC444V2)
    {
        /* AVC444: two views, framed as
             [4-byte LE len1][stream1][4-byte LE len2][stream2]
           Periodic IDR: see xrdp_accel_assist_x11_idr_ms(). */
        unsigned char *p = (unsigned char *) cdata;
        int avail = *cdata_bytes;
        int len1;
        int len2;
        enum encoder_result rv2;
        static int idr_period = -1;
        int frame_no = mi->avc444_frame_count++;
        /* The rect the aux was rendered over, for xrdp to declare (v2
           only; v1 is full-frame). */
        int have_aux_rect = 0;
        int aux_x1 = 0;
        int aux_y1 = 0;
        int aux_x2 = 0;
        int aux_y2 = 0;
        int send_aux;
        int aux_only;
        int aux_i;
        int aux_stage;
        unsigned int t_copy;
        unsigned int now_ms;
        int idr_ms;
        int idr_frame;

        if (idr_period < 0)
        {
            const char *env = g_getenv("XRDP_AVC444_IDR_PERIOD");
            idr_period = (env != NULL) ? g_atoi(env) : 0;
            if (idr_period < 0)
            {
                idr_period = 0;
            }
            LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_encode_pixmap: "
                "AVC444 synced IDR period %d frames (0 = timed, see "
                "XRDP_AVC444_IDR_MS)", idr_period);
        }
        now_ms = g_get_elapsed_ms();
        if (idr_period > 0)
        {
            /* frame-count override */
            if ((frame_no % idr_period) == 0)
            {
                flags |= XH_ENC_FLAGS_FORCEIDR;
            }
        }
        else
        {
            idr_ms = xrdp_accel_assist_x11_idr_ms();
            if (idr_ms > 0 &&
                    now_ms - mi->idr_last_ms >= (unsigned int) idr_ms &&
                    mi->idr_bytes_since >=
                    xrdp_accel_assist_x11_idr_min_bytes())
            {
                flags |= XH_ENC_FLAGS_FORCEIDR;
            }
        }
        /* Any IDR restarts the period. */
        idr_frame = ((flags & XH_ENC_FLAGS_FORCEIDR) != 0) || (frame_no == 0);
        if (idr_frame)
        {
            mi->idr_last_ms = now_ms;
        }

        if (!mi->avc444)
        {
            LOG(LOG_LEVEL_ERROR, "xrdp_accel_assist_x11_encode_pixmap: "
                "AVC444 requested but the pixmap was created without it -- "
                "XRDP_ACCEL_AVC444 must be set before the session starts");
            return ENCODER_ERROR;
        }

        /* Decide now whether this frame carries chroma, so both views can
           be submitted before either is waited on. */
        if (mi->dd_need_valid)
        {
            /* Damage detection says where the change needs the aux view;
               an area luma-only frames left behind (aux_dirty) goes too. */
            send_aux = mi->dd_need_any || mi->aux_dirty || mi->force_aux
                       || ((flags & XH_ENC_FLAGS_FORCEIDR) != 0);
        }
        else
        {
            /* Without its verdict (damage detection off): every frame. */
            send_aux = 1;
        }
        mi->force_aux = 0;
        /* A catch-up (nothing changed; owed cells settled) carries the aux
           view alone, as LC=2: the main view is current already. Never an
           IDR, which must start with the main view. */
        aux_only = mi->aux_only && send_aux && !idr_frame;
        g_dd_stats.frames++;
        g_dd_stats.aux_sent += send_aux != 0;

        /* main view (MV shader) */
        if (frame_no == 0)
        {
            g_dump_src_frame = frame_no;
        }
        t_copy = xrdp_accel_assist_x11_time_copy();
        si = g_si + XH_SHADERRGB2YUV420MV;
        if (!aux_only)
        {
            xrdp_accel_assist_x11_run_shader(left, top, width, height, mi, si,
                                             &mi->tgt[0], num_crects, crects,
                                             mi->buf_h, mi->enc_w4, 0);
        }
        /* Split the GL time into xorgxrdp's copy and our conversion. */
        xrdp_accel_assist_x11_time_gl(t_copy);
        if (frame_no == 0)
        {
            xrdp_accel_assist_x11_dump_plane("main", frame_no,
                                             &mi->tgt[0],
                                             mi->enc_w4, mi->buf_h * 3 / 2,
                                             1);
        }

        /* Aux view where a change needs it, or on an IDR; otherwise
           len2 stays 0 and xrdp sends LC=1. v1 renders full-frame (its
           tiling scatters rects); v2 renders the cells that get it, or the
           accumulated box; the metablock declares the same. */
        /* Accumulate damage for the aux view whether or not it goes out.
           With damage detection's verdict only the part that needs it:
           the rest is right at 4:2:0. */
        if (mi->dd_need_valid)
        {
            /* With the cells as a list, the aux view goes out over just
               them (below); the box is for a list that overflowed. */
            if (mi->dd_need_any && mi->dd_aux_n < 0)
            {
                struct xh_rect r;

                r.x = mi->dd_need_x1;
                r.y = mi->dd_need_y1;
                r.w = mi->dd_need_x2 - mi->dd_need_x1;
                r.h = mi->dd_need_y2 - mi->dd_need_y1;
                if (!mi->aux_dirty)
                {
                    mi->aux_dirty = 1;
                    mi->aux_x1 = r.x;
                    mi->aux_y1 = r.y;
                    mi->aux_x2 = r.x + r.w;
                    mi->aux_y2 = r.y + r.h;
                }
                else
                {
                    mi->aux_x1 = MIN(mi->aux_x1, r.x);
                    mi->aux_y1 = MIN(mi->aux_y1, r.y);
                    mi->aux_x2 = MAX(mi->aux_x2, r.x + r.w);
                    mi->aux_y2 = MAX(mi->aux_y2, r.y + r.h);
                }
            }
        }
        else
        {
            for (aux_i = 0; aux_i < num_crects; aux_i++)
            {
                struct xh_rect *r = crects + aux_i;

                if (!mi->aux_dirty)
                {
                    mi->aux_dirty = 1;
                    mi->aux_x1 = r->x;
                    mi->aux_y1 = r->y;
                    mi->aux_x2 = r->x + r->w;
                    mi->aux_y2 = r->y + r->h;
                }
                else
                {
                    if (r->x < mi->aux_x1)
                    {
                        mi->aux_x1 = r->x;
                    }
                    if (r->y < mi->aux_y1)
                    {
                        mi->aux_y1 = r->y;
                    }
                    if (r->x + r->w > mi->aux_x2)
                    {
                        mi->aux_x2 = r->x + r->w;
                    }
                    if (r->y + r->h > mi->aux_y2)
                    {
                        mi->aux_y2 = r->y + r->h;
                    }
                }
            }
        }
        if (send_aux)
        {
            struct xh_rect full_rect;
            struct xh_rect aux_rect;
            full_rect.x = 0;
            full_rect.y = 0;
            full_rect.w = mi->enc_w;
            full_rect.h = mi->pad_h;     /* v1: include the pad rows */
            si = g_si + (mi->avc444_v2 ? XH_SHADERRGB2YUV420AVV2
                         : XH_SHADERRGB2YUV420AV);
            mi->dd_aux_declare = 0;
            if (mi->avc444_v2 && mi->dd_need_valid && mi->dd_aux_n > 0 &&
                    !mi->aux_dirty && (flags & XH_ENC_FLAGS_FORCEIDR) == 0)
            {
                /* Just the cells that get it now; xrdp declares the list. */
                xrdp_accel_assist_x11_run_shader(0, 0, width, height,
                                                 mi, si, &mi->tgt[1],
                                                 mi->dd_aux_n,
                                                 mi->dd_aux_rects,
                                                 mi->buf_h, mi->enc_w4, 1);
                mi->dd_aux_declare = 1;
                have_aux_rect = 1;
                aux_x1 = mi->dd_need_x1;
                aux_y1 = mi->dd_need_y1;
                aux_x2 = mi->dd_need_x2;
                aux_y2 = mi->dd_need_y2;
            }
            else if (mi->avc444_v2)
            {
                /* Past half the frame, render it all. */
                aux_rect.x = mi->aux_x1;
                aux_rect.y = mi->aux_y1;
                aux_rect.w = mi->aux_x2 - mi->aux_x1;
                aux_rect.h = mi->aux_y2 - mi->aux_y1;
                if (!mi->aux_dirty ||
                        (flags & XH_ENC_FLAGS_FORCEIDR) != 0 ||
                        aux_rect.w * aux_rect.h * 2 >= width * height)
                {
                    /* Whole surface for an IDR (every macroblock is coded),
                       for an empty box, and past half the frame. */
                    aux_rect = full_rect;
                }
                xrdp_accel_assist_x11_run_shader(0, 0, width, height,
                                                 mi, si, &mi->tgt[1],
                                                 1, &aux_rect,
                                                 mi->buf_h, mi->enc_w4, 1);
                /* xrdp declares whatever was rendered. */
                have_aux_rect = 1;
                aux_x1 = aux_rect.x;
                aux_y1 = aux_rect.y;
                aux_x2 = aux_rect.x + aux_rect.w;
                aux_y2 = aux_rect.y + aux_rect.h;
            }
            else
            {
                xrdp_accel_assist_x11_run_shader(0, 0, width, height, mi, si,
                                                 &mi->tgt[1], 1,
                                                 &full_rect, mi->buf_h,
                                                 mi->enc_w4, 0);
                /* v1 renders everything, but declares only what changed */
                mi->dd_aux_declare = mi->dd_need_valid && mi->dd_aux_n > 0 &&
                                     !mi->aux_dirty &&
                                     (flags & XH_ENC_FLAGS_FORCEIDR) == 0;
            }
            mi->aux_dirty = 0;
            if (frame_no == 0)
            {
                xrdp_accel_assist_x11_dump_plane("aux", frame_no,
                                                 &mi->tgt[1],
                                                 mi->enc_w4,
                                                 mi->buf_h * 3 / 2, 1);
            }
        }
        XFlush(g_display);

        len2 = 0;
        rv2 = INCREMENTAL_FRAME_ENCODED;
        if (aux_only)
        {
            /* LC=2: no main picture, so its chain and frame_num stay as
               they are; the aux picture predicts from the last aux one */
            len1 = 0;
            len2 = avail - 8;
            rv = g_enc_funcs[g_enc].encode(mi->ei, mi->tgt[1].tex[0],
                                           p + 4 + 4, &len2,
                                           (flags & ~XH_ENC_FLAGS_FORCEIDR) |
                                           XH_ENC_FLAGS_AUXVIEW,
                                           mi->idr_seq);
            if (rv == ENCODER_ERROR)
            {
                return ENCODER_ERROR;
            }
        }
        else if (!send_aux)
        {
            len1 = avail - 8;
            rv = g_enc_funcs[g_enc].encode(mi->ei, mi->tgt[0].tex[0],
                                           p + 4, &len1, flags,
                                           mi->idr_seq);
            if (rv == ENCODER_ERROR)
            {
                return ENCODER_ERROR;
            }
        }
        else if (g_enc_funcs[g_enc].encode_dual != NULL)
        {
            /* Submit both views before waiting. The aux is encoded at the
               halfway mark and moved down once len1 is known. */
            aux_stage = 4 + avail / 2;
            len1 = avail / 2 - 8;
            len2 = avail / 2 - 8;
            rv = g_enc_funcs[g_enc].encode_dual(mi->ei,
                                                p + 4, &len1,
                                                p + aux_stage, &len2,
                                                flags, mi->idr_seq);
            if (rv == ENCODER_ERROR)
            {
                return ENCODER_ERROR;
            }
            g_memmove(p + 4 + len1 + 4, p + aux_stage, len2);
        }
        else
        {
            len1 = avail - 8;
            rv = g_enc_funcs[g_enc].encode(mi->ei, mi->tgt[0].tex[0],
                                           p + 4, &len1, flags,
                                           mi->idr_seq);
            if (rv == ENCODER_ERROR)
            {
                return ENCODER_ERROR;
            }
            len2 = avail - 8 - len1;
            /* Never FORCEIDR: only the first view of a frame may be an
               IDR. */
            rv2 = g_enc_funcs[g_enc].encode(mi->ei, mi->tgt[1].tex[0],
                                            p + 4 + len1 + 4, &len2,
                                            (flags & ~XH_ENC_FLAGS_FORCEIDR) |
                                            XH_ENC_FLAGS_AUXVIEW,
                                            mi->idr_seq);
            if (rv2 == ENCODER_ERROR)
            {
                return ENCODER_ERROR;
            }
        }

        /* write the two length prefixes (little-endian) */
        p[0] = len1 & 0xff;
        p[1] = (len1 >> 8) & 0xff;
        p[2] = (len1 >> 16) & 0xff;
        p[3] = (len1 >> 24) & 0xff;
        p[4 + len1 + 0] = len2 & 0xff;
        p[4 + len1 + 1] = (len2 >> 8) & 0xff;
        p[4 + len1 + 2] = (len2 >> 16) & 0xff;
        p[4 + len1 + 3] = (len2 >> 24) & 0xff;
        *cdata_bytes = 8 + len1 + len2;
        /* An IDR restarts the count, excluding its own bytes. Capped at the
           threshold. */
        if (idr_frame)
        {
            mi->idr_bytes_since = 0;
        }
        else if (mi->idr_bytes_since < xrdp_accel_assist_x11_idr_min_bytes())
        {
            mi->idr_bytes_since += (unsigned int) (len1 + len2);
        }
        if (have_aux_rect && (len2 > 0) &&
                (*cdata_bytes + XH_AVC444_AUX_RECT_BYTES <= avail))
        {
            unsigned char *t = p + *cdata_bytes;
            unsigned int vals[5];
            int vi;

            vals[0] = XH_AVC444_AUX_RECT_MAGIC;
            vals[1] = (unsigned int) aux_x1;
            vals[2] = (unsigned int) aux_y1;
            vals[3] = (unsigned int) aux_x2;
            vals[4] = (unsigned int) aux_y2;
            for (vi = 0; vi < 5; vi++)
            {
                t[vi * 4 + 0] = vals[vi] & 0xff;
                t[vi * 4 + 1] = (vals[vi] >> 8) & 0xff;
                t[vi * 4 + 2] = (vals[vi] >> 16) & 0xff;
                t[vi * 4 + 3] = (vals[vi] >> 24) & 0xff;
            }
            *cdata_bytes += XH_AVC444_AUX_RECT_BYTES;
        }
        return rv;   /* main and aux force IDR together, so rv reflects both */
    }

    /* AVC420 (default): single view */
    si = g_si + mi->tex_format % XH_NUM_SHADERS;
    xrdp_accel_assist_x11_run_shader(left, top, width, height, mi, si,
                                     &mi->tgt[0], num_crects, crects,
                                     mi->buf_h, mi->enc_w4, 0);
    /* flush before encoding, let encoders call glFinish() as needed */
    XFlush(g_display);
    /* encode */
    rv = g_enc_funcs[g_enc].encode(mi->ei, mi->tgt[0].tex[0],
                                   cdata, cdata_bytes, flags, mi->idr_seq);
    return rv;
}

/*****************************************************************************/
/* A new encoder for a monitor whose last one failed. Encoder-owned targets
   go with the old encoder, and the new ones start out blank, so the next
   main view is drawn whole rather than over its damage. */
static int
xrdp_accel_assist_x11_recreate_enc(struct mon_info *mi)
{
    int rv;

    g_create_v2 = mi->avc444_v2;
    if (mi->tgt_from_enc)
    {
        g_memset(mi->tgt, 0, sizeof(mi->tgt));
        rv = g_enc_funcs[g_enc].create_enc_targets(mi->enc_w, mi->pad_h,
             mi->avc444 ? 2 : 1,
             &mi->ei, mi->tgt);
        mi->full_pending = 1;
    }
    else
    {
        rv = g_enc_funcs[g_enc].create_enc(mi->enc_w, mi->pad_h,
                                           mi->tgt[0].tex[0],
                                           mi->tgt[1].tex[0],
                                           mi->tex_format, &mi->ei);
    }
    if (rv != 0)
    {
        mi->ei = NULL;
    }
    return rv;
}

/*****************************************************************************/
/* Damage detection. Toolkits report damage for pixels they repaint
   unchanged: Chrome redraws its whole page when the pointer crosses the
   address bar. Under AVC444 every rect of a luma-only frame is shown from
   the 4:2:0 main view until the next aux frame, so repainted text flickers
   between 4:2:0 and 4:4:4. Compare the damaged area with the source as last
   encoded, in 16x16 cells on the GPU, and pass on only the cells that
   changed. XRDP_AVC444_DAMAGE_DETECT=0 turns it off. */
#define DD_CELL 16
/* More changed rects than this (scattered changes): declare the damage as
   it came, rather than send kilobytes of rects. */
#define DD_MAX_RECTS 256
/* A cell that changes again within this long of its previous change is
   moving (video, scrolling, a drag): it goes luma-only and gets the aux
   view once it settles. Typing at a normal pace stays a run of one-off
   changes, each of which gets the aux view at once. */
#define DD_MOTION_MS 150
/* An owing cell gets the aux view once it has not changed for this long:
   a cell that changes on every other capture (video at a rate the capture
   does not match) is still moving. The idle flush comes later than this. */
#define DD_SETTLE_MS 120
/* Moving cells, of the 3x3 around one, that make the area moving. */
#define DD_AREA_MIN 3


static unsigned long long
dd_now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long) ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

static void
dd_stats_log(void)
{
    unsigned int now = g_get_elapsed_ms();

    if (!xrdp_accel_assist_x11_gl_timing())
    {
        g_memset(&g_dd_stats, 0, sizeof(g_dd_stats));
        return;
    }

    if (g_dd_stats.start_ms == 0)
    {
        g_dd_stats.start_ms = now;
        return;
    }
    if (now - g_dd_stats.start_ms < DD_STATS_MS)
    {
        return;
    }
    if (g_dd_stats.checked > 0)
    {
        LOG(LOG_LEVEL_INFO, "damage detection: %d frames checked, %.2f ms "
            "each; %d unchanged (skipped), %d filtered, %d passed as "
            "damaged, %d aux catch-ups; aux view "
            "on %d of %d frames",
            g_dd_stats.checked,
            g_dd_stats.checked > 0 ?
            g_dd_stats.us / 1000.0 / g_dd_stats.checked : 0.0,
            g_dd_stats.skipped, g_dd_stats.filtered, g_dd_stats.passed,
            g_dd_stats.aux_owed,
            g_dd_stats.aux_sent, g_dd_stats.frames);
    }
    g_memset(&g_dd_stats, 0, sizeof(g_dd_stats));
    g_dd_stats.start_ms = now;
}

static int
xrdp_accel_assist_x11_damage_detect_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0)
    {
        const char *env = g_getenv("XRDP_AVC444_DAMAGE_DETECT");

        enabled = (env == NULL) || (g_strcmp(env, "0") != 0);
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11: AVC444 damage detection "
            "%s", enabled ? "on" : "off (XRDP_AVC444_DAMAGE_DETECT=0)");
    }
    return enabled;
}

/*****************************************************************************/
/* The AVC444 aux view goes only where a change needs it: where the 4:2:0
   main view alone (the 2x2 chroma mean) would be off by more than
   XRDP_AVC444_AUX_THRESHOLD (0-255, default 30, the threshold MS-RDPEGFX
   3.3.8.3.3 gives the client's reverse filter). Grey and flat content goes
   without; coloured detail gets 4:4:4 on the frame it changes, or, while it
   moves, once it settles. */
static float
xrdp_accel_assist_x11_aux_threshold(void)
{
    static int thr = -1;

    if (thr < 0)
    {
        const char *env = g_getenv("XRDP_AVC444_AUX_THRESHOLD");

        thr = (env != NULL) ? g_atoi(env) : 30;
        if (thr < 0 || thr > 255)
        {
            thr = 30;
        }
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11: AVC444 aux threshold %d",
            thr);
    }
    return thr / 255.0f;
}

/*****************************************************************************/
static int
dd_alloc(struct mon_info *mi, int width, int height)
{
    if (mi->dd_prev_texture != 0 && mi->dd_w == width && mi->dd_h == height)
    {
        return 0;
    }
    if (mi->dd_prev_texture != 0)
    {
        glDeleteTextures(1, &(mi->dd_prev_texture));
        glDeleteTextures(1, &(mi->dd_mask_texture));
        glDeleteTextures(1, &(mi->dd_cell_texture));
        glDeleteTextures(1, &(mi->dd_m4_texture));
    }
    g_free(mi->dd_map);
    g_free(mi->dd_cand);
    mi->dd_w = width;
    mi->dd_h = height;
    mi->dd_cells_w = (width + DD_CELL - 1) / DD_CELL;
    mi->dd_cells_h = (height + DD_CELL - 1) / DD_CELL;
    mi->dd_map = g_new0(unsigned char, mi->dd_cells_w * mi->dd_cells_h * 4);
    mi->dd_cand = g_new0(unsigned char, mi->dd_cells_w * mi->dd_cells_h);
    g_free(mi->dd_streak);
    g_free(mi->dd_changed_ms);
    g_free(mi->dd_moving_ms);
    g_free(mi->dd_owe);
    g_free(mi->dd_auxc);
    g_free(mi->dd_aux_rects);
    mi->dd_moving_ms = g_new0(unsigned int, mi->dd_cells_w * mi->dd_cells_h);
    mi->dd_streak = g_new0(unsigned char, mi->dd_cells_w * mi->dd_cells_h);
    mi->dd_changed_ms = g_new0(unsigned int, mi->dd_cells_w * mi->dd_cells_h);
    mi->dd_owe = g_new0(unsigned char, mi->dd_cells_w * mi->dd_cells_h);
    mi->dd_auxc = g_new0(unsigned char, mi->dd_cells_w * mi->dd_cells_h);
    mi->dd_aux_rects = g_new(struct xh_rect, DD_MAX_RECTS);
    glGenTextures(1, &(mi->dd_prev_texture));
    glBindTexture(GL_TEXTURE_2D, mi->dd_prev_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenTextures(1, &(mi->dd_mask_texture));
    glBindTexture(GL_TEXTURE_2D, mi->dd_mask_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, width, height, 0,
                 GL_RG, GL_UNSIGNED_BYTE, NULL);
    glGenTextures(1, &(mi->dd_m4_texture));
    glBindTexture(GL_TEXTURE_2D, mi->dd_m4_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, (width + 3) / 4, (height + 3) / 4,
                 0, GL_RG, GL_UNSIGNED_BYTE, NULL);
    glGenTextures(1, &(mi->dd_cell_texture));
    glBindTexture(GL_TEXTURE_2D, mi->dd_cell_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mi->dd_cells_w, mi->dd_cells_h,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    mi->dd_prev_valid = 0;
    if (mi->dd_map == NULL || mi->dd_cand == NULL || mi->dd_streak == NULL ||
            mi->dd_changed_ms == NULL || mi->dd_moving_ms == NULL ||
            mi->dd_owe == NULL || mi->dd_auxc == NULL ||
            mi->dd_aux_rects == NULL)
    {
        return 1;
    }
    return 0;
}

/*****************************************************************************/
/* Draw vertices (two floats each, triangles) with the current program
   into enc_texture over a viewport of vp_w x vp_h. */
static void
dd_draw(GLuint target, int vp_w, int vp_h, const GLfloat *vertices,
        GLuint vertices_bytes, GLuint vertices_pointes)
{
    GLuint vao;
    GLuint vbo;

    glBindFramebuffer(GL_FRAMEBUFFER, g_fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, target, 0);
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices_bytes, vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, NULL);
    glViewport(0, 0, vp_w, vp_h);
    glDrawArrays(GL_TRIANGLES, 0, vertices_pointes);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
}

/*****************************************************************************/
/* Copy the source over rects (all of it when num_rects < 1) into the
   previous-frame texture. */
static void
dd_copy_to_prev(struct mon_info *mi, int num_rects, struct xh_rect *rects)
{
    struct shader_info *si = g_si + XH_SHADERCOPY;
    GLfloat *vertices;
    GLuint vertices_bytes;
    GLuint vertices_pointes;

    vertices = get_vertices444(&vertices_bytes, &vertices_pointes,
                               num_rects, rects, 0, 0, mi->dd_w, mi->dd_h);
    if (vertices == NULL)
    {
        mi->dd_prev_valid = 0;
        return;
    }
    glUseProgram(si->program);
    glUniform2f(si->tex_size_loc, mi->dd_w, mi->dd_h);
    dd_draw(mi->dd_prev_texture, mi->dd_w, mi->dd_h, vertices,
            vertices_bytes, vertices_pointes);
    g_free(vertices);
}

/*****************************************************************************/
/* Cells as rects: runs along each cell row, each merged into the rect
   above it when that rect ends on the row above with the same columns.
   Returns the count, or -1 if out_max is too small (the caller then passes
   the damage on unfiltered). */
static int
dd_cells_to_rects(struct mon_info *mi, const unsigned char *changed,
                  struct xh_rect *out, int out_max)
{
    int n = 0;
    int cy;
    int cx;
    int start;
    int i;
    int x;
    int w;
    int y;
    int h;
    int *above;                   /* rects ending on the previous row */
    int *here;                    /* rects ending on this row */
    int num_above = 0;
    int num_here;
    int *t;

    above = g_new(int, mi->dd_cells_w);
    here = g_new(int, mi->dd_cells_w);
    if (above == NULL || here == NULL)
    {
        g_free(above);
        g_free(here);
        return -1;
    }
    for (cy = 0; cy < mi->dd_cells_h; cy++)
    {
        y = cy * DD_CELL;
        h = MIN(DD_CELL, mi->dd_h - y);
        num_here = 0;
        cx = 0;
        while (cx < mi->dd_cells_w)
        {
            if (!changed[cy * mi->dd_cells_w + cx])
            {
                cx++;
                continue;
            }
            start = cx;
            while (cx < mi->dd_cells_w && changed[cy * mi->dd_cells_w + cx])
            {
                cx++;
            }
            x = start * DD_CELL;
            w = MIN(cx * DD_CELL, mi->dd_w) - x;
            for (i = 0; i < num_above; i++)
            {
                if (out[above[i]].x == x && out[above[i]].w == w)
                {
                    break;
                }
            }
            if (i < num_above)
            {
                out[above[i]].h += h;
                here[num_here++] = above[i];
                continue;
            }
            if (n >= out_max)
            {
                g_free(above);
                g_free(here);
                return -1;
            }
            out[n].x = x;
            out[n].y = y;
            out[n].w = w;
            out[n].h = h;
            here[num_here++] = n;
            n++;
        }
        t = above;
        above = here;
        here = t;
        num_above = num_here;
    }
    g_free(above);
    g_free(here);
    return n;
}

/*****************************************************************************/
/* Filters crects down to the cells that changed. Returns the number of
   rects in out (0: nothing changed), or -1 to pass crects on as they are.
   force_all: compare nothing, everything damaged counts as changed (an
   IDR), but still refresh the previous-frame copy. */
static int
damage_detect(struct mon_info *mi, int cur_buf, int num_crects,
              struct xh_rect *crects, int force_all,
              struct xh_rect *out, int out_max)
{
    struct shader_info *si;
    unsigned char *changed;
    int cells;
    int index;
    int cx;
    int cy;
    int num_cand = 0;
    int num_changed = 0;
    int n;
    GLuint vao;
    GLuint vbo;

    if (num_crects < 1 || dd_alloc(mi, mi->width, mi->height) != 0)
    {
        return -1;
    }
    cells = mi->dd_cells_w * mi->dd_cells_h;
    g_memset(mi->dd_cand, 0, cells);
    for (index = 0; index < num_crects; index++)
    {
        struct xh_rect *r = crects + index;
        int x1 = MAX(r->x, 0) / DD_CELL;
        int y1 = MAX(r->y, 0) / DD_CELL;
        int x2 = (MIN(r->x + r->w, mi->dd_w) + DD_CELL - 1) / DD_CELL;
        int y2 = (MIN(r->y + r->h, mi->dd_h) + DD_CELL - 1) / DD_CELL;

        for (cy = y1; cy < y2; cy++)
        {
            for (cx = x1; cx < x2; cx++)
            {
                mi->dd_cand[cy * mi->dd_cells_w + cx] = 1;
            }
        }
    }

    glEnable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mi->bmp_texture[cur_buf]);
    g_inf_funcs[g_inf].bind_tex_image(mi->inf_image[cur_buf]);
    changed = mi->dd_cand;
    if (!force_all && mi->dd_prev_valid)
    {
        struct xh_rect *cand_rects;
        int num_cand_rects = -1;
        GLfloat *vertices;
        GLuint vertices_bytes;
        GLuint vertices_pointes;

        /* mask pass, over the damaged cells only (every cell there gets a
           fresh mask; the rest of the mask is never read) */
        cand_rects = g_new(struct xh_rect, DD_MAX_RECTS);
        if (cand_rects != NULL)
        {
            num_cand_rects = dd_cells_to_rects(mi, mi->dd_cand, cand_rects,
                                               DD_MAX_RECTS);
        }
        vertices = get_vertices444(&vertices_bytes, &vertices_pointes,
                                   num_cand_rects > 0 ? num_cand_rects : 0,
                                   cand_rects, 0, 0, mi->dd_w, mi->dd_h);
        g_free(cand_rects);
        if (vertices == NULL)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            g_inf_funcs[g_inf].release_tex_image(mi->inf_image[cur_buf]);
            glBindTexture(GL_TEXTURE_2D, 0);
            return -1;
        }
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, mi->dd_prev_texture);
        glActiveTexture(GL_TEXTURE0);
        si = g_si + XH_SHADERMASKDIFF;
        glUseProgram(si->program);
        glUniform1i(si->tex_loc, 0);
        glUniform1i(si->prev_loc, 1);
        glUniform2f(si->tex_size_loc, mi->dd_w, mi->dd_h);
        glUniform1f(si->thr_loc, xrdp_accel_assist_x11_aux_threshold());
        dd_draw(mi->dd_mask_texture, mi->dd_w, mi->dd_h, vertices,
                vertices_bytes, vertices_pointes);
        g_free(vertices);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        /* cell passes: 4x4 of the mask per fragment, then 4x4 of those,
           so each cell's max takes two short loops rather than one of 256
           (1.7 ms down to 0.95 ms here); over the damaged cells' box only,
           and only that box read back */
        {
            int bx1 = mi->dd_cells_w;
            int by1 = mi->dd_cells_h;
            int bx2 = 0;
            int by2 = 0;
            int m4_w = (mi->dd_w + 3) / 4;
            int m4_h = (mi->dd_h + 3) / 4;

            for (index = 0; index < cells; index++)
            {
                if (mi->dd_cand[index])
                {
                    cx = index % mi->dd_cells_w;
                    cy = index / mi->dd_cells_w;
                    bx1 = MIN(bx1, cx);
                    by1 = MIN(by1, cy);
                    bx2 = MAX(bx2, cx + 1);
                    by2 = MAX(by2, cy + 1);
                }
            }
            si = g_si + XH_SHADERCELLMAX;
            glUseProgram(si->program);
            glUniform1i(si->tex_loc, 0);
            glGenVertexArrays(1, &vao);
            glGenBuffers(1, &vbo);
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(g_vertices), g_vertices,
                         GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                                  sizeof(float) * 2, NULL);
            glBindFramebuffer(GL_FRAMEBUFFER, g_fb);
            glEnable(GL_SCISSOR_TEST);
            if (bx2 > bx1 && by2 > by1)
            {
                glBindTexture(GL_TEXTURE_2D, mi->dd_mask_texture);
                glUniform2f(si->tex_size_loc, mi->dd_w, mi->dd_h);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, mi->dd_m4_texture, 0);
                glViewport(0, 0, m4_w, m4_h);
                glScissor(bx1 * 4, by1 * 4, (bx2 - bx1) * 4,
                          (by2 - by1) * 4);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glBindTexture(GL_TEXTURE_2D, mi->dd_m4_texture);
                glUniform2f(si->tex_size_loc, m4_w, m4_h);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, mi->dd_cell_texture, 0);
                glViewport(0, 0, mi->dd_cells_w, mi->dd_cells_h);
                glScissor(bx1, by1, bx2 - bx1, by2 - by1);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glPixelStorei(GL_PACK_ROW_LENGTH, mi->dd_cells_w);
                glReadPixels(bx1, by1, bx2 - bx1, by2 - by1, GL_RGBA,
                             GL_UNSIGNED_BYTE,
                             mi->dd_map + (by1 * mi->dd_cells_w + bx1) * 4);
                glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            }
            glDisable(GL_SCISSOR_TEST);
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        /* the source again on unit 0, for the copy below */
        glBindTexture(GL_TEXTURE_2D, mi->bmp_texture[cur_buf]);
        /* changed = damaged and different; reuse dd_cand in place. Then,
           per cell: a change that needs the aux view gets it now, unless the
           cell is moving; a moving one owes it, and an owing cell gets it
           once it stops changing. Those cells give the aux view's rects. */
        unsigned int now_ms = g_get_elapsed_ms();

        mi->dd_need_valid = 1;
        mi->dd_need_any = 0;
        /* first what changed, and which cells move on their own (changed
           again within DD_MOTION_MS); dd_moving_ms keeps when each last
           did */
        for (index = 0; index < cells; index++)
        {
            int changed_now = 0;

            if (mi->dd_cand[index])
            {
                num_cand++;
                changed_now = mi->dd_map[index * 4] > 127;
                mi->dd_cand[index] = changed_now;
                num_changed += changed_now;
            }
            if (changed_now && mi->dd_streak[index] &&
                    now_ms - mi->dd_changed_ms[index] < DD_MOTION_MS)
            {
                mi->dd_moving_ms[index] = now_ms;
            }
        }
        for (index = 0; index < cells; index++)
        {
            int changed_now = mi->dd_cand[index];
            int need = changed_now && mi->dd_map[index * 4 + 1] > 127;
            int moving;
            int aux_now;
            int area = 0;
            int area_settle = 0;
            int cx0 = index % mi->dd_cells_w;
            int cy0 = index / mi->dd_cells_w;
            int nx;
            int ny;

            /* The 3x3 around the cell: an area moves when enough of it
               moved lately (video, scrolling), which a typed character
               does not. Inside a moving area, a cell that changes only now
               and then is moving too, and none settles until the area
               has. Settling looks back no further than DD_SETTLE_MS: the
               idle flush, which may be the last capture, comes 150 ms on. */
            if (changed_now || mi->dd_owe[index])
            {
                for (ny = MAX(cy0 - 1, 0);
                        ny <= MIN(cy0 + 1, mi->dd_cells_h - 1); ny++)
                {
                    for (nx = MAX(cx0 - 1, 0);
                            nx <= MIN(cx0 + 1, mi->dd_cells_w - 1); nx++)
                    {
                        int k = ny * mi->dd_cells_w + nx;
                        unsigned int since = now_ms - mi->dd_moving_ms[k];
                        int mv = mi->dd_streak[k] && mi->dd_moving_ms[k] != 0;

                        area += mv && since < DD_MOTION_MS;
                        area_settle += mv && since < DD_SETTLE_MS;
                    }
                }
            }
            moving = 0;
            if (changed_now)
            {
                moving = mi->dd_moving_ms[index] == now_ms ||
                         area >= DD_AREA_MIN;
                mi->dd_changed_ms[index] = now_ms;
                mi->dd_streak[index] = 1;
            }
            if (need && moving)
            {
                mi->dd_owe[index] = 1;
            }
            aux_now = (need && !moving) ||
                      (mi->dd_owe[index] && !changed_now &&
                       now_ms - mi->dd_changed_ms[index] >= DD_SETTLE_MS &&
                       area_settle < DD_AREA_MIN);
            mi->dd_auxc[index] = aux_now;
            if (aux_now)
            {
                int x1 = (index % mi->dd_cells_w) * DD_CELL;
                int y1 = (index / mi->dd_cells_w) * DD_CELL;
                int x2 = MIN(x1 + DD_CELL, mi->dd_w);
                int y2 = MIN(y1 + DD_CELL, mi->dd_h);

                mi->dd_owe[index] = 0;
                if (!mi->dd_need_any)
                {
                    mi->dd_need_any = 1;
                    mi->dd_need_x1 = x1;
                    mi->dd_need_y1 = y1;
                    mi->dd_need_x2 = x2;
                    mi->dd_need_y2 = y2;
                }
                else
                {
                    mi->dd_need_x1 = MIN(mi->dd_need_x1, x1);
                    mi->dd_need_y1 = MIN(mi->dd_need_y1, y1);
                    mi->dd_need_x2 = MAX(mi->dd_need_x2, x2);
                    mi->dd_need_y2 = MAX(mi->dd_need_y2, y2);
                }
            }
        }
        mi->dd_aux_n = mi->dd_need_any ?
                       dd_cells_to_rects(mi, mi->dd_auxc, mi->dd_aux_rects,
                                         DD_MAX_RECTS) : 0;
    }
    else
    {
        for (index = 0; index < cells; index++)
        {
            num_changed += mi->dd_cand[index];
        }
        if (force_all)
        {
            /* an IDR sends the aux view over everything */
            g_memset(mi->dd_owe, 0, cells);
            g_memset(mi->dd_streak, 0, cells);
        }
    }

    n = (num_changed > 0) ? dd_cells_to_rects(mi, changed, out, out_max) : 0;
    /* the previous-frame copy follows what goes out */
    if (!mi->dd_prev_valid)
    {
        dd_copy_to_prev(mi, 0, NULL);
        mi->dd_prev_valid = 1;
    }
    else if (n > 0)
    {
        dd_copy_to_prev(mi, n, out);
    }
    else if (n < 0)
    {
        dd_copy_to_prev(mi, num_crects, crects);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_inf_funcs[g_inf].release_tex_image(mi->inf_image[cur_buf]);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    return n;
}

/*****************************************************************************/
/* Appends the changed-rects trailer after whatever encode_pixmap wrote. */
static void
dd_append_rects(void *cdata, int *cdata_bytes, int avail, unsigned int magic,
                int num_rects, const struct xh_rect *rects)
{
    unsigned char *t = (unsigned char *) cdata + *cdata_bytes;
    int need = XH_AVC444_RECTS_HEAD_BYTES + num_rects * 8;
    unsigned int head[2];
    int i;
    int k;

    if (*cdata_bytes + need > avail)
    {
        return;
    }
    head[0] = magic;
    head[1] = (unsigned int) num_rects;
    for (k = 0; k < 2; k++)
    {
        t[0] = head[k] & 0xff;
        t[1] = (head[k] >> 8) & 0xff;
        t[2] = (head[k] >> 16) & 0xff;
        t[3] = (head[k] >> 24) & 0xff;
        t += 4;
    }
    for (i = 0; i < num_rects; i++)
    {
        unsigned int v[4];

        v[0] = (unsigned int) rects[i].x;
        v[1] = (unsigned int) rects[i].y;
        v[2] = (unsigned int) (rects[i].x + rects[i].w);
        v[3] = (unsigned int) (rects[i].y + rects[i].h);
        for (k = 0; k < 4; k++)
        {
            t[0] = v[k] & 0xff;
            t[1] = (v[k] >> 8) & 0xff;
            t += 2;
        }
    }
    *cdata_bytes += need;
}

/*****************************************************************************/
enum encoder_result
xrdp_accel_assist_x11_encode_pixmap(int left, int top, int width, int height,
                                    int mon_id, int num_crects,
                                    struct xh_rect *crects,
                                    void *cdata, int *cdata_bytes,
                                    int codec_id, int flags)
{
    struct mon_info *mi = g_mons + mon_id % MAX_MON;
    enum encoder_result rv;
    struct xh_rect *dd_rects = NULL;
    int dd_n = -1;

    if (mi->ei == NULL)
    {
        /* its re-creation after a failure failed: try again */
        if (mi->enc_w <= 0 || xrdp_accel_assist_x11_recreate_enc(mi) != 0)
        {
            return ENCODER_ERROR;
        }
    }
    if (mi->idr_pending)
    {
        flags |= XH_ENC_FLAGS_FORCEIDR;
    }
    mi->dd_need_valid = 0;
    mi->aux_only = 0;
    mi->dd_aux_declare = 0;
    if ((codec_id == XH_CODECID_AVC444 || codec_id == XH_CODECID_AVC444V2) &&
            mi->avc444 && xrdp_accel_assist_x11_damage_detect_enabled())
    {
        int force_all = (flags & XH_ENC_FLAGS_FORCEIDR) != 0;
        unsigned long long t0 = dd_now_us();

        dd_rects = g_new(struct xh_rect, DD_MAX_RECTS);
        if (dd_rects != NULL)
        {
            dd_n = damage_detect(mi, (flags & ACCEL_ASSIST_BUFFER_1) ? 1 : 0,
                                 num_crects, crects, force_all,
                                 dd_rects, DD_MAX_RECTS);
        }
        g_dd_stats.checked++;
        g_dd_stats.us += dd_now_us() - t0;
        if (dd_n > 0)
        {
            g_dd_stats.filtered++;
        }
        else if (dd_n < 0)
        {
            g_dd_stats.passed++;
        }
        dd_stats_log();
        if (dd_n == 0 && !force_all && mi->dd_need_valid && mi->dd_need_any &&
                mi->dd_aux_n > 0 && !mi->aux_dirty)
        {
            /* Nothing changed, but cells that were moving have settled:
               their aux view now, declared for both views. */
            g_dd_stats.aux_owed++;
            g_memcpy(dd_rects, mi->dd_aux_rects,
                     mi->dd_aux_n * sizeof(struct xh_rect));
            dd_n = mi->dd_aux_n;
            mi->aux_only = 1;
        }
        else if (dd_n == 0 && !force_all)
        {
            if (!mi->aux_dirty)
            {
                /* Repainted, not changed: no frame at all. */
                g_dd_stats.skipped++;
                g_free(dd_rects);
                *cdata_bytes = 0;
                return FRAME_UNCHANGED;
            }
            g_dd_stats.aux_owed++;
            /* Nothing changed, but luma-only frames left part of the
               screen at 4:2:0: send the aux view now, over that part. */
            dd_rects[0].x = MAX(mi->aux_x1, 0);
            dd_rects[0].y = MAX(mi->aux_y1, 0);
            dd_rects[0].w = MIN(mi->aux_x2, mi->width) - dd_rects[0].x;
            dd_rects[0].h = MIN(mi->aux_y2, mi->height) - dd_rects[0].y;
            dd_n = (dd_rects[0].w > 0 && dd_rects[0].h > 0) ? 1 : -1;
            mi->force_aux = 1;
            mi->aux_only = dd_n > 0;
        }
    }
    if (dd_n > 0)
    {
        int avail = *cdata_bytes;

        rv = encode_pixmap(left, top, width, height, mon_id, dd_n, dd_rects,
                           cdata, cdata_bytes, codec_id, flags);
        if (rv != ENCODER_ERROR)
        {
            dd_append_rects(cdata, cdata_bytes, avail, XH_AVC444_RECTS_MAGIC,
                            dd_n, dd_rects);
            if (mi->dd_aux_declare)
            {
                dd_append_rects(cdata, cdata_bytes, avail,
                                XH_AVC444_AUX_RECTS_MAGIC,
                                mi->dd_aux_n, mi->dd_aux_rects);
            }
        }
    }
    else
    {
        rv = encode_pixmap(left, top, width, height, mon_id, num_crects,
                           crects, cdata, cdata_bytes, codec_id, flags);
    }
    g_free(dd_rects);
    mi->aux_only = 0;
    if (rv == ENCODER_ERROR)
    {
        /* The encoder may be left unusable (iHD: a picture too big for the
           coded buffer fails every later one too), so start a new one. The
           client lacks the failed picture: the next is an IDR, as a new
           encoder's first picture is anyway. */
        if (!mi->idr_pending)
        {
            LOG(LOG_LEVEL_WARNING, "monitor %d: a frame did not encode; new "
                "encoder, the next frame is an IDR", mon_id);
        }
        mi->idr_pending = 1;
        if (mi->ei != NULL)
        {
            g_enc_funcs[g_enc].destroy_enc(mi->ei);
            mi->ei = NULL;
            if (xrdp_accel_assist_x11_recreate_enc(mi) != 0)
            {
                LOG(LOG_LEVEL_ERROR, "monitor %d: no new encoder", mon_id);
            }
        }
    }
    else if (flags & XH_ENC_FLAGS_FORCEIDR)
    {
        mi->idr_pending = 0;
    }
    if (rv == KEY_FRAME_ENCODED)
    {
        /* Consecutive IDRs must differ in idr_pic_id (7.4.3). */
        mi->idr_seq = (mi->idr_seq + 1) & 0xffff;
    }
    return rv;
}
