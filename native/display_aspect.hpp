#pragma once

// TASK-080 P1: native display aspect ratios (decision 0071, docs/ULTRAWIDE-NATIVE-STUDY.md).
//
// GT4_DISPLAY_ASPECT picks the ratio R of the picture on screen:
//   unset, "auto"   with GT4_OUTPUT_SIZE (TASK-080 addendum, the launcher always sends it): R = W / H of the output
//                   size, fixed for the session, in windowed, fullscreen and headless runs alike;
//                   without it: R = the game window's client area (fullscreen: the monitor), followed on resize and
//                   Alt+Enter; headless runs (no window) use 16:9
//   "16:9", "4:3"   fixed ratios
//   "W:H"           a custom ratio, W and H positive decimals (for example "21:9", "32:9", "3440:1440", "21.5:9")
//   "X.YY"          a custom ratio as one decimal number (for example "2.39")
// Any other value (empty, zero, negative, a ratio outside 1:2 .. 8:1) refuses the start, like GT4_FULLSCREEN.
//
// GT4_OUTPUT_SIZE "WxH" (TASK-080 addendum, owner 2026-10-21): the output resolution the player chose on the launcher's
// Display page. W and H are decimal integers from 320 to 16384 and W / H must lie in 1:2 .. 8:1; anything else (also
// an empty value) refuses the start. The window opens at that client size (windowed) or covers the chosen monitor
// (fullscreen, GT4_MONITOR); the GS renders at the launcher's internal resolution k x 640 by k x 448 and the picture is
// scaled to the window. A fixed GT4_DISPLAY_ASPECT (a developer knob) wins over the output size.
//
// GT4_ASPECT_FOV_EXTRA (hidden comparison switch for owner test T1): a decimal factor f, default 1.0, range
// 0.5 .. 3.0. It widens the horizontal field of view by f on top of the geometry-correct value. The owner's tuned
// in-game values (Width 515 / Aspect 1.20, Width 573 / Aspect 1.34) correspond to f = s = R / (16/9).
//
// 3D (Confirmed, study sections 2-3): RaceBase::initMonitor (0x00388A80) builds the race camera's 4x4 monitor matrix
// on its own stack and hands it to the camera through a virtual call (jalr at 0x00388B30, return address 0x00388B38,
// a0 = camera, a1 = sp = the matrix, f12 = fovy). That call goes through ps2::dispatch, where the TASK-080 capsule
// overlay (scripts/display_aspect_overlay.inc) scales the stack copy before the camera reads it:
//   Hor+ (R >= 16:9): the x output column (m[0], m[4], m[8], m[12]) times (16/9) / (R * f)
//   Vert+ (R < 16:9): the y output column (m[1], m[5], m[9], m[13]) times R / (16/9); x column times 1 / f
// The matrix uses row vectors (clip = v * M; m[8], m[9] are the LAN monitor off-axis terms), so the x output is the
// column 0, 4, 8, 12. The presented frame is always the game's 16:9 frame stretched to R, so 16/9 is the reference
// whatever the game's own Wide setting is. At R = 16:9 and f = 1 nothing is written: frames stay byte-identical.
// The hook never writes the option object or the monitor record (save data); it writes only the 64-byte stack copy.
// Photo pause (func_003C49C8) calls initMonitor too, so leaving photo mode re-applies the same scale.
//
// Scene policy (presentation): the picture fills the screen (stretched to R) only while the race view is shown; menus,
// FMVs and photo mode stay pillarboxed 16:9. The race view is "RaceDisplay::render (0x0039F498, the race HUD, reached
// only through vtables, so through dispatch) ran in the last few VBlanks", minus photo mode: a display request for the
// photo/movie CRT row 2 (func_001074D0, struct at 0x006184D0) until the next camera hook or another request. The
// headless race trace (slice212b) showed that 480i races and menus make identical requests (func_001074D0 records only
// changes), so requests alone cannot mark the race; see ScenePolicy.

#include <cstdint>
#include <string>

#include "game_target.hpp"

