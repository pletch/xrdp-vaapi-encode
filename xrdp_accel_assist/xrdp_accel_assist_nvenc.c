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
#include "string_calls.h"

#include <epoxy/gl.h>

#include "encoder_headers/nvEncodeAPI_11_1.h"

#include "arch.h"
#include "os_calls.h"
#include "xrdp_accel_assist.h"
#include "xrdp_accel_assist_x11.h"
#include "xrdp_accel_assist_nvenc.h"
#include "log.h"

#define XH_NVENV_DEFAULT_QP 28
/* The aux view carries the chroma 4:2:0 drops; as in the VA-API encoder,
   it gets a finer quantiser than the main view. */
#define XH_NVENC_DEFAULT_AUX_QP 18

typedef NVENCSTATUS
(NVENCAPI *NvEncodeAPICreateInstanceProc)
(NV_ENCODE_API_FUNCTION_LIST *functionList);

static char g_lib_name[] = "libnvidia-encode.so";
static char g_lib_name1[] = "libnvidia-encode.so.1";
static char g_func_name[] = "NvEncodeAPICreateInstance";

static NvEncodeAPICreateInstanceProc g_NvEncodeAPICreateInstance = NULL;

static NV_ENCODE_API_FUNCTION_LIST g_enc_funcs;

static long g_lib = 0;

struct enc_info
{
    int width;
    int height;
    int frameCount;
    int avc444;                 /* both views, one H.264 sequence */
    int ltr_valid[2];           /* the view's long-term reference exists */
    int idr_count;              /* IDRs this encoder has produced */
    int qp_map_bytes;
    void *enc;
    NV_ENC_OUTPUT_PTR bitstreamBuffer;
    NV_ENC_INPUT_PTR mappedResource;
    NV_ENC_BUFFER_FORMAT mappedBufferFmt;
    NV_ENC_REGISTERED_PTR registeredResource;
    /* AVC444 auxiliary view */
    NV_ENC_INPUT_PTR mappedResourceAux;
    NV_ENC_BUFFER_FORMAT mappedBufferFmtAux;
    NV_ENC_REGISTERED_PTR registeredResourceAux;
    int8_t *qp_map_aux;         /* per-MB QP delta for the aux view */
    int8_t *qp_map_main;        /* all zero: NVENC keeps the last map
                                   applied to a picture that sets none */
};

/*****************************************************************************/
int
xrdp_accel_assist_nvenc_init(void)
{
    NVENCSTATUS nv_error;

    g_lib = g_load_library(g_lib_name);
    if (g_lib == 0)
    {
        g_lib = g_load_library(g_lib_name1);
        if (g_lib == 0)
        {
            LOG(LOG_LEVEL_ERROR, "load library for %s/%s failed", g_lib_name, g_lib_name1);
            return 1;
        }
    }
    g_NvEncodeAPICreateInstance = g_get_proc_address(g_lib, g_func_name);
    if (g_NvEncodeAPICreateInstance == NULL)
    {
        LOG(LOG_LEVEL_ERROR, "get proc address for %s failed", g_func_name);
        return 1;
    }
    g_memset(&g_enc_funcs, 0, sizeof(g_enc_funcs));
    g_enc_funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    nv_error = g_NvEncodeAPICreateInstance(&g_enc_funcs);
    LOG(LOG_LEVEL_INFO, "NvEncodeAPICreateInstance rv %d", nv_error);
    if (nv_error != NV_ENC_SUCCESS)
    {
        return 1;
    }
    return 0;
}

