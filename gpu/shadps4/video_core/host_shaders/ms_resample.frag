// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (The Order: 1886): nearest resample between a native scene target and its reduced proxy
// when either is multisampled (blits do not take multisampled images). The pipeline rasterizes
// at the destination's sample count with sample shading; SRC_MS reads the same sample of a
// multisampled source (sample 0 for a single-sample destination), otherwise every sample gets
// the single-sample texel. DEPTH writes gl_FragDepth, UINT an unsigned integer color, else color.

#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

#ifdef UINT
#define TEX utexture2D
#define TEX_MS utexture2DMS
#define VALUE uvec4
#else
#define TEX texture2D
#define TEX_MS texture2DMS
#define VALUE vec4
#endif

#ifdef SRC_MS
layout (binding = 0, set = 0) uniform TEX_MS src;
#else
layout (binding = 0, set = 0) uniform TEX src;
#endif

layout (location = 0) in vec2 uv;
#ifndef DEPTH
layout (location = 0) out VALUE color;
#endif

void main() {
#ifdef SRC_MS
    const ivec2 size = textureSize(src);
    const ivec2 coord = min(ivec2(uv * vec2(size)), size - 1);
    const VALUE value = texelFetch(src, coord, min(gl_SampleID, textureSamples(src) - 1));
#else
    const ivec2 size = textureSize(src, 0);
    const ivec2 coord = min(ivec2(uv * vec2(size)), size - 1);
    const VALUE value = texelFetch(src, coord, 0);
#endif
#ifdef DEPTH
    gl_FragDepth = value.r;
#else
    color = value;
#endif
}
