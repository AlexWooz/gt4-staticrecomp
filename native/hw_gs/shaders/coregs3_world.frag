// SPDX-License-Identifier: MIT
//
// CoreGS 3.0 live world fragment stage (TASK-090). Copyright (c) 2026 GT4Recomp contributors. Written from the measured
// sampler subset (native/coregs3/coregs3_sampler.cpp), the GS fog arithmetic (native/coregs3/coregs3_reflection.cpp,
// apply_vertex_fog) and the captured/tapped GS state, clean room (decision 0070).
//
// What this stage reproduces: the item's texture (T4/T8/CT32 decoded to RGBA8 in the atlas) sampled at level 0 with the
// measured 10.4 coordinate and the four CLAMP wrap modes, nearest or the GS bilinear (floor((u-0.5)*size) base, 4-bit
// weights, four independent wraps, sum >> 8, the same arithmetic as coregs3_sampler.cpp); MODULATE with the 8-bit
// integer product and TCC; the per-vertex fog toward FOGCOL with the GS mix (f*c + (255-f)*fogcol) >> 8, interpolated
// screen-linearly; the alpha test (ATE/ATST/AREF, AFAIL KEEP -> discard); and the FRAME.FBMSK lane merge. What it
// deliberately does not reproduce yet is named in the flags the CPU sets per item and counted there: blending (the world
// PRIM default has ABE off) and mip levels (the table decodes level 0 with its counted fallback). A fragment whose
// sampled texel falls outside the decoded rectangle keeps the vertex colour instead of inventing a texel.
//
// The pass has no colour attachment: the composed display image is an R32_UINT storage image, so the stage reads the
// old word only for FBMSK and writes the packed word itself.
#version 450

#extension GL_GOOGLE_include_directive : require
#include "coregs3_world_push.glsl"
#include "coregs3_post.glsl"

layout(early_fragment_tests) in;

layout(set = 0, binding = 0, r32ui) uniform uimage2D display_image;
layout(set = 0, binding = 1, rgba8) uniform readonly image2D world_atlas;
// TASK-090 car round: the written-fragment counters shared with the car stage (word 0 reflection, word 1 world).
layout(set = 0, binding = 2, std430) buffer FragmentCounters {
    uint reflection_fragments;
    uint world_fragments;
    uint reflection_date_failed;
    uint reflection_entered;
} counters;

layout(location = 0) in vec4 colour;
layout(location = 1) in vec2 model_uv;
layout(location = 2) noperspective in float fog;

const uint kFlagTextured = 1u;
const uint kFlagAlphaTest = 2u;
const uint kFlagFog = 4u;              // PRIM.FGE: the per-vertex fog toward FOGCOL is applied
const uint kFlagBlendDeferred = 8u;    // PRIM.ABE on: the destination blend is not reproduced yet
const uint kFlagAlphaFailDefaulted = 16u;  // AFAIL other than KEEP: the fragment still discards on fail
const uint kFlagLinear = 32u;          // TEX1 linear: the GS bilinear

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

// One texel of the decoded rectangle; `ok` is false when the wrapped coordinate lands outside it (the caller then keeps
// the vertex colour, the same counted fallback the CPU sampler makes).
bool fetch_texel(ivec2 local, ivec2 extent, ivec2 origin, out uvec4 texel) {
    if (local.x < 0 || local.y < 0 || local.x >= extent.x || local.y >= extent.y) return false;
    vec4 value = imageLoad(world_atlas, origin + local);
    texel = uvec4(channel(value.r), channel(value.g), channel(value.b), channel(value.a));
    return true;
}

// The GS fog mix's per-channel integer form: (f*c + (255-f)*fogcol) >> 8, clamped (apply_vertex_fog).
uint fog_mix(uint c, uint fc, int f) {
    const int value = (f * int(c) + (255 - f) * int(fc)) >> 8;
    return uint(clamp(value, 0, 255));
}