/*****************************************************************************/
/* Register a GL texture with the encoder and map it as an input. */
static int
nvenc_register_tex(void *enc, int tex, int width, int height,
                   int tex_format, NV_ENC_REGISTERED_PTR *registered,
                   NV_ENC_INPUT_PTR *mapped, NV_ENC_BUFFER_FORMAT *fmt)
{
    NV_ENC_MAP_INPUT_RESOURCE mapInputResource;
    NV_ENC_INPUT_RESOURCE_OPENGL_TEX res;
    NV_ENC_REGISTER_RESOURCE reg_res;
    NVENCSTATUS nv_error;

    g_memset(&res, 0, sizeof(res));
    res.texture = tex;
    res.target = GL_TEXTURE_2D;

    g_memset(&reg_res, 0, sizeof(reg_res));
    reg_res.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg_res.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_OPENGL_TEX;
    reg_res.width = width;
    reg_res.height = height;
    if (tex_format == XH_YUV420)
    {
        reg_res.pitch = width;
        reg_res.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    }
    else
    {
        reg_res.pitch = width * 4;
        reg_res.bufferFormat = NV_ENC_BUFFER_FORMAT_AYUV;
    }
    reg_res.resourceToRegister = &res;
    reg_res.bufferUsage = NV_ENC_INPUT_IMAGE;
    nv_error = g_enc_funcs.nvEncRegisterResource(enc, &reg_res);
    LOG(LOG_LEVEL_INFO, "nvEncRegisterResource tex %d rv %d", tex, nv_error);
    if (nv_error != NV_ENC_SUCCESS)
    {
        return 1;
    }

    g_memset(&mapInputResource, 0, sizeof(mapInputResource));
    mapInputResource.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
    mapInputResource.registeredResource = reg_res.registeredResource;
    nv_error = g_enc_funcs.nvEncMapInputResource(enc, &mapInputResource);
    LOG(LOG_LEVEL_INFO, "nvEncMapInputResource tex %d rv %d", tex, nv_error);
    if (nv_error != NV_ENC_SUCCESS)
    {
        g_enc_funcs.nvEncUnregisterResource(enc, reg_res.registeredResource);
        return 1;
    }
    *registered = reg_res.registeredResource;
    *mapped = mapInputResource.mappedResource;
    *fmt = mapInputResource.mappedBufferFmt;
    return 0;
}

/*****************************************************************************/
/* XRDP_NVENC_AUX_QP, 1-51, else the default. */
static int
nvenc_aux_qp(void)
{
    const char *env = g_getenv("XRDP_NVENC_AUX_QP");
    int qp;

    if (env != NULL)
    {
        qp = g_atoi(env);
        if ((qp >= 1) && (qp <= 51))
        {
            return qp;
        }
    }
    return XH_NVENC_DEFAULT_AUX_QP;
}

/*****************************************************************************/
int
xrdp_accel_assist_nvenc_create_encoder(int width, int height, int tex,
                                       int tex_aux, int tex_format,
                                       struct enc_info **ei)
{
    NV_ENC_CREATE_BITSTREAM_BUFFER bitstreamParams;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params;
    NV_ENC_INITIALIZE_PARAMS createEncodeParams;
    NV_ENC_CAPS_PARAM capsParam;
    NV_ENC_CONFIG encCfg;
    NV_ENC_CONFIG_H264_VUI_PARAMETERS *vui;
    NVENCSTATUS nv_error;
    struct enc_info *lei;
    char *rateControlMode_str;
    char *averageBitRate_str;
    char *qp_str;
    int qp_int;
    int averageBitRate_int;
    int rc_set;
    int max_ltr;
    int aux_delta;
    int index;

    lei = g_new0(struct enc_info, 1);
    if (lei == NULL)
    {
        return 1;
    }

    g_memset(&params, 0, sizeof(params));
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_OPENGL;
    params.apiVersion = NVENCAPI_VERSION;
    nv_error = g_enc_funcs.nvEncOpenEncodeSessionEx(&params, &(lei->enc));
    LOG(LOG_LEVEL_INFO, "nvEncOpenEncodeSessionEx rv %d enc %p", nv_error, lei->enc);
    if (nv_error != NV_ENC_SUCCESS)
    {
        g_free(lei);
        return 1;
    }
    lei->avc444 = (tex_aux != 0);
    if (lei->avc444)
    {
        /* Each view predicts only from its own long-term reference, so
           two LTR slots are needed. */
        g_memset(&capsParam, 0, sizeof(capsParam));
        capsParam.version = NV_ENC_CAPS_PARAM_VER;
        capsParam.capsToQuery = NV_ENC_CAPS_NUM_MAX_LTR_FRAMES;
        max_ltr = 0;
        nv_error = g_enc_funcs.nvEncGetEncodeCaps(lei->enc,
                   NV_ENC_CODEC_H264_GUID,
                   &capsParam, &max_ltr);
        LOG(LOG_LEVEL_INFO, "nvenc: AVC444, max LTR frames %d rv %d",
            max_ltr, nv_error);
        if ((nv_error != NV_ENC_SUCCESS) || (max_ltr < 2))
        {
            LOG(LOG_LEVEL_ERROR, "nvenc: AVC444 needs two long-term "
                "references, the encoder offers %d", max_ltr);
            g_enc_funcs.nvEncDestroyEncoder(lei->enc);
            g_free(lei);
            return 1;
        }
    }

