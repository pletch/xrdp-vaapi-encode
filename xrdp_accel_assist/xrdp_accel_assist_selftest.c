/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) 2026 Klotzi111, all xrdp contributors
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
 * Self-test of the VA-API encoder path.
 *
 * The helper tells xorgxrdp it is ready before it connects to the X server,
 * and from then on a failure cannot fall back to software encoding: the
 * session shows a frozen picture or loses its client. What can fail on a
 * GPU or driver nobody has tried happens after that point: allocating and
 * exporting the NV12 input surface, importing its planes into GL, rendering
 * into them, creating the encode context, and the first pictures (with the
 * long-term references of AVC444). So run all of it once before signalling
 * ready, on a GL context that needs no X server. A helper that fails here
 * exits, and xorgxrdp carries on with software encoding.
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <epoxy/gl.h>
#include <epoxy/egl.h>

#include "arch.h"
#include "os_calls.h"
#include "string_calls.h"
#include "xrdp_accel_assist.h"
#include "xrdp_accel_assist_x11.h"
#include "xrdp_accel_assist_vaapi.h"
#include "xrdp_accel_assist_selftest.h"
#include "log.h"

extern EGLDisplay g_egl_display; /* in xrdp_accel_assist_egl.c */
extern EGLContext g_egl_context; /* in xrdp_accel_assist_egl.c */

#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif
#ifndef EGL_PLATFORM_DEVICE_EXT
#define EGL_PLATFORM_DEVICE_EXT 0x313F
#endif
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

#define ST_MAX_DEVICES 32
/* Decoded or read-back samples may differ from the drawn ones by this. */
#define ST_TOLERANCE 2

/*****************************************************************************/
/* The pattern: four luma quadrants and one chroma pair, different for
   every frame and view, so a stale, swapped or scrambled picture shows. */
static int
st_y(int frame, int view, int quad)
{
    return 32 + ((frame * 29 + view * 61 + quad * 37) % 192);
}

/*****************************************************************************/
static int
st_u(int frame, int view)
{
    return 64 + ((frame * 13 + view * 7) % 128);
}

/*****************************************************************************/
static int
st_v(int frame, int view)
{
    return 64 + ((frame * 17 + view * 11) % 128);
}

/*****************************************************************************/
/* An EGL display on the encoder's GPU that needs no window system: the EGL
   device whose render node is ours, else Mesa's surfaceless platform. */
static EGLDisplay
st_get_display(const char *node)
{
    EGLDeviceEXT devs[ST_MAX_DEVICES];
    EGLint ndevs;
    EGLint index;
    struct stat want;
    struct stat have;
    const char *exts;
    const char *name;

    ndevs = 0;
    if (stat(node, &want) == 0 &&
            epoxy_has_egl_extension(EGL_NO_DISPLAY,
                                    "EGL_EXT_platform_device") &&
            (epoxy_has_egl_extension(EGL_NO_DISPLAY,
                                     "EGL_EXT_device_enumeration") ||
             epoxy_has_egl_extension(EGL_NO_DISPLAY,
                                     "EGL_EXT_device_base")) &&
            eglQueryDevicesEXT(ST_MAX_DEVICES, devs, &ndevs))
    {
        for (index = 0; index < ndevs; index++)
        {
            exts = eglQueryDeviceStringEXT(devs[index], EGL_EXTENSIONS);
            if (exts == NULL ||
                    strstr(exts, "EGL_EXT_device_drm_render_node") == NULL)
            {
                continue;
            }
            name = eglQueryDeviceStringEXT(devs[index],
                                           EGL_DRM_RENDER_NODE_FILE_EXT);
            if (name != NULL && stat(name, &have) == 0 &&
                    have.st_rdev == want.st_rdev)
            {
                return eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT,
                                                devs[index], NULL);
            }
        }
    }
    if (epoxy_has_egl_extension(EGL_NO_DISPLAY,
                                "EGL_MESA_platform_surfaceless"))
    {
        LOG(LOG_LEVEL_INFO, "selftest: no EGL device for %s, using the "
            "surfaceless platform's default GPU", node);
        return eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
                                        EGL_DEFAULT_DISPLAY, NULL);
    }
    return EGL_NO_DISPLAY;
}

