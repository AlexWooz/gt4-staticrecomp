// SPDX-License-Identifier: MIT
//
// CoreGS 3.0 live `present` vertex stage (TASK-090 M4, docs/COREGS3-HOOKS.md §10.3). Copyright (c) 2026 GT4Recomp contributors.
// Written from the measured VU1 transform (native/coregs3/coregs3_vertex.hpp, docs/COREGS3-HOOKS.md §10.2), clean room
// (decision 0070).
//
// The push constant carries the four raw columns of the draw's 0xC1 block (VU0 vf24..vf27, each four floats, unscaled),
// exactly as the capture recorded them. The model position is in metres (the raw 16-bit model value / 4096, the scale
// coregs3_shape already applies), so the shape's `value` cancels and no scaling happens here:
//
//   clip = c0 * x + c1 * y + c2 * z + c3 * 1                       -> (cx, cy, 1, cw)
//   gl_Position = vec4(cx, -cy, 1.0, cw)
//
// The negated y is the measured screen map (screen y grows down as ndc_y decreases). With z = 1 and w = cw the hardware
// keeps only cw > 0 (the near plane) and, with depth clamp enabled in the pipeline, clamps the resulting 1/cw depth into
// [0, 1]; the fragment stage tests GEQUAL (the GS's equal-depth rule), so the nearer vertex wins exactly as coregs3_render's w-buffer does. The
// 16-bit 12.4 wrapped screen coordinate of the GS never appears in this path.

#version 450

layout(push_constant) uniform CoreGS3Transform {
    vec4 columns[4];  // c0..c3 = VU0 vf24..vf27, raw
} transform;

layout(location = 0) in vec3 model_position;  // metres
layout(location = 1) in vec4 vertex_colour;   // UNORM8x4, the model's V4-8 colour channel

layout(location = 0) out vec4 colour;

void main() {
    vec4 clip = transform.columns[0] * model_position.x + transform.columns[1] * model_position.y +
                transform.columns[2] * model_position.z + transform.columns[3];
    gl_Position = vec4(clip.x, -clip.y, 1.0, clip.w);
    colour = vertex_colour;
}
