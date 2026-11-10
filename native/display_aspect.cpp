#include "display_aspect.hpp"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace gt4::display_aspect {
namespace {

std::uint32_t read_le32(const std::uint8_t *bytes) {
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

void write_le32(std::uint8_t *bytes, std::uint32_t value) {
  for (int i = 0; i < 4; ++i)
    bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// A plain positive decimal: digits with at most one '.', no sign, no exponent, no spaces. Returns false otherwise.
bool parse_positive_decimal(const char *begin, const char *end, double &out) {
  if (begin == end || end - begin > 16)
    return false;
  bool dot = false, digit = false;
  for (const char *p = begin; p != end; ++p) {
    if (*p == '.') {
      if (dot)
        return false;
      dot = true;
    } else if (*p >= '0' && *p <= '9') {
      digit = true;
    } else {
      return false;
    }
  }
  if (!digit)
    return false;
  const std::string text(begin, end);
  char *stop = nullptr;
  errno = 0;
  const double value = std::strtod(text.c_str(), &stop);
  if (errno != 0 || stop != text.c_str() + text.size() || !std::isfinite(value) || value <= 0.0)
    return false;
  out = value;
  return true;
}

[[noreturn]] void refuse(const char *variable, const char *value, const char *rule) {
  throw std::invalid_argument(std::string(variable) + " '" + value + "' is not valid (" + rule + ")");
}

// Column factors: x output = elements 0, 4, 8, 12; y output = 1, 5, 9, 13.
constexpr int kXColumn[4] = {0, 4, 8, 12};
constexpr int kYColumn[4] = {1, 5, 9, 13};

}  // namespace

Setting parse_display_aspect(const char *value) {
  constexpr const char *kRule = "use auto, 16:9, 4:3, W:H or a decimal ratio between 0.5 and 8";
  if (value == nullptr || std::strcmp(value, "auto") == 0)
    return Setting{Source::automatic, kGameRatio};
  const char *end = value + std::strlen(value);
  double ratio = 0.0;
  if (const char *colon = std::strchr(value, ':')) {
    double width = 0.0, height = 0.0;
    if (!parse_positive_decimal(value, colon, width) || !parse_positive_decimal(colon + 1, end, height))
      refuse("GT4_DISPLAY_ASPECT", value, kRule);
    ratio = width / height;
  } else if (!parse_positive_decimal(value, end, ratio)) {
    refuse("GT4_DISPLAY_ASPECT", value, kRule);
  }
  if (!(ratio >= kMinRatio && ratio <= kMaxRatio))
    refuse("GT4_DISPLAY_ASPECT", value, kRule);
  return Setting{Source::fixed, snap_ratio(ratio)};
}

OutputSize parse_output_size(const char *value) {
  constexpr const char *kRule = "use WxH with each side from 320 to 16384 and a ratio between 1:2 and 8:1";
  if (value == nullptr)
    return OutputSize{};
  const char *separator = std::strchr(value, 'x');
  const char *end = value + std::strlen(value);
  auto side = [&](const char *begin, const char *stop) -> std::uint32_t {
    if (begin == stop || stop - begin > 5)
      refuse("GT4_OUTPUT_SIZE", value, kRule);
    std::uint32_t number = 0;
    for (const char *p = begin; p != stop; ++p) {
      if (*p < '0' || *p > '9')
        refuse("GT4_OUTPUT_SIZE", value, kRule);
      number = number * 10u + static_cast<std::uint32_t>(*p - '0');
    }
    if (number < kMinOutputSide || number > kMaxOutputSide)
      refuse("GT4_OUTPUT_SIZE", value, kRule);
    return number;
  };
  if (separator == nullptr)
    refuse("GT4_OUTPUT_SIZE", value, kRule);
  const OutputSize size{side(value, separator), side(separator + 1, end)};
  const double ratio = static_cast<double>(size.width) / static_cast<double>(size.height);
  if (!(ratio >= kMinRatio && ratio <= kMaxRatio))
    refuse("GT4_OUTPUT_SIZE", value, kRule);
  return size;
}

Setting resolve_setting(const Setting &aspect, const OutputSize &output) {
  if (aspect.source != Source::automatic || !output.known())
    return aspect;
  return Setting{Source::output_size,
                 snap_ratio(static_cast<double>(output.width) / static_cast<double>(output.height))};
}

double parse_fov_extra(const char *value) {
  if (value == nullptr)
    return 1.0;
  double factor = 0.0;
  if (!parse_positive_decimal(value, value + std::strlen(value), factor) || factor < kMinFovExtra ||
      factor > kMaxFovExtra)
    refuse("GT4_ASPECT_FOV_EXTRA", value, "use a decimal factor between 0.5 and 3.0; 1.0 is geometry-correct");
  return factor;
}

double snap_ratio(double ratio) {
  if (std::fabs(ratio / kGameRatio - 1.0) <= kSnapTolerance)
    return kGameRatio;
  return ratio;
}

double client_ratio(std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0)
    return kGameRatio;
  const double ratio = static_cast<double>(width) / static_cast<double>(height);
  if (ratio < kMinRatio || ratio > kMaxRatio)
    return kGameRatio;  // a degenerate window (a few pixels high) is not a display ratio
  return snap_ratio(ratio);
}

bool is_game_ratio(double ratio) { return ratio == kGameRatio; }

ProjectionScale projection_scale(double ratio, double fov_extra) {
  ProjectionScale scale;
  const double r = snap_ratio(ratio);
  if (r >= kGameRatio) {
    scale.x = static_cast<float>(kGameRatio / (r * fov_extra));
  } else {
    scale.y = static_cast<float>(r / kGameRatio);
    scale.x = static_cast<float>(1.0 / fov_extra);
  }
  return scale;
}

void scale_matrix(float matrix[16], const ProjectionScale &scale) {
  if (scale.identity())
    return;
  if (scale.x != 1.0f)
    for (const int index : kXColumn)
      matrix[index] *= scale.x;
  if (scale.y != 1.0f)
    for (const int index : kYColumn)
      matrix[index] *= scale.y;
}

bool scale_guest_matrix(std::uint8_t *ram, std::uint32_t ram_size, std::uint32_t address, const ProjectionScale &scale) {
  if (scale.identity())
    return false;
  if (static_cast<std::uint64_t>(address) + kMatrixBytes > ram_size || (address & 3u) != 0)
    return false;
  float matrix[16];
  for (int i = 0; i < 16; ++i) {
    const std::uint32_t bits = read_le32(ram + address + 4u * static_cast<std::uint32_t>(i));
    std::memcpy(&matrix[i], &bits, sizeof bits);
  }
  scale_matrix(matrix, scale);
  for (int i = 0; i < 16; ++i) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &matrix[i], sizeof bits);
    write_le32(ram + address + 4u * static_cast<std::uint32_t>(i), bits);
  }
  return true;
}

