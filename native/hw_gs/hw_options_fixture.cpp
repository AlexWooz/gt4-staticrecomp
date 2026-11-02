// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from the TASK-026 specification (docs/hw-renderer-spec), clean room per decision 0069.
// CTest hw_options: GT4_RENDERER and GT4_HW_* parsing, defaults, validation and descriptions.

#include "hw_gs_options.hpp"

#include <cstdio>
#include <map>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const char *what) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

// Environment-like lookup over a map; missing keys read as unset.
gt4::hw::EnvironmentLookup env_of(const std::map<std::string, std::string> &values) {
  return [&values](const char *name) -> const char * {
    const auto found = values.find(name);
    return found == values.end() ? nullptr : found->second.c_str();
  };
}

// True when parsing `name=value` throws OptionError.
bool rejected(const char *name, const char *value) {
  try {
    gt4::hw::parse_options(env_of({{name, value}}));
  } catch (const gt4::hw::OptionError &) {
    return true;
  }
  return false;
}

} // namespace

int main() {
  using namespace gt4::hw;

  // Renderer selection (chapter 12.2).
  expect(parse_renderer(nullptr) == Renderer::donor, "unset GT4_RENDERER selects donor");
  expect(parse_renderer("") == Renderer::donor, "empty GT4_RENDERER selects donor");
  expect(parse_renderer("donor") == Renderer::donor, "donor");
  expect(parse_renderer("parallel") == Renderer::parallel, "parallel");
  expect(parse_renderer("hw") == Renderer::hw, "hw");
  for (const char *bad : {"HW", "vulkan", "hw ", "gs", "1"}) {
    bool threw = false;
    try {
      parse_renderer(bad);
    } catch (const OptionError &) {
      threw = true;
    }
    expect(threw, "unknown GT4_RENDERER value aborts");
  }

  // Defaults (chapter 12.8 default column).
  const Options defaults = parse_options(env_of({}));
  expect(defaults.scale == 1, "default scale 1");
  expect(defaults.half_pixel == HalfPixel::native, "default half pixel native");
  expect(defaults.sprite_fix == 0, "default sprite fix empty");
  expect(defaults.blend == Blend::high, "default blend high");
  expect(!defaults.accurate_atest, "default accurate alpha test off");
  expect(defaults.mipmap == Mipmap::full, "default mipmap full");
  expect(defaults.filter == Filter::ps2, "default filter ps2");
  expect(defaults.autoflush == AutoFlush::sprites, "default autoflush sprites");
  expect(defaults.sync_mode == SyncMode::cache, "default sync cache");
  expect(defaults.glm, "default GLM on");
  expect(defaults.readback == Readback::exact, "default readback exact");
  expect(defaults.antiblur, "default antiblur on");
  expect(defaults.shadeboost[0] == 50 && defaults.shadeboost[3] == 50, "default shadeboost neutral");
  expect(defaults.crop[0] == 0 && defaults.crop[3] == 0, "default crop zero");
  expect(defaults.textures_root.empty(), "default texture replacement off");
  expect(defaults.game_fixes, "default game fixes on");
  expect(defaults.dump_source == DumpSource::vram, "default dump source vram");
  expect(!defaults.gpu_timing && !defaults.gpu_decode, "default GPU timing and decode off");
  expect(defaults.decode_reuse, "default decode reuse on");
  expect(defaults.clut_defer, "default CLUT defer on");
  expect(defaults.tone_hle, "default tone HLE on (IN-383)");

  // Empty values behave as unset.
  const Options empties = parse_options(env_of({{"GT4_HW_SCALE", ""}, {"GT4_HW_GLM", ""}}));
  expect(empties.scale == 1 && empties.glm, "empty values take the default");

  // Valid values of each kind.
  const Options chosen = parse_options(env_of({
      {"GT4_HW_SCALE", "4"},
      {"GT4_HW_HALF_PIXEL", "normal"},
      {"GT4_HW_SPRITE_FIX", "round,align,merge"},
      {"GT4_HW_NATIVE_POSTFX", "aggressive"},
      {"GT4_HW_BLEND", "full"},
      {"GT4_HW_ACCURATE_ATEST", "1"},
      {"GT4_HW_MIPMAP", "basic"},
      {"GT4_HW_FILTER", "forced"},
      {"GT4_HW_DITHER", "off"},
      {"GT4_HW_AUTOFLUSH", "all"},
      {"GT4_HW_SYNC_MODE", "writeback"},
      {"GT4_HW_GLM", "0"},
      {"GT4_HW_READBACK", "none"},
      {"GT4_HW_READBACK_SPIN", "1"},
      {"GT4_HW_DIRTY_FILTER", "nearest"},
      {"GT4_HW_CLUT_ALWAYS_LOAD", "1"},
      {"GT4_HW_VERIFY_SHUFFLE", "1"},
      {"GT4_HW_DEINTERLACE", "bob_bff"},
      {"GT4_HW_ANTIBLUR", "0"},
      {"GT4_HW_POSTFX", "shadeboost,cas"},
      {"GT4_HW_SHADEBOOST", "10,20,30,100"},
      {"GT4_HW_CAS_SHARPNESS", "0"},
      {"GT4_HW_PRESENT_FILTER", "sharp"},
      {"GT4_HW_CROP", "0,8,0,8"},
      {"GT4_HW_TEXTURES", "C:/textures"},
      {"GT4_HW_TEXTURES_PRECACHE", "1"},
      {"GT4_HW_DUMP_TEXTURES", "mips"},
      {"GT4_HW_DUMP_FORMAT", "p"},
      {"GT4_HW_GAME_FIXES", "0"},
      {"GT4_HW_DUMP_SOURCE", "scanout"},
      {"GT4_HW_GPU_TIMING", "1"},
      {"GT4_HW_DECODE_REUSE", "0"},
      {"GT4_HW_CLUT_DEFER", "0"},
      {"GT4_HW_TONE_HLE", "0"},
      {"GT4_HW_GPU_DECODE", "1"},
  }));
  expect(chosen.scale == 4, "scale 4");
  expect(chosen.half_pixel == HalfPixel::normal, "half pixel normal");
  expect(chosen.sprite_fix == (sprite_fix_round | sprite_fix_align | sprite_fix_merge), "sprite fix bits");
  expect(chosen.native_postfx == NativePostfx::aggressive, "native postfx aggressive");
  expect(chosen.blend == Blend::full && chosen.accurate_atest, "blend full and accurate atest");
  expect(chosen.mipmap == Mipmap::basic && chosen.filter == Filter::forced, "mipmap and filter");
  expect(chosen.dither == Dither::off && chosen.autoflush == AutoFlush::all, "dither and autoflush");
  expect(chosen.sync_mode == SyncMode::writeback && !chosen.glm, "sync writeback and glm off");
  expect(chosen.readback == Readback::none && chosen.readback_spin, "readback none and spin");
  expect(chosen.dirty_filter == DirtyFilter::nearest, "dirty filter nearest");
  expect(chosen.clut_always_load && chosen.verify_shuffle, "clut and shuffle flags");
  expect(chosen.deinterlace.algorithm == DeinterlaceAlgorithm::bob && chosen.deinterlace.bottom_field_first,
         "deinterlace bob with bff");
  expect(!chosen.antiblur, "antiblur off");
  expect(chosen.postfx == (postfx_shadeboost | postfx_cas), "postfx bits");
  expect(chosen.shadeboost[0] == 10 && chosen.shadeboost[3] == 100, "shadeboost values");
  expect(chosen.cas_sharpness == 0 && chosen.present_filter == PresentFilter::sharp, "cas and present filter");
  expect(chosen.crop[1] == 8 && chosen.crop[3] == 8, "crop values");
  expect(chosen.textures_root == "C:/textures" && chosen.textures_precache, "textures");
  expect(chosen.dump_textures == DumpTextures::mips && chosen.dump_format == DumpFormat::p, "dump options");
  expect(!chosen.game_fixes && chosen.dump_source == DumpSource::scanout, "fixes off, scanout");
  expect(chosen.gpu_timing && chosen.gpu_decode, "gpu timing and decode");
  expect(!chosen.decode_reuse, "decode reuse off");
  expect(!chosen.clut_defer, "CLUT defer off");
  expect(!chosen.tone_hle, "tone HLE off (IN-383)");

  // IN-372: the upscaled dump source (GPU presentation compose at GT4_HW_SCALE).
  expect(parse_options(env_of({{"GT4_HW_DUMP_SOURCE", "upscaled"}})).dump_source == DumpSource::upscaled,
         "dump source upscaled");
  expect(rejected("GT4_HW_DUMP_SOURCE", "scaled"), "unknown dump source rejected");

  // Invalid values abort (throw), one per kind.
  expect(rejected("GT4_HW_SCALE", "0"), "scale 0 rejected");
  expect(rejected("GT4_HW_SCALE", "13"), "scale 13 rejected");
  expect(rejected("GT4_HW_SCALE", "2x"), "scale with suffix rejected");
  expect(rejected("GT4_HW_SCALE", "-1"), "negative scale rejected");
  expect(rejected("GT4_HW_HALF_PIXEL", "half"), "unknown half pixel rejected");
  expect(rejected("GT4_HW_SPRITE_FIX", "round,,align"), "empty list token rejected");
  expect(rejected("GT4_HW_SPRITE_FIX", "round,bogus"), "unknown sprite fix token rejected");
  expect(rejected("GT4_HW_ACCURATE_ATEST", "yes"), "flag not 0/1 rejected");
  expect(rejected("GT4_HW_GLM", "2"), "glm 2 rejected");
  expect(rejected("GT4_HW_SHADEBOOST", "1,2,3"), "shadeboost needs four values");
  expect(rejected("GT4_HW_SHADEBOOST", "1,2,3,101"), "shadeboost above 100 rejected");
  expect(rejected("GT4_HW_CROP", "0,0,0,70000"), "crop above 65535 rejected");
  expect(rejected("GT4_HW_DEINTERLACE", "off_bff"), "off with _bff rejected");
  expect(rejected("GT4_HW_DEINTERLACE", "wave"), "unknown deinterlacer rejected");
  expect(rejected("GT4_HW_DUMP_TEXTURES", "2"), "dump textures 2 rejected");
  expect(rejected("GT4_HW_BLEND", "HIGH"), "case-sensitive blend rejected");

  // Names and descriptions (result.json, F8 header).
  expect(option_names().size() == 35, "35 runtime GT4_HW_* variables: chapter 12.8 plus GT4_HW_DECODE_REUSE (IN-369), GT4_HW_CLUT_DEFER (IN-370) and GT4_HW_TONE_HLE (IN-383); GT4_HW_GS is CMake only");
  const auto lines = describe_options(defaults);
  expect(lines.size() == option_names().size(), "one description line per option");
  for (std::size_t i = 0; i < lines.size() && i < option_names().size(); ++i) {
    expect(lines[i].rfind(option_names()[i] + "=", 0) == 0, "description order matches names");
  }
  const std::string json = options_json(chosen);
  expect(json.front() == '{' && json.back() == '}', "json object");
  expect(json.find("\"GT4_HW_SCALE\": \"4\"") != std::string::npos, "json carries scale");
  expect(json.find("\"GT4_HW_DEINTERLACE\": \"bob_bff\"") != std::string::npos, "json carries deinterlace");

  if (failures == 0) std::printf("hw_options: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
