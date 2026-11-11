// SPDX-License-Identifier: MIT
//
// CoreGS 3.0 live `present` car fragment stage, textured path (TASK-090 car pass round). Copyright (c) 2026 GT4Recomp
// contributors. Written from the measured sampler (coregs3_sampler.cpp), the offline car rasteriser's arithmetic
// (coregs3_render.cpp: MODULATE, the alpha test, the 8-bit ALPHA blend, the destination-alpha test, FBMSK) and the
// captured/tapped GS state, clean room (decision 0070).
//
// The stage reproduces the offline textured car path: the item's texture (decoded to RGBA8 in the atlas) sampled at
// level 0 with the four CLAMP wraps, nearest or the GS bilinear; MODULATE with the lit 8-bit colour (TCC); the emitted
// per-vertex fog toward FOGCOL (the reflection pass's FOGCOL is black); the alpha test (ATE/ATST/AREF, AFAIL KEEP ->
// discard); the destination-alpha test (TEST DATE/DATM against the display word's alpha MSB); the measured ALPHA blend
// (read-modify-write of the display word, the GS `((A-B)*C)>>7+D` with the alpha lane = As, COLCLAMP); FRAME.FBMSK;
// then the captured post chain (coregs3_post.glsl). An untextured item keeps the offline prefetch rule: the lit colour
// with alpha 255, no blend, no FBMSK (coregs3_render's `!textured` write).
//
// The push block is the world's (coregs3_world_push.glsl): columns, TEX0/CLAMP words, atlas rectangle/FOGCOL, TEST/
// ALPHA/FBMSK/flags + the blend equation at flags bits 8..23, and the post word.

#version 450

#extension GL_GOOGLE_include_directive : require
#include "coregs3_world_push.glsl"
#include "coregs3_post.glsl"

layout(early_fragment_tests) in;

layout(set = 0, binding = 0, r32ui) uniform uimage2D display_image;
layout(set = 0, binding = 1, rgba8) uniform readonly image2D world_atlas;

layout(location = 0) in vec4 colour;
layout(location = 1) in vec2 model_uv;
layout(location = 2) noperspective in float fog;

const uint kFlagTextured = 1u;
const uint kFlagAlphaTest = 2u;
const uint kFlagFog = 4u;
const uint kFlagAlphaFailDefaulted = 16u;
const uint kFlagLinear = 32u;
const uint kFlagBlend = 64u;   // PRIM.ABE with a measured ALPHA equation: read-modify-write the display word
const uint kFlagDate = 128u;   // TEST.DATE/DATM: the destination alpha test

uint channel(float value) {
    return uint(clamp(value, 0.0, 1.0) * 255.0 + 0.5);
}

int wrap_texel(int texel, uint log2_size, uint mode, int minimum, int maximum) {
    int last = int((1u << log2_size) - 1u);
    if (mode == 0u) return texel & last;                       // REPEAT
    if (mode == 1u) return clamp(texel, 0, last);              // CLAMP
    if (mode == 2u) return clamp(texel, minimum, maximum);     // REGION_CLAMP
    return (texel & minimum) | maximum;                        // REGION_REPEAT
}

bool fetch_texel(ivec2 local, ivec2 extent, ivec2 origin, out uvec4 texel) {
    if (local.x < 0 || local.y < 0 || local.x >= extent.x || local.y >= extent.y) return false;
    vec4 value = imageLoad(world_atlas, origin + local);
    texel = uvec4(channel(value.r), channel(value.g), channel(value.b), channel(value.a));
    return true;
}

uint fog_mix(uint c, uint fc, int f) {
    const int value = (f * int(c) + (255 - f) * int(fc)) >> 8;
    return uint(clamp(value, 0, 255));
}