bool is_new_request(const DisplayRequest &previous, const DisplayRequest &current) {
  if (current.pending != 0 && previous.pending == 0)
    return true;
  return current.mode != previous.mode || current.row != previous.row;
}

void ScenePolicy::on_race_camera(std::uint64_t) { photo_ = false; }

void ScenePolicy::on_race_display_render(std::uint64_t vblank) {
  render_seen_ = true;
  render_vblank_ = vblank;
}

bool ScenePolicy::at_vblank(std::uint64_t vblank, const DisplayRequest &request) {
  if (have_request_ && is_new_request(last_request_, request)) {
    ++request_events_;
    photo_ = request.row == kPhotoMovieRow;
  }
  have_request_ = true;
  last_request_ = request;
  wanted_ = render_seen_ && vblank <= render_vblank_ + kRenderWindowVblanks && !photo_;
  if (wanted_ == presented_) {
    stable_ = 0;
  } else if (++stable_ >= kSettleVblanks) {
    presented_ = wanted_;
    stable_ = 0;
  }
  return presented_;
}

// --- P2: race HUD and UI ------------------------------------------------------------------------------------------

namespace {

std::uint32_t float_bits(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  return bits;
}

float bits_float(std::uint32_t bits) {
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

constexpr double kGameCanvasLeft = -320.0 / 3.0;
constexpr double kGameCanvasRight = 2240.0 / 3.0;
constexpr double kGameCanvasTop = 480.0;
constexpr double kGameCanvasBottom = 0.0;
constexpr double kCanvasTolerance = 0.05;
// Words of a setProjectionMatrix result other than m[0], m[5], m[12], m[13]: zero except m[10] = -1, m[15] = 1.
constexpr int kZeroIndices[10] = {1, 2, 3, 4, 6, 7, 8, 9, 11, 14};

}  // namespace

HudCanvas hud_canvas(double ratio) {
  const double r = snap_ratio(ratio);
  if (r == kGameRatio)
    return {kGameCanvasLeft, kGameCanvasRight, kGameCanvasTop, kGameCanvasBottom};
  if (r > kGameRatio)
    return {320.0 - 240.0 * r, 320.0 + 240.0 * r, kGameCanvasTop, kGameCanvasBottom};
  const double half_height = 240.0 * kGameRatio / r;
  return {kGameCanvasLeft, kGameCanvasRight, 240.0 + half_height, 240.0 - half_height};
}

HudMatrixWords hud_matrix_words(const HudCanvas &canvas) {
  const float left = static_cast<float>(canvas.left), right = static_cast<float>(canvas.right);
  const float top = static_cast<float>(canvas.top), bottom = static_cast<float>(canvas.bottom);
  HudMatrixWords words;
  words.m0 = float_bits(2.0f / (right - left));
  words.m5 = float_bits(2.0f / (bottom - top));
  words.m12 = float_bits((left + right) / (left - right));
  words.m13 = float_bits((top + bottom) / (top - bottom));
  return words;
}

namespace {

// Decodes a setProjectionMatrix result into its canvas edges. False when the words are not that layout.
bool decode_hud_canvas(const std::uint32_t words[16], HudCanvas &canvas) {
  for (const int index : kZeroIndices)
    if (words[index] != 0)
      return false;
  if (words[10] != float_bits(-1.0f) || words[15] != float_bits(1.0f))
    return false;
  const double m0 = bits_float(words[0]), m5 = bits_float(words[5]);
  const double m12 = bits_float(words[12]), m13 = bits_float(words[13]);
  if (!(m0 > 0.0) || !(m5 < 0.0) || !std::isfinite(m12) || !std::isfinite(m13))
    return false;
  const double width = 2.0 / m0;      // right - left
  const double height = -2.0 / m5;    // top - bottom
  const double sum_x = -m12 * width;  // left + right
  const double sum_y = m13 * height;  // top + bottom
  canvas = {(sum_x - width) / 2.0, (sum_x + width) / 2.0, (sum_y + height) / 2.0, (sum_y - height) / 2.0};
  return true;
}

bool same_edge(double a, double b) { return std::fabs(a - b) <= kCanvasTolerance; }

}  // namespace

bool is_game_hud_canvas(const std::uint32_t words[16]) {
  HudCanvas c{};
  return decode_hud_canvas(words, c) && same_edge(c.left, kGameCanvasLeft) && same_edge(c.right, kGameCanvasRight) &&
         same_edge(c.top, kGameCanvasTop) && same_edge(c.bottom, kGameCanvasBottom);
}

bool is_split_screen_hud_canvas(const std::uint32_t words[16]) {
  HudCanvas c{};
  if (!decode_hud_canvas(words, c))
    return false;
  const bool wide_half = same_edge(c.left, kGameCanvasLeft) && same_edge(c.right, 320.0) &&
                         same_edge(c.top - c.bottom, 480.0);
  const bool narrow_half = same_edge(c.left, 0.0) && same_edge(c.right, 640.0) && same_edge(c.top - c.bottom, 240.0);
  return wide_half || narrow_half;
}

bool HudCanvasHook::apply(std::uint8_t *ram, std::uint32_t ram_size, std::uint32_t self, double ratio) {
  last_split_ = false;
  if (!::gt4::target::available(::gt4::target::active().hooks.hud_matrix_offset)) {
    ::gt4::target::note_hook_unavailable("HUD canvas (display aspect)");
    return false;
  }
  const std::uint64_t address = static_cast<std::uint64_t>(self) + kHudMatrixOffset;
  if ((self & 3u) != 0 || address + kMatrixBytes > ram_size)
    return false;
  std::uint32_t words[16];
  for (std::uint32_t i = 0; i < 16; ++i)
    words[i] = read_le32(ram + address + 4u * i);
  const HudMatrixWords current{words[0], words[5], words[12], words[13]};
  last_split_ = is_split_screen_hud_canvas(words);
  Slot *slot = nullptr;
  for (Slot &candidate : slots_)
    if (candidate.used && candidate.address == static_cast<std::uint32_t>(address))
      slot = &candidate;
  const bool ours = slot != nullptr && current == slot->written && words[10] == float_bits(-1.0f) &&
                    words[15] == float_bits(1.0f);
  if (!ours) {
    if (!is_game_hud_canvas(words)) {
      ++foreign_;  // split screen, the 4:3 game canvas, or anything else: left alone
      return false;
    }
    if (slot == nullptr) {
      slot = &slots_[next_];
      next_ = (next_ + 1) % kSlots;
    }
    slot->used = true;
    slot->address = static_cast<std::uint32_t>(address);
    slot->original = current;
    slot->written = current;
  }
  const HudMatrixWords target =
      is_game_ratio(snap_ratio(ratio)) ? slot->original : hud_matrix_words(hud_canvas(ratio));
  if (target == current)
    return false;
  write_le32(ram + address + 0u, target.m0);
  write_le32(ram + address + 20u, target.m5);
  write_le32(ram + address + 48u, target.m12);
  write_le32(ram + address + 52u, target.m13);
  slot->written = target;
  ++writes_;
  return true;
}

HudParameters hud_parameters(double ratio) {
  const double r = snap_ratio(ratio);
  HudParameters block;
  if (r == kGameRatio) {
    block.words[kFontScaleOffset / 4u] = kGameFontScaleBits;
    block.words[kMirrorDivisorOffset / 4u] = kGameMirrorDivisor;
    return block;
  }
  block.words[kFontScaleOffset / 4u] = float_bits(static_cast<float>((4.0 / 3.0) / r));
  block.words[kMirrorDivisorOffset / 4u] =
      r > kGameRatio ? static_cast<std::uint32_t>(std::floor(640.0 * r / kGameRatio + 0.5)) : kGameMirrorDivisor;
  return block;
}

bool write_hud_parameters(std::uint8_t *ram, std::uint32_t ram_size, const HudParameters &parameters) {
  // The block is ours, but it only exists for the aspect profile of a target that has the HUD hook at all.
  if (!::gt4::target::available(::gt4::target::active().hooks.race_display_render))
    return false;
  if (static_cast<std::uint64_t>(kHudParameterBlock) + 4u * kHudParameterWords > ram_size)
    return false;
  bool written = false;
  for (std::uint32_t i = 0; i < kHudParameterWords; ++i) {
    std::uint8_t *word = ram + kHudParameterBlock + 4u * i;
    if (read_le32(word) != parameters.words[i]) {
      write_le32(word, parameters.words[i]);
      written = true;
    }
  }
  return written;
}

// --- TASK-095: 2D overlays over the race view -----------------------------------------------------------------------

namespace {

constexpr std::uint32_t kOverlayLeftBits = 0x00000000u;     // 0.0f
constexpr std::uint32_t kOverlayTopBits = 0x00000000u;      // 0.0f
constexpr std::uint32_t kOverlayBottomBits = 0x43F00000u;   // 480.0f
constexpr std::uint32_t kOverlayRightBits = 0x44200000u;    // 640.0f

// One axis widened by `factor` around its centre, in double, stored as f32 bits.
void widen_axis(std::uint32_t &low, std::uint32_t &high, double factor) {
  const double a = bits_float(low), b = bits_float(high);
  const double centre = (a + b) / 2.0;
  low = float_bits(static_cast<float>(centre + (a - centre) * factor));
  high = float_bits(static_cast<float>(centre + (b - centre) * factor));
}

}  // namespace

bool is_game_overlay_canvas(const OrthoCanvas &canvas) {
  return canvas.left == kOverlayLeftBits && canvas.top == kOverlayTopBits && canvas.bottom == kOverlayBottomBits &&
         canvas.right == kOverlayRightBits;
}

OrthoCanvas overlay_canvas(const OrthoCanvas &game, double ratio) {
  const double r = snap_ratio(ratio);
  OrthoCanvas canvas = game;
  if (r > kGameRatio)
    widen_axis(canvas.left, canvas.right, r / kGameRatio);
  else if (r < kGameRatio)
    widen_axis(canvas.bottom, canvas.top, kGameRatio / r);
  return canvas;
}

TextFactors overlay_text_factors(double ratio) {
  const double r = snap_ratio(ratio);
  TextFactors factors;
  if (r > kGameRatio)
    factors.x = kGameRatio / r;
  else if (r < kGameRatio)
    factors.y = r / kGameRatio;
  return factors;
}

std::uint32_t scale_text_word(std::uint32_t value, double factor) {
  if (factor == 1.0)
    return value;
  return float_bits(static_cast<float>(static_cast<double>(bits_float(value)) * factor));
}

// --- Runtime glue -------------------------------------------------------------------------------------------------

namespace {

struct Runtime {
  std::atomic<std::uint32_t> client_width{0}, client_height{0};
  // TASK-213 Phase 1d: the live output size picked on the Screen page (a display mode or a window client size). It
  // overrides the start GT4_OUTPUT_SIZE ratio for the projection/HUD without a restart.
  std::atomic<std::uint32_t> live_width{0}, live_height{0};
  std::atomic<bool> live_output{false};
  double projection_ratio = kGameRatio;
  ScenePolicy policy;
  std::uint64_t vblank = 0;
  PresentationSink sink = nullptr;
  bool presented_fill = false;
  double presented_ratio = kGameRatio;
  std::uint64_t hooks = 0, scaled = 0, renders = 0;
  std::uint32_t request_lines = 0;
  // P2: HUD canvas hook and parameter block.
  HudCanvasHook hud;
  double hud_logged_ratio = kGameRatio;
  bool block_known = false;
  HudParameters block{};
  std::uint32_t block_refills_logged = 0;
  std::uint64_t split_renders = 0;
  // TASK-095: the overlay canvas hooks, and per call site the ratio last logged and whether a foreign canvas was logged.
  struct OverlaySite {
    std::uint32_t return_label = 0;
    double logged_ratio = kGameRatio;
    bool foreign_logged = false;
  };
  static constexpr int kOverlaySites = 4;
  OverlaySite overlay_sites[kOverlaySites]{};
  std::uint64_t overlay_calls = 0, overlay_widened = 0, overlay_foreign = 0;
  // The strobe text window: open from a widened strobe overlay's ortho hook to the end of its draw.
  bool text_active = false;
  TextFactors text_factors{};
  std::uint64_t text_windows = 0, text_scaled = 0;
  OverlaySite &overlay_site(std::uint32_t return_label) {
    for (OverlaySite &site : overlay_sites)
      if (site.return_label == return_label || site.return_label == 0) {
        site.return_label = return_label;
        return site;
      }
    return overlay_sites[kOverlaySites - 1];  // only two labels exist; more would share the last slot's log state
  }
};

Runtime &runtime() {
  static Runtime state;
  return state;
}

struct Config {
  Setting setting;
  OutputSize output;
  double fov_extra = 1.0;
};

const Config &config() {
  static const Config value = [] {
    Config c;
    try {
      c.output = parse_output_size(std::getenv("GT4_OUTPUT_SIZE"));
      c.setting = resolve_setting(parse_display_aspect(std::getenv("GT4_DISPLAY_ASPECT")), c.output);
      c.fov_extra = parse_fov_extra(std::getenv("GT4_ASPECT_FOV_EXTRA"));
    } catch (const std::invalid_argument &error) {
      std::fprintf(stderr, "[aspect] %s, game not started\n", error.what());
      std::fflush(stderr);
      std::exit(1);
    }
    std::string described;
    if (c.setting.source == Source::automatic) {
      described = "auto (window client area)";
    } else if (c.setting.source == Source::output_size) {
      described = "auto (output size " + std::to_string(c.output.width) + "x" + std::to_string(c.output.height) + ", " +
                  describe_ratio(c.setting.ratio) + ")";
    } else {
      described = describe_ratio(c.setting.ratio);
    }
    std::fprintf(stderr, "[aspect] GT4_DISPLAY_ASPECT %s, FOV extra %.3f\n", described.c_str(), c.fov_extra);
    return c;
  }();
  return value;
}

}  // namespace

const Setting &configured_setting() { return config().setting; }
OutputSize configured_output_size() { return config().output; }
double configured_fov_extra() { return config().fov_extra; }

void set_window_client_size(std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0)
    return;
  Runtime &r = runtime();
  r.client_width.store(width);
  r.client_height.store(height);
}

