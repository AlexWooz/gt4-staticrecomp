// TASK-090 CoreGS 3.0: the `GT4_COREGS3_DITHER` switch of the modern pipeline.
//
// The PS2 scans out a CT16 frame: the rendered 8-bit channels are truncated to five bits (and the captured DTHE/DIMX
// dither is added first), then expanded back with << 3. On the captured scene that is the dotted pattern across the
// picture, and the faithful renderers keep it (GSdx, CoreGS 2.0, and CoreGS 3.0 with this switch at `1`). The owner's
// decision of 2026-11-11 is that the modern CoreGS 3.0 path defaults to no dither (`0`): the scanout conversion is a
// property of the PS2's display buffer, not of the image the native pipeline produces.
//
// What the switch covers:
//   * the offline post chain (`gt4_coregs3_render`, coregs3_wet_composite.hpp `apply_present_ct16`): `faithful`
//     reproduces the captured CT16 conversion exactly (dither when DTHE is on, five-bit truncation and << 3); `off`
//     writes the modulated 8-bit colour, with no dither and no five-bit truncation;
//   * the live present layer (native/hw_gs/coregs3/hw_coregs3_present.cpp): its fragment stage writes full 8-bit
//     RGBA8 words and never dithered in either mode, so the switch changes nothing in the pixels the CoreGS 3.0 pass
//     writes; the mode is parsed and reported there so one setting covers both paths.
// The CoreGS 2.0 composed base keeps its own CT16 dither in `GT4_COREGS3=present` until CoreGS 3.0 owns the 3D frame
// (phase 4); that dither is CoreGS 2.0's faithful output and is not changed by this switch.
//
// The value is read once from the environment; an absent or empty value means the modern default (`off`). Any other
// value than `0` or `1` is refused, never interpreted.
#pragma once

#include <cstdlib>
#include <cstring>
#include <string>

namespace gt4::coregs3 {

enum class DitherMode {
  off,       // no dither and no CT16 five-bit truncation: the modulated 8-bit colour
  faithful,  // the captured CT16 scanout model: DTHE/DIMX dither, five-bit truncation and << 3
};

inline constexpr DitherMode kDefaultDitherMode = DitherMode::off;  // owner 2026-11-11: the modern path has no dither

inline const char *dither_mode_name(DitherMode mode) noexcept {
  return mode == DitherMode::faithful ? "faithful" : "off";
}

// Parses the `GT4_COREGS3_DITHER` text. `nullptr` and the empty string select the modern default. Returns false for
// anything that is not exactly `0` or `1`; `mode` is left untouched on a refusal.
inline bool parse_dither_mode(const char *text, DitherMode &mode) noexcept {
  if (text == nullptr || *text == '\0') {
    mode = kDefaultDitherMode;
    return true;
  }
  if (std::strcmp(text, "0") == 0) {
    mode = DitherMode::off;
    return true;
  }
  if (std::strcmp(text, "1") == 0) {
    mode = DitherMode::faithful;
    return true;
  }
  return false;
}

// Reads `GT4_COREGS3_DITHER` from the environment. Never silently accepts an invalid value: `error` names it and the
// accepted set, and `mode` is left untouched.
inline bool dither_mode_from_environment(DitherMode &mode, std::string &error) {
  const char *text = std::getenv("GT4_COREGS3_DITHER");
  if (parse_dither_mode(text, mode)) return true;
  error = std::string("GT4_COREGS3_DITHER=") + text + " is not 0 or 1 (0 = no dither, the modern default; " +
          "1 = the captured CT16 dither and five-bit truncation)";
  return false;
}

}  // namespace gt4::coregs3
