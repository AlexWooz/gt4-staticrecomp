// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from the TASK-026 specification (docs/hw-renderer-spec), clean room per decision 0069.
// Implements chapter 12.2 (GT4_RENDERER) and chapter 12.8 (GT4_HW_* table).

#include "hw_gs_options.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace gt4::hw {
namespace {

// Splits a comma list. Empty tokens are invalid, so "a,,b" and "a," throw.
std::vector<std::string> split_list(const std::string &name, const std::string &value) {
  std::vector<std::string> tokens;
  std::string token;
  for (std::size_t i = 0; i <= value.size(); ++i) {
    if (i == value.size() || value[i] == ',') {
      if (token.empty())
        throw OptionError(name + ": empty token in list \"" + value + "\"");
      tokens.push_back(token);
      token.clear();
    } else {
      token.push_back(value[i]);
    }
  }
  return tokens;
}

[[noreturn]] void reject(const std::string &name, const std::string &value, const std::string &expected) {
  throw OptionError(name + ": invalid value \"" + value + "\" (expected " + expected + ")");
}

std::uint32_t parse_integer(const std::string &name, const std::string &value, std::uint32_t low,
                            std::uint32_t high) {
  if (value.empty() || value.size() > 9 ||
      !std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    reject(name, value, "a non-negative integer");
  }
  const auto number = static_cast<std::uint32_t>(std::stoul(value));
  if (number < low || number > high) {
    reject(name, value, std::to_string(low) + ".." + std::to_string(high));
  }
  return number;
}

bool parse_flag(const std::string &name, const std::string &value) {
  if (value == "0") return false;
  if (value == "1") return true;
  reject(name, value, "0 or 1");
}

// Maps one token of a closed set; `table` pairs token text with the enum value.
template <typename E, std::size_t N>
E parse_token(const std::string &name, const std::string &value,
              const std::pair<const char *, E> (&table)[N]) {
  std::string expected;
  for (const auto &entry : table) {
    if (value == entry.first) return entry.second;
    if (!expected.empty()) expected += "|";
    expected += entry.first;
  }
  reject(name, value, expected);
}

template <typename E, std::size_t N>
const char *token_text(E value, const std::pair<const char *, E> (&table)[N]) {
  for (const auto &entry : table) {
    if (entry.second == value) return entry.first;
  }
  return "?";
}

constexpr std::pair<const char *, HalfPixel> half_pixel_tokens[] = {
    {"native", HalfPixel::native}, {"normal", HalfPixel::normal}, {"off", HalfPixel::off}};
constexpr std::pair<const char *, NativePostfx> postfx_native_tokens[] = {
    {"off", NativePostfx::off}, {"normal", NativePostfx::normal},
    {"aggressive", NativePostfx::aggressive}};
constexpr std::pair<const char *, Blend> blend_tokens[] = {
    {"basic", Blend::basic}, {"medium", Blend::medium}, {"high", Blend::high}, {"full", Blend::full}};
constexpr std::pair<const char *, Mipmap> mipmap_tokens[] = {
    {"off", Mipmap::off}, {"basic", Mipmap::basic}, {"full", Mipmap::full}};
constexpr std::pair<const char *, Filter> filter_tokens[] = {
    {"ps2", Filter::ps2}, {"nearest", Filter::nearest}, {"forced", Filter::forced},
    {"forced_no_sprite", Filter::forced_no_sprite}};
constexpr std::pair<const char *, Dither> dither_tokens[] = {
    {"native", Dither::native}, {"fine", Dither::fine}, {"off", Dither::off}};
constexpr std::pair<const char *, AutoFlush> autoflush_tokens[] = {
    {"sprites", AutoFlush::sprites}, {"all", AutoFlush::all}, {"off", AutoFlush::off}};
constexpr std::pair<const char *, SyncMode> sync_tokens[] = {
    {"cache", SyncMode::cache}, {"writeback", SyncMode::writeback}};
constexpr std::pair<const char *, Readback> readback_tokens[] = {
    {"exact", Readback::exact}, {"full", Readback::full}, {"none", Readback::none}};
constexpr std::pair<const char *, DirtyFilter> dirty_filter_tokens[] = {
    {"auto", DirtyFilter::automatic}, {"linear", DirtyFilter::linear},
    {"nearest", DirtyFilter::nearest}};
constexpr std::pair<const char *, DumpFormat> dump_format_tokens[] = {
    {"g", DumpFormat::g}, {"p", DumpFormat::p}};
constexpr std::pair<const char *, DumpSource> dump_source_tokens[] = {
    {"vram", DumpSource::vram}, {"scanout", DumpSource::scanout}, {"upscaled", DumpSource::upscaled}};
constexpr std::pair<const char *, PresentFilter> present_filter_tokens[] = {
    {"bilinear", PresentFilter::bilinear}, {"nearest", PresentFilter::nearest},
    {"sharp", PresentFilter::sharp}};
constexpr std::pair<const char *, DumpTextures> dump_textures_tokens[] = {
    {"0", DumpTextures::off}, {"1", DumpTextures::on}, {"mips", DumpTextures::mips}};
constexpr std::pair<const char *, DeinterlaceAlgorithm> deinterlace_tokens[] = {
    {"auto", DeinterlaceAlgorithm::automatic}, {"off", DeinterlaceAlgorithm::off},
    {"weave", DeinterlaceAlgorithm::weave}, {"bob", DeinterlaceAlgorithm::bob},
    {"blend", DeinterlaceAlgorithm::blend}, {"adaptive", DeinterlaceAlgorithm::adaptive}};

constexpr std::pair<const char *, std::uint32_t> sprite_fix_tokens[] = {
    {"round", sprite_fix_round}, {"round_full", sprite_fix_round_full},
    {"align", sprite_fix_align}, {"merge", sprite_fix_merge}};
constexpr std::pair<const char *, std::uint32_t> postfx_tokens[] = {
    {"shadeboost", postfx_shadeboost}, {"fxaa", postfx_fxaa}, {"cas", postfx_cas},
    {"scanlines", postfx_scanlines}};

// Parses a comma list of closed tokens into OR-ed bits.
template <std::size_t N>
std::uint32_t parse_bit_list(const std::string &name, const std::string &value,
                             const std::pair<const char *, std::uint32_t> (&table)[N]) {
  std::uint32_t bits = 0;
  for (const auto &token : split_list(name, value)) {
    bits |= parse_token(name, token, table);
  }
  return bits;
}

template <std::size_t N>
std::string bit_list_text(std::uint32_t bits, const std::pair<const char *, std::uint32_t> (&table)[N]) {
  std::string text;
  for (const auto &entry : table) {
    if (bits & entry.second) {
      if (!text.empty()) text += ",";
      text += entry.first;
    }
  }
  return text;
}

std::string four_integers_text(const std::uint32_t values[4]) {
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%u,%u,%u,%u", values[0], values[1], values[2], values[3]);
  return buffer;
}

// Parses "a,b,c,d" with each integer in [low, high].
void parse_four(const std::string &name, const std::string &value, std::uint32_t low,
                std::uint32_t high, std::uint32_t out[4]) {
  const auto tokens = split_list(name, value);
  if (tokens.size() != 4) reject(name, value, "four comma-separated integers");
  for (std::size_t i = 0; i < 4; ++i) {
    out[i] = parse_integer(name, tokens[i], low, high);
  }
}

// "<algorithm>" or "<algorithm>_bff"; the suffix is not valid on "off".
Deinterlace parse_deinterlace(const std::string &name, const std::string &value) {
  Deinterlace result;
  std::string base = value;
  const std::string suffix = "_bff";
  if (value.size() > suffix.size() &&
      value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0) {
    base = value.substr(0, value.size() - suffix.size());
    result.bottom_field_first = true;
  }
  result.algorithm = parse_token(name, base, deinterlace_tokens);
  if (result.bottom_field_first && result.algorithm == DeinterlaceAlgorithm::off) {
    reject(name, value, "a deinterlacer name (off has no _bff form)");
  }
  return result;
}

std::string deinterlace_text(const Deinterlace &deinterlace) {
  std::string text = token_text(deinterlace.algorithm, deinterlace_tokens);
  if (deinterlace.bottom_field_first) text += "_bff";
  return text;
}

// Unset and empty both mean "use the default".
bool read_value(const EnvironmentLookup &lookup, const char *name, std::string &value) {
  const char *raw = lookup(name);
  if (!raw || !*raw) return false;
  value = raw;
  return true;
}

} // namespace

