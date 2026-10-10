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

#ifndef _XRDP_ACCEL_ASSIST_VAAPI_H
#define _XRDP_ACCEL_ASSIST_VAAPI_H

/* The render node to encode on, in xrdp_accel_assist.c. */
const char *
xrdp_accel_assist_render_node(void);
/* Called again once EGL is up, it checks GL and the encoder share a GPU. */
int
xrdp_accel_assist_vaapi_init(void);
/* nviews: 1 for AVC420, 2 for AVC444 (main and aux, pictures of one H.264
   sequence). The encoder allocates each view's input surface and fills
   targets[view] with GL textures over its planes for the shaders to draw
   into; they stay valid until delete_encoder. */
int
xrdp_accel_assist_vaapi_create_encoder(int width, int height, int nviews,
                                       struct enc_info **ei,
                                       struct xh_enc_target *targets);
int
xrdp_accel_assist_vaapi_delete_encoder(struct enc_info *ei);
/* Encode both AVC444 views, submitting each before waiting on either. */
enum encoder_result
xrdp_accel_assist_vaapi_encode_dual(struct enc_info *ei,
                                    void *cdata1, int *cdata1_bytes,
                                    void *cdata2, int *cdata2_bytes,
                                    int flags, int idr_pic_id);
enum encoder_result
xrdp_accel_assist_vaapi_encode(struct enc_info *ei, int tex,
                               void *cdata, int *cdata_bytes,
                               int flags, int idr_pic_id);

#endif
