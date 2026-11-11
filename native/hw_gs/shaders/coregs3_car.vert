// SPDX-License-Identifier: MIT
//
// CoreGS 3.0 live `present` car vertex stage, textured path (TASK-090 car pass round). Copyright (c) 2026 GT4Recomp
// contributors. Written from the measured VU1 transform (native/coregs3/coregs3_vertex.hpp) and the offline car
// renderer's vertex record (coregs3_render.cpp), clean room (decision 0070).
//
// The metre transform is the same as coregs3_present.vert: c0..c2 are VU0 vf24..vf26 scaled by 4096*value (V3-16) or
// value (V3-32), c3 is vf27 unscaled; the model position is in metres.
//
//   clip = c0 * x + c1 * y + c2 * z + c3 * 1
//   gl_Position = vec4(clip.x, -clip.y, 1.0, clip.w)
//
// The textured path adds the per-vertex model UV (the shape's 10.4 coordinate, as the offline car path samples) and the
// per-vertex fog byte (reflection items; 255 = none). The push block is the world's full layout so the fragment stage
// reads the per-item state, the texture rectangle and the captured post chain; this stage only uses `columns`.

#version 450

layout(push_constant) uniform CoreGS3CarPush {
    vec4 columns[4];
    uvec4 tex_words;
    uvec4 atlas;
    uvec4 state;
    uvec4 post;
} car;

layout(location = 0) in vec3 model_position;  // metres
layout(location = 1) in vec4 vertex_colour;   // UNORM8x4, the lit V4-8 colour (shade_mesh_items)
layout(location = 2) in vec2 model_uv;        // 10.4 units (the reflection's texgen S,T are scaled to this by the CPU)
layout(location = 3) in float vertex_fog;     // 0..255, 255 = no fog

layout(location = 0) out vec4 colour;
layout(location = 1) out vec2 uv;
layout(location = 2) noperspective out float fog;

void main() {
    vec4 clip = car.columns[0] * model_position.x + car.columns[1] * model_position.y +
                car.columns[2] * model_position.z + car.columns[3];
    gl_Position = vec4(clip.x, -clip.y, 1.0, clip.w);
    colour = vertex_colour;
    uv = model_uv;
    fog = vertex_fog;
}