    g_memset(&encCfg, 0, sizeof(encCfg));
    encCfg.version = NV_ENC_CONFIG_VER;
    encCfg.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
    encCfg.gopLength = NVENC_INFINITE_GOPLENGTH;
    encCfg.frameIntervalP = 1;  /* 1 + B_Frame_Count */
    encCfg.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    encCfg.mvPrecision = NV_ENC_MV_PRECISION_QUARTER_PEL;

    /* these env vars can be added / changed in sesman.ini SessionVariables
       example
       XRDP_NVENC_RATE_CONTROL_MODE=NV_ENC_PARAMS_RC_CONSTQP
       XRDP_NVENC_QP=30
       or
       XRDP_NVENC_RATE_CONTROL_MODE=NV_ENC_PARAMS_RC_VBR
       XRDP_NVENC_AVERAGE_BITRATE=2000000 */
    rateControlMode_str = g_getenv("XRDP_NVENC_RATE_CONTROL_MODE");
    averageBitRate_str = g_getenv("XRDP_NVENC_AVERAGE_BITRATE");
    qp_str = g_getenv("XRDP_NVENC_QP");
    rc_set = 0;
    if (rateControlMode_str != NULL)
    {
        if (g_strcmp(rateControlMode_str, "NV_ENC_PARAMS_RC_CONSTQP") == 0)
        {
            if (qp_str != NULL)
            {
                qp_int = g_atoi(qp_str);
                if ((qp_int >= 0) && (qp_int <= 51))
                {
                    LOG(LOG_LEVEL_INFO,
                        "using NV_ENC_PARAMS_RC_CONSTQP qp %d",
                        qp_int);
                    encCfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
                    encCfg.rcParams.constQP.qpInterP = qp_int;
                    encCfg.rcParams.constQP.qpInterB = qp_int;
                    encCfg.rcParams.constQP.qpIntra = qp_int;
                    rc_set = 1;
                }
            }
        }
        else if (g_strcmp(rateControlMode_str, "NV_ENC_PARAMS_RC_VBR") == 0)
        {
            if (averageBitRate_str != NULL)
            {
                averageBitRate_int = g_atoi(averageBitRate_str);
                if ((averageBitRate_int >= 5000) &&
                        (averageBitRate_int <= 1000000000))
                {
                    LOG(LOG_LEVEL_INFO,
                        "using NV_ENC_PARAMS_RC_VBR averageBitRate %d",
                        averageBitRate_int);
                    encCfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
                    encCfg.rcParams.averageBitRate = averageBitRate_int;
                    rc_set = 1;
                }
            }
        }
    }
    if (!rc_set)
    {
        LOG(LOG_LEVEL_INFO,
            "using default NV_ENC_PARAMS_RC_CONSTQP qp %d",
            XH_NVENV_DEFAULT_QP);
        encCfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        encCfg.rcParams.constQP.qpInterP = XH_NVENV_DEFAULT_QP;
        encCfg.rcParams.constQP.qpInterB = XH_NVENV_DEFAULT_QP;
        encCfg.rcParams.constQP.qpIntra = XH_NVENV_DEFAULT_QP;
        rc_set = 1;
    }

