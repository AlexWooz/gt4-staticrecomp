// SPDX-License-Identifier: MIT
//
// CORE2 TASK-182 R1: the self copy drawn inside the open render pass. Copyright (c) 2026 GT4Recomp contributors.
// Written from docs/hw-renderer-spec/STUDY-BARRIER-SCOPE.md, row R1, clean room (decision 0070).
//
// A full-screen triangle for core2_self_copy.frag. The scissor of the draw limits it to the copied rectangle, so the
// rasteriser runs the fragment stage over exactly that rectangle's pixels and nowhere else. No vertex buffer is bound
// (the pipeline declares no vertex input); the three corners come from gl_VertexIndex.

#version 450

void main() {
    // (-1, -1), (3, -1), (-1, 3): one triangle that covers the whole viewport.
    const vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