Renderer parse_renderer(const char *value) {
  if (!value || !*value || std::strcmp(value, "donor") == 0) return Renderer::donor;
  if (std::strcmp(value, "parallel") == 0) return Renderer::parallel;
  if (std::strcmp(value, "hw") == 0) return Renderer::hw;
  throw OptionError(std::string("GT4_RENDERER: unknown value \"") + value +
                    "\" (expected donor, parallel or hw)");
}

const char *renderer_name(Renderer renderer) {
  switch (renderer) {
  case Renderer::donor: return "donor";
  case Renderer::parallel: return "parallel";
  case Renderer::hw: return "hw";
  }
  return "?";
}

const std::vector<std::string> &option_names() {
  static const std::vector<std::string> names = {
      "GT4_HW_SCALE",           "GT4_HW_HALF_PIXEL",       "GT4_HW_SPRITE_FIX",
      "GT4_HW_NATIVE_POSTFX",   "GT4_HW_BLEND",            "GT4_HW_ACCURATE_ATEST",
      "GT4_HW_MIPMAP",          "GT4_HW_FILTER",           "GT4_HW_DITHER",
      "GT4_HW_AUTOFLUSH",       "GT4_HW_SYNC_MODE",        "GT4_HW_GLM",
      "GT4_HW_READBACK",        "GT4_HW_READBACK_SPIN",    "GT4_HW_DIRTY_FILTER",
      "GT4_HW_CLUT_ALWAYS_LOAD", "GT4_HW_VERIFY_SHUFFLE",  "GT4_HW_DEINTERLACE",
      "GT4_HW_ANTIBLUR",        "GT4_HW_POSTFX",           "GT4_HW_SHADEBOOST",
      "GT4_HW_CAS_SHARPNESS",   "GT4_HW_PRESENT_FILTER",   "GT4_HW_CROP",
      "GT4_HW_TEXTURES",        "GT4_HW_TEXTURES_PRECACHE", "GT4_HW_DUMP_TEXTURES",
      "GT4_HW_DUMP_FORMAT",     "GT4_HW_GAME_FIXES",       "GT4_HW_DUMP_SOURCE",
      "GT4_HW_GPU_TIMING",      "GT4_HW_GPU_DECODE",       "GT4_HW_DECODE_REUSE",
      "GT4_HW_CLUT_DEFER",      "GT4_HW_TONE_HLE",
  };
  return names;
}