void main() {
    uvec4 vertex = uvec4(channel(colour.r), channel(colour.g), channel(colour.b), channel(colour.a));
    uvec4 result = vertex;
    uint flags = world.state.w;
    if ((flags & kFlagTextured) != 0u) {
        const uint tex0_lo = world.tex_words.x;
        const uint tex0_hi = world.tex_words.y;
        const uint tw = (tex0_lo >> 26) & 0xFu;
        // TEX0.TH is bits 30..33 and TEX0.TCC bit 34: both straddle the 32-bit halves of the register word (TH bits
        // 32..33 and TCC bit 34 live in tex0_hi bits 0..1 and 2), exactly like the GS layout and hw_gs_registers.hpp.
        const uint th = ((tex0_lo >> 30) & 0x3u) | ((tex0_hi & 0x3u) << 2);
        const uint tcc = (tex0_hi >> 2) & 1u;
        const uint clamp_lo = world.tex_words.z;
        const uint clamp_hi = world.tex_words.w;
        const uint wms = clamp_lo & 3u;
        const uint wmt = (clamp_lo >> 2) & 3u;
        const int minu = int((clamp_lo >> 4) & 0x3FFu);
        const int maxu = int((clamp_lo >> 14) & 0x3FFu);
        const int minv = int(((clamp_hi & 3u) << 8) | (clamp_lo >> 24));
        const int maxv = int((clamp_hi >> 2) & 0x3FFu);
        // The GS 10.4 coordinate, then nearest or bilinear (coregs3_sampler.cpp).
        const int s = int(clamp(floor(model_uv.x * float(1u << tw) * 16.0), -1.0e9, 1.0e9));
        const int t = int(clamp(floor(model_uv.y * float(1u << th) * 16.0), -1.0e9, 1.0e9));
        const uint origin_word = world.atlas.x;
        const uint rect_word = world.atlas.y;
        const uint size_word = world.atlas.z;
        const ivec2 origin = ivec2(int(origin_word & 0xFFFFu), int(origin_word >> 16));
        const ivec2 rect = ivec2(int(rect_word & 0xFFFFu), int(rect_word >> 16));
        const ivec2 extent = ivec2(int(size_word & 0xFFFFu), int(size_word >> 16));
        uvec4 texel = uvec4(0u);
        bool sampled = false;
        if ((flags & kFlagLinear) != 0u) {
            // Bilinear: (u - 0.5) to a floor base plus 4-bit weights; the four taps wrap independently; sum >> 8.
            const int ss = s - 8;
            const int st = t - 8;
            const int base_x = ss >> 4;
            const int base_y = st >> 4;
            const int fx = ss & 15;
            const int fy = st & 15;
            const ivec2 p00 = ivec2(wrap_texel(base_x, tw, wms, minu, maxu) - rect.x,
                                    wrap_texel(base_y, th, wmt, minv, maxv) - rect.y);
            const ivec2 p10 = ivec2(wrap_texel(base_x + 1, tw, wms, minu, maxu) - rect.x, p00.y);
            const ivec2 p01 = ivec2(p00.x, wrap_texel(base_y + 1, th, wmt, minv, maxv) - rect.y);
            const ivec2 p11 = ivec2(p10.x, p01.y);
            uvec4 c00, c10, c01, c11;
            if (fetch_texel(p00, extent, origin, c00) && fetch_texel(p10, extent, origin, c10) &&
                fetch_texel(p01, extent, origin, c01) && fetch_texel(p11, extent, origin, c11)) {
                const int w00 = (16 - fx) * (16 - fy);
                const int w10 = fx * (16 - fy);
                const int w01 = (16 - fx) * fy;
                const int w11 = fx * fy;
                texel = uvec4(uint((int(c00.x) * w00 + int(c10.x) * w10 + int(c01.x) * w01 + int(c11.x) * w11) >> 8),
                              uint((int(c00.y) * w00 + int(c10.y) * w10 + int(c01.y) * w01 + int(c11.y) * w11) >> 8),
                              uint((int(c00.z) * w00 + int(c10.z) * w10 + int(c01.z) * w01 + int(c11.z) * w11) >> 8),
                              uint((int(c00.w) * w00 + int(c10.w) * w10 + int(c01.w) * w01 + int(c11.w) * w11) >> 8));
                sampled = true;
            }
        } else {
            const ivec2 local = ivec2(wrap_texel(s >> 4, tw, wms, minu, maxu) - rect.x,
                                      wrap_texel(t >> 4, th, wmt, minv, maxv) - rect.y);
            sampled = fetch_texel(local, extent, origin, texel);
        }
        if (sampled) {
            result = uvec4(min(255u, (texel.x * vertex.x) >> 7u), min(255u, (texel.y * vertex.y) >> 7u),
                           min(255u, (texel.z * vertex.z) >> 7u),
                           tcc != 0u ? min(255u, (texel.w * vertex.w) >> 7u) : vertex.w);
        }
    }
    if ((flags & kFlagFog) != 0u) {
        // The GS fog mix, apply_vertex_fog's integer form: (f*c + (255-f)*fogcol) >> 8, alpha unchanged. The varying is
        // the emitted fog byte interpolated screen-linearly; the CPU rounds it the same way (fog + 0.5).
        const int f = int(clamp(fog + 0.5, 0.0, 255.0));
        const uint fogc = world.atlas.w;
        result = uvec4(fog_mix(result.x, fogc & 0xFFu, f), fog_mix(result.y, (fogc >> 8) & 0xFFu, f),
                       fog_mix(result.z, (fogc >> 16) & 0xFFu, f), result.w);
    }
    if ((flags & kFlagAlphaTest) != 0u) {
        const uint test = world.state.x;
        const uint atst = (test >> 1) & 7u;
        const uint aref = (test >> 4) & 0xFFu;
        const uint alpha = result.w;
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
    // The captured post chain on this fragment (coregs3_post.glsl): the two multiplies and the CT16 model.
    result = post_epilogue(result, world.post);
    uint word = result.x | (result.y << 8) | (result.z << 16) | (result.w << 24);
    const uint fbmsk = world.state.z;
    if (fbmsk != 0u) {
        const uint old = imageLoad(display_image, ivec2(gl_FragCoord.xy)).x;
        word = (old & fbmsk) | (word & ~fbmsk);
    }
    atomicAdd(counters.world_fragments, 1u);
    imageStore(display_image, ivec2(gl_FragCoord.xy), uvec4(word, 0u, 0u, 0u));
}
