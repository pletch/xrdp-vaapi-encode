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
    int (*create_enc)(int width, int height, int tex, int tex_aux,
                      int tex_format, struct enc_info **ei);
    int (*destroy_enc)(struct enc_info *ei);
    enum encoder_result (*encode)(struct enc_info *ei, int tex,
                                  void *cdata, int *cdata_bytes,
                                  int flags);
    /* Optional: encode both AVC444 views, submitting each before waiting
       on either. NULL falls back to two encode() calls. */
    enum encoder_result (*encode_dual)(struct enc_info *ei,
                                       void *cdata1, int *cdata1_bytes,
                                       void *cdata2, int *cdata2_bytes,
                                       int flags);
};

static struct enc_funcs g_enc_funcs[] =
{
    {
#if defined(XRDP_VAAPI)
        xrdp_accel_assist_vaapi_init,
        xrdp_accel_assist_vaapi_create_encoder,
        xrdp_accel_assist_vaapi_delete_encoder,
        xrdp_accel_assist_vaapi_encode,
        xrdp_accel_assist_vaapi_encode_dual
#else
        NULL, NULL, NULL, NULL, NULL
#endif
    },
    {
#if defined(XRDP_NVENC)
        xrdp_accel_assist_nvenc_init,
        xrdp_accel_assist_nvenc_create_encoder,
        xrdp_accel_assist_nvenc_delete_encoder,
        xrdp_accel_assist_nvenc_encode,
        NULL
#else
        NULL, NULL, NULL, NULL, NULL
#endif
    }
};

/*****************************************************************************/
static int
inf_dmabuf_nop(inf_image_t inf_image)
{
    (void) inf_image;
    return 0;
}

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
    },
    {
        /* dma-buf source (headless, e.g. Wayland capture): the EGLImage is
           attached to the source texture once, so bind/release are no-ops */
        NULL,
        NULL,
        xrdp_accel_assist_inf_egl_destroy_dmabuf,
        inf_dmabuf_nop,
        inf_dmabuf_nop
    }
};

/* 0 = EGL, 1 = GLX */
/* 0 = va, 1 = nvenc */
#define INF_EGL     0
#define INF_GLX     1
#define INF_DMABUF  2
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
    Pixmap pixmap[ACCEL_ASSIST_MAX_BUFFERS];
    inf_image_t inf_image[ACCEL_ASSIST_MAX_BUFFERS];
    GLuint bmp_texture[ACCEL_ASSIST_MAX_BUFFERS];
    GLuint enc_texture;
    int cur_buf;                  /* capture buffer this frame used */
    /* Damage since the aux view was last rendered, as a bounding box. A
       box, not a region: it only has to be a superset, and one that grows
       past half the frame means render everything. */
    int aux_dirty;                /* 0 = nothing accumulated */
    int aux_x1, aux_y1, aux_x2, aux_y2;
    /* When the aux view last went out (monotonic ms). */
    unsigned int aux_last_ms;
    int aux_last_valid;           /* 0 until the first aux frame */
    /* When the last IDR went out, for any reason (monotonic ms). Set at
       creation, since the first picture is an IDR. */
    unsigned int idr_last_ms;
    /* Bytes sent since the last IDR, excluding it. */
    unsigned int idr_bytes_since;
    /* A frame failed to encode (e.g. too big for the shared buffer): the
       client lacks a picture the next ones may refer to, so the next frame
       is an IDR. */
    int idr_pending;
    int tex_format;
    GLfloat *(*get_vertices)(GLuint *vertices_bytes,
                             GLuint *vertices_pointes,
                             int num_crects, struct xh_rect *crects,
                             int left, int top, int width, int height);
    struct xh_rect viewport;
    struct enc_info *ei;
    /* AVC444 auxiliary view, set up on first use */
    GLuint enc_texture_aux;       /* AVC444 auxiliary view, 0 if not enabled */
    int avc444;                   /* both views live in mi->ei, one sequence */
    int avc444_v2;                /* ChromaV2 aux layout (codec id 0x000F) */
    int enc_w;                    /* encode width; 16-aligned for v2 */
    int enc_w4;                   /* enc_w / 4: the packed viewport width */
    int pad_h;                    /* encode height; == height for v2 */
    int avc444_frame_count;
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

#define XH_NUM_SHADERS 7