Options parse_options(const EnvironmentLookup &lookup) {
  Options o;
  std::string v;

  if (read_value(lookup, "GT4_HW_SCALE", v)) o.scale = parse_integer("GT4_HW_SCALE", v, 1, 12);  // owner 2026-10-27: up to 12x (8K)
  if (read_value(lookup, "GT4_HW_HALF_PIXEL", v)) o.half_pixel = parse_token("GT4_HW_HALF_PIXEL", v, half_pixel_tokens);
  if (read_value(lookup, "GT4_HW_SPRITE_FIX", v)) o.sprite_fix = parse_bit_list("GT4_HW_SPRITE_FIX", v, sprite_fix_tokens);
  if (read_value(lookup, "GT4_HW_NATIVE_POSTFX", v)) o.native_postfx = parse_token("GT4_HW_NATIVE_POSTFX", v, postfx_native_tokens);
  if (read_value(lookup, "GT4_HW_BLEND", v)) o.blend = parse_token("GT4_HW_BLEND", v, blend_tokens);
  if (read_value(lookup, "GT4_HW_ACCURATE_ATEST", v)) o.accurate_atest = parse_flag("GT4_HW_ACCURATE_ATEST", v);
  if (read_value(lookup, "GT4_HW_MIPMAP", v)) o.mipmap = parse_token("GT4_HW_MIPMAP", v, mipmap_tokens);
  if (read_value(lookup, "GT4_HW_FILTER", v)) o.filter = parse_token("GT4_HW_FILTER", v, filter_tokens);
  if (read_value(lookup, "GT4_HW_DITHER", v)) o.dither = parse_token("GT4_HW_DITHER", v, dither_tokens);
  if (read_value(lookup, "GT4_HW_AUTOFLUSH", v)) o.autoflush = parse_token("GT4_HW_AUTOFLUSH", v, autoflush_tokens);
  if (read_value(lookup, "GT4_HW_SYNC_MODE", v)) o.sync_mode = parse_token("GT4_HW_SYNC_MODE", v, sync_tokens);
  if (read_value(lookup, "GT4_HW_GLM", v)) o.glm = parse_flag("GT4_HW_GLM", v);
  if (read_value(lookup, "GT4_HW_READBACK", v)) o.readback = parse_token("GT4_HW_READBACK", v, readback_tokens);
  if (read_value(lookup, "GT4_HW_READBACK_SPIN", v)) o.readback_spin = parse_flag("GT4_HW_READBACK_SPIN", v);
  if (read_value(lookup, "GT4_HW_DIRTY_FILTER", v)) o.dirty_filter = parse_token("GT4_HW_DIRTY_FILTER", v, dirty_filter_tokens);
  if (read_value(lookup, "GT4_HW_CLUT_ALWAYS_LOAD", v)) o.clut_always_load = parse_flag("GT4_HW_CLUT_ALWAYS_LOAD", v);
  if (read_value(lookup, "GT4_HW_VERIFY_SHUFFLE", v)) o.verify_shuffle = parse_flag("GT4_HW_VERIFY_SHUFFLE", v);
  if (read_value(lookup, "GT4_HW_DEINTERLACE", v)) o.deinterlace = parse_deinterlace("GT4_HW_DEINTERLACE", v);
  if (read_value(lookup, "GT4_HW_ANTIBLUR", v)) o.antiblur = parse_flag("GT4_HW_ANTIBLUR", v);
  if (read_value(lookup, "GT4_HW_POSTFX", v)) o.postfx = parse_bit_list("GT4_HW_POSTFX", v, postfx_tokens);
  if (read_value(lookup, "GT4_HW_SHADEBOOST", v)) parse_four("GT4_HW_SHADEBOOST", v, 0, 100, o.shadeboost);
  if (read_value(lookup, "GT4_HW_CAS_SHARPNESS", v)) o.cas_sharpness = parse_integer("GT4_HW_CAS_SHARPNESS", v, 0, 100);
  if (read_value(lookup, "GT4_HW_PRESENT_FILTER", v)) o.present_filter = parse_token("GT4_HW_PRESENT_FILTER", v, present_filter_tokens);
  if (read_value(lookup, "GT4_HW_CROP", v)) parse_four("GT4_HW_CROP", v, 0, 65535, o.crop);
  if (read_value(lookup, "GT4_HW_TEXTURES", v)) o.textures_root = v;
  if (read_value(lookup, "GT4_HW_TEXTURES_PRECACHE", v)) o.textures_precache = parse_flag("GT4_HW_TEXTURES_PRECACHE", v);
  if (read_value(lookup, "GT4_HW_DUMP_TEXTURES", v)) o.dump_textures = parse_token("GT4_HW_DUMP_TEXTURES", v, dump_textures_tokens);
  if (read_value(lookup, "GT4_HW_DUMP_FORMAT", v)) o.dump_format = parse_token("GT4_HW_DUMP_FORMAT", v, dump_format_tokens);
  if (read_value(lookup, "GT4_HW_GAME_FIXES", v)) o.game_fixes = parse_flag("GT4_HW_GAME_FIXES", v);
  if (read_value(lookup, "GT4_HW_DUMP_SOURCE", v)) o.dump_source = parse_token("GT4_HW_DUMP_SOURCE", v, dump_source_tokens);
  if (read_value(lookup, "GT4_HW_GPU_TIMING", v)) o.gpu_timing = parse_flag("GT4_HW_GPU_TIMING", v);
  if (read_value(lookup, "GT4_HW_GPU_DECODE", v)) o.gpu_decode = parse_flag("GT4_HW_GPU_DECODE", v);
  if (read_value(lookup, "GT4_HW_DECODE_REUSE", v)) o.decode_reuse = parse_flag("GT4_HW_DECODE_REUSE", v);
  if (read_value(lookup, "GT4_HW_CLUT_DEFER", v)) o.clut_defer = parse_flag("GT4_HW_CLUT_DEFER", v);
  if (read_value(lookup, "GT4_HW_TONE_HLE", v)) o.tone_hle = parse_flag("GT4_HW_TONE_HLE", v);

  return o;
}