void set_live_output_size(std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0)
    return;
  Runtime &r = runtime();
  r.live_width.store(width);
  r.live_height.store(height);
  r.live_output.store(true);
  std::fprintf(stderr, "[aspect] live output size %ux%u (ratio %s); the next race camera uses it\n", width, height,
               describe_ratio(snap_ratio(static_cast<double>(width) / static_cast<double>(height))).c_str());
}

double current_ratio() {
  const Setting &setting = configured_setting();
  if (setting.source == Source::output_size) {
    const Runtime &r = runtime();
    const std::uint32_t width = r.live_width.load();
    const std::uint32_t height = r.live_height.load();
    if (r.live_output.load() && width != 0 && height != 0)
      return snap_ratio(static_cast<double>(width) / static_cast<double>(height));
    return setting.ratio;
  }
  if (setting.source != Source::automatic)
    return setting.ratio;
  const Runtime &r = runtime();
  return client_ratio(r.client_width.load(), r.client_height.load());
}

double race_projection_ratio() { return runtime().projection_ratio; }

bool on_race_camera(std::uint8_t *ram, std::uint32_t ram_size, std::uint32_t matrix_address) {
  Runtime &r = runtime();
  const double ratio = current_ratio();
  const ProjectionScale scale = projection_scale(ratio, configured_fov_extra());
  r.projection_ratio = ratio;
  r.policy.on_race_camera(r.vblank);
  ++r.hooks;
  const bool scaled = scale_guest_matrix(ram, ram_size, matrix_address, scale);
  if (scaled)
    ++r.scaled;
  // One line per race camera init (a race start, a replay, leaving photo mode): rare.
  std::fprintf(stderr, "[aspect] race camera hook %llu at vblank %llu: R %s, matrix 0x%08X %s (x %.6f, y %.6f)\n",
               static_cast<unsigned long long>(r.hooks), static_cast<unsigned long long>(r.vblank),
               describe_ratio(ratio).c_str(), matrix_address, scaled ? "scaled" : "unchanged",
               static_cast<double>(scale.x), static_cast<double>(scale.y));
  return scaled;
}

