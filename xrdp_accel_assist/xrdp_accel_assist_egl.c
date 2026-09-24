/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Jay Sorg 2022-2024
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

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <X11/Xlib.h>

#include <epoxy/gl.h>
#include <epoxy/egl.h>

#include "arch.h"
#include "os_calls.h"
#include "string_calls.h"
#include "xrdp_accel_assist.h"
#include "xrdp_accel_assist_x11.h"
#include "xrdp_accel_assist_egl.h"
#include "log.h"

EGLDisplay g_egl_display;
EGLContext g_egl_context;
static EGLSurface g_egl_surface;
static EGLConfig g_ecfg;
static EGLint g_num_config;

/* X11 */
extern Display *g_display; /* in xrdp_accel_assist_x11.c */
extern Window g_root_window; /* in xrdp_accel_assist_x11.c */

static EGLint g_choose_config_attr[] =
{
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_NONE
};

static EGLint g_create_context_attr[] =
{
    EGL_CONTEXT_MAJOR_VERSION, 3,
    EGL_CONTEXT_MINOR_VERSION, 3,
    EGL_NONE
};

static const EGLint g_create_surface_attr[] =
{
    EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
    EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
    EGL_NONE
};

/*****************************************************************************/
static EGLBoolean
xrdp_accel_assist_check_ext(const char *ext_name)
{
    if (!epoxy_has_egl_extension(g_egl_display, ext_name))
    {
        LOG(LOG_LEVEL_INFO, "%s not present", ext_name);
        return EGL_FALSE;
    }
    LOG(LOG_LEVEL_INFO, "%s present", ext_name);
    return EGL_TRUE;
}

/*****************************************************************************/
int
xrdp_accel_assist_inf_egl_init(void)
{
    int egl_ver;
    int ok;

    ok = eglBindAPI(EGL_OPENGL_API);
    LOG(LOG_LEVEL_INFO, "eglBindAPI ok %d", ok);
    g_egl_display = eglGetDisplay((EGLNativeDisplayType) g_display);
    LOG(LOG_LEVEL_INFO, "g_egl_display %p", g_egl_display);
    eglInitialize(g_egl_display, NULL, NULL);
    egl_ver = epoxy_egl_version(g_egl_display);
    LOG(LOG_LEVEL_INFO, "egl_ver %d", egl_ver);
    if (egl_ver < 11) /* EGL version 1.1 */
    {
        LOG(LOG_LEVEL_INFO, "egl_ver too old %d", egl_ver);
        eglTerminate(g_egl_display);
        return 1;
    }
    if ((!xrdp_accel_assist_check_ext("EGL_NOK_texture_from_pixmap")) ||
            (!xrdp_accel_assist_check_ext("EGL_MESA_image_dma_buf_export")) ||
            (!xrdp_accel_assist_check_ext("EGL_KHR_image_base")))
    {
        LOG(LOG_LEVEL_INFO, "missing ext");
        eglTerminate(g_egl_display);
        return 1;
    }
    eglChooseConfig(g_egl_display, g_choose_config_attr, &g_ecfg,
                    1, &g_num_config);
    LOG(LOG_LEVEL_INFO, "g_ecfg %p g_num_config %d", g_ecfg, g_num_config);
    g_egl_surface = eglCreateWindowSurface(g_egl_display, g_ecfg,
                                           g_root_window, NULL);
    LOG(LOG_LEVEL_INFO, "g_egl_surface %p", g_egl_surface);
    g_egl_context = eglCreateContext(g_egl_display, g_ecfg,
                                     EGL_NO_CONTEXT, g_create_context_attr);
    LOG(LOG_LEVEL_INFO, "g_egl_context %p", g_egl_context);
    ok = eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface,
                        g_egl_context);
    LOG(LOG_LEVEL_INFO, "eglMakeCurrent ok %d", ok);
    return 0;
}

/*****************************************************************************/
/* Headless init for a non-X source (Wayland capture): EGL on the GBM
   platform with a surfaceless context, no X display. */