struct shader_info
{
    GLuint vertex_shader;
    GLuint fragment_shader;
    GLuint program;
    GLint tex_loc;
    GLint tex_size_loc;
    GLint pad_h_loc;       /* only present in the AV (aux) shader; -1 elsewhere */
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
static int
xrdp_accel_assist_gl_init(void);

/*****************************************************************************/
int
xrdp_accel_assist_x11_init(void)
{
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
    return xrdp_accel_assist_gl_init();
}

/*****************************************************************************/
/* Headless init for a non-X frame source: EGL on GBM, VA-API encode. */
int
xrdp_accel_assist_x11_init_headless(void *gbm_device)
{
    g_inf = INF_DMABUF;
    g_enc = ENC_VA;
    if (xrdp_accel_assist_inf_egl_init_gbm(gbm_device) != 0)
    {
        LOG(LOG_LEVEL_ERROR, "xrdp_accel_assist_x11_init_headless: "
            "EGL (GBM) init failed");
        return 1;
    }
    return xrdp_accel_assist_gl_init();
}

/*****************************************************************************/
static int
xrdp_accel_assist_gl_init(void)
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
    if (g_display == NULL)
    {
        return 0; /* headless: no X connection */
    }
    objs[*obj_count] = g_x_socket;
    (*obj_count)++;
    return 0;
}

/*****************************************************************************/
int
xrdp_accel_assist_x11_check_wait_objs(void)
{
    XEvent xevent;

    if (g_display == NULL)
    {
        return 0;
    }
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
        if (mi->pixmap[0] != 0 || mi->ei != NULL)
        {
            int buf;

            g_enc_funcs[g_enc].destroy_enc(mi->ei);
            mi->ei = NULL;
            glDeleteTextures(1, &(mi->enc_texture));
            if (mi->enc_texture_aux != 0)
            {
                glDeleteTextures(1, &(mi->enc_texture_aux));
                mi->enc_texture_aux = 0;
            }
            for (buf = 0; buf < ACCEL_ASSIST_MAX_BUFFERS; buf++)
            {
                glDeleteTextures(1, &(mi->bmp_texture[buf]));
                if (mi->inf_image[buf] != 0)
                {
                    g_inf_funcs[g_inf].destroy_image(mi->inf_image[buf]);
                    mi->inf_image[buf] = 0;
                }
                if (mi->pixmap[buf] != 0)
                {
                    XFreePixmap(g_display, mi->pixmap[buf]);
                    mi->pixmap[buf] = 0;
                }
            }
        }
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
    fh = mi->pad_h * 3 / 2;
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
        rows[1][0] = mi->pad_h + r->y / 2;
        rows[1][1] = mi->pad_h + (r->y + r->h + 1) / 2 + 1;
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
/* Send the aux (chroma) view every Nth frame; the others carry luma only
   (LC=1, MS-RDPEGFX 2.2.4.5), as a Windows host does. Picture count is
   the binding cost. A long-term aux reference keeps skipped frames cheap.
   Larger intervals add chroma lag and let the aux damage box grow toward
   the full-frame fallback. XRDP_AVC444_CHROMA_INTERVAL, default 4. */
static int
xrdp_accel_assist_x11_chroma_interval(void)
{
    static int interval = -1;
    const char *env;

    if (interval < 0)
    {
        env = g_getenv("XRDP_AVC444_CHROMA_INTERVAL");
        interval = (env != NULL) ? g_atoi(env) : 4;
        if (interval < 1)
        {
            interval = 1;
        }
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11: AVC444 chroma interval "
            "%d (1 = send the aux view every frame)", interval);
    }
    return interval;
}

/* Upper bound on chroma lag in ms. Frames are damage-driven, so on a quiet
   desktop "every Nth frame" can be seconds; the aux also goes out once this
   much time has passed. XRDP_AVC444_CHROMA_MAX_MS, default 200; 0
   disables. */
static int
xrdp_accel_assist_x11_chroma_max_ms(void)
{
    static int max_ms = -1;
    const char *env;

    if (max_ms < 0)
    {
        env = g_getenv("XRDP_AVC444_CHROMA_MAX_MS");
        max_ms = (env != NULL) ? g_atoi(env) : 200;
        if (max_ms < 0)
        {
            max_ms = 0;
        }
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11: AVC444 chroma deadline "
            "%d ms (0 = off, frame interval only)", max_ms);
    }
    return max_ms;
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

static int
create_encode_surface(struct mon_info *mi, int width, int height);

