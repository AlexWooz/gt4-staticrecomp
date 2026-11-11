// SPDX-License-Identifier: MIT
//
// CoreGS 3.0 live world vertex stage (TASK-090). Copyright (c) 2026 GT4Recomp contributors. Written from the measured
// VU1 transform (native/coregs3/coregs3_vertex.hpp, docs/COREGS3-HOOKS.md §10.2 and §21.3), clean room (decision 0070).
//
// The world passes are drawn with the draw's captured 0xC1 upload columns and the captured model units: the course's
// position family is the STMOD 2 int32 delta chain (or a raw V3-16 halfword), which the VU1 multiplies as it stands,
// and a V3-32 channel is the stored float. The CPU uploads the position already in those units, so this stage only
// applies the columns:
//
//   clip = c0 * x + c1 * y + c2 * z + c3 * 1
//   gl_Position = vec4(clip.x, -clip.y, 1.0, clip.w)
//
// exactly as the car pass does, with depth = 1/cw and the GS's GEQUAL rule for both the world and the car pipelines.
#version 450

#extension GL_GOOGLE_include_directive : require
#include "coregs3_world_push.glsl"

layout(location = 0) in vec3 model_position;  // captured model units (int32 lane, int16, or V3-32 float)
layout(location = 1) in vec4 vertex_colour;   // UNORM8x4, the model's V4-8 colour channel
layout(location = 2) in vec2 model_uv;        // the VU1's u, v before the Q multiply
layout(location = 3) in float vertex_fog;     // the VU1's emitted fog lane (0..255)

layout(location = 0) out vec4 colour;
layout(location = 1) out vec2 uv;
// The GS fog lane is interpolated linearly in screen space (it has no Q), so the varying is noperspective; the colour
// and the UV stay perspective-correct like the GS's Gouraud colour and S/T.
layout(location = 2) noperspective out float fog;

void main() {
    vec4 clip = world.columns[0] * model_position.x + world.columns[1] * model_position.y +
                world.columns[2] * model_position.z + world.columns[3];
    gl_Position = vec4(clip.x, -clip.y, 1.0, clip.w);
    colour = vertex_colour;
    uv = model_uv;
    fog = vertex_fog;
}
