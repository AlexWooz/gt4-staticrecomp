// TASK-090 CoreGS 3.0: the offline reference renderer for a captured car pass.
//
// This is the first picture of the native pipeline (docs/COREGS3-PLAN.md §4 step 1c): it draws the captured cars with the
// matrix the game composed, at the capture's own resolution, and nothing else. It is deliberately a CPU reference with a
// documented list of what it does not do yet, so the live Vulkan renderer (step 1e) has a ground truth to match:
//
//   does     the measured VU1 vertex transform (coregs3_vertex.hpp: the 0xC1 block as four unscaled columns), clip-space
//            rejection and near-plane clipping of triangles, a perspective divide, a top-left rasteriser, a w-buffer
//            (nearer wins), Gouraud vertex colours and perspective-correct UVs
//   does not lighting, fog, texgen, textures, destination alpha, culling, the guard-band clip and back-face fill rules
//            that the VU1 + GS pair applies. A picture from this tool is evidence of geometry and coverage, not of final
//            fidelity.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"
#include "coregs3_vertex.hpp"

namespace gt4::coregs3 {

struct RenderOptions {
  std::uint32_t width = 640;
  std::uint32_t height = 480;
  std::array<std::uint8_t, 4> background{24, 24, 28, 255};
  std::array<std::uint8_t, 4> fallback_color{200, 200, 200, 255};  // used when the draw has no colour channel
  bool flip_v = true;
  bool flip_y = true;
  bool depth_test = true;
  float position_scale = 1.0f / 4096.0f;
  Topology topology = Topology::strip;
  float max_edge = 0.0f;
  // Flat-shaded diagnostic: one face normal per triangle, shaded by a fixed headlight in view space and the mean vertex
  // colour. It shows the decoded surface without the VU1 lighting (not derived yet) and is what the M2 sheet uses.
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
  std::uint64_t covered_pixels = 0;
  std::vector<std::string> refusals;  // one line per refused shape (deduplicated message + count in the tool)
};

// The transform of one draw, in the measured convention (docs/COREGS3-HOOKS.md §10.2): four unscaled columns
// (`columns`, c0..c3 = VU0 vf24..vf27) plus the equivalent row-major matrix used by the flat-shading normal rotation
// and the `--ndc` diagnostic, where row[r][c] is component r of column c. `valid` is false when a column is not finite
// or the matrix has no usable linear part.
struct DrawMatrix {
  float row[4][4] = {};
  VertexColumns columns;
  bool valid = false;
};
DrawMatrix draw_matrix(const CapturedDraw &draw);

// Renders every captured draw of the frame into an RGBA8 image (top-down rows) and a coverage mask (1 byte per pixel).
bool render_frame(const FrameSnapshot &frame, const RenderOptions &options, std::vector<std::uint8_t> &rgba,
                  std::vector<std::uint8_t> &mask, RenderStats &stats, std::string &error);

}  // namespace gt4::coregs3
