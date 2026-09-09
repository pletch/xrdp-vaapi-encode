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

/* GLSL shaders
 * this file is not compiled directly, it is included in
 * xrdp_accel_assist_x11.c */

static const GLchar g_vs[] = "\
attribute vec4 position;\n\
void main(void)\n\
{\n\
    gl_Position = vec4(position.xy, 0.0, 1.0);\n\
}\n";

static const GLchar g_fs_copy[] = "\
uniform sampler2D tex;\n\
uniform vec2 tex_size;\n\
void main(void)\n\
{\n\
    gl_FragColor = texture2D(tex, gl_FragCoord.xy / tex_size);\n\
}\n";

/* Same four-bytes-per-fragment packing as the MV shader below; see the
   comment there. This one keeps the 2x2 chroma average of plain 4:2:0. */
static const GLchar g_fs_rgb_to_yuv420[] = "\
uniform sampler2D tex;\n\
uniform vec2 tex_size;\n\
uniform vec4 ymath;\n\
uniform vec4 umath;\n\
uniform vec4 vmath;\n\
void main(void)\n\
{\n\
    vec4 p0;\n\
    vec4 p1;\n\
    vec4 p2;\n\
    vec4 p3;\n\
    vec4 ca;\n\
    vec4 cb;\n\
    float bx;\n\
    float y;\n\
    float sy;\n\
    bx = floor(gl_FragCoord.x) * 4.0;\n\
    y = gl_FragCoord.y;\n\
    if (y < tex_size.y)\n\
    {\n\
        p0 = texture2D(tex, vec2(bx + 0.5, y) / tex_size); p0.a = 1.0;\n\
        p1 = texture2D(tex, vec2(bx + 1.5, y) / tex_size); p1.a = 1.0;\n\
        p2 = texture2D(tex, vec2(bx + 2.5, y) / tex_size); p2.a = 1.0;\n\
        p3 = texture2D(tex, vec2(bx + 3.5, y) / tex_size); p3.a = 1.0;\n\
        gl_FragColor = clamp(vec4(dot(ymath, p0), dot(ymath, p1),\n\
                                  dot(ymath, p2), dot(ymath, p3)),\n\
                             0.0, 1.0);\n\
    }\n\
    else\n\
    {\n\
        sy = floor(y - tex_size.y) * 2.0 + 0.5;\n\
        ca = texture2D(tex, vec2(bx + 0.5, sy) / tex_size)\n\
           + texture2D(tex, vec2(bx + 1.5, sy) / tex_size)\n\
           + texture2D(tex, vec2(bx + 0.5, sy + 1.0) / tex_size)\n\
           + texture2D(tex, vec2(bx + 1.5, sy + 1.0) / tex_size);\n\
        ca *= 0.25; ca.a = 1.0;\n\
        cb = texture2D(tex, vec2(bx + 2.5, sy) / tex_size)\n\
           + texture2D(tex, vec2(bx + 3.5, sy) / tex_size)\n\
           + texture2D(tex, vec2(bx + 2.5, sy + 1.0) / tex_size)\n\
           + texture2D(tex, vec2(bx + 3.5, sy + 1.0) / tex_size);\n\
        cb *= 0.25; cb.a = 1.0;\n\
        gl_FragColor = clamp(vec4(dot(umath, ca), dot(vmath, ca),\n\
                                  dot(umath, cb), dot(vmath, cb)),\n\
                             0.0, 1.0);\n\
    }\n\
}\n";

static const GLchar g_fs_rgb_to_yuv422[] = "\
uniform sampler2D tex;\n\
uniform vec2 tex_size;\n\
uniform vec4 ymath;\n\
uniform vec4 umath;\n\
uniform vec4 vmath;\n\
void main(void)\n\
{\n\
    vec4 pix;\n\
    vec4 pix1;\n\
    vec4 pixs;\n\
    float x;\n\
    float y;\n\
    x = gl_FragCoord.x;\n\
    x = floor(x) * 2.0 + 0.5;\n\
    y = gl_FragCoord.y;\n\
    pix = texture2D(tex, vec2(x, y) / tex_size);\n\
    pix1 = texture2D(tex, vec2(x + 1.0, y) / tex_size);\n\
    pixs = (pix + pix1) / 2.0;\n\
    pix.a = 1.0;\n\
    pix1.a = 1.0;\n\
    pixs.a = 1.0;\n\
    pix.r = dot(ymath, pix);\n\
    pix.g = dot(umath, pixs);\n\
    pix.b = dot(ymath, pix1);\n\
    pix.a = dot(vmath, pixs);\n\
    gl_FragColor = clamp(pix, 0.0, 1.0);\n\
}\n";