void on_race_display_render() {
  Runtime &r = runtime();
  if (r.renders++ == 0)
    std::fprintf(stderr, "[aspect] first RaceDisplay::render at vblank %llu\n", static_cast<unsigned long long>(r.vblank));
  r.policy.on_race_display_render(r.vblank);
}

void on_race_display_render(std::uint8_t *ram, std::uint32_t ram_size, std::uint32_t self) {
  Runtime &r = runtime();
  const double ratio = r.projection_ratio;
  const std::uint64_t foreign_before = r.hud.foreign();
  const bool written = r.hud.apply(ram, ram_size, self, ratio);
  if (r.hud.last_was_split_screen()) {
    // Split screen: its own half-screen canvases, left alone, and not the race view (pillarboxed 16:9).
    if (r.split_renders++ == 0)
      std::fprintf(stderr, "[aspect] RaceDisplay 0x%08X has a split-screen HUD canvas at vblank %llu: not the race "
                   "view, the picture stays 16:9\n", self, static_cast<unsigned long long>(r.vblank));
    return;
  }
  on_race_display_render();
  if (written && ratio != r.hud_logged_ratio) {
    r.hud_logged_ratio = ratio;
    const HudCanvas canvas = hud_canvas(ratio);
    std::fprintf(stderr, "[aspect] HUD canvas of RaceDisplay 0x%08X at vblank %llu: R %s, left %.3f right %.3f top %.3f "
                 "bottom %.3f\n", self, static_cast<unsigned long long>(r.vblank), describe_ratio(ratio).c_str(),
                 canvas.left, canvas.right, canvas.top, canvas.bottom);
  }
  if (r.hud.foreign() != foreign_before && r.hud.foreign() == 1)
    std::fprintf(stderr, "[aspect] RaceDisplay 0x%08X: HUD matrix is not the full-screen 16:9 canvas (4:3 game or "
                 "unknown); left unchanged\n", self);
}