std::vector<std::string> describe_options(const Options &o) {
  const auto flag = [](bool value) { return std::string(value ? "1" : "0"); };
  return {
      "GT4_HW_SCALE=" + std::to_string(o.scale),
      std::string("GT4_HW_HALF_PIXEL=") + token_text(o.half_pixel, half_pixel_tokens),
      "GT4_HW_SPRITE_FIX=" + bit_list_text(o.sprite_fix, sprite_fix_tokens),
      std::string("GT4_HW_NATIVE_POSTFX=") + token_text(o.native_postfx, postfx_native_tokens),
      std::string("GT4_HW_BLEND=") + token_text(o.blend, blend_tokens),
      "GT4_HW_ACCURATE_ATEST=" + flag(o.accurate_atest),
      std::string("GT4_HW_MIPMAP=") + token_text(o.mipmap, mipmap_tokens),
      std::string("GT4_HW_FILTER=") + token_text(o.filter, filter_tokens),
      std::string("GT4_HW_DITHER=") + token_text(o.dither, dither_tokens),
      std::string("GT4_HW_AUTOFLUSH=") + token_text(o.autoflush, autoflush_tokens),
      std::string("GT4_HW_SYNC_MODE=") + token_text(o.sync_mode, sync_tokens),
      "GT4_HW_GLM=" + flag(o.glm),
      std::string("GT4_HW_READBACK=") + token_text(o.readback, readback_tokens),
      "GT4_HW_READBACK_SPIN=" + flag(o.readback_spin),
      std::string("GT4_HW_DIRTY_FILTER=") + token_text(o.dirty_filter, dirty_filter_tokens),
      "GT4_HW_CLUT_ALWAYS_LOAD=" + flag(o.clut_always_load),
      "GT4_HW_VERIFY_SHUFFLE=" + flag(o.verify_shuffle),
      "GT4_HW_DEINTERLACE=" + deinterlace_text(o.deinterlace),
      "GT4_HW_ANTIBLUR=" + flag(o.antiblur),
      "GT4_HW_POSTFX=" + bit_list_text(o.postfx, postfx_tokens),
      "GT4_HW_SHADEBOOST=" + four_integers_text(o.shadeboost),
      "GT4_HW_CAS_SHARPNESS=" + std::to_string(o.cas_sharpness),
      std::string("GT4_HW_PRESENT_FILTER=") + token_text(o.present_filter, present_filter_tokens),
      "GT4_HW_CROP=" + four_integers_text(o.crop),
      "GT4_HW_TEXTURES=" + o.textures_root,
      "GT4_HW_TEXTURES_PRECACHE=" + flag(o.textures_precache),
      std::string("GT4_HW_DUMP_TEXTURES=") + token_text(o.dump_textures, dump_textures_tokens),
      std::string("GT4_HW_DUMP_FORMAT=") + token_text(o.dump_format, dump_format_tokens),
      "GT4_HW_GAME_FIXES=" + flag(o.game_fixes),
      std::string("GT4_HW_DUMP_SOURCE=") + token_text(o.dump_source, dump_source_tokens),
      "GT4_HW_GPU_TIMING=" + flag(o.gpu_timing),
      "GT4_HW_GPU_DECODE=" + flag(o.gpu_decode),
      "GT4_HW_DECODE_REUSE=" + flag(o.decode_reuse),
      "GT4_HW_CLUT_DEFER=" + flag(o.clut_defer),
      "GT4_HW_TONE_HLE=" + flag(o.tone_hle),
  };
}

std::string options_json(const Options &options) {
  std::string json = "{";
  const auto lines = describe_options(options);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto equals = lines[i].find('=');
    json += (i ? ", \"" : "\"") + lines[i].substr(0, equals) + "\": \"" + lines[i].substr(equals + 1) + "\"";
  }
  return json + "}";
}

} // namespace gt4::hw
