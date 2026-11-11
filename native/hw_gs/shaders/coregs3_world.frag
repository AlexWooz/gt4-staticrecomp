// SPDX-License-Identifier: MIT
//
// CoreGS 3.0 live world fragment stage (TASK-090). Copyright (c) 2026 GT4Recomp contributors. Written from the measured
// sampler subset (native/coregs3/coregs3_sampler.cpp), the tapped/pass-journal GS state and the display image's own
// format (R32_UINT RGBA8 words), clean room (decision 0070).
//
// What this stage reproduces: the item's texture (T4/T8/CT32 decoded to RGBA8 in the atlas) sampled nearest at level 0
// with the measured 10.4 coordinate and the four wrap modes, MODULATE with the 8-bit integer product, the alpha test
// (ATE/ATST/AREF, AFAIL KEEP -> discard) and the FRAME.FBMSK lane merge. What it deliberately does not reproduce yet is
// named in the flags the CPU sets per item and counted there: fog (the per-vertex fog needs the tap), blending (the
// world PRIM default has ABE off) and mip levels (the table decodes level 0 with its counted fallback). A fragment
// whose sampled texel falls outside the decoded rectangle keeps the vertex colour instead of inventing a texel.
//
// The pass has no colour attachment: the composed display image is an R32_UINT storage image, so the stage reads the
// old word only for FBMSK and writes the packed word itself.
#version 450

#extension GL_GOOGLE_include_directive : require
#include "coregs3_world_push.glsl"

layout(early_fragment_tests) in;

layout(set = 0, binding = 0, r32ui) uniform uimage2D display_image;
layout(set = 0, binding = 1, rgba8) uniform readonly image2D world_atlas;

layout(location = 0) in vec4 colour;
layout(location = 1) in vec2 model_uv;

const uint kFlagTextured = 1u;
const uint kFlagAlphaTest = 2u;
const uint kFlagFogDeferred = 4u;       // PRIM.FGE on: no per-vertex fog is available (the tap carries it)
const uint kFlagBlendDeferred = 8u;     // PRIM.ABE on: the destination blend is not reproduced yet
const uint kFlagAlphaFailDefaulted = 16u;  // AFAIL other than KEEP: the fragment still discards on fail

uint channel(float value) {
    return uint(clamp(value, 0.0, 1.0) * 255.0 + 0.5);
}

// The GS 10.4 coordinate's axis wrap (chapter 01.1, coregs3_sampler.cpp's wrap_texel).
int wrap_texel(int texel, uint log2_size, uint mode, int minimum, int maximum) {
    int last = int((1u << log2_size) - 1u);
    if (mode == 0u) return texel & last;                       // REPEAT
    if (mode == 1u) return clamp(texel, 0, last);              // CLAMP
    if (mode == 2u) return clamp(texel, minimum, maximum);     // REGION_CLAMP
    return (texel & minimum) | maximum;                        // REGION_REPEAT
}

void main() {
    uvec4 vertex = uvec4(channel(colour.r), channel(colour.g), channel(colour.b), channel(colour.a));
    uvec4 result = vertex;
    uint flags = world.state.w;
    if ((flags & kFlagTextured) != 0u) {
        const uint tex0_lo = world.tex_words.x;
        const uint tex0_hi = world.tex_words.y;
        const uint tw = (tex0_lo >> 26) & 0xFu;
        const uint th = (tex0_lo >> 30) & 0xFu;
        const uint tcc = tex0_hi & 1u;
        const uint clamp_lo = world.tex_words.z;
        const uint clamp_hi = world.tex_words.w;
        const uint wms = clamp_lo & 3u;
        const uint wmt = (clamp_lo >> 2) & 3u;
        const int minu = int((clamp_lo >> 4) & 0x3FFu);
        const int maxu = int((clamp_lo >> 14) & 0x3FFu);
        const int minv = int(((clamp_hi & 3u) << 8) | (clamp_lo >> 24));
        const int maxv = int((clamp_hi >> 2) & 0x3FFu);
        // Nearest, level 0: floor of the 10.4 coordinate, then the axis wrap (coregs3_sampler.cpp).
        const float scaled_s = floor(model_uv.x * float(1u << tw) * 16.0);
        const float scaled_t = floor(model_uv.y * float(1u << th) * 16.0);
        const int texel_s = wrap_texel(int(scaled_s) >> 4, tw, wms, minu, maxu);
        const int texel_t = wrap_texel(int(scaled_t) >> 4, th, wmt, minv, maxv);
        const uint origin = world.atlas.x;
        const uint rect = world.atlas.y;
        const uint size = world.atlas.z;
        const ivec2 local = ivec2(texel_s - int(rect & 0xFFFFu), texel_t - int(rect >> 16));
        const ivec2 extent = ivec2(int(size & 0xFFFFu), int(size >> 16));
        if (local.x >= 0 && local.y >= 0 && local.x < extent.x && local.y < extent.y) {
            const ivec2 at = ivec2(int(origin & 0xFFFFu), int(origin >> 16)) + local;
            const vec4 texel = imageLoad(world_atlas, at);
            const uvec4 tex = uvec4(channel(texel.r), channel(texel.g), channel(texel.b), channel(texel.a));
            result = uvec4(min(255u, (tex.r * vertex.r) >> 7u), min(255u, (tex.g * vertex.g) >> 7u),
                           min(255u, (tex.b * vertex.b) >> 7u),
                           tcc != 0u ? min(255u, (tex.a * vertex.a) >> 7u) : vertex.a);
        }
    }
    if ((flags & kFlagAlphaTest) != 0u) {
        const uint test = world.state.x;
        const uint atst = (test >> 1) & 7u;
        const uint aref = (test >> 4) & 0xFFu;
        const uint alpha = result.a;
        bool pass = true;
        if (atst == 0u) pass = false;          // NEVER
        else if (atst == 1u) pass = true;      // ALWAYS
        else if (atst == 2u) pass = alpha < aref;
        else if (atst == 3u) pass = alpha <= aref;
        else if (atst == 4u) pass = alpha == aref;
        else if (atst == 5u) pass = alpha >= aref;
        else if (atst == 6u) pass = alpha > aref;
        else pass = alpha != aref;             // NOTEQUAL
        if (!pass) discard;
    }
    uint word = result.r | (result.g << 8) | (result.b << 16) | (result.a << 24);
    const uint fbmsk = world.state.z;
    if (fbmsk != 0u) {
        const uint old = imageLoad(display_image, ivec2(gl_FragCoord.xy)).x;
        word = (old & fbmsk) | (word & ~fbmsk);
    }
    imageStore(display_image, ivec2(gl_FragCoord.xy), uvec4(word, 0u, 0u, 0u));
}
