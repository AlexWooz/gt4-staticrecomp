// TASK-090 CoreGS 3.0: the offline reference renderer for a captured car pass.
//
// This is the first picture of the native pipeline (docs/COREGS3-PLAN.md §4 step 1c): it draws the captured cars with the
// matrix the game composed, at the capture's own resolution, and nothing else. It is deliberately a CPU reference with a
// documented list of what it does not do yet, so the live Vulkan renderer (step 1e) has a ground truth to match:
//
//   does     the measured VU1 vertex transform (coregs3_vertex.hpp: the 0xC1 block as four columns, its linear part
//            scaled by the shape's `value` and by the position encoding's units), the measured per-item VU1 lighting
//            programs (coregs3_lighting.hpp: each item's own TOPS+0xC9 block and ITOP selector, selected by
//            `MeshVertex::item`), clip-space rejection and near-plane clipping of triangles, a perspective divide,
//            a top-left rasteriser, a w-buffer (nearer wins), Gouraud vertex colours and perspective-correct UVs
//   does not fog, texgen, textures, destination alpha, culling, the guard-band clip and back-face fill rules that the
//            VU1 + GS pair applies. A picture from this tool is evidence of geometry, coverage and diffuse lighting, not
//            of final fidelity.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "coregs3_lighting.hpp"
#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"
#include "coregs3_vertex.hpp"

namespace gt4::coregs3 {

struct RenderOptions {
  std::uint32_t width = 640;
  std::uint32_t height = 480;
  std::array<std::uint8_t, 4> background{24, 24, 28, 255};
  std::array<std::uint8_t, 4> fallback_color{200, 200, 200, 255};  // used when the draw has no colour channel
  bool flip_y = true;
  bool depth_test = true;
  float position_scale = 1.0f / 4096.0f;
  Topology topology = Topology::strip;
  float max_edge = 0.0f;
  // Flat-shaded diagnostic: one face normal per triangle, shaded by a fixed headlight in view space and the mean vertex
  // colour. It shows the decoded surface under a synthetic light and deliberately skips the measured VU1 lighting, so
  // it is what the M2 sheet uses and stays independent of the per-draw block.
  bool flat_shading = false;
  std::array<float, 3> light_direction{0.35f, 0.45f, -0.82f};  // view space, towards the light
  // Object-space view: ignore the draw's captured matrix and frame the captured geometry with one generated camera
  // (yaw/pitch in degrees, orthographic). It is what produces a recognisable picture of the model while the VU1 vertex
  // convention is still being derived (plan step 1d); a picture in this mode is evidence of geometry, not of placement.
  bool points = false;   // draw vertices as dots instead of triangles (a topology-independent diagnostic)
  std::uint32_t point_size = 1;  // splat radius in pixels
  bool object_space = false;
  float object_yaw = 35.0f;
  float object_pitch = 20.0f;
};

struct RenderStats {
  std::uint32_t draws = 0;
  std::uint32_t shapes_drawn = 0;
  std::uint32_t shapes_refused = 0;
  std::uint32_t vertices = 0;
  std::uint32_t triangles = 0;
  std::uint32_t triangles_drawn = 0;
  std::uint32_t triangles_clipped = 0;
  // The VU1 lighting outcome, summed over the frame. Each item is shaded with its own program and block
  // (coregs3_lighting.hpp, shade_mesh_items); the flat-shading and object-space diagnostics keep the base colours and
  // are counted as fallbacks. `lighting` carries the per-family and per-status detail; the scalar fields are the frame
  // totals the tools have printed since round 12.
  std::uint32_t draws_lit = 0;        // draws with at least one lit vertex
  std::uint32_t draws_fallback = 0;   // draws left entirely on the base colour
  std::uint32_t vertices_lit = 0;
  std::uint32_t vertices_fallback = 0;
  LightCounters lighting;
  std::uint64_t covered_pixels = 0;
  std::vector<std::string> refusals;  // one line per refused shape (deduplicated message + count in the tool)
};

// The measured position encoding of a mesh vertex: a V3-16 channel keeps its raw signed 16-bit value
// (MeshVertex::has_raw_position), a V3-32 channel stores the float as it stands (coregs3_vertex.hpp).
PositionEncoding mesh_position_encoding(const MeshVertex &vertex) noexcept;

// The transform of one draw, in the measured convention (docs/COREGS3-HOOKS.md §10.2):
//   * `row` is the row-major form of the uploaded 0xC1 matrix: rows 0-2 hold the linear columns scaled by the shape's
//     `value` (or unscaled when it is zero), row 3 the translation. The flat-shading normal rotation and the timeline
//     use it (a uniform linear scale does not change a normalised normal);
//   * `integer_positions` and `float_positions` are the column sets to multiply METRE positions with, one per measured
//     model position encoding (coregs3_vertex.hpp). The screen renderer picks per vertex, because the VU1 multiplies a
//     raw 16-bit integer for a V3-16 channel and the stored float for a V3-32 channel;
//   * `upload` is the 0xC1 block exactly as the game uploads it, for diagnostics and the packet comparison.
// `valid` is false when a column is not finite or the matrix has no usable linear part.
struct DrawMatrix {
  float row[4][4] = {};
  VertexColumns integer_positions;
  VertexColumns float_positions;
  VertexColumns upload;
  bool valid = false;
};
DrawMatrix draw_matrix(const CapturedDraw &draw);

// Renders every captured draw of the frame into an RGBA8 image (top-down rows) and a coverage mask (1 byte per pixel).
bool render_frame(const FrameSnapshot &frame, const RenderOptions &options, std::vector<std::uint8_t> &rgba,
                  std::vector<std::uint8_t> &mask, RenderStats &stats, std::string &error);

}  // namespace gt4::coregs3