/*****************************************************************************/
/* GL as the helper uses it (OpenGL 3.3), current without a surface. */
static int
st_egl_up(const char *node, EGLDisplay *dpy_out, EGLContext *ctx_out)
{
    static const EGLint ctx_attr[] =
    {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_NONE
    };
    static const EGLint cfg_attr[] =
    {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLDisplay dpy;
    EGLContext ctx;
    EGLConfig cfg;
    EGLint num;
    EGLint major;
    EGLint minor;

    *dpy_out = EGL_NO_DISPLAY;
    *ctx_out = EGL_NO_CONTEXT;
    dpy = st_get_display(node);
    if (dpy == EGL_NO_DISPLAY)
    {
        LOG(LOG_LEVEL_ERROR, "selftest: no EGL display without a window "
            "system (needs EGL_EXT_platform_device or "
            "EGL_MESA_platform_surfaceless)");
        return 1;
    }
    if (!eglInitialize(dpy, &major, &minor))
    {
        LOG(LOG_LEVEL_ERROR, "selftest: eglInitialize failed 0x%x",
            eglGetError());
        return 1;
    }
    if (!eglBindAPI(EGL_OPENGL_API) ||
            !epoxy_has_egl_extension(dpy, "EGL_KHR_surfaceless_context"))
    {
        LOG(LOG_LEVEL_ERROR, "selftest: EGL %d.%d here has no desktop GL or "
            "no EGL_KHR_surfaceless_context", major, minor);
        eglTerminate(dpy);
        return 1;
    }
    cfg = (EGLConfig) 0; /* EGL_NO_CONFIG_KHR */
    if (!epoxy_has_egl_extension(dpy, "EGL_KHR_no_config_context"))
    {
        num = 0;
        if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &num) || num < 1)
        {
            LOG(LOG_LEVEL_ERROR, "selftest: no EGL config");
            eglTerminate(dpy);
            return 1;
        }
    }
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT)
    {
        LOG(LOG_LEVEL_ERROR, "selftest: eglCreateContext (OpenGL 3.3) "
            "failed 0x%x", eglGetError());
        eglTerminate(dpy);
        return 1;
    }
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
    {
        LOG(LOG_LEVEL_ERROR, "selftest: eglMakeCurrent failed 0x%x",
            eglGetError());
        eglDestroyContext(dpy, ctx);
        eglTerminate(dpy);
        return 1;
    }
    LOG(LOG_LEVEL_INFO, "selftest: EGL %d.%d, GL renderer '%s'", major, minor,
        (const char *) glGetString(GL_RENDERER));
    *dpy_out = dpy;
    *ctx_out = ctx;
    return 0;
}

/*****************************************************************************/
static void
st_egl_down(EGLDisplay dpy, EGLContext ctx)
{
    if (dpy == EGL_NO_DISPLAY)
    {
        return;
    }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (ctx != EGL_NO_CONTEXT)
    {
        eglDestroyContext(dpy, ctx);
    }
    eglTerminate(dpy);
}

/*****************************************************************************/
/* Fill a rectangle of one plane of a view. x, y, w, h are in samples of
   that plane (a chroma sample is a U,V pair), all multiples of 16. a is the
   luma value, or U with b as V. A fragment covers bpf bytes of the plane,
   see struct xh_enc_target. */
static void
st_fill(GLuint fb, const struct xh_enc_target *tgt, int plane,
        int x, int y, int w, int h, int a, int b)
{
    int bpf = tgt->bpf[plane];
    int sample_bytes = (plane == 0) ? 1 : 2;

    if (bpf < 1)
    {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tgt->tex[plane], 0);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x * sample_bytes / bpf, y, w * sample_bytes / bpf, h);
    if (plane == 0)
    {
        glClearColor(a / 255.0f, a / 255.0f, a / 255.0f, a / 255.0f);
    }
    else
    {
        glClearColor(a / 255.0f, b / 255.0f, a / 255.0f, b / 255.0f);
    }
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

