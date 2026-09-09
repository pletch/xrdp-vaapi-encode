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

#ifndef _XRDP_ACCEL_ASSIST_VAAPI_H
#define _XRDP_ACCEL_ASSIST_VAAPI_H

int
xrdp_accel_assist_vaapi_init(void);
/* tex is the main view's NV12 texture; tex_aux is the AVC444 auxiliary
   view's, or 0 for plain AVC420. When tex_aux is given, ONE encoder --
   one H.264 sequence -- carries both views as alternating pictures. */
int
xrdp_accel_assist_vaapi_create_encoder(int width, int height, int tex,
                                       int tex_aux, int tex_format,
                                       struct enc_info **ei);
int
xrdp_accel_assist_vaapi_delete_encoder(struct enc_info *ei);
/* Encode both AVC444 views for one frame, submitting each before waiting on
   either, so the GPU can overlap them. */
enum encoder_result
xrdp_accel_assist_vaapi_encode_dual(struct enc_info *ei,
                                    void *cdata1, int *cdata1_bytes,
                                    void *cdata2, int *cdata2_bytes,
                                    int flags);
enum encoder_result
xrdp_accel_assist_vaapi_encode(struct enc_info *ei, int tex,
                               void *cdata, int *cdata_bytes,
                               int flags);

#endif