static const GLchar g_fs_rgb_to_yuv444[] = "\
uniform sampler2D tex;\n\
uniform vec2 tex_size;\n\
uniform vec4 ymath;\n\
uniform vec4 umath;\n\
uniform vec4 vmath;\n\
void main(void)\n\
{\n\
    vec4 pix;\n\
    pix = texture2D(tex, gl_FragCoord.xy / tex_size);\n\
    pix.a = 1.0;\n\
    pix = vec4(dot(vmath, pix), dot(umath, pix), dot(ymath, pix), 1.0);\n\
    gl_FragColor = clamp(pix, 0.0, 1.0);\n\
}\n";

/*
RGB
    00 10 20 30 40 50 60 70 80 90 A0 B0 C0 D0 E0 F0
    01 11 21 31 41 51 61 71 81 91 A1 B1 C1 D1 E1 F1
    02 12 22 32 42 52 62 72 82 92 A2 B2 C2 D2 E2 F2
    03 13 23 33 43 53 63 73 83 93 A3 B3 C3 D3 E3 F3
    04 14 24 34 44 54 64 74 84 94 A4 B4 C4 D4 E4 F4
    05 15 25 35 45 55 65 75 85 95 A5 B5 C5 D5 E5 F5
    06 16 26 36 46 56 66 76 86 96 A6 B6 C6 D6 E6 F6
    07 17 27 37 47 57 67 77 87 97 A7 B7 C7 D7 E7 F7
    08 18 28 38 48 58 68 78 88 98 A8 B8 C8 D8 E8 F8
    09 19 29 39 49 59 69 79 89 99 A9 B9 C9 D9 E9 F9
    0A 1A 2A 3A 4A 5A 6A 7A 8A 9A AA BA CA DA EA FA
    0B 1B 2B 3B 4B 5B 6B 7B 8B 9B AB BB CB DB EB FB
    0C 1C 2C 3C 4C 5C 6C 7C 8C 9C AC BC CC DC EC FC
    0D 1D 2D 3D 4D 5D 6D 7D 8D 9D AD BD CD DD ED FD
    0E 1E 2E 3E 4E 5E 6E 7E 8E 9E AE BE CE DE EE FE
    0F 1F 2F 3F 4F 5F 6F 7F 8F 9F AF BF CF DF EF FF

MAIN VIEW - NV12

    /---------------------Y-----------------------\
    00 10 20 30 40 50 60 70 80 90 A0 B0 C0 D0 E0 F0
    01 11 21 31 41 51 61 71 81 91 A1 B1 C1 D1 E1 F1
    ...
    0F 1F 2F 3F 4F 5F 6F 7F 8F 9F AF BF CF DF EF FF

    /U /V /U /V /U /V /U /V /U /V /U /V /U /V /U /V
    00 00 20 20 40 40 60 60 80 80 A0 A0 C0 C0 E0 E0
    02 02 22 22 42 42 62 62 82 82 A2 A2 C2 C2 E2 E2
    ...
    0E 0E 2E 2E 4E 4E 6E 6E 8E 8E AE AE CE CE EE EE
*/
/* pad_h: the Y/UV plane boundary of the destination NV12 surface. For plain
   AVC420 that is the source height. For AVC444 both views must be pictures of
   ONE H.264 sequence, so both are encoded at the 16-aligned height that the
   aux view already requires (MS-RDPEGFX 2.2.4.4.2) -- a resolution change
   mid-sequence would force a new SPS and an IDR on every picture. The main
   view's rows H..pad_h-1 sample past the source and come back clamp-to-edge;
   the client only ever reads the first H rows of the luma view. */
