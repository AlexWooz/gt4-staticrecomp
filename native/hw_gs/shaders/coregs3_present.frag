// SPDX-License-Identifier: MIT
//
// CoreGS 3.0 live `present` fragment stage (TASK-090 M4, docs/COREGS3-HOOKS.md §10.3). Copyright (c) 2026 GT4Recomp contributors.
// Written from the display image's own format (core2_gpu.cpp: R32_UINT with the RGBA8 word core2_compose.comp writes),
// clean room (decision 0070).
//
// The pass has no colour attachment: the composed display image is a storage image of the CoreGS 2.0 device (its creation
// does not request VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT on a discrete GPU), and this stage writes the packed word with an
// imageStore, which is the "R32_UINT colour, no blending" of the design in its validation-clean form. early_fragment_tests
// makes the D32 depth test (GREATER, depth = 1/cw) happen before the store, so an occluded fragment writes nothing.
// Vertex colour only, alpha forced opaque as the CPU reference (coregs3_render) does.

#version 450

layout(early_fragment_tests) in;

layout(set = 0, binding = 0, r32ui) uniform writeonly uimage2D display_image;

layout(location = 0) in vec4 colour;

uint level(float channel) {
    return uint(clamp(channel, 0.0, 1.0) * 255.0 + 0.5);
}

void main() {
    // The display word of core2_compose.comp: red in the low byte, then green, blue, alpha.
    uint red = level(colour.r);
    uint green = level(colour.g);
    uint blue = level(colour.b);
    uint word = red | (green << 8) | (blue << 16) | (255u << 24);
    imageStore(display_image, ivec2(gl_FragCoord.xy), uvec4(word, 0u, 0u, 0u));
}