namespace gt4::display_aspect {

inline constexpr double kGameRatio = 16.0 / 9.0;
// A ratio within this relative distance of 16:9 is treated as exactly 16:9 (1366x768 = 1.7786 is 16:9).
inline constexpr double kSnapTolerance = 0.005;
inline constexpr double kMinRatio = 0.5;
inline constexpr double kMaxRatio = 8.0;
inline constexpr double kMinFovExtra = 0.5;
inline constexpr double kMaxFovExtra = 3.0;

// Guest addresses (Confirmed, docs/ULTRAWIDE-NATIVE-STUDY.md section 3.1 and the slice208 translation unit_00A2.cpp).
// TASK-100 S3: the numbers live in native/game_target.hpp; a target without the hook gets kNeverMatch, which no guest
// PC or return address can equal.
inline constexpr std::uint32_t kInitMonitor = ::gt4::target::active().hooks.init_monitor;
inline constexpr std::uint32_t kCameraCallReturn =
    ::gt4::target::match_or_never(::gt4::target::active().hooks.camera_call_return);
inline constexpr std::uint32_t kMatrixBytes = 64u;               // 16 x f32 on the stack at a1 = sp
// RaceDisplay::render (the race HUD, every race frame), reached only through vtables 0x67E294 / 0x680D1C (study
// section 2.2; unit_00A7 entry 413 in the slice208 translation, no direct call site).
inline constexpr std::uint32_t kRaceDisplayRender =
    ::gt4::target::match_or_never(::gt4::target::active().hooks.race_display_render);
// Display request state written by func_001074D0 (docs/INGAME-SETTINGS-STUDY.md section 1.4).
inline constexpr std::uint32_t kDisplayRequestState = ::gt4::target::active().hooks.display_request_state;
inline constexpr std::uint32_t kRequestPendingOffset = ::gt4::target::active().hooks.display_request_pending_offset;
inline constexpr std::uint32_t kRequestModeOffset = ::gt4::target::active().hooks.display_request_mode_offset;
inline constexpr std::uint32_t kRequestRowOffset = ::gt4::target::active().hooks.display_request_row_offset;

enum class Source { automatic, fixed, output_size };

struct Setting {
  Source source = Source::automatic;
  double ratio = kGameRatio;  // fixed ratio (snapped); for automatic, the headless default
};

// GT4_DISPLAY_ASPECT. nullptr = automatic. Throws std::invalid_argument with a message for any invalid value.
Setting parse_display_aspect(const char *value);

inline constexpr std::uint32_t kMinOutputSide = 320;
inline constexpr std::uint32_t kMaxOutputSide = 16384;

struct OutputSize {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool known() const { return width != 0 && height != 0; }
};

// GT4_OUTPUT_SIZE. nullptr = not known (0 x 0). Throws std::invalid_argument for any invalid value (see above).
OutputSize parse_output_size(const char *value);

// The setting for a GT4_DISPLAY_ASPECT value and an output size: "auto" (or unset) with a known output size becomes
// Source::output_size at the snapped W / H; a fixed ratio stays fixed. Pure.
Setting resolve_setting(const Setting &aspect, const OutputSize &output);

// GT4_ASPECT_FOV_EXTRA. nullptr = 1.0. Throws std::invalid_argument for an invalid value.
double parse_fov_extra(const char *value);

// R snapped: exactly kGameRatio when within kSnapTolerance of 16:9, otherwise unchanged.
double snap_ratio(double ratio);

// Ratio of a client area; 16:9 when either side is zero (minimized or unknown). Snapped.
double client_ratio(std::uint32_t width, std::uint32_t height);

bool is_game_ratio(double ratio);

struct ProjectionScale {
  float x = 1.0f;  // factor on the x output column (0, 4, 8, 12)
  float y = 1.0f;  // factor on the y output column (1, 5, 9, 13)
  bool identity() const { return x == 1.0f && y == 1.0f; }
};

// Hor+ / Vert+ factors for a snapped ratio R and the extra FOV factor f (see the header comment). Pure.
ProjectionScale projection_scale(double ratio, double fov_extra);

// Scales a host-order 4x4 matrix in place. Identity scales leave it untouched. Pure.
void scale_matrix(float matrix[16], const ProjectionScale &scale);

// Scales the 16 little-endian f32 words at guest `address`. Returns false (no write) when the 64 bytes are not inside
// guest RAM of `ram_size` bytes or the scale is the identity.
bool scale_guest_matrix(std::uint8_t *ram, std::uint32_t ram_size, std::uint32_t address, const ProjectionScale &scale);

// --- Scene policy -----------------------------------------------------------------------------------------------

// One sample of the display request state (func_001074D0) read at guest VBlank.
struct DisplayRequest {
  std::uint32_t pending = 0;
  std::uint32_t mode = 0;  // a1 of func_001074D0 (1 interlaced, 2 progressive / 1080i)
  std::uint32_t row = 0;   // CRT table row (default row for menus and 480i races, 2 photo and movies, 6 480p, 7 1080i)
  bool operator==(const DisplayRequest &other) const {
    return pending == other.pending && mode == other.mode && row == other.row;
  }
};

// Pure state machine. Race view wanted = RaceDisplay::render ran within the last kRenderWindowVblanks VBlanks and
// photo mode is not on. Photo mode = the last new display request asked for kPhotoMovieRow; it ends at the next race
// camera hook (leaving photo pause re-runs initMonitor) or at another request. The presented state changes only after
// the wanted state has been the same for kSettleVblanks VBlanks (hysteresis against single missed HUD frames).
class ScenePolicy {
 public:
  static constexpr std::uint32_t kRenderWindowVblanks = 30;  // the HUD paused ~23 VBlanks at a race start (slice212b trace)
  static constexpr std::uint32_t kSettleVblanks = 3;
  static constexpr std::uint32_t kPhotoMovieRow = 2;