/* The NV12 destination is written four bytes at a time, as RGBA8 over a
   quarter-width viewport, rather than one byte at a time as R8.

   Two reasons. The target has to be linear, because it is exported as a
   dma-buf for VAAPI, and single-byte writes to linear memory coalesce badly.
   And the UV row's "is this byte U or V" test diverges between neighbouring
   fragments, so every quad executed both sides of it; taking four bytes at
   once turns that branch into straight-line code. Measured cost of this pass
   before the change: 13.5 ms per frame at 3008x3000, against 7.6 ms for the
   encode it feeds.

   gl_FragColor.r is the lowest address of the texel, so the four components
   land as consecutive bytes, which is what NV12 wants. Fragment x covers
   destination bytes 4x .. 4x+3. */
static const GLchar g_fs_rgb_to_yuv420_mv[] = "\
uniform sampler2D tex;\n\
uniform vec2 tex_size;\n\
uniform float pad_h;\n\
uniform vec4 ymath;\n\
uniform vec4 umath;\n\
uniform vec4 vmath;\n\
void main(void)\n\
{\n\
    vec4 p0;\n\
    vec4 p1;\n\
    vec4 p2;\n\
    vec4 p3;\n\
    vec4 ca;\n\
    vec4 cb;\n\
    float bx;\n\
    float y;\n\
    float sy;\n\
    bx = floor(gl_FragCoord.x) * 4.0;\n\
    y = gl_FragCoord.y;\n\
    if (y < pad_h)\n\
    {\n\
        /* four luma bytes from four consecutive source pixels */\n\
        p0 = texture2D(tex, vec2(bx + 0.5, y) / tex_size); p0.a = 1.0;\n\
        p1 = texture2D(tex, vec2(bx + 1.5, y) / tex_size); p1.a = 1.0;\n\
        p2 = texture2D(tex, vec2(bx + 2.5, y) / tex_size); p2.a = 1.0;\n\
        p3 = texture2D(tex, vec2(bx + 3.5, y) / tex_size); p3.a = 1.0;\n\
        gl_FragColor = clamp(vec4(dot(ymath, p0), dot(ymath, p1),\n\
                                  dot(ymath, p2), dot(ymath, p3)),\n\
                             0.0, 1.0);\n\
    }\n\
    else\n\
    {\n\
        /* NV12 UV row: bytes are U,V,U,V, each the MEAN of its 2x2 source\n\
           block rather than the even/even sample of it.\n\
\n\
           This matters only under AVC444, and it is not a filtering\n\
           preference. The auxiliary view carries three of every four chroma\n\
           samples -- odd/even, even/odd, odd/odd -- and a decoder recovers\n\
           the fourth by solving u0 = 4*mean - u1 - u2 - u3. FreeRDP does\n\
           exactly that in YUV444 to RGB (prim_YUV.c, the (i==0 && j==0)\n\
           arm), and clients derived from it do the same. Storing the sample\n\
           here instead of the mean makes that recovery wrong: on flat\n\
           colour 4u - 3u is still u and nothing shows, but across a\n\
           high-contrast edge the four samples differ and one pixel in four\n\
           lands far from its true value -- visible as coloured speckling on\n\
           icon and glyph edges, and on nothing else. */\n\
        sy = floor(y - pad_h) * 2.0 + 0.5;\n\
        ca = texture2D(tex, vec2(bx + 0.5, sy) / tex_size)\n\
           + texture2D(tex, vec2(bx + 1.5, sy) / tex_size)\n\
           + texture2D(tex, vec2(bx + 0.5, sy + 1.0) / tex_size)\n\
           + texture2D(tex, vec2(bx + 1.5, sy + 1.0) / tex_size);\n\
        ca *= 0.25; ca.a = 1.0;\n\
        cb = texture2D(tex, vec2(bx + 2.5, sy) / tex_size)\n\
           + texture2D(tex, vec2(bx + 3.5, sy) / tex_size)\n\
           + texture2D(tex, vec2(bx + 2.5, sy + 1.0) / tex_size)\n\
           + texture2D(tex, vec2(bx + 3.5, sy + 1.0) / tex_size);\n\
        cb *= 0.25; cb.a = 1.0;\n\
        gl_FragColor = clamp(vec4(dot(umath, ca), dot(vmath, ca),\n\
                                  dot(umath, cb), dot(vmath, cb)),\n\
                             0.0, 1.0);\n\
    }\n\
}\n";

