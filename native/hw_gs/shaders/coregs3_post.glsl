// SPDX-License-Identifier: MIT
//
// TASK-090 CoreGS 3.0 live post chain: the fragment epilogue shared by the world and the car stages. Copyright (c) 2026
// GT4Recomp contributors. Written from the captured chain (docs/COREGS3-HOOKS.md §28-29: apply_full_screen_multiply,
// apply_present_ct16) and the dither round, clean room (decision 0070).
//
// The live present draws after CoreGS 2.0's compose, so its fragments must repeat the operations the base already
// carries: the two full-screen multiplies `(Cd * FIX) >> 7` with FIX 127 and FIX 255, then the CT16 present conversion
// (the captured DTHE/DIMX dither, COLCLAMP, five bits and << 3) or, in the modern default, the modulated 8-bit colour.
// The word the CPU packs is hw_coregs3_post.hpp's `PostParameters`: post.x holds the output size, post.y the native
// size and the flags, post.zw the DIMX register. A zero native size has no defined dither cell and yields no dither;
// the caller refuses the faithful mode instead of guessing when the frame environment is missing.

uint post_multiply(uint value, uint fix) {
    return min(255u, (value * fix) >> 7u);
}

// The captured DIMX entry at this output pixel: the native 4x4 cell it falls in, signed -4..3 (gs_dither_entry's
// arithmetic with the native cell derived from the output/native ratio, so any output resolution works).
int post_dither_entry(uvec4 post) {
    const uint output_width = post.x & 0xFFFFu;
    const uint output_height = post.x >> 16u;
    const uint native_width = post.y & 0x7FFu;
    const uint native_height = (post.y >> 11u) & 0x7FFu;
    if (output_width == 0u || output_height == 0u || native_width == 0u || native_height == 0u) return 0;
    const uint nx = (uint(gl_FragCoord.x) * native_width) / output_width;
    const uint ny = (uint(gl_FragCoord.y) * native_height) / output_height;
    const uint cell = 4u * (ny & 3u) + (nx & 3u);
    const uint field = cell < 8u ? ((post.z >> (4u * cell)) & 7u) : ((post.w >> (4u * (cell - 8u))) & 7u);
    return field >= 4u ? int(field) - 8 : int(field);
}

// ct16_channel of coregs3_wet_composite.cpp: COLCLAMP (or the low 8 bits), five bits, expanded with << 3.
uint post_ct16_channel(int value, bool clamp_enabled) {
    if (clamp_enabled) value = clamp(value, 0, 255);
    value &= 0xFF;
    return (uint(value >> 3) & 0x1Fu) << 3u;
}

uvec4 post_epilogue(uvec4 colour, uvec4 post) {
    const uint flags = post.y >> 22u;
    const bool chain = (flags & 1u) != 0u;       // GT4_COREGS3_POST=1: the captured multiplies
    const bool faithful = (flags & 2u) != 0u;    // GT4_COREGS3_DITHER=1: the captured CT16 present model
    if (!chain && !faithful) return colour;      // the modern default: the modulated 8-bit colour, unchanged
    uint red = colour.x;
    uint green = colour.y;
    uint blue = colour.z;
    if (chain && (flags & 16u) != 0u) {  // FIX 127
        red = post_multiply(red, 127u);
        green = post_multiply(green, 127u);
        blue = post_multiply(blue, 127u);
    }
    if (chain && (flags & 32u) != 0u) {  // FIX 255
        red = post_multiply(red, 255u);
        green = post_multiply(green, 255u);
        blue = post_multiply(blue, 255u);
    }
    if (faithful) {  // the captured CT16 present model (dither when DTHE is on, five bits, << 3)
        const int dither = ((flags & 4u) != 0u) ? post_dither_entry(post) : 0;
        const bool clamp_enabled = (flags & 8u) != 0u;
        red = post_ct16_channel(int(red) + dither, clamp_enabled);
        green = post_ct16_channel(int(green) + dither, clamp_enabled);
        blue = post_ct16_channel(int(blue) + dither, clamp_enabled);
    }
    return uvec4(red, green, blue, colour.w);
}