  void on_race_camera(std::uint64_t vblank);
  void on_race_display_render(std::uint64_t vblank);
  // Called at every guest VBlank with the current request sample. Returns the presented "race view" state.
  bool at_vblank(std::uint64_t vblank, const DisplayRequest &request);

  bool race_view_wanted() const { return wanted_; }
  bool race_view_presented() const { return presented_; }
  bool photo_mode() const { return photo_; }
  std::uint64_t request_events() const { return request_events_; }

 private:
  bool have_request_ = false;
  DisplayRequest last_request_{};
  bool render_seen_ = false;
  std::uint64_t render_vblank_ = 0;
  bool photo_ = false;
  bool wanted_ = false;
  bool presented_ = false;
  std::uint32_t stable_ = 0;
  std::uint64_t request_events_ = 0;
};

// Whether a new display request event happened between two samples: pending rises, or mode/row change.
bool is_new_request(const DisplayRequest &previous, const DisplayRequest &current);

// --- P2: race HUD and UI (slice217; decision 0071 table) ---------------------------------------------------------
//
// Owner design: the 3D fills the screen, the race HUD keeps the game's 16:9 layout, undistorted and centred ("HUD
// centred 16:9"). The game draws the HUD into the same 640-pixel frame after the 3D, so the frame's stretch to R
// (fill) is undone for the HUD only:
//   (a) HUD canvas, runtime hook at the dispatch of RaceDisplay::render (0x0039F498, a0 = this): the HUD ortho matrix
//       at this + 0x70 (written by RaceDisplay::setProjectionMatrix 0x0039BB38 at initialize; read by render through
//       func_004A74B4) is rewritten from the game's 16:9 canvas (left -106.667, right 746.667, top 480, bottom 0) to
//       the largest centred 16:9 box of a screen of ratio R: R >= 16:9: left/right = 320 -/+ 240 R (wider canvas);
//       R < 16:9: top/bottom = 240 +/- 240 (16/9) / R (taller canvas). Only matrices that are the game's full-screen
//       16:9 canvas, or one this hook wrote, are touched: split-screen canvases (RaceSplitDisplayBase::initialize,
//       -106.667..320 or 0..640 x 240) and the 4:3 game canvas (0..640) are left alone. At R = 16:9 the game's own words
//       are restored exactly (kept per object).
//   (b) Constants compiled into the translated code (an immediate becomes a C++ constant, and every call after the
//       render entry is a direct jal, so no dispatch point exists between the store and its use): the ELF patch table
//       (scripts/aspect_elf_patch.py, re-translated library profile "aspect") turns each one into a load from the HUD
//       parameter block below. The host fills the block at every guest VBlank; at 16:9 it holds the original constants,
//       so the patched code computes exactly what the original did.
inline constexpr std::uint32_t kHudMatrixOffset =
    ::gt4::target::active().hooks.hud_matrix_offset;  // RaceDisplay + this: 16 x f32, setProjectionMatrix layout

struct HudCanvas {
  double left, right, top, bottom;
};

// The canvas for a snapped ratio R (16:9 = the game's canvas -320/3, 2240/3, 480, 0). Pure.
HudCanvas hud_canvas(double ratio);

// The four words of the HUD matrix a canvas sets, in single precision like RaceDisplay::setProjectionMatrix:
// m[0] = 2 / (right - left), m[5] = 2 / (bottom - top), m[12] = (left + right) / (left - right),
// m[13] = (top + bottom) / (top - bottom).
struct HudMatrixWords {
  std::uint32_t m0 = 0, m5 = 0, m12 = 0, m13 = 0;
  bool operator==(const HudMatrixWords &other) const {
    return m0 == other.m0 && m5 == other.m5 && m12 == other.m12 && m13 == other.m13;
  }
};
HudMatrixWords hud_matrix_words(const HudCanvas &canvas);

// Whether 16 matrix words are an orthographic HUD matrix of the game's full-screen 16:9 canvas (decoded edges within
// 0.05; the other words 0 except m[10] = -1 and m[15] = 1).
bool is_game_hud_canvas(const std::uint32_t words[16]);

// Whether 16 matrix words are one of the split-screen HUD canvases of RaceSplitDisplayBase::initialize: wide
// (left -106.667, right 320, 480 high) or 4:3 (0..640, 240 high). A RaceDisplay::render with such a canvas is not
// counted as the race view, so split screen stays pillarboxed 16:9 with the game's own HUD (owner rule, P2).
bool is_split_screen_hud_canvas(const std::uint32_t words[16]);

// Per-object state of the canvas hook: the game's own words of up to kSlots RaceDisplay objects and the words the hook
// last wrote, so 16:9 restores them exactly and a fresh object at a reused address is recognised again.
class HudCanvasHook {
 public:
  static constexpr int kSlots = 8;
  // Rewrites the matrix at `self + kHudMatrixOffset` for the ratio. Returns true when words were written.
  bool apply(std::uint8_t *ram, std::uint32_t ram_size, std::uint32_t self, double ratio);
  std::uint64_t writes() const { return writes_; }
  std::uint64_t foreign() const { return foreign_; }
  // Whether the last apply() found a split-screen canvas (see is_split_screen_hud_canvas).
  bool last_was_split_screen() const { return last_split_; }