/*
AUXILIARY VIEW - NV12

    /---------------------U-----------------------\
    01 11 21 31 41 51 61 71 81 91 A1 B1 C1 D1 E1 F1
    03 13 23 33 43 53 63 73 83 93 A3 B3 C3 D3 E3 F4
    ...
    0F 1F 2F 3F 4F 5F 6F 7F 8F 9F AF BF CF DF EF FF
    /---------------------V-----------------------\
    01 11 21 31 41 51 61 71 81 91 A1 B1 C1 D1 E1 F1
    03 13 23 33 43 53 63 73 83 93 A3 B3 C3 D3 E3 F4
    ...
    0F 1F 2F 3F 4F 5F 6F 7F 8F 9F AF BF CF DF EF FF
    ... (8 LINES U, 8 LINES V, REPEAT)

    /U /V /U /V /U /V /U /V /U /V /U /V /U /V /U /V
    10 10 30 30 50 50 70 70 90 90 B0 B0 D0 D0 F0 F0
    12 12 32 32 52 52 72 72 92 92 B2 B2 D2 D2 F2 F2
    ...
    1E 1E 3E 3E 5E 5E 7E 7E 9E 9E BE BE DE DE FE FE
*/
/* The aux view NV12 surface is sized (W, H_PAD) where H_PAD = ((H+15)&~15)
   per MS-RDPEGFX 2.2.4.4.2. We need pad_h (= H_PAD) for the Y/UV plane
   gate and the UV offset, but tex_size (= source pixmap dims) for source
   sampling normalisation. With pad_h=1088 but source H=1080, the last
   ~4 dest rows sample beyond the source -- texture wrap clamp-to-edge
   makes those reuse the last source row; those rows are not read by the
   decoder anyway (pos >= H), so the values don't matter. */
static const GLchar g_fs_rgb_to_yuv420_av[] = "\
uniform sampler2D tex;\n\
uniform vec2 tex_size;\n\
uniform float pad_h;\n\
uniform vec4 umath;\n\
uniform vec4 vmath;\n\
void main(void)\n\
{\n\
    vec4 p0;\n\
    vec4 p1;\n\
    vec4 p2;\n\
    vec4 p3;\n\
    vec4 ca;\n\
    vec4 cb;\n\
    float bx;\n\
    float y;\n\
    float y1;\n\
    float sy;\n\
    bx = floor(gl_FragCoord.x) * 4.0;\n\
    y = gl_FragCoord.y;\n\
    if (y < pad_h)\n\
    {\n\
        y1 = mod(y, 16.0);\n\
        if (y1 < 8.0)\n\
        {\n\
            sy = floor(y / 16.0) * 8.0 + y1;\n\
            sy = floor(sy) * 2.0 + 1.5;\n\
            p0 = texture2D(tex, vec2(bx + 0.5, sy) / tex_size); p0.a = 1.0;\n\
            p1 = texture2D(tex, vec2(bx + 1.5, sy) / tex_size); p1.a = 1.0;\n\
            p2 = texture2D(tex, vec2(bx + 2.5, sy) / tex_size); p2.a = 1.0;\n\
            p3 = texture2D(tex, vec2(bx + 3.5, sy) / tex_size); p3.a = 1.0;\n\
            gl_FragColor = clamp(vec4(dot(umath, p0), dot(umath, p1),\n\
                                      dot(umath, p2), dot(umath, p3)),\n\
                                 0.0, 1.0);\n\
        }\n\
        else\n\
        {\n\
            sy = floor(y / 16.0) * 8.0 + (y1 - 8.0);\n\
            sy = floor(sy) * 2.0 + 1.5;\n\
            p0 = texture2D(tex, vec2(bx + 0.5, sy) / tex_size); p0.a = 1.0;\n\
            p1 = texture2D(tex, vec2(bx + 1.5, sy) / tex_size); p1.a = 1.0;\n\
            p2 = texture2D(tex, vec2(bx + 2.5, sy) / tex_size); p2.a = 1.0;\n\
            p3 = texture2D(tex, vec2(bx + 3.5, sy) / tex_size); p3.a = 1.0;\n\
            gl_FragColor = clamp(vec4(dot(vmath, p0), dot(vmath, p1),\n\
                                      dot(vmath, p2), dot(vmath, p3)),\n\
                                 0.0, 1.0);\n\
        }\n\
    }\n\
    else\n\
    {\n\
        /* bx is a multiple of four, so the four destination bytes are\n\
           even, odd, even, odd: U and V of source column bx + 1, then of\n\
           bx + 3. Two fetches cover them. */\n\
        sy = floor(y - pad_h) * 2.0 + 0.5;\n\
        ca = texture2D(tex, vec2(bx + 1.5, sy) / tex_size); ca.a = 1.0;\n\
        cb = texture2D(tex, vec2(bx + 3.5, sy) / tex_size); cb.a = 1.0;\n\
        gl_FragColor = clamp(vec4(dot(umath, ca), dot(vmath, ca),\n\
                                  dot(umath, cb), dot(vmath, cb)),\n\
                             0.0, 1.0);\n\
    }\n\
}\n";

