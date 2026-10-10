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

#ifndef _XRDP_ACCEL_ASSIST_X11_H
#define _XRDP_ACCEL_ASSIST_X11_H

/* generic type that can hold either a GLXPixmap(XID, unsigned int or long)
 * or EGLSurface(void*) */
typedef intptr_t inf_image_t;

/* forward declaration used in xrdp_accel_assist_nvenc and
   xrdp_accel_assist_yami */
struct enc_info;

/* An NV12 encode target as the shaders draw it. Plane 0 is Y, plane 1 the
   interleaved UV. tex[1] == 0: one texture holds both, Y rows then UV rows
   (the GL-allocated target NVENC reads). Otherwise the planes are the
   layers of the encoder's own surface, each in its own texture. bpf: bytes
   each fragment writes, 4 for RGBA8 over a linear plane, 1 for an R8
   plane, 2 for a GR88 one. */
struct xh_enc_target
{
    unsigned int tex[2];
    int bpf[2];
};

int
xrdp_accel_assist_x11_init(void);
/* Headless: no X display; frames come from dma-bufs (Wayland capture). */
int
xrdp_accel_assist_x11_init_headless(void *gbm_device, int nvenc);
int
xrdp_accel_assist_x11_create_surface(int width, int height, int mon_id);
int
xrdp_accel_assist_x11_set_source_image(int mon_id, int buf,
                                       inf_image_t inf_image);
int
xrdp_accel_assist_x11_get_wait_objs(intptr_t *objs, int *obj_count);
int
xrdp_accel_assist_x11_check_wait_objs(void);
int
xrdp_accel_assist_x11_delete_all_pixmaps(void);
void
xrdp_accel_assist_x11_set_caps(int caps);
int
xrdp_accel_assist_x11_avc444_v2(void);
int
xrdp_accel_assist_x11_mon_avc444_v2(int mon_id);
int
xrdp_accel_assist_x11_encoder_avc444_v2(void);
int
xrdp_accel_assist_x11_create_pixmap(int width, int height, int magic,
                                    int con_id, int mon_id);
void
xrdp_accel_assist_x11_note_rtt(int rtt_ms);
enum encoder_result
xrdp_accel_assist_x11_encode_pixmap(int left, int top, int width, int height,
                                    int mon_id, int num_crects,
                                    struct xh_rect *crects,
                                    void *cdata, int *cdata_bytes,
                                    int codec_id, int flags);

#endif
