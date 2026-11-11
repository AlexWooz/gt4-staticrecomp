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

void test_dither_and_ct16() {
  // The captured DIMX entry layout: sixteen nibbles, entry (row, column) at 4*(4*row + column), 3-bit signed.
  std::uint64_t dimx = 0;
  dimx |= 0x4ull << (4 * 0);   // (0,0) = 4 -> -4
  dimx |= 0x3ull << (4 * 5);   // (1,1) = 3
  dimx |= 0x7ull << (4 * 10);  // (2,2) = 7 -> -1
  check(gs_dither_entry(dimx, 0, 0, 3) == -4, "DIMX entry (0,0) = -4");
  check(gs_dither_entry(dimx, 3, 3, 3) == 3, "native dither cells: output x/y 3..5 select the (1,1) entry");
  check(gs_dither_entry(dimx, 5, 5, 3) == 3, "the output pixel is divided by the 3x scale before the 4x4 phase");
  check(gs_dither_entry(dimx, 27, 27, 3) == 3, "the dither entry is (output / 3) & 3");
  check(gs_dither_entry(dimx, 6, 6, 3) == -1, "the (2,2) entry maps to -1");
  check(gs_dither_entry(dimx, 9, 9, 3) == 0, "the (3,3) entry is the zero field");
  // CT16 conversion: truncation to five bits then << 3, with the optional signed dither and COLCLAMP.
  check(ct16_channel(8, true) == 8, "8 -> 1 << 3");
  check(ct16_channel(7, true) == 0, "7 truncates to 0");
  check(ct16_channel(247, true) == 240, "247 truncates to 30 << 3");
  check(ct16_channel(255, true) == 248, "255 truncates to 31 << 3 (the reference maximum)");
  check(ct16_channel(10 + 3, true) == 8, "a +3 dither moves 10 to 8 (13 >> 3 = 1)");
  check(ct16_channel(7 - 4, true) == 0, "a -4 dither clamps before the shift");
  check(ct16_channel(260, false) == ct16_channel(4, false), "without COLCLAMP the value keeps the low 8 bits");
}

GsDecodeState clear_state(std::uint64_t frame, std::uint64_t test) {
  GsDecodeState state;
  state.prim = 0x006;  // sprite, TME/ABE off
  state.rgba = 0x3F80000000000000ull;  // black, alpha 0
  state.frame = frame;
  state.test = test;
  state.xyoffset = 0;
  return state;
}

void test_clear() {
  GsDecodeBatch batch;
  batch.state = clear_state(0x00000000000A00D2ull, 0x30000);  // FBP 210, FBMSK 0
  batch.verts.resize(2);
  batch.verts[0].drawing = false;
  batch.verts[1].drawing = true;
  batch.verts[0].rgba = batch.verts[1].rgba = 0x00000000u;
  batch.verts[0].x = 0.0f;
  batch.verts[0].y = 0.0f;
  batch.verts[1].x = 2.0f;
  batch.verts[1].y = 2.0f;
  batch.min_x = 0.0f;
  batch.min_y = 0.0f;
  batch.max_x = 2.0f;
  batch.max_y = 2.0f;
  std::vector<std::uint8_t> rgba(2 * 2 * 4, 77);
  WetCompositeStats stats;
  std::string error;
  check(apply_full_screen_clear(batch, rgba, 2, 2, 2, 2, stats, error), "the RGB frame clear applies");
  check(stats.pixels == 4 && stats.changed_pixels == 4, "the clear changes four pixels");
  check(rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 0, "the clear writes black and alpha 0");
  // The alpha-only clear keeps RGB and zeroes alpha.
  batch.state = clear_state(0x00FFFFFF000A00D2ull, 0x30000);
  std::vector<std::uint8_t> alpha_only(2 * 2 * 4, 77);
  WetCompositeStats alpha_stats;
  check(apply_full_screen_clear(batch, alpha_only, 2, 2, 2, 2, alpha_stats, error),
        "the alpha-only frame clear applies");
  check(alpha_only[0] == 77 && alpha_only[1] == 77 && alpha_only[2] == 77 && alpha_only[3] == 0,
        "FBMSK 0x00FFFFFF keeps RGB and writes alpha 0");
  check(alpha_stats.written_pixels == 4 && alpha_stats.changed_pixels == 4, "the alpha lane write is counted");
  // A blend-carrying state is not a measured clear.
  batch.state.prim = 0x04E;  // ABE on
  check(!apply_full_screen_clear(batch, alpha_only, 2, 2, 2, 2, alpha_stats, error),
        "a blended draw is refused as a clear");
}