/*****************************************************************************/
int
xrdp_accel_assist_x11_create_pixmap(int width, int height, int magic,
                                    int con_id, int mon_id)
{
    struct mon_info *mi;
    XImage *ximage;
    int img[64];
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
    return create_encode_surface(mi, width, height);
}

/*****************************************************************************/
/* A monitor surface fed by a non-X source: no pixmaps; the caller attaches
   each capture buffer with xrdp_accel_assist_x11_set_source_image(). */
int
xrdp_accel_assist_x11_create_surface(int width, int height, int mon_id)
{
    struct mon_info *mi;

    mi = g_mons + mon_id % MAX_MON;
    if (mi->ei != NULL)
    {
        LOG(LOG_LEVEL_ERROR, "xrdp_accel_assist_x11_create_surface: "
            "error already setup");
        return 1;
    }
    LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_create_surface: "
        "width %d height %d mon_id %d", width, height, mon_id);
    return create_encode_surface(mi, width, height);
}

/*****************************************************************************/
/* Attach an image (a dma-buf EGLImage) as capture buffer buf's source
   texture. Replaces, and destroys, any image attached before. */
int
xrdp_accel_assist_x11_set_source_image(int mon_id, int buf,
                                       inf_image_t inf_image)
{
    struct mon_info *mi;

    mi = g_mons + mon_id % MAX_MON;
    if (buf < 0 || buf >= ACCEL_ASSIST_MAX_BUFFERS ||
            mi->bmp_texture[buf] == 0)
    {
        return 1;
    }
    if (mi->inf_image[buf] != 0)
    {
        g_inf_funcs[g_inf].destroy_image(mi->inf_image[buf]);
    }
    mi->inf_image[buf] = inf_image;
    glBindTexture(GL_TEXTURE_2D, mi->bmp_texture[buf]);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES) inf_image);
    glBindTexture(GL_TEXTURE_2D, 0);
    return 0;
}

/*****************************************************************************/
static int
create_encode_surface(struct mon_info *mi, int width, int height)
{
    GLuint enc_texture;
    int buf;

    glEnable(GL_TEXTURE_2D);
    /* texture that gets encoded */
    glGenTextures(1, &enc_texture);
    glBindTexture(GL_TEXTURE_2D, enc_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if ((g_enc == ENC_NVENC) || (g_enc == ENC_VA))
    {
        /* NV12 in a single R8 texture: Y (width x height), then
           interleaved UV (width x height / 2). */
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_create_pixmap: "
            "using XH_YUV420");
        mi->tex_format = XH_YUV420;
        /* A new surface invalidates the accumulated damage box. */
        mi->aux_dirty = 0;
        mi->aux_last_valid = 0;
        mi->aux_last_ms = 0;
        mi->idr_last_ms = g_get_elapsed_ms();
        mi->idr_bytes_since = 0;
        mi->idr_pending = 0;
        mi->avc444 = xrdp_accel_assist_x11_avc444_enabled();
        mi->avc444_v2 = mi->avc444 && xrdp_accel_assist_x11_avc444_v2();
        /* v2 splits the aux plane into U and V halves at half the
           16-aligned width (FreeRDP's nTotalWidth; mstsc agrees), so encode
           at the aligned width. Clients copy only the surface width. */
        mi->enc_w = mi->avc444_v2 ? ((width + 15) & ~15) : width;
        /* v1's tiled layout needs the aux padded to 16 rows (MS-RDPEGFX
           2.2.4.4.2); v2 needs none. */
        mi->pad_h = (mi->avc444 && !mi->avc444_v2)
                    ? ((height + 15) & ~15) : height;
        /* RGBA8 over a quarter-width viewport: four bytes per fragment.
           Same bytes as the R8 view, so the exported dma-buf is unchanged. */
        mi->enc_w4 = (mi->enc_w + 3) / 4;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mi->enc_w4,
                     mi->pad_h * 3 / 2, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        mi->get_vertices = get_vertices420;
        mi->viewport.x = 0;
        mi->viewport.y = 0;
        mi->viewport.w = mi->enc_w4;
        mi->viewport.h = mi->pad_h * 3 / 2;
    }
    else
    {
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_create_pixmap: "
            "using XH_YUV444");
        mi->tex_format = XH_YUV444;
        mi->enc_w = width;
        mi->enc_w4 = width;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, NULL);
        mi->get_vertices = get_vertices444;
        mi->viewport.x = 0;
        mi->viewport.y = 0;
        mi->viewport.w = width;
        mi->viewport.h = height;
    }
    /* one source texture per capture buffer */
    for (buf = 0; buf < ACCEL_ASSIST_MAX_BUFFERS; buf++)
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
        glGenTextures(1, &(mi->enc_texture_aux));
        glBindTexture(GL_TEXTURE_2D, mi->enc_texture_aux);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        /* Packed four bytes to a fragment, like the main view. */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mi->enc_w4,
                     mi->pad_h * 3 / 2, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_create_pixmap: "
            "AVC444%s enabled, encoding both views at %dx%d (desktop %dx%d)",
            mi->avc444_v2 ? "v2" : "v1", mi->enc_w, mi->pad_h, width, height);
    }

    if (g_enc_funcs[g_enc].create_enc(mi->enc_w, mi->pad_h,
                                      enc_texture, mi->enc_texture_aux,
                                      mi->tex_format,
                                      &(mi->ei)) != 0)
    {
        return 1;
    }

    mi->enc_texture = enc_texture;
    mi->width = width;
    mi->height = height;

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
xrdp_accel_assist_x11_dump_plane(const char *tag, int frame, GLuint tex,
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
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, packed ? GL_RGBA : GL_RED,
                 GL_UNSIGNED_BYTE, pixels);
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
/* pad_h: 0 uses mi->height as the Y/UV boundary; >0 sets it for the v1
   aux surface padded to 16 rows (MS-RDPEGFX 2.2.4.4.2). */
