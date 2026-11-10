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
//            a top-left rasteriser, a w-buffer (nearer wins), Gouraud vertex colours, perspective-correct model UVs and
//            the measured car-pass texture subset (coregs3_sampler.hpp: T4/T8/CT32 decode output, MODULATE, level 0,
//            linear/nearest, the four wrap modes, the alpha test with AFAIL KEEP, the normal/additive blends and FBMSK)
//   does not fog, texgen (the body/paint CT32 reflection ST is deferred, counted), destination alpha (DATE), culling,
//            the guard-band clip and back-face fill rules that the VU1 + GS pair applies. A picture from this tool is
//            evidence of geometry, coverage, diffuse lighting and the sampled car textures, not of final fidelity.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "coregs3_lighting.hpp"
#include "coregs3_sampler.hpp"
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
  // The per-frame texture table (coregs3_sampler.hpp, built by coregs3_texture_frame.hpp). Null keeps the pre-texture
  // picture. Items whose binding has no texture (body/paint texgen deferral, no record, no model UV, a refused decode
  // or a state outside the measured subset) keep the lit/base colour and are counted in the stats.
  const FrameTextureTable *textures = nullptr;
  // TASK-090 reflection milestone: draw the body/paint items' second pass (the environment-map reflection) from their
  // reflection bindings, sampling the CT32 map at the pinned texgen S,T and applying the tapped PRIM 0x7C state
  // (destination alpha test, additive blend, vertex fog, no depth write). False keeps the pre-reflection picture byte
  // for byte: the body items are the lit colour. A reflection item whose binding is outside the measured subset keeps
  // the lit colour and is counted.
  bool reflections = false;
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
  // The texture path (screen mode with a FrameTextureTable). An item is textured only when its binding passes the static
  // checks; every other outcome keeps the lit colour and lands in one of the counted fields, never a silent default.
  std::uint32_t items_textured = 0;
  std::uint32_t items_untextured = 0;
  std::uint32_t items_refused_tfx = 0;
  std::uint32_t items_refused_filter = 0;
  std::uint32_t items_refused_psm = 0;
  std::uint32_t items_refused_mip = 0;
  std::uint32_t items_refused_blend = 0;
  std::uint32_t items_refused_date = 0;
  std::uint32_t items_refused_alpha_fail = 0;
  std::uint64_t textured_pixels = 0;
  std::uint64_t sample_refused_pixels = 0;  // a per-pixel region refusal: the lit colour is kept and counted
  std::uint64_t alpha_test_failed = 0;
  std::uint64_t blended_pixels = 0;
  std::uint64_t additive_pixels = 0;
  std::uint64_t fbmsk_pixels = 0;
  std::uint64_t depth_masked_pixels = 0;  // ZBUF.ZMSK = 1: the fragment wrote colour but not depth
  // The reflection pass (options.reflections). A body/paint item is reflected only when its binding passes the
  // static subset (coregs3_reflection.hpp); every other outcome keeps the lit colour and lands in a counted field.
  std::uint32_t items_reflected = 0;            // body/paint items drawn as their environment-map pass
  std::uint32_t items_reflection_refused = 0;   // body/paint items with a binding outside the measured subset
  std::uint32_t items_reflection_untextured = 0;  // body/paint items with no decoded environment map
  std::uint32_t reflection_refused_psm = 0;
  std::uint32_t reflection_refused_tfx = 0;
  std::uint32_t reflection_refused_filter = 0;
  std::uint32_t reflection_refused_mip = 0;
  std::uint32_t reflection_refused_alpha_fail = 0;
  std::uint32_t reflection_refused_blend = 0;
  std::uint32_t reflection_refused_depth = 0;
  std::uint32_t reflection_refused_st = 0;  // items where a vertex had no coordinate (the whole item falls back)
  std::uint64_t reflection_pixels = 0;          // fragments the reflection pass wrote
  std::uint64_t reflection_date_failed = 0;     // destination alpha test masked the fragment (no write)
  std::uint64_t reflection_alpha_failed = 0;    // ATE/ATST masked the fragment (AFAIL KEEP, no write)
  std::uint64_t reflection_depth_failed = 0;    // ZTST GEQUAL rejected the fragment
  std::uint64_t reflection_sample_refused = 0;  // the sampler refused a coordinate (a region refusal)
  std::uint64_t reflection_alpha_missing = 0;   // DATE tested a pixel no captured draw had written (the background
                                                // alpha stands in for the frame the capture does not carry)
  std::uint64_t reflection_fog_missing = 0;     // fragments drawn without a tapped vertex fog (fog 255 = none)
  std::uint64_t reflection_blended_pixels = 0;  // additive blend applications
  std::uint64_t reflection_fbmsk_pixels = 0;
  std::uint64_t reflection_depth_masked_pixels = 0;  // ZBUF.ZMSK = 1: colour written, depth kept
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
// `textured_mask`, when not null, receives one byte per pixel: 1 where a triangle of an item with an active texture
// binding covers the pixel (the textured draws' mask the texture milestone compares in). It is geometric coverage of
// those draws, before their depth test, so both the textured and untextured pictures use the same mask.
// `reflection_mask`, when not null, receives one byte per pixel: 1 where the reflection pass wrote a fragment (after the
// destination alpha test, the depth test and the alpha test), the mask the reflection milestone compares in.
bool render_frame(const FrameSnapshot &frame, const RenderOptions &options, std::vector<std::uint8_t> &rgba,
                  std::vector<std::uint8_t> &mask, RenderStats &stats, std::string &error,
                  std::vector<std::uint8_t> *textured_mask = nullptr,
                  std::vector<std::uint8_t> *reflection_mask = nullptr);

}  // namespace gt4::coregs3