/*
AUXILIARY VIEW V2 - NV12

The U/V split sits at half the 16-ALIGNED width, not half the surface width:
FreeRDP passes the aligned width as nTotalWidth (libfreerdp/codec/yuv.c:507)
and mstsc uses the same convention. Splitting at half the surface width puts
every V sample eight source pixels out on any width that is not a multiple of
16 -- coloured fringing on sharp edges. So x1 = ceil(W/16)*8, and the plane is
ceil(W/16)*16 wide. The columns between W/2 and x1 sample past the source and
come back clamp-to-edge; the client never reads them.

    /----------U----------\ /----------V----------\
    10 30 50 70 90 B0 D0 F0 10 30 50 70 90 B0 D0 F0
    11 31 51 71 91 B1 D1 F1 11 31 51 71 91 B1 D1 F1
    ...
    1F 3F 5F 7F 9F BF DF FF 1F 3F 5F 7F 9F BF DF FF

    /----------U----------\ /----------V----------\
    01 21 41 61 81 A1 C1 E1 01 21 41 61 81 A1 C1 E1
    03 23 43 63 83 A3 C3 E3 03 23 43 63 83 A3 C3 E3
    ...
    0F 2F 4F 6F 8F AF CF EF 0F 2F 4F 6F 8F AF CF EF
*/
static const GLchar g_fs_rgb_to_yuv420_av_v2[] = "\
uniform sampler2D tex;\n\
uniform vec2 tex_size;\n\
uniform vec4 umath;\n\
uniform vec4 vmath;\n\
void main(void)\n\
{\n\
    vec4 p0;\n\
    vec4 p1;\n\
    vec4 p2;\n\
    vec4 p3;\n\
    vec4 m;\n\
    float bx;\n\
    float y;\n\
    float x1;\n\
    float base;\n\
    float sx;\n\
    float sy;\n\
    bx = floor(gl_FragCoord.x) * 4.0;\n\
    y = gl_FragCoord.y;\n\
    x1 = ceil(tex_size.x / 16.0) * 8.0;\n\
    /* x1 is a multiple of eight, so a four-byte group never straddles the\n\
       U/V split and one branch settles the whole fragment. */\n\
    if (bx < x1)\n\
    {\n\
        base = bx;\n\
        m = umath;\n\
    }\n\
    else\n\
    {\n\
        base = bx - x1;\n\
        m = vmath;\n\
    }\n\
    if (y < tex_size.y)\n\
    {\n\
        sx = base * 2.0 + 1.5;\n\
        sy = y;\n\
    }\n\
    else\n\
    {\n\
        sx = base * 2.0 + 0.5;\n\
        sy = floor(y - tex_size.y) * 2.0 + 1.5;\n\
    }\n\
    p0 = texture2D(tex, vec2(sx,       sy) / tex_size); p0.a = 1.0;\n\
    p1 = texture2D(tex, vec2(sx + 2.0, sy) / tex_size); p1.a = 1.0;\n\
    p2 = texture2D(tex, vec2(sx + 4.0, sy) / tex_size); p2.a = 1.0;\n\
    p3 = texture2D(tex, vec2(sx + 6.0, sy) / tex_size); p3.a = 1.0;\n\
    gl_FragColor = clamp(vec4(dot(m, p0), dot(m, p1),\n\
                              dot(m, p2), dot(m, p3)),\n\
                         0.0, 1.0);\n\
}\n";