static void
xrdp_accel_assist_x11_run_shader(int left, int top, int width, int height,
                                 struct mon_info *mi,
                                 struct shader_info *si, GLuint enc_texture,
                                 int num_crects, struct xh_rect *crects,
                                 int pad_h, int vp_w, int aux_v2)
{
    GLuint vao;
    GLuint vbo;
    GLfloat *vertices;
    GLuint vertices_bytes;
    GLuint vertices_pointes;

    /* rgb to yuv */
    glEnable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mi->bmp_texture[mi->cur_buf]);
    g_inf_funcs[g_inf].bind_tex_image(mi->inf_image[mi->cur_buf]);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, enc_texture, 0);
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
        vertices = mi->get_vertices(&vertices_bytes, &vertices_pointes,
                                    num_crects, crects,
                                    left, top, width, height);
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
       differ in byte width under v1. */
    if (pad_h > 0)
    {
        glViewport(mi->viewport.x, mi->viewport.y, vp_w, pad_h * 3 / 2);
    }
    else
    {
        glViewport(mi->viewport.x, mi->viewport.y, vp_w, mi->viewport.h);
    }
    glDrawArrays(GL_TRIANGLES, 0, vertices_pointes);
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
static void
flush_gl(void)
{
    if (g_display != NULL)
    {
        XFlush(g_display);
    }
    else
    {
        glFlush();
    }
}

