// TASK-090 CoreGS 3.0, phase S2: controls for the offline wet-road composite and the captured post multiplies.
//
// Hand-authored source texture, destination alpha and sprite pair pin the measured arithmetic: MODULATE with the
// captured vertex colour, the blend `(Cs - 0) * Ad + Cd`, the alpha lane written as As, the half-open sprite rectangle
// (no double write at a shared edge), the `Cd * FIX / 128` multiply, and the refusals for a state outside the measured
// signature.
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "coregs3_wet_composite.hpp"

namespace {

using namespace gt4::coregs3;

int failures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

GsDecodeState composite_state() {
  GsDecodeState state;
  state.prim = 0x15E;  // type 6 sprite, TME + ABE on
  state.rgba = 0x3F80000080404040ull;  // R 64, G 64, B 64, A 128, Q 1.0
  state.tex0 = 0x3140ull | (2ull << 14) | (2ull << 26) | (2ull << 30) | (1ull << 34);  // 4x4 CT32, TCC 1
  state.tex1 = (1ull << 5) | (1ull << 6);  // MMAG/MMIN linear, MXL 0
  state.clamp = 2ull | (2ull << 2) | (3ull << 14) | (3ull << 34);
  state.alpha = 0x58;  // (Cs - 0) * Ad + Cd
  state.test = 0;      // no alpha test, ZTST always
  state.zbuf = 1ull << 32;  // ZMSK 1
  state.frame = 0x00000000000A00D2ull;  // FBP 210, FBW 10, CT32, FBMSK 0
  state.xyoffset = 0;
  return state;
}

GsDecodeBatch composite_batch() {
  GsDecodeBatch batch;
  batch.vblank = 101;
  batch.sequence = 934;
  batch.path = 2;
  batch.vertices = 2;
  batch.drawing_vertices = 1;
  batch.state = composite_state();
  GsDecodeVertex latch;
  latch.x = 0.0f;
  latch.y = 0.0f;
  latch.drawing = false;
  latch.u = 0.0f;
  latch.v = 0.0f;
  latch.has_uv = true;
  GsDecodeVertex corner;
  corner.x = 2.0f;
  corner.y = 2.0f;
  corner.drawing = true;
  corner.u = 4.0f;
  corner.v = 4.0f;
  corner.has_uv = true;
  batch.verts = {latch, corner};
  batch.full_screen = false;
  return batch;
}

void test_composite() {
  std::vector<std::uint8_t> source(4 * 4 * 4, 0);
  for (int pixel = 0; pixel < 4 * 4; ++pixel) {
    source[pixel * 4 + 0] = 200;
    source[pixel * 4 + 1] = 100;
    source[pixel * 4 + 2] = 50;
    source[pixel * 4 + 3] = 64;
  }
  std::vector<std::uint8_t> rgba(2 * 2 * 4, 0);
  for (int pixel = 0; pixel < 4; ++pixel) {
    rgba[pixel * 4 + 0] = 10;
    rgba[pixel * 4 + 1] = 20;
    rgba[pixel * 4 + 2] = 30;
    rgba[pixel * 4 + 3] = 0;
  }
  rgba[1 * 4 + 3] = 128;  // (1,0)
  rgba[2 * 4 + 3] = 255;  // (0,1)
  rgba[3 * 4 + 3] = 64;   // (1,1)
  GsDecodeBatch batch = composite_batch();
  WetCompositeStats stats;
  std::string error;
  check(apply_wet_composite(batch, source.data(), 4, 4, rgba, 2, 2, 2, 2, stats, error), "the composite applies");
  if (!error.empty()) std::fprintf(stderr, "  error: %s\n", error.c_str());
  check(stats.sprites == 1 && stats.pixels == 4 && stats.sampled_pixels == 4, "one sprite covers four pixels");
  check(stats.masked_pixels == 3, "three pixels have Ad != 0");
  check(stats.changed_pixels == 3, "the Ad = 0 pixel is unchanged");
  check(stats.alpha_written == 4, "the alpha lane is written on every pixel");
  // (0,0): Ad 0 -> unchanged.
  check(rgba[0] == 10 && rgba[1] == 20 && rgba[2] == 30, "Ad = 0 keeps the destination colour");
  check(rgba[3] == 64, "the written alpha is As (64)");
  // (1,0): Ad 128 -> Cs + Cd.
  check(rgba[4] == 110 && rgba[5] == 70 && rgba[6] == 55, "Ad 128 adds the modulated texel");
  // (0,1): Ad 255 -> Cs*255/128 + Cd.
  check(rgba[8] == 209 && rgba[9] == 119 && rgba[10] == 79, "Ad 255 adds the full modulated texel");
  // (1,1): Ad 64 -> Cs*64/128 + Cd.
  check(rgba[12] == 60 && rgba[13] == 45 && rgba[14] == 42, "Ad 64 adds half the modulated texel");
  // A wrong blend is refused, never defaulted.
  GsDecodeBatch wrong = batch;
  wrong.state.alpha = 0x44;  // normal alpha blend
  std::vector<std::uint8_t> copy = rgba;
  WetCompositeStats refused_stats;
  check(!apply_wet_composite(wrong, source.data(), 4, 4, copy, 2, 2, 2, 2, refused_stats, error),
        "a wrong ALPHA equation is refused");
  check(copy == rgba, "a refused composite writes nothing");
  // A non-sprite PRIM is refused.
  GsDecodeBatch triangle = batch;
  triangle.state.prim = 0x3C;  // triangle, TME on, ABE off
  check(!apply_wet_composite(triangle, source.data(), 4, 4, copy, 2, 2, 2, 2, refused_stats, error),
        "a non-sprite PRIM is refused");
}

void test_multiply() {
  GsDecodeBatch batch;
  batch.verts.resize(2);
  batch.verts[0].drawing = false;
  batch.verts[1].drawing = true;
  batch.verts[0].x = 0.0f;
  batch.verts[0].y = 0.0f;
  batch.verts[1].x = 2.0f;
  batch.verts[1].y = 2.0f;
  batch.state.prim = 0x04E;  // sprite, ABE on, TME off
  batch.state.rgba = 0x80000000ull;  // alpha 128
  batch.state.alpha = 1u | (2u << 2) | (2u << 4) | (2u << 6) | (127ull << 32);
  std::vector<std::uint8_t> rgba(2 * 2 * 4, 0);
  for (int pixel = 0; pixel < 4; ++pixel) {
    rgba[pixel * 4 + 0] = 100;
    rgba[pixel * 4 + 1] = 50;
    rgba[pixel * 4 + 2] = 200;
    rgba[pixel * 4 + 3] = 12;
  }
  WetCompositeStats stats;
  std::string error;
  check(apply_full_screen_multiply(batch, rgba, 2, 2, 4, 2, stats, error), "the FIX 127 multiply applies");
  check(rgba[0] == 99 && rgba[1] == 49 && rgba[2] == 198, "Cd * 127 / 128 truncates like the GS");
  check(rgba[3] == 128, "the multiply writes the captured RGBA alpha");
  batch.state.alpha = 1u | (2u << 2) | (2u << 4) | (2u << 6) | (255ull << 32);
  check(apply_full_screen_multiply(batch, rgba, 2, 2, 4, 2, stats, error), "the FIX 255 multiply applies");
  check(rgba[0] == 197 && rgba[1] == 97 && rgba[2] == 255, "Cd * 255 / 128 clamps at 255");
  // The normal alpha blend is not a multiply.
  batch.state.alpha = 0x44;
  check(!apply_full_screen_multiply(batch, rgba, 2, 2, 4, 2, stats, error), "a normal blend is refused as a multiply");
}

}  // namespace

int main() {
  test_composite();
  test_multiply();
  if (failures != 0) {
    std::fprintf(stderr, "coregs3_wet_composite: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("coregs3_wet_composite: all checks passed\n");
  return 0;
}