OrthoCanvas on_overlay_ortho(const OrthoCanvas &game, std::uint32_t return_label) {
  Runtime &r = runtime();
  ++r.overlay_calls;
  // The canvas follows what is on screen: widened only while the presentation stretches the frame to R.
  if (!r.presented_fill || is_game_ratio(r.presented_ratio))
    return game;
  Runtime::OverlaySite &site = r.overlay_site(return_label);
  if (!is_game_overlay_canvas(game)) {
    ++r.overlay_foreign;
    if (!site.foreign_logged) {
      site.foreign_logged = true;
      std::fprintf(stderr, "[aspect] overlay ortho call (return 0x%08X) with an unexpected canvas (left 0x%08X right 0x%08X "
                   "bottom 0x%08X top 0x%08X) at vblank %llu: left unchanged\n", return_label, game.left, game.right,
                   game.bottom, game.top, static_cast<unsigned long long>(r.vblank));
    }
    return game;
  }
  const OrthoCanvas canvas = overlay_canvas(game, r.presented_ratio);
  ++r.overlay_widened;
  // The strobe overlay's text is sized by the text style parser, not by the canvas: the same factor goes there for the draw.
  if (return_label == kOverlayOrthoReturn)
    begin_overlay_text(r.presented_ratio);
  if (r.presented_ratio != site.logged_ratio) {
    site.logged_ratio = r.presented_ratio;
    std::fprintf(stderr, "[aspect] overlay canvas (return 0x%08X) at vblank %llu: R %s, left %.3f right %.3f bottom %.3f "
                 "top %.3f\n", return_label, static_cast<unsigned long long>(r.vblank),
                 describe_ratio(r.presented_ratio).c_str(), bits_float(canvas.left), bits_float(canvas.right),
                 bits_float(canvas.bottom), bits_float(canvas.top));
  }
  return canvas;
}