// One channel of the GS ALPHA blend (coregs3_sampler.cpp's blend_fragment): ((A - B) * C) >> 7 + D, arithmetic shift,
// COLCLAMP to 0..255. `equation` packs a(0-1), b(2-3), c(4-5), d(6-7) and FIX (8-15); the selectors are 0 = source,
// 1 = destination; C is 0 = As, 1 = Ad, else FIX.
int blend_channel(uint cs, uint cd, uint as, uint ad, uint equation) {
    const uint a = equation & 3u;
    const uint b = (equation >> 2) & 3u;
    const uint c = (equation >> 4) & 3u;
    const uint d = (equation >> 6) & 3u;
    const uint fix = (equation >> 8) & 0xFFu;
    const int factor = c == 0u ? int(as) : c == 1u ? int(ad) : int(fix);
    const int av = a == 0u ? int(cs) : a == 1u ? int(cd) : 0;
    const int bv = b == 0u ? int(cs) : b == 1u ? int(cd) : 0;
    const int dv = d == 0u ? int(cs) : d == 1u ? int(cd) : 0;
    return clamp((((av - bv) * factor) >> 7) + dv, 0, 255);
}

void main() {
    uvec4 vertex = uvec4(channel(colour.r), channel(colour.g), channel(colour.b), channel(colour.a));
    uvec4 result = vertex;
    const uint flags = world.state.w;
    const bool textured = (flags & kFlagTextured) != 0u;
    bool sampled = false;
    if (textured) {
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
        const int s = int(clamp(floor(model_uv.x * float(1u << tw) * 16.0), -1.0e9, 1.0e9));
        const int t = int(clamp(floor(model_uv.y * float(1u << th) * 16.0), -1.0e9, 1.0e9));
        const uint origin_word = world.atlas.x;
        const uint rect_word = world.atlas.y;
        const uint size_word = world.atlas.z;
        const ivec2 origin = ivec2(int(origin_word & 0xFFFFu), int(origin_word >> 16));
        const ivec2 rect = ivec2(int(rect_word & 0xFFFFu), int(rect_word >> 16));
        const ivec2 extent = ivec2(int(size_word & 0xFFFFu), int(size_word >> 16));
        uvec4 texel = uvec4(0u);
        if ((flags & kFlagLinear) != 0u) {
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
        if (atst == 0u) pass = false;
        else if (atst == 1u) pass = true;
        else if (atst == 2u) pass = alpha < aref;
        else if (atst == 3u) pass = alpha <= aref;
        else if (atst == 4u) pass = alpha == aref;
        else if (atst == 5u) pass = alpha >= aref;
        else if (atst == 6u) pass = alpha > aref;
        else pass = alpha != aref;
        if (!pass) discard;
    }
    uint old_word = 0u;
    bool old_read = false;
    const ivec2 at = ivec2(gl_FragCoord.xy);
    if (textured && (flags & kFlagDate) != 0u) {
        old_word = imageLoad(display_image, at).x;
        old_read = true;
        const bool expected = ((world.state.x >> 15) & 1u) != 0u;
        const bool msb = (old_word & 0x80000000u) != 0u;
        if (msb != expected) discard;
    }
    if (textured && (flags & kFlagBlend) != 0u) {
        if (!old_read) {
            old_word = imageLoad(display_image, at).x;
            old_read = true;
        }
        const uint equation = world.state.w >> 8u;
        const uint as = result.w;
        const uint ad = (old_word >> 24) & 0xFFu;
        result.x = uint(blend_channel(result.x, old_word & 0xFFu, as, ad, equation));
        result.y = uint(blend_channel(result.y, (old_word >> 8) & 0xFFu, as, ad, equation));
        result.z = uint(blend_channel(result.z, (old_word >> 16) & 0xFFu, as, ad, equation));
        result.w = as;  // the GS writes As to the alpha lane
    }
    uint word = result.x | (result.y << 8) | (result.z << 16) | (result.w << 24);
    if (!textured) {
        // The offline prefetch write: the lit colour with an opaque alpha, no blend, no FBMSK.
        word = result.x | (result.y << 8) | (result.z << 16) | (255u << 24);
    } else {
        if (!old_read) {
            old_word = imageLoad(display_image, at).x;
            old_read = true;
        }
        // The captured post chain on this fragment's final colour (the alpha lane is not post-processed).
        const uvec4 post = post_epilogue(uvec4(word & 0xFFu, (word >> 8) & 0xFFu, (word >> 16) & 0xFFu,
                                               (word >> 24) & 0xFFu), world.post);
        word = post.x | (post.y << 8) | (post.z << 16) | (word & 0xFF000000u);
        const uint fbmsk = world.state.z;
        if (fbmsk != 0u) word = (old_word & fbmsk) | (word & ~fbmsk);
    }
    imageStore(display_image, at, uvec4(word, 0u, 0u, 0u));
}
