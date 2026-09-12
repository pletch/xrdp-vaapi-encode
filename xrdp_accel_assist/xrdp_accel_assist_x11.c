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
    /* Optional: encode both AVC444 views for one frame, submitting each
       before waiting on either. NULL falls back to two encode() calls. */
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
    /* Two capture buffers. xorgxrdp copies a frame into one while the helper
       is still reading the other, so the two overlap; the surface command
       carries ACCEL_ASSIST_BUFFER_1 to say which one this frame used. Each
       buffer is given the damage it missed as well as the current frame's,
       so both are complete pictures -- which the full-frame AVC444 auxiliary
       pass depends on. */
    Pixmap pixmap[2];
    inf_image_t inf_image[2];
    GLuint bmp_texture[2];
    GLuint enc_texture;
    int cur_buf;                  /* capture buffer this frame used */
    /* Damage accumulated since the auxiliary view was last rendered, as a
       bounding box. The aux goes out every XRDP_AVC444_CHROMA_INTERVAL
       frames, so rendering only the current frame's damage would leave
       chroma stale wherever the screen changed in the frames between. Kept
       as a box rather than a region because it only has to be a superset,
       and because a box that grows to most of the frame is the signal to
       give up and render everything. */
    int aux_dirty;                /* 0 = nothing accumulated */
    int aux_x1, aux_y1, aux_x2, aux_y2;
    int tex_format;
    GLfloat *(*get_vertices)(GLuint *vertices_bytes,
                             GLuint *vertices_pointes,
                             int num_crects, struct xh_rect *crects,
                             int left, int top, int width, int height);
    struct xh_rect viewport;
    struct enc_info *ei;
    /* AVC444: second (auxiliary chroma) view, set up lazily on first use */
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
            glDeleteTextures(1, &(mi->enc_texture));
            if (mi->enc_texture_aux != 0)
            {
                glDeleteTextures(1, &(mi->enc_texture_aux));
                mi->enc_texture_aux = 0;
            }
            for (buf = 0; buf < 2; buf++)
            {
                glDeleteTextures(1, &(mi->bmp_texture[buf]));
                g_inf_funcs[g_inf].destroy_image(mi->inf_image[buf]);
                XFreePixmap(g_display, mi->pixmap[buf]);
                mi->pixmap[buf] = 0;
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
        /* Each fragment of the packed destination covers four bytes, so a
           rect edge falling inside one would leave the rest of it unwritten.
           Round outward to a multiple of four. */
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
/* Destination quads for the v2 auxiliary view, from source damage rects.

   The aux was rendered full-frame because v1 has no choice: its B4/B5 tiling
   sends one damage rect to scattered destination rows. v2's mapping is
   affine, so a rect maps to four destination quads and the shader pass can be
   confined to them.

   From the shader: with x1 = ceil(W/16)*8, destination byte column c and row
   y read source column 2c+1 (c < x1) or 2(c-x1)+1 (c >= x1) at row y in the
   luma region, and 2c or 2(c-x1) at row 2(y-H)+1 in the chroma region. So a
   source rect covers one column range in each of the U and V halves, over two
   row ranges. Bounds are rounded outward: writing a few extra bytes is free,
   missing one leaves a stale pixel that the full-frame pass would have
   covered.

   This saves the shader pass only. Both views encode a whole H.264 picture
   regardless and the encoder skips macroblocks that did not change, and the
   full-frame render writes identical values to unchanged areas -- so the
   surface handed to the encoder, and the bitstream, are the same either way.
   Measured: the auxiliary picture is 15.5 KB at a chroma interval of 8 and
   3.2 KB at 1, with the same full-frame rendering both times, because its
   size follows how old its reference is rather than how much was drawn. */
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
            /* Byte columns to fragment columns: four bytes to a fragment. */
            GLfloat fx1 = ((c1 + cols[half]) / 4) / fw * 2.0f - 1.0f;
            GLfloat fx2 = ((c2 + cols[half] + 3) / 4) / fw * 2.0f - 1.0f;
            GLfloat fy1 = rows[band][0] / fh * 2.0f - 1.0f;
            GLfloat fy2 = rows[band][1] / fh * 2.0f - 1.0f;

            vert = vertices + nquads * 12;
            vert[0]  = fx1; vert[1]  = fy1;
            vert[2]  = fx1; vert[3]  = fy2;
            vert[4]  = fx2; vert[5]  = fy1;
            vert[6]  = fx1; vert[7]  = fy2;
            vert[8]  = fx2; vert[9]  = fy1;
            vert[10] = fx2; vert[11] = fy2;
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

/* Session capabilities from xorgxrdp (message type 4), zero until it sends
   them. XH_CAPS_AVC444 means the EGFX capability set xrdp confirmed to the
   client permits RDPGFX_CODECID_AVC444; XH_CAPS_AVC444_V2 additionally says
   the v2 chroma layout (codec id 0x000F) may be used. */
static int g_session_caps = 0;


/*****************************************************************************/
void
xrdp_accel_assist_x11_set_caps(int caps)
{
    g_session_caps = caps;
}

/*****************************************************************************/
/* AVC444 has to be decided when the pixmap is created, not lazily on the
   first AVC444 frame: both views are pictures of one H.264 sequence and must
   share its dimensions, so the main view's NV12 surface has to be allocated
   at the 16-aligned height too.

   Normally this follows what the client actually negotiated, which xorgxrdp
   forwards in the capability message. XRDP_ACCEL_AVC444 overrides that, and
   is tri-state:

     unset  follow what the client negotiated
     "0"    force AVC444 off, whatever the client supports
     other  force AVC444 on -- how AVC444 was reached before negotiation
            existed, and unsafe against a client that cannot decode 0x000E

   The "0" case matters. Before negotiation, this was a presence test and
   simply removing the variable turned AVC444 off. Now that negotiation can
   turn it on by itself, an off switch has to exist, and a presence test
   would make the obvious spelling of it (=0) do the opposite.

   xorgxrdp applies the same rule to the codec id it asks for, reading the
   same variable out of the session environment, so the two cannot
   disagree. */
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
/* ChromaV1 (codec id 0x000E) or ChromaV2 (0x000F) for the auxiliary view.

   v2 compresses the auxiliary view considerably better, so it is worth
   having on a link where bandwidth costs more than it does on a LAN. v1's
   aux plane is vertically subsampled and discontinuous -- within each group
   of eight rows it carries U from every other source chroma row, then jumps
   to V of those same rows -- while v2's row mapping is the identity: every
   source row, full vertical resolution, and one seam in the whole plane.

   It is correct on both mstsc and FreeRDP at any width now that the U/V
   split goes at half the 16-aligned width (see create_pixmap), verified by
   aa444map -2 offline and on mstsc at 3000x2000, the resolution that
   previously fringed.

   Default, having been confirmed on mstsc and on guacd's WebGL2 combiner
   at 3000x2000 -- a non-aligned width, the case that used to fringe -- as
   well as at aligned widths.

   One consequence to know: for v2 the H.264 picture is the 16-aligned
   width and carries no horizontal cropping, which is the arrangement
   clients expect. A client that sizes its output from the decoded picture
   rather than from the surface would show a few clamp-to-edge columns on
   the right; neither client tested does.

   Which layout to use is not something the client can state. MS-RDPEGFX
   defines no capability flag separating them -- the whole set is
   THINCLIENT, SMALL_CACHE, AVC420_ENABLED, AVC_DISABLED, AVC_THINCLIENT
   and SCALEDMAP_DISABLE -- so the confirmed capability version is the only
   signal, and the choice is finally the server's. xrdp makes it and sends
   the answer down as XH_CAPS_AVC444_V2; it holds a bare 10.0 client to v1,
   since the v2 layout post-dates that capability set and a client that
   cannot parse codec id 0x000F has no way to say so.

   XRDP_ACCEL_AVC444_V2 overrides, tri-state: unset follows what xrdp
   decided, "0" forces v1, anything else forces v2. xorgxrdp applies the
   same rule to the same variable, so the codec id it asks for and the
   encoder built here cannot disagree. */
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
/* How often the auxiliary (chroma) view is sent.

   MS-RDPEGFX 2.2.4.5 lets a frame carry luma only, by setting LC=1 and
   omitting the second bitstream, and a Windows host exploits that.

   The cost that binds is PICTURES per second, not bytes -- encode
   submissions, decode calls and WebCodecs chunks all scale with picture
   count. Measured on video playback, busiest sustained 30s of each run so
   idle time does not skew the frame rate:

                     fps   Mbit/s   KB/frame   pictures/s
     AVC420         29.6     0.91        3.8         29.6
     interval 3     28.9     8.24       34.8         38.6
     interval 1     24.3     2.21       11.1         48.5

   Interval 3 pushed 4x the bitrate there and still ran faster. That 4x was
   never inherent: before the auxiliary view became a long-term reference, an
   interval above 1 had to code the aux non-reference (see single_ref in the
   VAAPI encoder), so it predicted from the main picture of the same frame --
   luma, not packed chroma -- and coded as good as intra, 77.3 KB against 4.3
   KB. With LTR the previous aux survives the sliding window across however
   many main pictures are skipped, and the aux stays predicted: interval 3
   came down to 1.84 Mbit/s, aux p50 5.9 KB. So the bitrate argument for
   interval 1 is gone, and the numbers above are kept only as the pre-LTR
   history that explains the shape.

   What now argues against a LARGE interval is on the other side:

     - Latency. Chroma trails luma by up to interval-1 frames -- around 280
       ms at interval 8 and 25 fps, against 80 ms at interval 3.

     - The damage box. The v2 aux pass is confined to the accumulated damage
       (see the caller), and that box is only reset when the aux is actually
       sent. A longer interval accumulates more damage per aux frame and
       reaches the half-the-frame full-frame fallback sooner, so the cost of
       an aux frame rises with the interval instead of staying flat. It
       degrades to the old full-frame behaviour, no worse, but the saving
       from raising the interval is sublinear.

   And the saving itself saturates. Pictures per frame: interval 1 = 2.0,
   3 = 1.33, 5 = 1.20, 8 = 1.125. 1->3 removes a third of the pictures,
   3->8 another sixth. Two linear costs against a saturating benefit put the
   useful range around 3 to 5 rather than higher.

   Default 1 is conservative and not what the measurements favour; raise it
   once a run at 4 has been compared against 1 on feel and on the frame log.
   Worth counting how often the aux hits the full-frame fallback while doing
   that -- if it fires on most aux frames, the interval is buying latency and
   nothing else. */
static int
xrdp_accel_assist_x11_chroma_interval(void)
{
    static int interval = -1;
    const char *env;

    if (interval < 0)
    {
        env = g_getenv("XRDP_AVC444_CHROMA_INTERVAL");
        interval = (env != NULL) ? g_atoi(env) : 1;
        if (interval < 1)
        {
            interval = 1;
        }
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11: AVC444 chroma interval "
            "%d (1 = send the aux view every frame)", interval);
    }
    return interval;
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

    /* Two capture buffers, registered separately. The buffer index rides in
       the high bits of the mon_id word of the magic message, which xorgxrdp
       splits back out. */
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
    /* texture that gets encoded */
    glGenTextures(1, &enc_texture);
    glBindTexture(GL_TEXTURE_2D, enc_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if ((g_enc == ENC_NVENC) || (g_enc == ENC_VA))
    {
        /* NV12: single R8 texture, Y plane (width x height) followed by
           interleaved UV plane (width x height / 2). Both nvenc and the
           VA-API encoder (Intel iHD H.264 low-power) consume this layout
           via a dma-buf export described as NV12. */
        LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_create_pixmap: "
            "using XH_YUV420");
        mi->tex_format = XH_YUV420;
        /* A new surface holds nothing, so the accumulated damage box from a
           previous one means nothing either. */
        mi->aux_dirty = 0;
        mi->avc444 = xrdp_accel_assist_x11_avc444_enabled();
        mi->avc444_v2 = mi->avc444 && xrdp_accel_assist_x11_avc444_v2();
        /* v2's aux plane is split into a U half and a V half at half the
           16-ALIGNED width -- FreeRDP passes the aligned width as
           nTotalWidth (yuv.c:507) and mstsc agrees. So the encode width has
           to be the aligned width, or the V half does not fit and every V
           sample lands eight source pixels out. The main view shares the
           encoder's dimensions, and the client copies only the surface width
           from each row, so the extra columns cost a little bandwidth and
           are otherwise ignored. */
        mi->enc_w = mi->avc444_v2 ? ((width + 15) & ~15) : width;
        /* v1's tiled B4/B5 layout needs the aux surface padded to a multiple
           of 16 rows (MS-RDPEGFX 2.2.4.4.2); v2's row mapping is the
           identity, so it needs no padding at all. Both views share the
           encoder's dimensions either way. */
        mi->pad_h = (mi->avc444 && !mi->avc444_v2)
                    ? ((height + 15) & ~15) : height;
        /* Four destination bytes per fragment: RGBA8 over a quarter-width
           viewport. The row is the same size in bytes either way, so the
           dma-buf handed to VAAPI is unchanged; only the shader's view of
           it differs. See the comment above g_fs_rgb_to_yuv420_mv. */
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
    for (buf = 0; buf < 2; buf++)
    {
        glGenTextures(1, &(mi->bmp_texture[buf]));
        glBindTexture(GL_TEXTURE_2D, mi->bmp_texture[buf]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    /* AVC444 auxiliary view: same NV12 geometry as the main view, since both
       are pictures of one H.264 sequence. */
    if (mi->avc444)
    {
        glGenTextures(1, &(mi->enc_texture_aux));
        glBindTexture(GL_TEXTURE_2D, mi->enc_texture_aux);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        /* Packed four bytes to a fragment like the main view: RGBA8 over a
           quarter-width viewport. Same bytes, same stride, so the dma-buf
           VAAPI imports is unchanged -- only the shader's view of it. */
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
/* test/validation helper: expose the X pixmap so a harness can inject a
   known image before encoding (returns the Pixmap XID) */
long
xrdp_accel_assist_x11_get_pixmap(int mon_id)
{
    return (long) (g_mons[mon_id % MAX_MON].pixmap[0]);
}

/*****************************************************************************/
/* AVC444 plane dump.
 *
 * Reads an R8 encode texture back through the shared FBO and writes it raw,
 * so the GPU's packing can be checked against the CPU model of the same
 * shader (aa444map in ~/aa444work). The main texture is W x (H*3/2) NV12,
 * the aux texture W x (H_PAD*3/2) NV12.
 *
 * Enable with XRDP_AVC444_DUMP_DIR=/some/dir; XRDP_AVC444_DUMP_FRAMES sets
 * how many frames to capture (default 1). Files land as
 *   <dir>/<tag>_<frame>_<w>x<h>.nv12
 * where h is the *full* texture height including the UV plane. Off unless
 * the directory is set, so this costs a single getenv per session.
 */
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
    /* A packed target is RGBA8 at a quarter of the byte width; reading it
       back as RGBA gives the same byte stream the encoder sees. */
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
/* XRDP_VAAPI_TIMING: drain and time the GL pass, so the conversion can be
   separated from the encode. A stall, so measurement only. */
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

/* Drain before the shader: this waits for xorgxrdp's CopyArea, which is
   chained ahead of us by implicit sync since we flush rather than drain on
   that side. Whatever is left afterwards is our own conversion. */
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
/* set by the caller to the frame number when the next run_shader should
   also dump its *source* texture; -1 = don't. One-shot. */
static int g_dump_src_frame = -1;

/*****************************************************************************/
/* Dump the BGRA source texture. Must be called while the caller still holds
   bind_tex_image on it, i.e. from inside run_shader after the draw. */
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
static int
xrdp_accel_assist_x11_dump_frames(void)
{
    static int frames = -1;
    const char *env;

    if (frames < 0)
    {
        env = g_getenv("XRDP_AVC444_DUMP_FRAMES");
        frames = (env != NULL) ? g_atoi(env) : 1;
        if (frames < 0)
        {
            frames = 0;
        }
    }
    return frames;
}

/*****************************************************************************/
/* pad_h: 0 = use mi->height as the Y/UV plane boundary (default for MV/444
   paths); >0 = override for the AV aux path where the NV12 surface is sized
   to (W, ((H+15)&~15)) per MS-RDPEGFX 2.2.4.4.2. When pad_h > 0 the viewport
   height is widened to pad_h*3/2 and the pad_h shader uniform is populated. */
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
    /* The v2 auxiliary view maps a damage rect to four destination quads, so
       it builds its own vertices; everything else goes through the monitor's
       usual builder. */
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
    /* Both views are now packed four bytes to a fragment, so vp_w is a
       quarter of the byte width for each. It stays a parameter because the
       two planes have different byte widths under v1, and because using one
       viewport for both once left the aux plane a quarter filled -- a
       vertical split with the chroma halves wrong. */
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
enum encoder_result
xrdp_accel_assist_x11_encode_pixmap(int left, int top, int width, int height,
                                    int mon_id, int num_crects,
                                    struct xh_rect *crects,
                                    void *cdata, int *cdata_bytes,
                                    int codec_id, int flags)
{
    struct mon_info *mi;
    struct shader_info *si;
    enum encoder_result rv;

    mi = g_mons + mon_id % MAX_MON;
    /* Which of the two capture buffers xorgxrdp copied this frame into. It
       alternates them so it can capture the next frame while we are still
       reading this one; counting on our side instead would desync the moment
       a frame is dropped. */
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
    if (codec_id == XH_CODECID_AVC444 || codec_id == XH_CODECID_AVC444V2)
    {
        /* AVC444: two H.264 streams. Main (luma + 1/4 chroma) via the MV
           shader, auxiliary (remaining 3/4 chroma) via the AV shader. The
           output buffer is framed as:
             [4-byte LE len1][stream1][4-byte LE len2][stream2]

           The synchronised periodic IDR that used to live here is now off by
           default. It was a mitigation for what was diagnosed as
           "reference-frame drift" between the two encoders, but two H.264
           streams cannot drift from their own decoders -- decode is
           bit-exact. What was actually visible was the main and aux
           reconstructions of the same chroma being refreshed at different
           times, because the aux metablock declared the full frame while the
           main declared only the damage rects (see gfx_wiretosurface1 in
           xrdp/xrdp_encoder.c, now fixed to declare matching rects). A
           full-frame IDR on both streams hid that by refreshing everything
           at once, at 3-4x the bandwidth.

           Set XRDP_AVC444_IDR_PERIOD=N to bring the cadence back for A/B
           comparison. */
        unsigned char *p = (unsigned char *) cdata;
        int avail = *cdata_bytes;
        int len1;
        int len2;
        enum encoder_result rv2;
        static int idr_period = -1;
        int frame_no = mi->avc444_frame_count++;
        /* The rect the aux was rendered over, forwarded to xrdp so it can
           declare it rather than assuming the whole frame. Only meaningful
           for v2: the v1 pass is full-frame by construction. */
        int have_aux_rect = 0;
        int aux_x1 = 0;
        int aux_y1 = 0;
        int aux_x2 = 0;
        int aux_y2 = 0;
        int send_aux;
        int aux_i;
        int aux_stage;
        unsigned int t_copy;

        if (idr_period < 0)
        {
            const char *env = g_getenv("XRDP_AVC444_IDR_PERIOD");
            idr_period = (env != NULL) ? g_atoi(env) : 0;
            if (idr_period < 0)
            {
                idr_period = 0;
            }
            LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_x11_encode_pixmap: "
                "AVC444 synced IDR period %d (0 = off)", idr_period);
        }
        if (idr_period > 0 && (frame_no % idr_period) == 0)
        {
            flags |= XH_ENC_FLAGS_FORCEIDR;
        }

        if (!mi->avc444)
        {
            LOG(LOG_LEVEL_ERROR, "xrdp_accel_assist_x11_encode_pixmap: "
                "AVC444 requested but the pixmap was created without it -- "
                "XRDP_ACCEL_AVC444 must be set before the session starts");
            return ENCODER_ERROR;
        }

        /* Decide up front whether this frame carries chroma, so both views
           can be submitted before either is waited on. */
        send_aux = ((frame_no % xrdp_accel_assist_x11_chroma_interval()) == 0)
                   || ((flags & XH_ENC_FLAGS_FORCEIDR) != 0);

        /* main view -> enc_texture (MV shader) */
        if (frame_no < xrdp_accel_assist_x11_dump_frames())
        {
            g_dump_src_frame = frame_no;
        }
        t_copy = xrdp_accel_assist_x11_time_copy();
        si = g_si + XH_SHADERRGB2YUV420MV;
        xrdp_accel_assist_x11_run_shader(left, top, width, height, mi, si,
                                         mi->enc_texture, num_crects, crects,
                                         mi->pad_h, mi->enc_w4, 0);
        /* Splits the GL cost into waiting for xorgxrdp's copy and running our
           own conversion. Both are drains, so measurement only. */
        xrdp_accel_assist_x11_time_gl(t_copy);
        if (frame_no < xrdp_accel_assist_x11_dump_frames())
        {
            xrdp_accel_assist_x11_dump_plane("main", frame_no,
                                             mi->enc_texture,
                                             mi->enc_w4, mi->pad_h * 3 / 2,
                                             1);
        }

        /* Auxiliary (chroma) view.

           Sent every chroma_interval-th frame and on every IDR; otherwise
           len2 stays 0 and xrdp emits LC=1, a luma-only frame with no second
           bitstream. See xrdp_accel_assist_x11_chroma_interval().

           v1 is rendered full-frame and has no choice: its B4/B5 tiling maps
           a damage rect to scattered destination rows. v2's mapping is
           affine, so its pass is confined to the accumulated damage box and
           that same box is what the metablock declares (aa444map says left
           must be a multiple of 4 and top even). */
        /* Accumulate this frame's damage for the auxiliary view, whether or
           not it goes out now. */
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
                if (r->x < mi->aux_x1) { mi->aux_x1 = r->x; }
                if (r->y < mi->aux_y1) { mi->aux_y1 = r->y; }
                if (r->x + r->w > mi->aux_x2) { mi->aux_x2 = r->x + r->w; }
                if (r->y + r->h > mi->aux_y2) { mi->aux_y2 = r->y + r->h; }
            }
        }
        if (send_aux)
        {
            struct xh_rect full_rect;
            struct xh_rect aux_rect;
            full_rect.x = 0;
            full_rect.y = 0;
            full_rect.w = mi->enc_w;
            full_rect.h = mi->pad_h;     /* for v1, out to the pad rows so B5
                                            V-chroma for the bottom desktop
                                            rows reaches aux Y H..pad_h-1 */
            si = g_si + (mi->avc444_v2 ? XH_SHADERRGB2YUV420AVV2
                                       : XH_SHADERRGB2YUV420AV);
            /* v2's mapping is affine, so the pass can be confined to the
               damage; v1's B4/B5 tiling scatters a rect across
               non-contiguous destination rows and stays full-frame. */
            if (mi->avc444_v2)
            {
                /* The accumulated box, not this frame's rects. Past half the
                   frame there is nothing left to save, and a box that big is
                   usually about to grow further, so take the whole thing. */
                aux_rect.x = mi->aux_x1;
                aux_rect.y = mi->aux_y1;
                aux_rect.w = mi->aux_x2 - mi->aux_x1;
                aux_rect.h = mi->aux_y2 - mi->aux_y1;
                if (!mi->aux_dirty ||
                    (flags & XH_ENC_FLAGS_FORCEIDR) != 0 ||
                    aux_rect.w * aux_rect.h * 2 >= width * height)
                {
                    /* An IDR codes every macroblock, so the whole surface has
                       to be current -- anything outside the accumulated box
                       would be coded from whatever was left there. Likewise
                       when nothing has been accumulated, and when the box has
                       grown past half the frame and there is nothing left to
                       save. */
                    aux_rect = full_rect;
                }
                xrdp_accel_assist_x11_run_shader(0, 0, width, height,
                                                 mi, si, mi->enc_texture_aux,
                                                 1, &aux_rect,
                                                 mi->pad_h, mi->enc_w4, 1);
                /* Whatever the branch above settled on, including the
                   full-frame fallbacks, is what xrdp must declare. */
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
            if (frame_no < xrdp_accel_assist_x11_dump_frames())
            {
                xrdp_accel_assist_x11_dump_plane("aux", frame_no,
                                                 mi->enc_texture_aux,
                                                 mi->enc_w4,
                                                 mi->pad_h * 3 / 2, 1);
            }
        }
        XFlush(g_display);

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
            /* Both views submitted before either is waited on, so the GPU
               overlaps them. The wire framing is [len1][s1][len2][s2], so
               the aux's final offset depends on len1 -- which is not known
               until the main has been copied out. Encode it at the halfway
               mark instead and move it down afterwards; the move is a few
               tens of KB against a round-trip to the GPU. */
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
            /* Same encoder, same H.264 sequence -- the aux picture follows
               the main picture. Never carries FORCEIDR: an IDR resets the
               sequence, so only the first view of a frame may be one. */
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
        p[0] = len1 & 0xff;          p[1] = (len1 >> 8) & 0xff;
        p[2] = (len1 >> 16) & 0xff;  p[3] = (len1 >> 24) & 0xff;
        p[4 + len1 + 0] = len2 & 0xff;         p[4 + len1 + 1] = (len2 >> 8) & 0xff;
        p[4 + len1 + 2] = (len2 >> 16) & 0xff; p[4 + len1 + 3] = (len2 >> 24) & 0xff;
        *cdata_bytes = 8 + len1 + len2;
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
    XFlush(g_display);
    /* encode */
    rv = g_enc_funcs[g_enc].encode(mi->ei, mi->enc_texture,
                                   cdata, cdata_bytes, flags);
    return rv;
}