GsDecodeState frame_sample_state() {
  GsDecodeState state;
  state.prim = 0x15E;  // sprite, TME + ABE
  state.rgba = 0x3F80000080004D80ull;  // (128,77,0,128)
  state.tex0 = 0x0006300668029A40ull;  // TBP0 0x1A40, CT32, TW 10, TH 9, TCC 1, TFX 0
  state.tex1 = 0;
  state.clamp = 0x000006FC009FC00Aull;
  state.frame = 0xFFFFFFFF000A00D2ull;  // FBMSK 0xFFFFFFFF
  state.alpha = 0x44;                    // the normal blend (Cs - Cd) * As + Cd
  state.test = 0x3800F;
  state.zbuf = 0x0000000001000046ull;  // ZMSK 0: the draw stamps z = 1 (the frame's depth clear)
  state.xyoffset = 0;
  return state;
}

void test_frame_sample() {
  GsDecodeBatch batch;
  batch.state = frame_sample_state();
  batch.verts.resize(2);
  batch.verts[0].drawing = false;
  batch.verts[1].drawing = true;
  batch.verts[0].rgba = batch.verts[1].rgba = 0x80004D80u;
  batch.verts[0].x = 0.0f;
  batch.verts[0].y = 0.0f;
  batch.verts[1].x = 2.0f;
  batch.verts[1].y = 2.0f;
  batch.verts[0].u = 0.0f;
  batch.verts[0].v = 0.0f;
  batch.verts[1].u = 2.0f;
  batch.verts[1].v = 2.0f;
  batch.verts[0].has_uv = batch.verts[1].has_uv = true;
  batch.min_x = 0.0f;
  batch.min_y = 0.0f;
  batch.max_x = 2.0f;
  batch.max_y = 2.0f;
  std::vector<std::uint8_t> rgba(2 * 2 * 4, 0);
  for (int pixel = 0; pixel < 4; ++pixel) {
    rgba[pixel * 4 + 0] = 100;
    rgba[pixel * 4 + 1] = 100;
    rgba[pixel * 4 + 2] = 100;
    rgba[pixel * 4 + 3] = 128;
  }
  const std::vector<std::uint8_t> original = rgba;
  WetCompositeStats stats;
  std::string error;
  check(apply_frame_sample(batch, rgba, 2, 2, 2, 2, stats, error), "the frame sample applies");
  check(stats.sampled_pixels == 4 && stats.pixels == 4, "it samples the whole screen");
  check(stats.written_pixels == 0 && stats.changed_pixels == 0 && rgba == original,
        "FBMSK 0xFFFFFFFF writes no lane: the measured frame sample is a render-target read");
  // With the mask lifted (a diagnostic variant) the arithmetic is the measured MODULATE + (Cs-Cd)*Cs+Cd.
  batch.state.frame = 0x00000000000A00D2ull;
  std::vector<std::uint8_t> mixed(2 * 2 * 4, 0);
  for (int pixel = 0; pixel < 4; ++pixel) {
    mixed[pixel * 4 + 0] = 100;
    mixed[pixel * 4 + 1] = 100;
    mixed[pixel * 4 + 2] = 100;
    mixed[pixel * 4 + 3] = 128;
  }
  WetCompositeStats write_stats;
  check(apply_frame_sample(batch, mixed, 2, 2, 2, 2, write_stats, error), "the unmasked variant applies");
  check(write_stats.written_pixels == 4 && write_stats.changed_pixels == 4, "the unmasked variant writes");
  // ALPHA 0x44 is the normal blend (Cs - Cd) * As + Cd with As = 1.0 (the modulated alpha 128), so the written colour
  // is the modulated frame: (100,100,100) * (1.0, 0.602, 0.0) = (100, 60, 0).
  check(mixed[0] == 100 && mixed[1] == 60 && mixed[2] == 0,
        "the unmasked frame sample blends like the captured equation");
}

