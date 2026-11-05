// SPDX-License-Identifier: MIT
//
// CORE2 TASK-182 R1: the self copy as a draw inside the open render pass. Copyright (c) 2026 GT4Recomp contributors.
// Written from docs/hw-renderer-spec/STUDY-BARRIER-SCOPE.md, row R1, clean room (decision 0070).
//
// Spec 8.8 copies the sampled rectangle (plus the border the copy uses) of a target a batch samples before that batch
// draws into it. The transfer (vkCmdCopyImage) needs the render pass closed; this draw does the same words from inside
// the pass and is ordered with the fragment barriers around it (draw to draw). Both images are the core 2 target
// format, r32ui GS words at the internal scale, and the copy keeps each word at its own pixel, so a word-for-word copy
// is exactly what the transfer did.

#version 450

layout(set = 0, binding = 0, r32ui) uniform readonly uimage2D copy_source;
layout(set = 0, binding = 1, r32ui) uniform writeonly uimage2D copy_destination;

void main() {
    // The draw's viewport starts at (0, 0), so a fragment's gl_FragCoord is its pixel of the image (the same convention
    // hw_pixel_uber.frag uses); the scissor has already limited the work to the copied rectangle.
    const ivec2 texel = ivec2(gl_FragCoord.xy);
    imageStore(copy_destination, texel, imageLoad(copy_source, texel));
}