void begin_overlay_text(double ratio) {
  Runtime &r = runtime();
  r.text_active = true;
  r.text_factors = overlay_text_factors(ratio);
  if (r.text_windows++ == 0)
    std::fprintf(stderr, "[aspect] overlay text scale at vblank %llu: x %.6f y %.6f while the strobe overlay is drawn\n",
                 static_cast<unsigned long long>(r.vblank), r.text_factors.x, r.text_factors.y);
}

void end_overlay_text() { runtime().text_active = false; }

bool overlay_text_active() { return runtime().text_active; }

std::uint32_t overlay_text_scale_x(std::uint32_t value) {
  Runtime &r = runtime();
  if (!r.text_active)
    return value;
  ++r.text_scaled;
  return scale_text_word(value, r.text_factors.x);
}

std::uint32_t overlay_text_scale_y(std::uint32_t value) {
  Runtime &r = runtime();
  if (!r.text_active)
    return value;
  ++r.text_scaled;
  return scale_text_word(value, r.text_factors.y);
}

void set_presentation_sink(PresentationSink sink) { runtime().sink = sink; }

void at_guest_vblank(std::uint8_t *ram, std::uint32_t ram_size) {
  Runtime &r = runtime();
  configured_setting();  // a headless run refuses an invalid value here
  ++r.vblank;
  DisplayRequest request;
  if (::gt4::target::available(kDisplayRequestState) &&
      static_cast<std::uint64_t>(kDisplayRequestState) + kRequestRowOffset + 4u <= ram_size) {
    request.pending = read_le32(ram + kDisplayRequestState + kRequestPendingOffset);
    request.mode = read_le32(ram + kDisplayRequestState + kRequestModeOffset);
    request.row = read_le32(ram + kDisplayRequestState + kRequestRowOffset);
  }
  // P2: the HUD parameter block for the ratio of the last race camera hook (the original constants before it and at
  // 16:9). Refilled whenever guest RAM differs (the boot clears low RAM before the game runs).
  const HudParameters wanted_block = hud_parameters(r.projection_ratio);
  if (write_hud_parameters(ram, ram_size, wanted_block)) {
    if (!r.block_known || !(wanted_block == r.block) || r.block_refills_logged < 4) {
      if (r.block_known && wanted_block == r.block)
        ++r.block_refills_logged;
      std::fprintf(stderr, "[aspect] HUD parameter block 0x%08X at vblank %llu: font x scale 0x%08X, mirror divisor %u\n",
                   kHudParameterBlock, static_cast<unsigned long long>(r.vblank),
                   wanted_block.words[kFontScaleOffset / 4u], wanted_block.words[kMirrorDivisorOffset / 4u]);
    }
    r.block_known = true;
    r.block = wanted_block;
  }
  const std::uint64_t events_before = r.policy.request_events();
  const bool race_view = r.policy.at_vblank(r.vblank, request);
  if (r.policy.request_events() != events_before && r.request_lines < 256) {
    ++r.request_lines;
    std::fprintf(stderr, "[aspect] display request at vblank %llu: pending %u mode %u row %u; photo/movie %s\n",
                 static_cast<unsigned long long>(r.vblank), request.pending, request.mode, request.row,
                 r.policy.photo_mode() ? "yes" : "no");
  }
  // Fill only when the race projection was built for a ratio other than 16:9: never at 16:9, so a 16:9 player never
  // sees a presentation change and a 1.2 GS process is never sent anything.
  const double ratio = r.projection_ratio;
  const bool fill = race_view && !is_game_ratio(ratio);
  if (fill == r.presented_fill && (!fill || ratio == r.presented_ratio))
    return;
  r.presented_fill = fill;
  r.presented_ratio = ratio;
  std::fprintf(stderr, "[aspect] presentation at vblank %llu: %s\n", static_cast<unsigned long long>(r.vblank),
               fill ? ("fill " + describe_ratio(ratio)).c_str() : "16:9 pillarbox");
  if (r.sink)
    r.sink(fill, ratio);
}

std::string describe_ratio(double ratio) {
  char text[64];
  if (ratio == kGameRatio)
    std::snprintf(text, sizeof text, "16:9 (%.4f)", ratio);
  else if (ratio == 4.0 / 3.0)
    std::snprintf(text, sizeof text, "4:3 (%.4f)", ratio);
  else
    std::snprintf(text, sizeof text, "%.2f:9 (%.4f)", ratio * 9.0, ratio);
  return text;
}

}  // namespace gt4::display_aspect
