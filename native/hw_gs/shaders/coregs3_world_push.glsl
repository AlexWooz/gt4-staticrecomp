// SPDX-License-Identifier: MIT
//
// CoreGS 3.0 live world: the push-constant block shared by the world vertex and fragment stages (TASK-090). Copyright
// (c) 2026 GT4Recomp contributors. The CPU side is WorldPush in native/hw_gs/coregs3/hw_coregs3_present.cpp; both stages
// must declare the identical block, so it lives in one file.
//
//   columns    the captured 0xC1 upload columns c0..c3 (vf24..vf27 scaled by the shape's `value`). The world vertices
//              carry their captured integer/float units, so unlike the car pass there is no metre conversion.
//   tex_words  the raw TEX0 and CLAMP register words the item's state names (low, high, low, high). The item's pixels
//              are already decoded RGBA8 in the atlas; these words only carry TW/TH/TCC and the wrap rectangle.
//   atlas      origin (x | y << 16), decoded rectangle origin (x0 | y0 << 16), size (w | h << 16), FOGCOL (24-bit).
//   state      TEST, ALPHA, FRAME.FBMSK and the flags below (the alpha test and the two deferred effects are named so a
//              reader can tell an implemented state from a simplified one).

layout(push_constant) uniform CoreGS3WorldPush {
    vec4 columns[4];
    uvec4 tex_words;
    uvec4 atlas;
    uvec4 state;
} world;