/*****************************************************************************/
static void
st_draw(GLuint fb, const struct xh_enc_target *tgt, int buf_w, int buf_h,
        int frame, int view, int stress)
{
    int mx = (buf_w / 2) & ~15;
    int my = (buf_h / 2) & ~15;
    int index;

    /* Work for the GPU that the encoder must wait behind: if it does not,
       it codes one of these instead of the pattern. */
    for (index = 0; index < stress; index++)
    {
        st_fill(fb, tgt, 0, 0, 0, buf_w, buf_h, (index * 53 + 7) & 255, 0);
        st_fill(fb, tgt, 1, 0, 0, buf_w / 2, buf_h / 2,
                (index * 31 + 3) & 255, (index * 71 + 5) & 255);
    }
    st_fill(fb, tgt, 0, 0, 0, mx, my, st_y(frame, view, 0), 0);
    st_fill(fb, tgt, 0, mx, 0, buf_w - mx, my, st_y(frame, view, 1), 0);
    st_fill(fb, tgt, 0, 0, my, mx, buf_h - my, st_y(frame, view, 2), 0);
    st_fill(fb, tgt, 0, mx, my, buf_w - mx, buf_h - my,
            st_y(frame, view, 3), 0);
    st_fill(fb, tgt, 1, 0, 0, buf_w / 2, buf_h / 2,
            st_u(frame, view), st_v(frame, view));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/*****************************************************************************/
/* Does libva see in a view's input surface what GL drew there? Catches a
   plane layout (pitch, offset, tiling) the two sides disagree on. Returns 0
   if it matches, 1 if not, -1 if the surface could not be read back (not a
   failure of the path under test). */
static int
st_check_input(struct enc_info *ei, int view, int buf_w, int buf_h, int frame)
{
    int mx = (buf_w / 2) & ~15;
    int my = (buf_h / 2) & ~15;
    int xs[4];
    int ys[4];
    int yuv[12];
    int quad;
    int bad;

    xs[0] = mx / 2;
    ys[0] = my / 2;
    xs[1] = mx + (buf_w - mx) / 2;
    ys[1] = my / 2;
    xs[2] = mx / 2;
    ys[2] = my + (buf_h - my) / 2;
    xs[3] = mx + (buf_w - mx) / 2;
    ys[3] = my + (buf_h - my) / 2;
    glFinish();
    if (xrdp_accel_assist_vaapi_read_input(ei, view, 4, xs, ys, yuv) != 0)
    {
        LOG(LOG_LEVEL_WARNING, "selftest: view %d: the input surface cannot "
            "be read back, its contents are not checked", view);
        return -1;
    }
    bad = 0;
    for (quad = 0; quad < 4; quad++)
    {
        if (abs(yuv[quad * 3] - st_y(frame, view, quad)) > ST_TOLERANCE ||
                abs(yuv[quad * 3 + 1] - st_u(frame, view)) > ST_TOLERANCE ||
                abs(yuv[quad * 3 + 2] - st_v(frame, view)) > ST_TOLERANCE)
        {
            LOG(LOG_LEVEL_ERROR, "selftest: view %d at %d,%d: libva reads "
                "Y %d U %d V %d where GL drew Y %d U %d V %d", view,
                xs[quad], ys[quad], yuv[quad * 3], yuv[quad * 3 + 1],
                yuv[quad * 3 + 2], st_y(frame, view, quad),
                st_u(frame, view), st_v(frame, view));
            bad = 1;
        }
    }
    return bad;
}

/*****************************************************************************/
static void
st_dump(FILE *fs, FILE *fe, const void *data, int bytes, int view, int frame)
{
    if (fs == NULL || fe == NULL || bytes < 1)
    {
        return;
    }
    fwrite(data, 1, bytes, fs);
    fprintf(fe, "%d %d %d %d %d %d %d %d\n", view, frame,
            st_y(frame, view, 0), st_y(frame, view, 1),
            st_y(frame, view, 2), st_y(frame, view, 3),
            st_u(frame, view), st_v(frame, view));
}

/*****************************************************************************/
/* Which views a frame codes. AVC444 in use skips the aux view of moving
   areas and sends it alone once they settle, and each view predicts from
   its own previous picture across those gaps: do the same.
   1 = main, 2 = aux, 3 = both. */
static int
st_views_of(int nviews, int frame)
{
    static const int avc444[6] = { 3, 3, 1, 1, 2, 3 };

    return (nviews > 1) ? avc444[frame % 6] : 1;
}

/*****************************************************************************/
int
xrdp_accel_assist_selftest(int width, int height, int nviews, int frames,
                           int stress, const char *dump_path)
{
    EGLDisplay dpy;
    EGLContext ctx;
    EGLDisplay old_dpy = g_egl_display;
    EGLContext old_ctx = g_egl_context;
    struct enc_info *ei = NULL;
    struct xh_enc_target tgt[2];
    GLuint fb = 0;
    FILE *fs = NULL;
    FILE *fe = NULL;
    char expect_path[512];
    char *cdata[2] = { NULL, NULL };
    int cbytes[2];
    int cmax;
    int buf_w = (width + 15) & ~15;
    int buf_h = (height + 15) & ~15;
    int frame;
    int view;
    int which;
    int flags;
    int rv = 1;
    int checked = 0;
    int pictures = 0;
    long total_bytes = 0;
    unsigned int t_start;
    unsigned int t_enc;
    unsigned int t_enc_total = 0;
    enum encoder_result er;

    nviews = (nviews > 1) ? 2 : 1;
    t_start = g_get_elapsed_ms();
    if (st_egl_up(xrdp_accel_assist_render_node(), &dpy, &ctx) != 0)
    {
        return 1;
    }
    /* The encoder imports and draws through the helper's EGL globals. */
    g_egl_display = dpy;
    g_egl_context = ctx;

    cmax = buf_w * buf_h * 3 / 2 + 4096;
    cdata[0] = g_new(char, cmax);
    cdata[1] = g_new(char, cmax);
    if (cdata[0] == NULL || cdata[1] == NULL)
    {
        goto done;
    }
    if (dump_path != NULL)
    {
        g_snprintf(expect_path, sizeof(expect_path), "%s.expect", dump_path);
        fs = fopen(dump_path, "wb");
        fe = fopen(expect_path, "w");
        if (fs == NULL || fe == NULL)
        {
            LOG(LOG_LEVEL_ERROR, "selftest: cannot write %s", dump_path);
            goto done;
        }
    }
    g_memset(tgt, 0, sizeof(tgt));
    if (xrdp_accel_assist_vaapi_create_encoder(width, height, nviews, &ei,
            tgt) != 0)
    {
        ei = NULL;
        LOG(LOG_LEVEL_ERROR, "selftest: no %dx%d encoder with %d view(s)",
            width, height, nviews);
        goto done;
    }
    glGenFramebuffers(1, &fb);
    for (frame = 0; frame < frames; frame++)
    {
        which = st_views_of(nviews, frame);
        for (view = 0; view < nviews; view++)
        {
            if (which & (1 << view))
            {
                st_draw(fb, &tgt[view], buf_w, buf_h, frame, view, stress);
            }
        }
        if (frame == 0)
        {
            for (view = 0; view < nviews; view++)
            {
                int ok = st_check_input(ei, view, buf_w, buf_h, frame);

                if (ok > 0)
                {
                    goto done;
                }
                checked += (ok == 0);
            }
        }
        flags = (frame == 0) ? XH_ENC_FLAGS_FORCEIDR : 0;
        cbytes[0] = cmax;
        cbytes[1] = cmax;
        t_enc = g_get_elapsed_ms();
        if (which == 3)
        {
            er = xrdp_accel_assist_vaapi_encode_dual(ei, cdata[0], &cbytes[0],
                    cdata[1], &cbytes[1], flags, (frame == 0) ? 0 : -1);
        }
        else if (which == 2)
        {
            cbytes[0] = 0;
            er = xrdp_accel_assist_vaapi_encode(ei, tgt[1].tex[0], cdata[1],
                                                &cbytes[1],
                                                flags | XH_ENC_FLAGS_AUXVIEW,
                                                -1);
        }
        else
        {
            cbytes[1] = 0;
            er = xrdp_accel_assist_vaapi_encode(ei, tgt[0].tex[0], cdata[0],
                                                &cbytes[0], flags,
                                                (frame == 0) ? 0 : -1);
        }
        t_enc_total += g_get_elapsed_ms() - t_enc;
        if (er == ENCODER_ERROR)
        {
            LOG(LOG_LEVEL_ERROR, "selftest: frame %d (%s) did not encode",
                frame, which == 3 ? "both views" :
                which == 2 ? "aux view" : "main view");
            goto done;
        }
        if (frame == 0 && er != KEY_FRAME_ENCODED)
        {
            LOG(LOG_LEVEL_ERROR, "selftest: the first frame is not an IDR");
            goto done;
        }
        for (view = 0; view < 2; view++)
        {
            if ((which & (1 << view)) == 0)
            {
                continue;
            }
            if (cbytes[view] < 1)
            {
                LOG(LOG_LEVEL_ERROR, "selftest: frame %d view %d coded to "
                    "nothing", frame, view);
                goto done;
            }
            st_dump(fs, fe, cdata[view], cbytes[view], view, frame);
            total_bytes += cbytes[view];
            pictures++;
        }
    }
    rv = 0;
    LOG(LOG_LEVEL_INFO, "selftest: passed: %dx%d, %d view(s), %d frame(s), "
        "%d picture(s), %ld bytes, encode %u ms in all (%.1f ms a frame), "
        "input surface %s, %u ms with setup", width, height, nviews, frames,
        pictures, total_bytes, t_enc_total,
        frames > 0 ? (double) t_enc_total / frames : 0.0,
        checked == nviews ? "read back as drawn" : "not read back",
        g_get_elapsed_ms() - t_start);

done:
    if (fs != NULL)
    {
        fclose(fs);
    }
    if (fe != NULL)
    {
        fclose(fe);
    }
    if (fb != 0)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fb);
    }
    if (ei != NULL)
    {
        xrdp_accel_assist_vaapi_delete_encoder(ei);
    }
    g_free(cdata[0]);
    g_free(cdata[1]);
    g_egl_display = old_dpy;
    g_egl_context = old_ctx;
    st_egl_down(dpy, ctx);
    if (rv != 0)
    {
        LOG(LOG_LEVEL_ERROR, "selftest: FAILED: %dx%d, %d view(s)", width,
            height, nviews);
    }
    return rv;
}