int
xrdp_accel_assist_inf_egl_init_gbm(void *gbm_device)
{
    int egl_ver;
    int ok;

    ok = eglBindAPI(EGL_OPENGL_API);
    LOG(LOG_LEVEL_INFO, "eglBindAPI ok %d", ok);
    /* the EXT entry point: epoxy cannot resolve the EGL 1.5 one before a
       display exists */
    g_egl_display = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm_device,
                    NULL);
    LOG(LOG_LEVEL_INFO, "g_egl_display %p (GBM)", g_egl_display);
    if (g_egl_display == EGL_NO_DISPLAY ||
            !eglInitialize(g_egl_display, NULL, NULL))
    {
        LOG(LOG_LEVEL_ERROR, "eglInitialize failed on the GBM platform");
        return 1;
    }
    egl_ver = epoxy_egl_version(g_egl_display);
    LOG(LOG_LEVEL_INFO, "egl_ver %d", egl_ver);
    if ((!xrdp_accel_assist_check_ext("EGL_EXT_image_dma_buf_import")) ||
            (!xrdp_accel_assist_check_ext("EGL_EXT_image_dma_buf_import_modifiers")) ||
            (!xrdp_accel_assist_check_ext("EGL_MESA_image_dma_buf_export")) ||
            (!xrdp_accel_assist_check_ext("EGL_KHR_surfaceless_context")) ||
            (!xrdp_accel_assist_check_ext("EGL_KHR_no_config_context")))
    {
        LOG(LOG_LEVEL_ERROR, "missing ext");
        eglTerminate(g_egl_display);
        return 1;
    }
    g_egl_context = eglCreateContext(g_egl_display, EGL_NO_CONFIG_KHR,
                                     EGL_NO_CONTEXT, g_create_context_attr);
    LOG(LOG_LEVEL_INFO, "g_egl_context %p", g_egl_context);
    if (g_egl_context == EGL_NO_CONTEXT)
    {
        eglTerminate(g_egl_display);
        return 1;
    }
    ok = eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        g_egl_context);
    LOG(LOG_LEVEL_INFO, "eglMakeCurrent (surfaceless) ok %d", ok);
    return ok ? 0 : 1;
}

/*****************************************************************************/
/* Wrap a single-plane dma-buf (e.g. a captured XRGB8888 frame) in an
   EGLImage. The fd stays owned by the caller. */
int
xrdp_accel_assist_inf_egl_import_dmabuf(int width, int height,
                                        unsigned int fourcc, int fd,
                                        unsigned int offset,
                                        unsigned int stride,
                                        unsigned long long modifier,
                                        inf_image_t *inf_image)
{
    EGLAttrib attrs[] =
    {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLAttrib)(modifier & 0xffffffff),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLAttrib)(modifier >> 32),
        EGL_NONE
    };
    EGLImage image;

    image = eglCreateImage(g_egl_display, EGL_NO_CONTEXT,
                           EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (image == EGL_NO_IMAGE)
    {
        LOG(LOG_LEVEL_ERROR, "eglCreateImage(dma-buf) failed 0x%x",
            eglGetError());
        return 1;
    }
    *inf_image = (inf_image_t) image;
    return 0;
}

/*****************************************************************************/
int
xrdp_accel_assist_inf_egl_destroy_dmabuf(inf_image_t inf_image)
{
    eglDestroyImage(g_egl_display, (EGLImage) inf_image);
    return 0;
}

/*****************************************************************************/
int
xrdp_accel_assist_inf_egl_create_image(Pixmap pixmap, inf_image_t *inf_image)
{
    *inf_image = (inf_image_t)eglCreatePixmapSurface(g_egl_display,
                 g_ecfg, pixmap, g_create_surface_attr);
    return 0;
}

/*****************************************************************************/
int
xrdp_accel_assist_inf_egl_destroy_image(inf_image_t inf_image)
{
    eglDestroySurface(g_egl_display, (EGLSurface)inf_image);
    return 0;
}

/*****************************************************************************/
int
xrdp_accel_assist_inf_egl_bind_tex_image(inf_image_t inf_image)
{
    eglBindTexImage(g_egl_display, (EGLSurface)inf_image, EGL_BACK_BUFFER);
    return 0;
}

/*****************************************************************************/
int
xrdp_accel_assist_inf_egl_release_tex_image(inf_image_t inf_image)
{
    eglReleaseTexImage(g_egl_display, (EGLSurface)inf_image, EGL_BACK_BUFFER);
    return 0;
}