 private:
  struct Slot {
    std::uint32_t address = 0;
    bool used = false;
    HudMatrixWords original{}, written{};
  };
  Slot slots_[kSlots]{};
  int next_ = 0;
  std::uint64_t writes_ = 0, foreign_ = 0;
  bool last_split_ = false;
};

// HUD parameter block in guest kernel RAM that neither the game nor our HLE kernel uses (the kernel uses 0x800-0x848,
// 0x1800-0x1BFF and 0x1950+, its stacks grow down from 0x80000; 0x7000-0x7FFF read zero in the uw-study-01 RAM dump).
// The ELF patch table's loads read it as `lw rt, offset(zero)`, so it must sit below 0x8000. scripts/aspect_elf_patch.py
// holds the same table; CTest checks the old words against the pinned ELF.
inline constexpr std::uint32_t kHudParameterBlock = 0x00007F00u;
inline constexpr std::uint32_t kHudParameterWords = 2u;
// TASK-100 S3: our own scratch block, not a game address, so it stays a literal for every target; the v2.00 table
// documents the same values for the hooks that are game addresses.
static_assert(kHudParameterBlock == ::gt4::target::kV200Hooks.hud_parameter_block, "HUD block differs from the v2.00 table");
static_assert(kHudParameterWords == ::gt4::target::kV200Hooks.hud_parameter_words, "HUD word count differs from the v2.00 table");
// +0x00 f32: race HUD font x scale (RaceDisplay::render 0x0039F554, `lui at,0x3F40` = 0.75 = (4/3) / (16/9)).
// +0x04 s32: rear-view mirror horizontal divisor (0x0033EF98, `addiu t0,zero,640`): the mirror rectangle is
//            260 x width / divisor frame pixels wide, centred on the frame; 640 x s keeps its 16:9 shape.
inline constexpr std::uint32_t kFontScaleOffset = 0x00u;
inline constexpr std::uint32_t kMirrorDivisorOffset = 0x04u;
inline constexpr std::uint32_t kGameFontScaleBits = 0x3F400000u;  // 0.75f
inline constexpr std::uint32_t kGameMirrorDivisor = 640u;

struct HudParameters {
  std::uint32_t words[kHudParameterWords] = {};
  bool operator==(const HudParameters &other) const {
    for (std::uint32_t i = 0; i < kHudParameterWords; ++i)
      if (words[i] != other.words[i])
        return false;
    return true;
  }
};

// The block for a snapped ratio R; 16:9 gives the original constants exactly. Pure.
//   font x scale = (4/3) / R: glyphs (drawn in frame pixels, then stretched to R) keep their 16:9 shape;
//   mirror divisor = round(640 x R / (16/9)) for R > 16:9, 640 otherwise.
HudParameters hud_parameters(double ratio);

// Writes the block into guest RAM when it differs. Returns true when words were written.
bool write_hud_parameters(std::uint8_t *ram, std::uint32_t ram_size, const HudParameters &parameters);

// --- TASK-095: 2D overlays over the race view --------------------------------------------------------------------
//
// While the presentation fills the screen (the race view at R != 16:9), everything the game draws into its frame is
// stretched with it, and so were the 2D screens drawn over the race (docs/ULTRAWIDE-OVERLAYS.md). Each sets its 2D canvas
// through the ortho function (v2.00 0x004A5A90, Spec II 0x005A2600: f12 left, f13 right, f14 bottom, f15 top, f16 near,
// f17 far; it builds the matrix on its own stack and multiplies it in) by a direct `jal` with immediate arguments:
//   menu context render (the pre-race screens: the event's entry list and icon row, the Time Trial lobby with its session
//     record): v2.00 mUpdateContextPS2 render 0x00214FB0, Spec II 0x0036D278; 0..640 x 480..0;
//   strobe overlay renderer (the pause menu, the pit menu, the results, photo mode's panel): v2.00 func_003EC0B0 (called by
//     RacePause::render, PhotoPause::update, PitMenu, ResultArcade::render and the race monitor), Spec II 0x001FDCC8;
//     0..640 x 480..0.
// (Names from gt4-decomp 3b1f2ae5b; Spec II matched by code shape.) scripts/camera360_units.py adds a host call between the
// delay slot and each of those calls (native/display_aspect_hook.cpp), and the host widens the canvas like the HUD's: while
// the presentation fills the screen at R, the x extent times s = R / (16/9) around its centre for R > 16:9 (21:9: 0..640
// becomes -100..740), the y extent times (16/9) / R around its centre for R < 16:9. The overlay then keeps its 16:9 shape,
// centred. While the presentation is the 16:9 pillarbox (menus, photo mode on v2.00), and always at 16:9, the game's own
// arguments stay. The race monitor (RaceMonitor, drawn by RaceBasic::render_panel) is left as the game draws it: it also
// draws its own items with the race font, which no hook narrows, so its strobe part is not widened either.
inline constexpr std::uint32_t kMenuOrthoReturn =
    ::gt4::target::match_or_never(::gt4::target::active().hooks.menu_ortho_call_return);
inline constexpr std::uint32_t kOverlayOrthoReturn =
    ::gt4::target::match_or_never(::gt4::target::active().hooks.overlay_ortho_call_return);
// The strobe renderer keeps its caller's return address at sp + this, and the race monitor's call returns here.
inline constexpr std::uint32_t kOverlayCallerSlot = ::gt4::target::active().hooks.overlay_caller_slot;
inline constexpr std::uint32_t kMonitorOverlayCallReturn =
    ::gt4::target::match_or_never(::gt4::target::active().hooks.monitor_overlay_call_return);

// The four edges of an ortho call, as the f32 bits of its argument registers.
struct OrthoCanvas {
  std::uint32_t left = 0, right = 0, bottom = 0, top = 0;
  bool operator==(const OrthoCanvas &other) const {
    return left == other.left && right == other.right && bottom == other.bottom && top == other.top;
  }
};

// Whether a canvas is the one the hooks expect from the game: left 0, top 0, bottom 480, right 640. Anything else is left
// alone.
bool is_game_overlay_canvas(const OrthoCanvas &canvas);

// The canvas for a presented ratio R (snapped): R > 16:9 widens x by R / (16/9) around the centre, R < 16:9 widens y by
// (16/9) / R around the centre, 16:9 returns the same bits. Pure.
OrthoCanvas overlay_canvas(const OrthoCanvas &game, double ratio);

// The strobe overlays' text does not follow the canvas: its glyphs are sized in frame pixels by the text context's x and y
// scale, which the text style parser (v2.00 0x00490290, Spec II 0x0033AF90) stores for every text: 1.0, then the style's
// percentages. scripts/camera360_units.py routes those four stores through overlay_text_scale_x / _y; while a widened strobe
// overlay is being drawn (from its ortho hook to after_overlay_draw, before the renderer's first call after its draw: v2.00
// return 0x003EC248, Spec II 0x001FDE60) they return the value times the inverse of the canvas factor on the same axis, and
// the value itself otherwise. Nothing is written to guest RAM by the host; the game stores the scaled value itself.
inline constexpr std::uint32_t kOverlayDrawEndReturn =
    ::gt4::target::match_or_never(::gt4::target::active().hooks.overlay_draw_end_return);

struct TextFactors {
  double x = 1.0, y = 1.0;
};

// The text factors for a presented ratio R (snapped): R > 16:9: x = (16/9) / R; R < 16:9: y = R / (16/9); 16:9: 1 and 1. Pure.
TextFactors overlay_text_factors(double ratio);

// An f32 word times a factor, as f32 bits; a factor of exactly 1 returns the same bits. Pure.
std::uint32_t scale_text_word(std::uint32_t value, double factor);

// --- Runtime glue (one process-wide state; EE thread unless noted) ------------------------------------------------

// Reads GT4_DISPLAY_ASPECT, GT4_OUTPUT_SIZE and GT4_ASPECT_FOV_EXTRA once. An invalid value prints the refusal and exits with status 1
// (called at window start, and again at the first hook or VBlank so headless runs refuse too).
const Setting &configured_setting();
OutputSize configured_output_size();
double configured_fov_extra();

// Window thread: the game window's client size (WM_SIZE, fullscreen). 0 x 0 is ignored (minimized).
void set_window_client_size(std::uint32_t width, std::uint32_t height);

// TASK-213 Phase 1d: the live output size picked in the game (Resolution row). With GT4_DISPLAY_ASPECT=auto and a
// known GT4_OUTPUT_SIZE, it replaces the start size's ratio for the next race camera and HUD block; a fixed
// GT4_DISPLAY_ASPECT still wins. Zero sides are ignored.
void set_live_output_size(std::uint32_t width, std::uint32_t height);

// R now: the fixed or output-size ratio, or the client area ratio (16:9 before any window size is known). Snapped.
double current_ratio();

// The ratio the last race camera hook used (16:9 before the first hook).
double race_projection_ratio();

// Called by the dispatch overlay at the camera virtual call (return 0x00388B38). `matrix_address` = a1. Returns true
// when the matrix was scaled.
bool on_race_camera(std::uint8_t *ram, std::uint32_t ram_size, std::uint32_t matrix_address);

// Called by the dispatch overlay at every dispatch of RaceDisplay::render (0x0039F498): marks the race view.
// (The slice212c capsule's form; kept so that capsule still builds.)
void on_race_display_render();

// P2 (slice217 capsule): the same marker plus the HUD canvas hook on `self` = a0 (RaceDisplay or a subclass such as
// RaceLicenseDisplay, vtable 0x680D00). Uses the ratio of the last race camera hook, the ratio the presentation fills.
void on_race_display_render(std::uint8_t *ram, std::uint32_t ram_size, std::uint32_t self);

// TASK-095: called by the overlay hooks before the ortho call (`return_label` = ra) with the game's canvas. Returns the
// canvas to draw with: overlay_canvas() at the presented ratio while the presentation fills the screen, otherwise (16:9
// pillarbox, R = 16:9, an unexpected canvas) the game's own.
OrthoCanvas on_overlay_ortho(const OrthoCanvas &game, std::uint32_t return_label);

// TASK-095: the strobe text window. begin: the hook widened a strobe overlay's canvas at `ratio`; end: its draw is over.
void begin_overlay_text(double ratio);
void end_overlay_text();
bool overlay_text_active();

// TASK-095: the text style parser's scale stores (translated statements, scripts/camera360_units.py). `value` is the f32 the
// game is storing; the result is what it stores.
std::uint32_t overlay_text_scale_x(std::uint32_t value);
std::uint32_t overlay_text_scale_y(std::uint32_t value);

// Presentation sink: race_view = fill the screen at `ratio`; otherwise pillarbox 16:9. Called on the EE thread only
// when the presented state or ratio changes (never while R stays 16:9).
using PresentationSink = void (*)(bool race_view, double ratio);
void set_presentation_sink(PresentationSink sink);

// Guest VBlank entry (mtvu_runtime.cpp vblank_sync): samples the display request, drives the scene policy and (P2)
// keeps the HUD parameter block filled for the ratio of the last race camera hook (16:9 values before it).
void at_guest_vblank(std::uint8_t *ram, std::uint32_t ram_size);

// "21:9 (2.333)" style text for logs.
std::string describe_ratio(double ratio);

}  // namespace gt4::display_aspect
