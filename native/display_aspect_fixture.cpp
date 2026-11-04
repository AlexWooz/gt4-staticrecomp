// Fixture for TASK-080 P1 (native/display_aspect.hpp): GT4_DISPLAY_ASPECT and GT4_ASPECT_FOV_EXTRA parsing, the 16:9
// snap, the Hor+/Vert+ factors, the matrix scale on a synthetic matrix (host and guest little-endian words), and the
// scene policy that drives the presentation.

#include "display_aspect.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

using namespace gt4::display_aspect;

void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

bool near(double a, double b, double tolerance = 1e-6) { return std::fabs(a - b) <= tolerance; }

bool refused(const char *value) {
  try {
    parse_display_aspect(value);
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

bool fov_refused(const char *value) {
  try {
    parse_fov_extra(value);
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

void test_parse() {
  require(parse_display_aspect(nullptr).source == Source::automatic, "unset is auto");
  require(parse_display_aspect("auto").source == Source::automatic, "auto is auto");
  const Setting wide = parse_display_aspect("16:9");
  require(wide.source == Source::fixed && wide.ratio == kGameRatio, "16:9 is exactly the game ratio");
  require(parse_display_aspect("4:3").ratio == 4.0 / 3.0, "4:3");
  require(near(parse_display_aspect("21:9").ratio, 21.0 / 9.0), "21:9");
  require(near(parse_display_aspect("32:9").ratio, 32.0 / 9.0), "32:9");
  require(near(parse_display_aspect("3440:1440").ratio, 3440.0 / 1440.0), "3440:1440");
  require(near(parse_display_aspect("21.5:9").ratio, 21.5 / 9.0), "21.5:9");
  require(near(parse_display_aspect("2.39").ratio, 2.39), "decimal 2.39");
  require(near(parse_display_aspect("16:10").ratio, 1.6), "16:10");
  require(parse_display_aspect("1.7778").ratio == kGameRatio, "a near-16:9 decimal snaps to 16:9");
  require(parse_display_aspect("1366:768").ratio == kGameRatio, "1366x768 snaps to 16:9");
  const char *bad[] = {"", "Auto", "AUTO", "0:9", "16:0", "abc", "-16:9", "16:9:1", " 16:9", "16:9 ", "100:1",
                       "1:100", "1e3", "16/9", ":9", "16:", ".", "1..7", "0", "nan", "inf", "0x10:9"};
  for (const char *value : bad) {
    if (!refused(value)) {
      std::fprintf(stderr, "value '%s' was accepted\n", value);
      require(false, "an invalid GT4_DISPLAY_ASPECT is refused");
    }
  }
}

void test_fov_extra() {
  require(parse_fov_extra(nullptr) == 1.0, "unset FOV extra is 1.0");
  require(parse_fov_extra("1.0") == 1.0, "1.0");
  require(near(parse_fov_extra("1.37"), 1.37), "1.37");
  require(parse_fov_extra("0.5") == 0.5 && parse_fov_extra("3") == 3.0, "range ends");
  const char *bad[] = {"", "0", "0.49", "3.01", "x", "-1", "1,2", "1.2.3"};
  for (const char *value : bad)
    require(fov_refused(value), "an invalid GT4_ASPECT_FOV_EXTRA is refused");
}

void test_ratios() {
  require(client_ratio(0, 0) == kGameRatio && client_ratio(1920, 0) == kGameRatio, "unknown client is 16:9");
  require(client_ratio(1920, 1080) == kGameRatio, "1920x1080 is exactly 16:9");
  require(client_ratio(1280, 721) == kGameRatio, "a window one row off is 16:9");
  require(near(client_ratio(3440, 1440), 3440.0 / 1440.0), "3440x1440");
  require(near(client_ratio(3088, 1440), 3088.0 / 1440.0), "S23 3088x1440");
  require(near(client_ratio(1440, 900), 1.6), "16:10 window");
  require(client_ratio(4000, 10) == kGameRatio, "a degenerate window is not a ratio");
  require(is_game_ratio(snap_ratio(16.0 / 9.0)) && !is_game_ratio(1.6), "is_game_ratio");
  require(describe_ratio(kGameRatio).rfind("16:9", 0) == 0, "describe 16:9");
}

void test_projection_scale() {
  require(projection_scale(kGameRatio, 1.0).identity(), "16:9 with factor 1 is the identity");
  const ProjectionScale ultra = projection_scale(21.0 / 9.0, 1.0);
  require(ultra.x == static_cast<float>(16.0 / 21.0) && ultra.y == 1.0f, "21:9 Hor+: x * 16/21");
  require(projection_scale(32.0 / 9.0, 1.0).x == 0.5f, "32:9 Hor+: x * 0.5");
  const ProjectionScale tall = projection_scale(4.0 / 3.0, 1.0);
  require(tall.x == 1.0f && tall.y == 0.75f, "4:3 Vert+: y * 0.75");
  const ProjectionScale ten = projection_scale(1.6, 1.0);
  require(ten.x == 1.0f && ten.y == 0.9f, "16:10 Vert+: y * 0.9");
  const ProjectionScale extra = projection_scale(kGameRatio, 1.25);
  require(extra.x == 0.8f && extra.y == 1.0f, "16:9 with factor 1.25: x * 0.8");
  // The owner's 3440x1440 values (Width 573, Aspect 1.34) equal f = s; geometry-correct is f = 1.
  const double s = (3440.0 / 1440.0) / kGameRatio;
  const ProjectionScale owner = projection_scale(3440.0 / 1440.0, s);
  require(near(owner.x, 1.0 / (s * s), 1e-6), "owner-style factor applies s twice");
  const ProjectionScale tall_extra = projection_scale(4.0 / 3.0, 2.0);
  require(tall_extra.x == 0.5f && tall_extra.y == 0.75f, "Vert+ with a factor widens x too");
}

void fill_matrix(float m[16]) {
  // A GT4-like monitor matrix: m[0] = 2*D/W, m[5] = 2*D/H, off-axis m[8], m[9], m[10] = m[15] = 1.
  const float values[16] = {1.299f, 0.0f, 0.0f, 0.0f, 0.0f, 2.300f, 0.0f, 0.0f,
                            0.125f, -0.25f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  std::memcpy(m, values, sizeof values);
}

void test_scale_matrix() {
  float m[16], original[16];
  fill_matrix(m);
  fill_matrix(original);
  scale_matrix(m, projection_scale(kGameRatio, 1.0));
  require(std::memcmp(m, original, sizeof m) == 0, "identity leaves the matrix bit-identical");
  scale_matrix(m, projection_scale(32.0 / 9.0, 1.0));
  require(m[0] == original[0] * 0.5f && m[8] == original[8] * 0.5f, "Hor+ scales the x column");
  require(m[5] == original[5] && m[9] == original[9] && m[10] == 1.0f && m[15] == 1.0f, "Hor+ leaves the rest");
  require(std::signbit(m[4]) == std::signbit(original[4]) && m[12] == 0.0f, "zero entries stay zero");
  fill_matrix(m);
  scale_matrix(m, projection_scale(4.0 / 3.0, 1.0));
  require(m[5] == original[5] * 0.75f && m[9] == original[9] * 0.75f && m[0] == original[0], "Vert+ scales y");
  // tan(half hFOV) / tan(half vFOV) = m[5] / m[0]: the picture ratio follows R.
  fill_matrix(m);
  const double before = static_cast<double>(m[5]) / m[0];
  scale_matrix(m, projection_scale(21.0 / 9.0, 1.0));
  require(near(static_cast<double>(m[5]) / m[0], before * (21.0 / 16.0), 1e-5), "frustum ratio grows by R/(16/9)");
}

void put_le32(std::vector<std::uint8_t> &ram, std::uint32_t address, std::uint32_t value) {
  for (int i = 0; i < 4; ++i)
    ram[address + static_cast<std::uint32_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::uint32_t get_le32(const std::vector<std::uint8_t> &ram, std::uint32_t address) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i)
    value |= static_cast<std::uint32_t>(ram[address + static_cast<std::uint32_t>(i)]) << (8 * i);
  return value;
}

void test_guest_matrix() {
  constexpr std::uint32_t ram_size = 1u << 20;
  constexpr std::uint32_t stack = 0x000FFF00u;
  std::vector<std::uint8_t> ram(ram_size, 0xCD);
  float m[16];
  fill_matrix(m);
  for (std::uint32_t i = 0; i < 16; ++i) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &m[i], 4);
    put_le32(ram, stack + 4 * i, bits);
  }
  const std::vector<std::uint8_t> before = ram;
  require(!scale_guest_matrix(ram.data(), ram_size, stack, projection_scale(kGameRatio, 1.0)), "16:9 writes nothing");
  require(ram == before, "16:9 leaves guest RAM byte-identical");
  require(scale_guest_matrix(ram.data(), ram_size, stack, projection_scale(21.0 / 9.0, 1.0)), "21:9 writes");
  float scaled0 = 0.0f;
  const std::uint32_t bits0 = get_le32(ram, stack);
  std::memcpy(&scaled0, &bits0, 4);
  require(scaled0 == m[0] * static_cast<float>(16.0 / 21.0), "guest m[0] scaled");
  require(get_le32(ram, stack + 20) == get_le32(before, stack + 20), "guest m[5] unchanged");
  for (std::uint32_t at = 0; at < ram_size; ++at)
    if (at < stack || at >= stack + 64)
      require(ram[at] == before[at], "nothing outside the 64 matrix bytes is written");
  require(!scale_guest_matrix(ram.data(), ram_size, ram_size - 32, projection_scale(2.4, 1.0)), "outside RAM refused");
  require(!scale_guest_matrix(ram.data(), ram_size, stack + 2, projection_scale(2.4, 1.0)), "unaligned refused");
}

void test_scene_policy() {
  const DisplayRequest menu{0, 1, 1}, menu_pending{1, 1, 1}, race480p{1, 2, 6}, photo{1, 1, 2};
  require(!is_new_request(menu, menu), "same sample is not a request");
  require(is_new_request(menu, menu_pending), "pending rising edge is a request");
  require(!is_new_request(menu_pending, menu), "pending falling edge is not a request");
  require(is_new_request(menu, race480p) && is_new_request(race480p, photo), "row or mode change is a request");

  ScenePolicy policy;
  std::uint64_t v = 0;
  for (; v < 100; ++v)
    require(!policy.at_vblank(v, menu), "menus (no RaceDisplay::render) are not the race view");
  policy.on_race_camera(v);  // the camera init alone (pre-race, loading) is not the race view
  for (int i = 0; i < 10; ++i)
    require(!policy.at_vblank(v++, menu), "no HUD yet: pillarbox");
  // The HUD renders every frame: race view after the settle VBlanks.
  policy.on_race_display_render(v);
  require(!policy.at_vblank(v++, menu), "hysteresis 1");
  policy.on_race_display_render(v);
  require(!policy.at_vblank(v++, menu), "hysteresis 2");
  policy.on_race_display_render(v);
  require(policy.at_vblank(v++, menu), "race view after kSettleVblanks");
  // A few frames without the HUD (a hitch) stay inside the render window.
  for (int i = 0; i < 5; ++i)
    require(policy.at_vblank(v++, menu), "short HUD gap keeps the race view");
  // Photo pause: a request for row 2 ends the race view even if render still runs.
  for (std::uint32_t i = 0; i < ScenePolicy::kSettleVblanks; ++i) {
    policy.on_race_display_render(v);
    policy.at_vblank(v++, photo);
  }
  require(policy.photo_mode() && !policy.race_view_presented(), "photo mode is pillarboxed");
  // Leaving photo mode re-runs initMonitor (camera hook): race view again while the HUD renders.
  policy.on_race_camera(v);
  for (std::uint32_t i = 0; i < ScenePolicy::kSettleVblanks; ++i) {
    policy.on_race_display_render(v);
    policy.at_vblank(v++, photo);
  }
  require(!policy.photo_mode() && policy.race_view_presented(), "after photo mode the race view returns");
  // Race over: the HUD stops; after the render window and the settle time the menus are pillarboxed.
  for (std::uint32_t i = 0; i < ScenePolicy::kRenderWindowVblanks + ScenePolicy::kSettleVblanks + 1; ++i)
    policy.at_vblank(v++, menu);
  require(!policy.race_view_presented(), "menus after the race are pillarboxed");
  // A movie request (row 2) with no HUD stays pillarboxed; a later 480p race request clears photo mode.
  policy.at_vblank(v++, photo);
  require(policy.photo_mode(), "movie row marks photo/movie");
  policy.at_vblank(v++, race480p);
  require(!policy.photo_mode(), "another request clears it");
  // A single HUD frame lasts at most the render window plus the settle time.
  ScenePolicy brief;
  brief.on_race_display_render(0);
  for (std::uint64_t t = 0; t < 40; ++t) {
    const bool shown = brief.at_vblank(t, menu);
    require(!shown || t < ScenePolicy::kRenderWindowVblanks + ScenePolicy::kSettleVblanks + 1, "one HUD frame");
  }
  require(!brief.race_view_presented(), "a single HUD frame is gone after the window");
}

// --- P2 (slice217) --------------------------------------------------------------------------------------------------

std::uint32_t bits_of(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, 4);
  return bits;
}

float float_of(std::uint32_t bits) {
  float value = 0.0f;
  std::memcpy(&value, &bits, 4);
  return value;
}

// A RaceDisplay HUD matrix as RaceDisplay::setProjectionMatrix (0x0039BB38) writes it, at self + 0x70.
void put_game_hud_matrix(std::vector<std::uint8_t> &ram, std::uint32_t self, float left, float right, float top,
                         float bottom) {
  const std::uint32_t at = self + kHudMatrixOffset;
  for (std::uint32_t i = 0; i < 16; ++i)
    put_le32(ram, at + 4 * i, 0);
  put_le32(ram, at + 0, bits_of(2.0f / (right - left)));
  put_le32(ram, at + 20, bits_of(2.0f / (bottom - top)));
  put_le32(ram, at + 40, bits_of(-1.0f));
  put_le32(ram, at + 48, bits_of((left + right) / (left - right)));
  put_le32(ram, at + 52, bits_of((top + bottom) / (top - bottom)));
  put_le32(ram, at + 60, bits_of(1.0f));
}

// Maps a canvas x (or y) through the HUD matrix to the frame's normalised device coordinate.
double ndc_x(const std::vector<std::uint8_t> &ram, std::uint32_t self, double x) {
  const std::uint32_t at = self + kHudMatrixOffset;
  return x * float_of(get_le32(ram, at + 0)) + float_of(get_le32(ram, at + 48));
}

double ndc_y(const std::vector<std::uint8_t> &ram, std::uint32_t self, double y) {
  const std::uint32_t at = self + kHudMatrixOffset;
  return y * float_of(get_le32(ram, at + 20)) + float_of(get_le32(ram, at + 52));
}

void test_hud_canvas() {
  const HudCanvas game = hud_canvas(kGameRatio);
  require(near(game.left, -320.0 / 3.0) && near(game.right, 2240.0 / 3.0) && game.top == 480.0 && game.bottom == 0.0,
          "16:9 canvas = the game's wide canvas");
  const HudCanvas ultra = hud_canvas(21.0 / 9.0);
  require(near(ultra.left, 320.0 - 560.0) && near(ultra.right, 320.0 + 560.0) && ultra.top == 480.0,
          "21:9 canvas = 320 -/+ 240 R");
  const HudCanvas super = hud_canvas(32.0 / 9.0);
  require(near(super.right - super.left, 480.0 * 32.0 / 9.0) && near(super.left + super.right, 640.0),
          "32:9 canvas twice as wide, centred on 320");
  const HudCanvas tall = hud_canvas(1.6);
  require(near(tall.left, -320.0 / 3.0) && near(tall.top - tall.bottom, 480.0 * kGameRatio / 1.6) &&
              near(tall.top + tall.bottom, 480.0),
          "16:10 (Vert+) canvas taller, centred on 240");

  constexpr std::uint32_t ram_size = 1u << 20;
  constexpr std::uint32_t self = 0x00080000u, split = 0x00090000u, narrow = 0x000A0000u;
  std::vector<std::uint8_t> ram(ram_size, 0xCD);
  put_game_hud_matrix(ram, self, -106.666664f, 746.666687f, 480.0f, 0.0f);
  put_game_hud_matrix(ram, split, -106.666664f, 320.0f, 480.0f, 0.0f);  // RaceSplitDisplayBase, wide
  put_game_hud_matrix(ram, narrow, 0.0f, 640.0f, 480.0f, 0.0f);          // the 4:3 game canvas
  std::uint32_t words[16];
  for (std::uint32_t i = 0; i < 16; ++i)
    words[i] = get_le32(ram, self + kHudMatrixOffset + 4 * i);
  require(is_game_hud_canvas(words), "the game's 16:9 HUD matrix is recognised");
  for (std::uint32_t i = 0; i < 16; ++i)
    words[i] = get_le32(ram, split + kHudMatrixOffset + 4 * i);
  require(!is_game_hud_canvas(words), "a split-screen canvas is not");
  require(is_split_screen_hud_canvas(words), "the wide split-screen canvas is recognised as split screen");
  for (std::uint32_t i = 0; i < 16; ++i)
    words[i] = get_le32(ram, self + kHudMatrixOffset + 4 * i);
  require(!is_split_screen_hud_canvas(words), "the full-screen canvas is not split screen");
  {
    std::vector<std::uint8_t> half(ram_size, 0);
    put_game_hud_matrix(half, self, 0.0f, 640.0f, 230.0f, -10.0f);  // 4:3 split screen, top player
    for (std::uint32_t i = 0; i < 16; ++i)
      words[i] = get_le32(half, self + kHudMatrixOffset + 4 * i);
    require(is_split_screen_hud_canvas(words) && !is_game_hud_canvas(words), "the 4:3 split-screen canvas");
  }
  const std::vector<std::uint8_t> original = ram;

  HudCanvasHook hook;
  require(!hook.apply(ram.data(), ram_size, self, kGameRatio) && ram == original, "16:9 writes nothing");
  require(!hook.apply(ram.data(), ram_size, split, 21.0 / 9.0), "split screen left alone");
  require(hook.last_was_split_screen(), "split screen reported (not the race view)");
  require(!hook.apply(ram.data(), ram_size, narrow, 21.0 / 9.0), "4:3 game canvas left alone");
  require(!hook.last_was_split_screen(), "the 4:3 full-screen canvas is not split screen");
  require(ram == original && hook.foreign() == 2, "foreign canvases untouched and counted");

  // 21:9: the 16:9 HUD keeps its shape once the frame is stretched by s = R / (16/9), and stays centred.
  const double s = (21.0 / 9.0) / kGameRatio;
  require(hook.apply(ram.data(), ram_size, self, 21.0 / 9.0), "21:9 writes the canvas");
  const std::uint32_t m = self + kHudMatrixOffset;
  for (std::uint32_t at = 0; at < ram_size; ++at) {
    const std::uint32_t index = (at - m) / 4;
    const bool hud_word = at >= m && at < m + 64 && (index == 0 || index == 5 || index == 12 || index == 13);
    if (!hud_word)
      require(ram[at] == original[at], "only m[0], m[5], m[12], m[13] of that object change");
  }
  require(std::fabs(ndc_x(ram, self, 320.0)) < 1e-6, "the HUD centre stays at the frame centre");
  const double width_ndc = ndc_x(ram, self, 746.666687) - ndc_x(ram, self, -106.666664);
  require(near(width_ndc * s, 2.0, 1e-5), "the 16:9 canvas spans 1/s of the frame (full 16:9 width after the stretch)");
  require(near(ndc_y(ram, self, 480.0), -1.0, 1e-6) && near(ndc_y(ram, self, 0.0), 1.0, 1e-6),
          "the vertical mapping is unchanged");
  require(!hook.apply(ram.data(), ram_size, self, 21.0 / 9.0), "same ratio again: no write");
  // A ratio change follows (auto mode, window resized) and 16:9 restores the game's words exactly.
  require(hook.apply(ram.data(), ram_size, self, 32.0 / 9.0), "32:9 rewrites");
  require(near((ndc_x(ram, self, 746.666687) - ndc_x(ram, self, -106.666664)) * 2.0, 2.0, 1e-5), "32:9: half width");
  require(hook.apply(ram.data(), ram_size, self, kGameRatio) && ram == original, "16:9 restores the exact words");
  // 16:10 (Vert+): the canvas is taller; the horizontal mapping stays.
  require(hook.apply(ram.data(), ram_size, self, 1.6), "16:10 writes");
  require(near(ndc_x(ram, self, -106.666664), -1.0, 1e-5) && std::fabs(ndc_y(ram, self, 240.0)) < 1e-6,
          "16:10: full width, vertical centre kept");
  require(near((ndc_y(ram, self, 0.0) - ndc_y(ram, self, 480.0)) * (kGameRatio / 1.6), 2.0, 1e-5),
          "16:10: the 16:9 canvas spans R / (16/9) of the frame height");
  // A new RaceDisplay at the same address (fresh 16:9 words from initialize) is recognised again.
  put_game_hud_matrix(ram, self, -106.666664f, 746.666687f, 480.0f, 0.0f);
  require(hook.apply(ram.data(), ram_size, self, 21.0 / 9.0), "a fresh object at a reused address is rewritten");
  require(!hook.apply(ram.data(), ram_size, ram_size - 16, 21.0 / 9.0), "outside RAM refused");
  require(!hook.apply(ram.data(), ram_size, self + 2, 21.0 / 9.0), "unaligned refused");
}

void test_hud_parameters() {
  const HudParameters game = hud_parameters(kGameRatio);
  require(game.words[kFontScaleOffset / 4] == 0x3F400000u, "16:9 font x scale = the original 0.75");
  require(game.words[kMirrorDivisorOffset / 4] == 640u, "16:9 mirror divisor = the original 640");
  require(hud_parameters(1366.0 / 768.0) == game, "a near-16:9 ratio snaps to the original block");
  const HudParameters ultra = hud_parameters(21.0 / 9.0);
  require(float_of(ultra.words[0]) == static_cast<float>((4.0 / 3.0) / (21.0 / 9.0)), "21:9 font x scale (4/3)/R");
  require(ultra.words[1] == 840u, "21:9 mirror divisor 640 x 21/16 = 840");
  const HudParameters super = hud_parameters(32.0 / 9.0);
  require(float_of(super.words[0]) == 0.375f && super.words[1] == 1280u, "32:9: 0.375 and 1280");
  const HudParameters custom = hud_parameters(2.44);
  require(custom.words[1] == 878u, "2.44: divisor round(640 x 2.44 / (16/9)) = 878");
  const HudParameters tall = hud_parameters(1.6);
  require(float_of(tall.words[0]) == static_cast<float>((4.0 / 3.0) / 1.6) && tall.words[1] == 640u,
          "16:10: font (4/3)/R, mirror unchanged");

  constexpr std::uint32_t ram_size = 1u << 16;
  std::vector<std::uint8_t> ram(ram_size, 0);
  require(write_hud_parameters(ram.data(), ram_size, game), "the first fill writes");
  require(get_le32(ram, kHudParameterBlock) == 0x3F400000u && get_le32(ram, kHudParameterBlock + 4) == 640u,
          "block at 0x7F00");
  require(!write_hud_parameters(ram.data(), ram_size, game), "unchanged block: no write");
  for (std::uint32_t at = 0; at < ram_size; ++at)
    if (at < kHudParameterBlock || at >= kHudParameterBlock + 4 * kHudParameterWords)
      require(ram[at] == 0, "nothing outside the block is written");
  require(kHudParameterBlock + 4 * kHudParameterWords <= 0x8000u, "the block is reachable as offset(zero)");
}

}  // namespace

bool output_refused(const char *value) {
  try {
    parse_output_size(value);
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

// TASK-080 addendum: GT4_OUTPUT_SIZE and auto from the output size.
void test_output_size() {
  require(!parse_output_size(nullptr).known(), "unset output size is not known");
  const OutputSize full_hd = parse_output_size("1920x1080");
  require(full_hd.width == 1920 && full_hd.height == 1080, "1920x1080");
  const OutputSize wide = parse_output_size("3440x1440");
  require(wide.width == 3440 && wide.height == 1440, "3440x1440");
  require(parse_output_size("320x320").known() && parse_output_size("16384x8192").known(), "side limits");
  const char *bad[] = {"", "1920", "1920x", "x1080", "1920X1080", "1920x1080x1", " 1920x1080", "1920x1080 ", "-1920x1080",
                       "319x240", "16385x8192", "100000x1080", "1920.0x1080", "0x0", "5000x320", "320x5000", "auto"};
  for (const char *value : bad)
    require(output_refused(value), value);

  const Setting automatic = parse_display_aspect(nullptr);
  const Setting from_full_hd = resolve_setting(automatic, full_hd);
  require(from_full_hd.source == Source::output_size && from_full_hd.ratio == kGameRatio,
          "1920x1080 is exactly 16:9: nothing changes");
  require(resolve_setting(automatic, parse_output_size("1366x768")).ratio == kGameRatio, "1366x768 snaps to 16:9");
  const Setting from_wide = resolve_setting(automatic, wide);
  require(from_wide.source == Source::output_size && near(from_wide.ratio, 3440.0 / 1440.0), "3440x1440 ratio");
  require(near(resolve_setting(automatic, parse_output_size("2560x1080")).ratio, 2560.0 / 1080.0), "2560x1080 ratio");
  require(near(resolve_setting(automatic, parse_output_size("1920x1200")).ratio, 1.6), "16:10 ratio");
  require(resolve_setting(automatic, OutputSize{}).source == Source::automatic, "no output size: window client area");
  const Setting fixed = parse_display_aspect("4:3");
  require(resolve_setting(fixed, wide).source == Source::fixed && resolve_setting(fixed, wide).ratio == 4.0 / 3.0,
          "a fixed GT4_DISPLAY_ASPECT wins over the output size");
  // The ultrawide path: Hor+ at 3440x1440, untouched at 1920x1080.
  require(projection_scale(from_full_hd.ratio, 1.0).identity(), "16:9 output: identity projection");
  require(!projection_scale(from_wide.ratio, 1.0).identity(), "3440x1440 output: Hor+ projection");
}

// TASK-095: the 2D overlay canvas of the menu context and the strobe renderer (the ortho call's f12..f15), and the strobe text.
OrthoCanvas canvas_of(float left, float right, float bottom, float top) {
  return {bits_of(left), bits_of(right), bits_of(bottom), bits_of(top)};
}

// x of a canvas point in normalised device coordinates, the way the ortho function (0x004A5A90) builds its matrix in single
// precision: m[0] = 2 / (right - left), m[12] = (left + right) / (left - right).
double ortho_ndc_x(const OrthoCanvas &canvas, float x) {
  const float left = float_of(canvas.left), right = float_of(canvas.right);
  return static_cast<double>(x * (2.0f / (right - left)) + (left + right) / (left - right));
}

double ortho_ndc_y(const OrthoCanvas &canvas, float y) {
  const float bottom = float_of(canvas.bottom), top = float_of(canvas.top);
  return static_cast<double>(y * (2.0f / (top - bottom)) + (top + bottom) / (bottom - top));
}

void test_overlay_canvas() {
  const OrthoCanvas game = canvas_of(0.0f, 640.0f, 480.0f, 0.0f);  // the menu context and the strobe renderer
  require(is_game_overlay_canvas(game), "the game's overlay canvas is recognised");
  require(!is_game_overlay_canvas(canvas_of(-106.666664f, 746.666687f, 480.0f, 0.0f)), "the HUD canvas is not an overlay");
  require(!is_game_overlay_canvas(canvas_of(0.0f, 640.0f, 240.0f, 0.0f)), "a half-height canvas is not");
  require(!is_game_overlay_canvas(canvas_of(-320.0f, 320.0f, -224.0f, 224.0f)), "the centred frame canvas is not");
  require(!is_game_overlay_canvas(OrthoCanvas{0u, 0x44555554u, bits_of(480.0f), 0u}), "the race monitor's own canvas is not");

  // 16:9, and a ratio that snaps to it (1366 x 768), give the same bits back.
  require(overlay_canvas(game, kGameRatio) == game, "16:9 keeps the canvas");
  require(overlay_canvas(game, 1366.0 / 768.0) == game, "a ratio that snaps to 16:9 keeps the canvas");

  // 21:9: 0..640 becomes 640 x 21/16 = 840 wide around 320; the height stays.
  const OrthoCanvas ultra = overlay_canvas(game, 21.0 / 9.0);
  require(ultra.left == bits_of(-100.0f) && ultra.right == bits_of(740.0f), "21:9 canvas = -100 .. 740");
  require(ultra.bottom == game.bottom && ultra.top == game.top, "21:9 keeps the height");
  const double s = (21.0 / 9.0) / kGameRatio;
  require(near(ortho_ndc_x(ultra, 320.0f), 0.0, 1e-6), "21:9: the centre 320 stays in the middle of the frame");
  require(near((ortho_ndc_x(ultra, 640.0f) - ortho_ndc_x(ultra, 0.0f)) * s, 2.0, 1e-5),
          "21:9: 0..640 covers 1/s of the frame, stretched back to the full 16:9 width by the presentation");
  require(near(ortho_ndc_y(ultra, 0.0f), 1.0, 1e-6) && near(ortho_ndc_y(ultra, 480.0f), -1.0, 1e-6), "21:9: y unchanged");

  // 32:9: twice as wide, centred.
  const OrthoCanvas super = overlay_canvas(game, 32.0 / 9.0);
  require(super.left == bits_of(-320.0f) && super.right == bits_of(960.0f), "32:9 canvas = -320 .. 960");
  require(near(ortho_ndc_x(super, 640.0f) - ortho_ndc_x(super, 0.0f), 1.0, 1e-6), "32:9: 0..640 covers half the frame");

  // R < 16:9 (Vert+): x stays, y is taller around 240.
  const OrthoCanvas tall = overlay_canvas(game, 1.6);
  require(tall.left == game.left && tall.right == game.right, "16:10 keeps x");
  require(near(float_of(tall.bottom) - float_of(tall.top), 480.0 * kGameRatio / 1.6, 1e-3) &&
              near(float_of(tall.bottom) + float_of(tall.top), 480.0, 1e-3),
          "16:10 canvas taller, centred on 240");
  const OrthoCanvas four_three = overlay_canvas(game, 4.0 / 3.0);
  require(four_three.bottom == bits_of(560.0f) && four_three.top == bits_of(-80.0f), "4:3 canvas = 560 .. -80");

  // The strobe text: the inverse of the canvas factor on the same axis, 1 at 16:9.
  const TextFactors flat = overlay_text_factors(kGameRatio);
  require(flat.x == 1.0 && flat.y == 1.0 && overlay_text_factors(1366.0 / 768.0).x == 1.0, "16:9 text factors = 1 and 1");
  const TextFactors text_ultra = overlay_text_factors(21.0 / 9.0);
  require(near(text_ultra.x, 16.0 / 21.0) && text_ultra.y == 1.0, "21:9 text x factor = 16/21");
  require(near(text_ultra.x * s, 1.0, 1e-12), "21:9: the narrowed text is stretched back to its own width");
  require(overlay_text_factors(32.0 / 9.0).x == 0.5, "32:9 text x factor = 1/2");
  const TextFactors text_tall = overlay_text_factors(4.0 / 3.0);
  require(text_tall.x == 1.0 && near(text_tall.y, 0.75), "4:3 text y factor = 3/4");
  require(scale_text_word(bits_of(0.8f), 1.0) == bits_of(0.8f), "a factor of 1 keeps the word");
  require(scale_text_word(bits_of(0.8f), 0.5) == bits_of(0.4f), "0.8 at a factor of 1/2 is 0.4");

  // The window: closed, the stores pass the game's value; open (a widened strobe overlay), they are scaled; closed again.
  require(!overlay_text_active() && overlay_text_scale_x(bits_of(0.97f)) == bits_of(0.97f), "closed: the game's value");
  begin_overlay_text(32.0 / 9.0);
  require(overlay_text_active() && overlay_text_scale_x(bits_of(1.0f)) == bits_of(0.5f) &&
              overlay_text_scale_y(bits_of(0.8f)) == bits_of(0.8f),
          "open at 32:9: x halved, y kept");
  end_overlay_text();
  require(!overlay_text_active() && overlay_text_scale_x(bits_of(1.0f)) == bits_of(1.0f), "closed again: the game's value");
}

int main() {
  test_parse();
  test_output_size();
  test_fov_extra();
  test_ratios();
  test_projection_scale();
  test_scale_matrix();
  test_guest_matrix();
  test_scene_policy();
  test_hud_canvas();
  test_hud_parameters();
  test_overlay_canvas();
  std::puts("display_aspect fixture: all checks passed");
  return 0;
}