    encCfg.encodeCodecConfig.h264Config.chromaFormatIDC = 1;
    encCfg.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    encCfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    encCfg.encodeCodecConfig.h264Config.disableSPSPPS = 0;
    /* The shaders convert to full-range BT.709. Say so in the VUI, as the
       VA-API encoder does, or decoders assume limited range and stretch
       the contrast. */
    vui = &(encCfg.encodeCodecConfig.h264Config.h264VUIParameters);
    vui->videoSignalTypePresentFlag = 1;
    vui->videoFormat = 5;               /* unspecified */
    vui->videoFullRangeFlag = 1;
    vui->colourDescriptionPresentFlag = 1;
    vui->colourPrimaries = 1;           /* BT.709 */
    vui->transferCharacteristics = 1;   /* BT.709 */
    vui->colourMatrix = 1;              /* BT.709 */
    /* No B-frames, so no reordering; say so. Without it a decoder may
       assume the worst and hold pictures back (Chromium's holds as many
       as the level's DPB). */
    vui->bitstreamRestrictionFlag = 1;
    aux_delta = 0;
    if (lei->avc444)
    {
        /* Both views in one sequence. Every picture is marked long-term
           in its view's slot (main 0, aux 1) and predicts only from that
           slot, so neither view references the other. The third frame of
           DPB lets a client drop the aux view and still decode. */
        encCfg.encodeCodecConfig.h264Config.enableLTR = 1;
        encCfg.encodeCodecConfig.h264Config.ltrTrustMode = 0;
        encCfg.encodeCodecConfig.h264Config.ltrNumFrames = 2;
        encCfg.encodeCodecConfig.h264Config.maxNumRefFrames = 3;
        /* A per-picture QP for the aux view, as a delta on the constant
           QP. Rate control would pick its own under VBR. */
        if (encCfg.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CONSTQP)
        {
            aux_delta = nvenc_aux_qp() - (int) encCfg.rcParams.constQP.qpInterP;
        }
        if (aux_delta != 0)
        {
            encCfg.rcParams.qpMapMode = NV_ENC_QP_MAP_DELTA;
        }
        LOG(LOG_LEVEL_INFO, "nvenc: AVC444, aux QP delta %d", aux_delta);
    }

    g_memset(&createEncodeParams, 0, sizeof(createEncodeParams));
    createEncodeParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    createEncodeParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
    createEncodeParams.encodeWidth = width;
    createEncodeParams.encodeHeight = height;
    createEncodeParams.darWidth = width;
    createEncodeParams.darHeight = height;
    createEncodeParams.frameRateNum = 30;
    createEncodeParams.frameRateDen = 1;
    createEncodeParams.enablePTD = 1;
    createEncodeParams.encodeConfig = &encCfg;
    nv_error = g_enc_funcs.nvEncInitializeEncoder(lei->enc,
               &createEncodeParams);
    LOG(LOG_LEVEL_INFO, "nvEncInitializeEncoder rv %d", nv_error);
    if (nv_error != NV_ENC_SUCCESS)
    {
        g_free(lei);
        return 1;
    }

    if (nvenc_register_tex(lei->enc, tex, width, height, tex_format,
                           &(lei->registeredResource),
                           &(lei->mappedResource),
                           &(lei->mappedBufferFmt)) != 0)
    {
        g_enc_funcs.nvEncDestroyEncoder(lei->enc);
        g_free(lei);
        return 1;
    }
    if (lei->avc444)
    {
        if (nvenc_register_tex(lei->enc, tex_aux, width, height, tex_format,
                               &(lei->registeredResourceAux),
                               &(lei->mappedResourceAux),
                               &(lei->mappedBufferFmtAux)) != 0)
        {
            g_enc_funcs.nvEncUnmapInputResource(lei->enc,
                                                lei->mappedResource);
            g_enc_funcs.nvEncUnregisterResource(lei->enc,
                                                lei->registeredResource);
            g_enc_funcs.nvEncDestroyEncoder(lei->enc);
            g_free(lei);
            return 1;
        }
        if (aux_delta != 0)
        {
            lei->qp_map_bytes = ((width + 15) / 16) * ((height + 15) / 16);
            lei->qp_map_aux = (int8_t *) g_malloc(lei->qp_map_bytes, 0);
            lei->qp_map_main = (int8_t *) g_malloc(lei->qp_map_bytes, 1);
            if ((lei->qp_map_aux == NULL) || (lei->qp_map_main == NULL))
            {
                g_free(lei->qp_map_aux);
                g_free(lei->qp_map_main);
                lei->qp_map_aux = NULL;
                lei->qp_map_main = NULL;
            }
            else
            {
                for (index = 0; index < lei->qp_map_bytes; index++)
                {
                    lei->qp_map_aux[index] = (int8_t) aux_delta;
                }
            }
        }
    }

    g_memset(&bitstreamParams, 0, sizeof(bitstreamParams));
    bitstreamParams.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    nv_error = g_enc_funcs.nvEncCreateBitstreamBuffer(lei->enc,
               &bitstreamParams);
    LOG(LOG_LEVEL_INFO, "nvEncCreateBitstreamBuffer rv %d", nv_error);
    if (nv_error != NV_ENC_SUCCESS)
    {
        g_free(lei);
        return 1;
    }

    lei->bitstreamBuffer = bitstreamParams.bitstreamBuffer;
    lei->width = width;
    lei->height = height;

    *ei = lei;

    return 0;
}

