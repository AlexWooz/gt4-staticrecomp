// TASK-090 CoreGS 3.0: the measured body/paint reflection (the environment-map texgen pass of the offline renderer).
//
// The formula is the one pinned by the round-18 texgen study (build/analysis-texgen, docs/COREGS3-HOOKS.md): the real
// AOT body fragment (microcode entry 0x10A0) was executed on captured body vertices and its two outputs at TOPS+0x40
// (the normalised S,T texgen coordinate) were reproduced in float32:
//
//   D = P - LP
//   c = (D.x*N.x + D.y*N.y) + D.z*N.z        (lane 0, summed left to right)
//   R = D - N*c - N*c                        (two msubs)
//   V = (C9*R.x + CA*R.y) + CB*R.z           (block qwords 0xC9..0xCB used as columns)
//   W = V/|V| + (0,0,1)
//   q = 0.5 / |W|                            (the EFU rsqrt's numerator is the block's vf20.x = 0.5)
//   S = 0.5 + W.x*q
//   T = 0.5 - W.y*q
//
// Inputs are the item's own six-qword VU1 light block at TOPS+0xC9 (the body/paint family's measured form, command
// 0x6C06C0C9) and the mesh vertex the fragment sees: the raw V3-16 model position (MeshVertex::raw_position, before
// position_scale) and the V3-8 normal at TOPS 0x40 divided by 127 (MeshVertex::normal). The fragment's (0.5,0.5,0.5,1.5)
// vector is confirmed and its 1.5 lane is unused, so it does not appear here.
//
// The pass state is the measured reflection subset (round 16/18): PRIM 0x7C (TME on, ABE on), TEX0 CT32 0x2BC0 with
// linear/linear filtering and a region clamp over the whole 256x256 map, ALPHA additive `(Cs-0)*Ad+Cd`, FOGCOL black
// with the vertex fog, ZBUF Z24 with ZMSK=1 (no depth write) and ZTST GEQUAL, FRAME FBMSK, COLCLAMP 1, and the
// destination alpha test (TEST.DATE/DATM) against the framebuffer alpha the base draws wrote. Every state outside this
// subset is a refusal with a name, never a guess.
#pragma once

#include <array>
#include <cstdint>

#include "coregs3_sampler.hpp"
#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"

namespace gt4::coregs3 {

// The body/paint family's light block form (rounds 12/13): six qwords uploaded by VIF UNPACK 0x6C06C0C9. Only this
// form carries the texgen matrix and the light position the fragment reads.
inline constexpr std::uint32_t kBodyLightBlockCommand = 0x6C06C0C9u;
inline constexpr std::uint32_t kBodyLightBlockQwords = 6u;

// Why a vertex has no reflection coordinate. `ok` is the only success.
enum class ReflectionStatus : std::uint32_t {
  ok = 0,
  refused_block = 1,       // the item's block is not the measured six-qword body form
  refused_position = 2,    // the vertex has no raw V3-16 position
  refused_normal = 3,      // the vertex has no V3-8 normal at TOPS 0x40
  refused_non_finite = 4,  // an input or an intermediate was not finite
  refused_degenerate = 5,  // |V| or |W| is zero, so the study's formula has no coordinate
};
const char *reflection_status_name(ReflectionStatus status) noexcept;

struct ReflectionCoordinate {
  float s = 0.0f;
  float t = 0.0f;
};

// Runs the pinned float32 formula for one vertex. The block is the item's own (captured per item), the position and
// normal are the mesh fields the VU1 fragment consumes. Returns ok with the coordinate, or a refusal status and leaves
// `out` untouched. A block that is present but a different VU1 variant is `refused_block`: the body fragment's form is
// the only one the study executed.
ReflectionStatus reflection_st(const CapturedItem &item, const MeshVertex &vertex, ReflectionCoordinate &out) noexcept;

// The GS fog mixing of the measured pass, in the hw_gs integer form (native/hw_gs/pixel/hw_gs_pixel.cpp):
//   out.rgb = (fog * c + (255 - fog) * fog_colour.rgb) >> 8,  out.a = c.a
// The reflection pass measured FOGCOL black and per-vertex fog 214..255, so the map is dimmed toward black by the
// fog fraction (255 = no fog). Fog never touches the alpha lane.
std::array<std::uint8_t, 4> apply_vertex_fog(const std::array<std::uint8_t, 4> &colour, std::uint32_t fog,
                                             std::uint32_t fog_colour) noexcept;

// The reflection pass's static state checks, run once per item before rasterising. `ok` is the only acceptance.
enum class ReflectionBindingStatus : std::uint32_t {
  ok = 0,
  no_texture = 1,          // no decoded environment map (a counted fallback, not a refusal)
  refused_psm = 2,
  refused_tfx = 3,
  refused_filter = 4,
  refused_mip = 5,
  refused_alpha_fail = 6,  // ATE on with an AFAIL other than KEEP
  refused_blend = 7,       // ABE off, or an ALPHA equation other than the measured additive one
  refused_depth = 8,       // ZTST is not GEQUAL (the measured pass)
};
const char *reflection_binding_status_name(ReflectionBindingStatus status) noexcept;
ReflectionBindingStatus check_reflection_binding(const TextureBinding &binding) noexcept;

// True when the item is the body/paint family the reflection formula was measured on. The table builder uses this to
// decide whether a body item is a reflection pass.
bool is_body_paint_item(const CapturedItem &item) noexcept;

}  // namespace gt4::coregs3