void test_present() {
  GsDecodeBatch batch;
  batch.state.prim = 0x11E;  // sprite, TME, no ABE
  batch.state.rgba = 0x3F80000080808080ull;  // (128,128,128,128), neutral
  batch.state.tex0 = 0x0007140668029A40ull;  // TBP0 0x1A40, CT32, TW 10, TH 9, TCC 1, TFX 0
  batch.state.tex1 = 0;
  batch.state.clamp = 0x000006FC009FC00Aull;
  batch.state.frame = 0x00000000020A008Cull;  // FBP 140, FBW 10, PSM 2 (CT16)
  batch.state.alpha = 0x44;
  batch.state.test = 0x3820C;
  batch.state.zbuf = 1ull << 32;
  batch.state.colclamp = 1;
  batch.state.dthe = 0;
  batch.verts.resize(2);
  batch.verts[0].drawing = false;
  batch.verts[1].drawing = true;
  batch.verts[0].rgba = batch.verts[1].rgba = 0x80808080u;
  batch.verts[0].x = 0.0f;
  batch.verts[0].y = 0.0f;
  batch.verts[1].x = 2.0f;
  batch.verts[1].y = 2.0f;
  batch.verts[0].u = 0.0f;
  batch.verts[0].v = 0.0f;
  batch.verts[1].u = 2.0f;
  batch.verts[1].v = 2.0f;
  batch.verts[0].has_uv = batch.verts[1].has_uv = true;
  batch.min_x = 0.0f;
  batch.min_y = 0.0f;
  batch.max_x = 2.0f;
  batch.max_y = 2.0f;
  std::vector<std::uint8_t> rgba(2 * 2 * 4, 0);
  const std::uint8_t values[4][3] = {{7, 0, 255}, {8, 247, 129}, {200, 100, 50}, {16, 24, 32}};
  for (int pixel = 0; pixel < 4; ++pixel) {
    rgba[pixel * 4 + 0] = values[pixel][0];
    rgba[pixel * 4 + 1] = values[pixel][1];
    rgba[pixel * 4 + 2] = values[pixel][2];
    rgba[pixel * 4 + 3] = 255;
  }
  std::vector<std::uint8_t> display;
  WetCompositeStats stats;
  std::string error;
  check(apply_present_ct16(batch, rgba, 2, 2, 2, 2, display, stats, error), "the CT16 present applies");
  check(display.size() == rgba.size(), "the present writes the display image");
  check(stats.pixels == 4 && stats.written_pixels == 4, "every screen pixel is presented");
  check(display[0] == 0 && display[1] == 0 && display[2] == 248, "7/0/255 -> 0/0/248 (five bits, then << 3)");
  check(display[4] == 8 && display[5] == 240 && display[6] == 128, "8/247/129 -> 8/240/128");
  check(display[8] == 200 && display[9] == 96 && display[10] == 48, "200/100/50 -> 200/96/48");
  check(display[12] == 16 && display[13] == 24 && display[14] == 32, "16/24/32 -> unchanged (exact five-bit values)");
  // Dither: the same image with DTHE on and a known matrix entry.
  batch.state.dthe = 1;
  batch.state.dimx = 0x4ull;  // entry (0,0) = -4, other cells 0
  std::vector<std::uint8_t> dithered;
  WetCompositeStats dither_stats;
  check(apply_present_ct16(batch, rgba, 2, 2, 2, 2, dithered, dither_stats, error), "the dithered present applies");
  check(dithered[0] == 0 && dithered[1] == 0 && dithered[2] == 248,
        "the -4 entry shifts the (0,0) native cell (7/0/255 -> 0/0/248)");
  check(dithered[4] == 8 && dithered[5] == 240 && dithered[6] == 128,
        "the other native cells keep their value");
  // A blend-carrying state is refused, never defaulted.
  batch.state.prim = 0x15E;
  std::vector<std::uint8_t> refused_display;
  check(!apply_present_ct16(batch, rgba, 2, 2, 2, 2, refused_display, stats, error),
        "a blended present is refused");
}

}  // namespace

int main() {
  test_composite();
  test_multiply();
  test_dither_and_ct16();
  test_clear();
  test_frame_sample();
  test_present();
  if (failures != 0) {
    std::fprintf(stderr, "coregs3_wet_composite: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("coregs3_wet_composite: all checks passed\n");
  return 0;
}