/*****************************************************************************/
int
xrdp_accel_assist_nvenc_delete_encoder(struct enc_info *ei)
{
    g_enc_funcs.nvEncUnmapInputResource(ei->enc, ei->mappedResource);
    g_enc_funcs.nvEncUnregisterResource(ei->enc, ei->registeredResource);
    if (ei->avc444)
    {
        g_enc_funcs.nvEncUnmapInputResource(ei->enc, ei->mappedResourceAux);
        g_enc_funcs.nvEncUnregisterResource(ei->enc,
                                            ei->registeredResourceAux);
    }
    g_free(ei->qp_map_aux);
    g_free(ei->qp_map_main);
    g_enc_funcs.nvEncDestroyBitstreamBuffer(ei->enc, ei->bitstreamBuffer);
    g_enc_funcs.nvEncDestroyEncoder(ei->enc);
    g_free(ei);
    return 0;
}

/*****************************************************************************/
/* XRDP_NVENC_DUMP_STREAM=<prefix>: append every picture to <prefix>.h264,
   and a line per picture (view, IDR, bytes) to <prefix>.index, for checking
   the reference structure offline. Stops at 64 MB. */
static void
nvenc_dump_stream(int view, int idr, const void *data, int bytes)
{
    static const char *prefix = NULL;
    static int checked = 0;
    static int total = 0;
    static int seq = 0;
    char filename[512];
    char line[128];
    int fd;
    int len;

    if (!checked)
    {
        checked = 1;
        prefix = g_getenv("XRDP_NVENC_DUMP_STREAM");
        if (prefix != NULL)
        {
            LOG(LOG_LEVEL_INFO, "nvenc: dumping the stream to %s.h264, "
                "index in %s.index", prefix, prefix);
        }
    }
    if ((prefix == NULL) || (bytes <= 0) || (total > 64 * 1024 * 1024))
    {
        return;
    }
    g_snprintf(filename, sizeof(filename) - 1, "%s.h264", prefix);
    fd = g_file_open_ex(filename, 0, 1, 1, seq == 0);
    if (fd < 0)
    {
        return;
    }
    g_file_seek(fd, total);
    g_file_write(fd, (const char *) data, bytes);
    g_file_close(fd);
    g_snprintf(filename, sizeof(filename) - 1, "%s.index", prefix);
    fd = g_file_open_ex(filename, 0, 1, 1, seq == 0);
    if (fd >= 0)
    {
        g_file_seek_end(fd, 0);
        len = g_snprintf(line, sizeof(line), "%d view %d idr %d offset %d "
                         "bytes %d\n", seq, view, idr, total, bytes);
        g_file_write(fd, line, len);
        g_file_close(fd);
    }
    total += bytes;
    seq++;
}

/*****************************************************************************/
/* Encode one picture. cdata NULL: encode it and discard the output. */
static enum encoder_result
nvenc_submit(struct enc_info *ei, NV_ENC_PIC_PARAMS *picParams,
             void *cdata, int *cdata_bytes)
{
    NV_ENC_LOCK_BITSTREAM lockBitstream;
    NVENCSTATUS nv_error;
    enum encoder_result rv;

    nv_error = g_enc_funcs.nvEncEncodePicture(ei->enc, picParams);
    if (nv_error != NV_ENC_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "error nvEncEncodePicture %d", nv_error);
        return ENCODER_ERROR;
    }
    ei->frameCount++;
    rv = ENCODER_ERROR;
    g_memset(&lockBitstream, 0, sizeof(lockBitstream));
    lockBitstream.version = NV_ENC_LOCK_BITSTREAM_VER;
    lockBitstream.outputBitstream = ei->bitstreamBuffer;
    lockBitstream.doNotWait = 0;
    nv_error = g_enc_funcs.nvEncLockBitstream(ei->enc, &lockBitstream);
    if (nv_error != NV_ENC_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "error nvEncLockBitstream %d", nv_error);
        return ENCODER_ERROR;
    }
    if (cdata == NULL)
    {
        rv = INCREMENTAL_FRAME_ENCODED;
    }
    else if (*cdata_bytes >= ((int) (lockBitstream.bitstreamSizeInBytes)))
    {
        g_memcpy(cdata, lockBitstream.bitstreamBufferPtr,
                 lockBitstream.bitstreamSizeInBytes);
        *cdata_bytes = lockBitstream.bitstreamSizeInBytes;
        rv = INCREMENTAL_FRAME_ENCODED;
    }
    else
    {
        LOG(LOG_LEVEL_ERROR, "error not enough room %d %d",
            *cdata_bytes, (int) (lockBitstream.bitstreamSizeInBytes));
    }
    g_enc_funcs.nvEncUnlockBitstream(ei->enc, lockBitstream.outputBitstream);
    if ((rv != ENCODER_ERROR) &&
            (picParams->encodePicFlags & NV_ENC_PIC_FLAG_FORCEIDR))
    {
        ei->idr_count++;
    }
    return rv;
}