/*****************************************************************************/
static enum encoder_result
encode_pixmap(int left, int top, int width, int height,
              int mon_id, int num_crects, struct xh_rect *crects,
              void *cdata, int *cdata_bytes, int codec_id, int flags)
{
    struct mon_info *mi;
    struct shader_info *si;
    enum encoder_result rv;

    mi = g_mons + mon_id % MAX_MON;
    /* Which capture buffer xorgxrdp used for this frame. */
    mi->cur_buf = flags & ACCEL_ASSIST_BUFFER_MASK;
    mi->cur_buf >>= ACCEL_ASSIST_BUFFER_SHIFT;
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
        int aux_i;
        int aux_stage;
        unsigned int t_copy;
        unsigned int now_ms;
        int max_ms;
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
        max_ms = xrdp_accel_assist_x11_chroma_max_ms();
        send_aux = ((frame_no % xrdp_accel_assist_x11_chroma_interval()) == 0)
                   || ((flags & XH_ENC_FLAGS_FORCEIDR) != 0)
                   || (max_ms > 0 && (!mi->aux_last_valid
                                      || now_ms - mi->aux_last_ms
                                      >= (unsigned int) max_ms));
        if (send_aux)
        {
            mi->aux_last_ms = now_ms;
            mi->aux_last_valid = 1;
        }

        /* main view (MV shader) */
        if (frame_no == 0)
        {
            g_dump_src_frame = frame_no;
        }
        t_copy = xrdp_accel_assist_x11_time_copy();
        si = g_si + XH_SHADERRGB2YUV420MV;
        xrdp_accel_assist_x11_run_shader(left, top, width, height, mi, si,
                                         mi->enc_texture, num_crects, crects,
                                         mi->pad_h, mi->enc_w4, 0);
        /* Split the GL time into xorgxrdp's copy and our conversion. */
        xrdp_accel_assist_x11_time_gl(t_copy);
        if (frame_no == 0)
        {
            xrdp_accel_assist_x11_dump_plane("main", frame_no,
                                             mi->enc_texture,
                                             mi->enc_w4, mi->pad_h * 3 / 2,
                                             1);
        }

        /* Aux view, on the chroma interval, deadline or an IDR; otherwise
           len2 stays 0 and xrdp sends LC=1. v1 renders full-frame (its
           tiling scatters rects); v2 renders the accumulated damage box,
           which is also what the metablock declares. */
        /* Accumulate damage for the aux view whether or not it goes out. */
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
            if (mi->avc444_v2)
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
                                                 mi, si, mi->enc_texture_aux,
                                                 1, &aux_rect,
                                                 mi->pad_h, mi->enc_w4, 1);
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
                                                 mi->enc_texture_aux, 1,
                                                 &full_rect, mi->pad_h,
                                                 mi->enc_w4, 0);
            }
            mi->aux_dirty = 0;
            if (frame_no == 0)
            {
                xrdp_accel_assist_x11_dump_plane("aux", frame_no,
                                                 mi->enc_texture_aux,
                                                 mi->enc_w4,
                                                 mi->pad_h * 3 / 2, 1);
            }
        }
        flush_gl();

        len2 = 0;
        rv2 = INCREMENTAL_FRAME_ENCODED;
        if (!send_aux)
        {
            len1 = avail - 8;
            rv = g_enc_funcs[g_enc].encode(mi->ei, mi->enc_texture,
                                           p + 4, &len1, flags);
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
                                                flags);
            if (rv == ENCODER_ERROR)
            {
                return ENCODER_ERROR;
            }
            g_memmove(p + 4 + len1 + 4, p + aux_stage, len2);
        }
        else
        {
            len1 = avail - 8;
            rv = g_enc_funcs[g_enc].encode(mi->ei, mi->enc_texture,
                                           p + 4, &len1, flags);
            if (rv == ENCODER_ERROR)
            {
                return ENCODER_ERROR;
            }
            len2 = avail - 8 - len1;
            /* Never FORCEIDR: only the first view of a frame may be an
               IDR. */
            rv2 = g_enc_funcs[g_enc].encode(mi->ei, mi->enc_texture_aux,
                                            p + 4 + len1 + 4, &len2,
                                            (flags & ~XH_ENC_FLAGS_FORCEIDR) |
                                            XH_ENC_FLAGS_AUXVIEW);
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
                                     mi->enc_texture, num_crects, crects,
                                     mi->pad_h, mi->enc_w4, 0);
    /* flush before encoding, let encoders call glFinish() as needed */
    flush_gl();
    /* encode */
    rv = g_enc_funcs[g_enc].encode(mi->ei, mi->enc_texture,
                                   cdata, cdata_bytes, flags);
    return rv;
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

    if (mi->ei == NULL)
    {
        /* its re-creation after a failure failed: try again */
        if (mi->enc_w <= 0 ||
                g_enc_funcs[g_enc].create_enc(mi->enc_w, mi->pad_h,
                                              mi->enc_texture,
                                              mi->enc_texture_aux,
                                              mi->tex_format, &mi->ei) != 0)
        {
            mi->ei = NULL;
            return ENCODER_ERROR;
        }
    }
    if (mi->idr_pending)
    {
        flags |= XH_ENC_FLAGS_FORCEIDR;
    }
    rv = encode_pixmap(left, top, width, height, mon_id, num_crects, crects,
                       cdata, cdata_bytes, codec_id, flags);
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
            if (g_enc_funcs[g_enc].create_enc(mi->enc_w, mi->pad_h,
                                              mi->enc_texture,
                                              mi->enc_texture_aux,
                                              mi->tex_format, &mi->ei) != 0)
            {
                LOG(LOG_LEVEL_ERROR, "monitor %d: no new encoder", mon_id);
                mi->ei = NULL;
            }
        }
    }
    else if (flags & XH_ENC_FLAGS_FORCEIDR)
    {
        mi->idr_pending = 0;
    }
    return rv;
}