/*****************************************************************************/
enum encoder_result
xrdp_accel_assist_nvenc_encode(struct enc_info *ei, int tex,
                               void *cdata, int *cdata_bytes,
                               int flags, int idr_pic_id)
{
    NV_ENC_PIC_PARAMS picParams;
    NV_ENC_PIC_PARAMS_H264 *h264;
    enum encoder_result rv;
    int view;
    int idr;

    /* sync before encoding */
    glFinish();

    view = (ei->avc444 && (flags & XH_ENC_FLAGS_AUXVIEW)) ? 1 : 0;
    /* Only the main view may be an IDR. */
    idr = (view == 0) &&
    ((flags & XH_ENC_FLAGS_FORCEIDR) || (ei->frameCount < 1));

    g_memset(&picParams, 0, sizeof(picParams));
    picParams.version = NV_ENC_PIC_PARAMS_VER;
    if (view == 0)
    {
        picParams.inputBuffer = ei->mappedResource;
        picParams.bufferFmt = ei->mappedBufferFmt;
        picParams.encodePicFlags = NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        if (ei->qp_map_main != NULL)
        {
            picParams.qpDeltaMap = ei->qp_map_main;
            picParams.qpDeltaMapSize = ei->qp_map_bytes;
        }
    }
    else
    {
        picParams.inputBuffer = ei->mappedResourceAux;
        picParams.bufferFmt = ei->mappedBufferFmtAux;
        if (ei->qp_map_aux != NULL)
        {
            picParams.qpDeltaMap = ei->qp_map_aux;
            picParams.qpDeltaMapSize = ei->qp_map_bytes;
        }
    }
    picParams.inputWidth = ei->width;
    picParams.inputHeight = ei->height;
    picParams.outputBitstream = ei->bitstreamBuffer;
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    if (idr)
    {
        picParams.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
        LOG(LOG_LEVEL_INFO, "Forcing NVENC H264 IDR SPSPPS for frame id: %d",
            ei->frameCount);
        /* An IDR empties the DPB. */
        ei->ltr_valid[0] = 0;
        ei->ltr_valid[1] = 0;
        /* NVENC numbers IDRs itself, 0, 1, 0, ... from 0 in each session,
           and can't be told which. When that parity isn't the stream's,
           spend one IDR unsent: across an encoder rebuild, consecutive IDRs
           must still differ. */
        if ((idr_pic_id >= 0) && (((idr_pic_id ^ ei->idr_count) & 1) != 0))
        {
            picParams.inputTimeStamp = ei->frameCount;
            if (nvenc_submit(ei, &picParams, NULL, NULL) == ENCODER_ERROR)
            {
                return ENCODER_ERROR;
            }
        }
    }
    picParams.inputTimeStamp = ei->frameCount;
    if (ei->avc444)
    {
        /* Mark this picture long-term in its view's slot and predict only
           from that slot. A view with no reference yet (the aux after an
           IDR) is coded intra. */
        h264 = &(picParams.codecPicParams.h264PicParams);
        h264->ltrMarkFrame = 1;
        h264->ltrMarkFrameIdx = view;
        if (!idr)
        {
            if (ei->ltr_valid[view])
            {
                h264->ltrUseFrames = 1;
                h264->ltrUseFrameBitmap = 1 << view;
            }
            else
            {
                picParams.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEINTRA;
            }
        }
    }
    rv = nvenc_submit(ei, &picParams, cdata, cdata_bytes);
    if (rv == ENCODER_ERROR)
    {
        return ENCODER_ERROR;
    }
    nvenc_dump_stream(view, idr, cdata, *cdata_bytes);
    if (ei->avc444)
    {
        ei->ltr_valid[view] = 1;
    }
    return idr ? KEY_FRAME_ENCODED : INCREMENTAL_FRAME_ENCODED;
}
