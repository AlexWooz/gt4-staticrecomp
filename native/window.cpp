#include "window.hpp"
#include "race_display.hpp"
#include "race_widescreen.hpp"
#include "display_aspect.hpp"
#include "ps2/memory.hpp"
#include "gpu.hpp"
#include "gs/gs.hpp"
#include "host_io.hpp"
#include "native_vulkan.hpp"
#include "ps2/system.hpp"
#include "perf_probe.hpp"
#include "frame_clock.hpp"
#include "pacing.hpp"
#include "window_mode.hpp"
#include "cursor_autohide.hpp"
#include "achievements.hpp"
#include "input_recording.hpp"
#ifdef GT4_GS_STREAM_CLIENT
#include "gs_stream/gs_stream_client.hpp"
#endif
#ifdef GT4_MTVU_MEASURE
#include "mtvu_measure.hpp"
#endif
#ifdef GT4_SHAPE_CAPTURE
#include "shape_capture_control.hpp"
#include "shape_observer_adapter.hpp"
#include <array>
#endif

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr wchar_t window_title[] = L"Gran Turismo 4";
HWND game_window = nullptr;
bool surface_attached = false;
bool closing = false;
ControllerInput *controller = nullptr;
gt4::window_mode::ToggleState fullscreen_state;
#ifdef GT4_SHAPE_CAPTURE
std::unique_ptr<gt4::observation::CaptureControl> capture;
#endif
// Pacing state (see pacing.hpp). The clock and histogram run in both modes;
// the release grid only in paced mode.
gt4::pacing::Mode pacing_mode = gt4::pacing::Mode::paced;
std::unique_ptr<FrameClock> frame_clock;
std::unique_ptr<gt4::pacing::FramePacer> frame_pacer;
gt4::pacing::Diagnostics pacing_diagnostics;
// Session totals at the last F7 start or F8 report; the next report prints the difference.
gt4::pacing::Counters counters_at_last_report;
// GT4_FORCE_480P (slice164): set at window start; counts guest VBlank writes of the race display index.
bool force_480p_enabled = false;
gt4::display::Force480pState force_480p_state;
AudioOutput *audio_output = nullptr;
// Input bindings (controls.hpp). open_window loads them before the window exists.
gt4::controls::ControlMap active_controls;
// TASK-075: gamepad backend (GT4_INPUT=sdl default, xinput for one release). open_window loads it.
gt4::pad::Backend input_backend = gt4::pad::Backend::sdl;
std::vector<gt4::controls::KeyboardBinding> keyboard_keys;
// TASK-060: cursor auto-hide over this window's picture (GT4_CURSOR_AUTOHIDE, cursor_autohide.hpp). In pcsx2 mode the
// picture is the gt4-gs child window (another process), which applies the same rule itself (CONTROL cursor); this
// window then only owns its own client area, which the child covers.
gt4::cursor::AutoHide cursor_autohide;
HCURSOR arrow_cursor = nullptr;

// One cursor sample per VBlank (and per WM_SETCURSOR). The cursor is changed only while it is over this window's own
// client area and this window is the one under it.
void update_cursor() {
  if (!game_window || !cursor_autohide.policy().auto_hide)
    return;
  POINT point{};
  if (!GetCursorPos(&point))
    return;
  bool over_picture = WindowFromPoint(point) == game_window;
  if (over_picture) {
    POINT local = point;
    RECT client{};
    over_picture = ScreenToClient(game_window, &local) && GetClientRect(game_window, &client) &&
                   PtInRect(&client, local);
  }
  const bool was_hidden = cursor_autohide.hidden();
  const bool hide = cursor_autohide.update(point.x, point.y, GetTickCount64(), over_picture);
  if (hide && !was_hidden)
    SetCursor(nullptr);
  else if (!hide && was_hidden && over_picture)
    SetCursor(arrow_cursor);
}

// TASK-080 addendum: the monitor named by GT4_MONITOR (a GDI device name such as \\.\DISPLAY2).
struct MonitorSearch {
  const std::wstring *device;
  HMONITOR found;
};

BOOL CALLBACK find_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
  auto *search = reinterpret_cast<MonitorSearch *>(data);
  MONITORINFOEXW info{};
  info.cbSize = sizeof info;
  if (GetMonitorInfoW(monitor, &info) && *search->device == info.szDevice) {
    search->found = monitor;
    return FALSE;
  }
  return TRUE;
}

// The chosen monitor, or the primary monitor when none is named or the named one is not connected.
HMONITOR chosen_monitor(const std::string &device) {
  const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  if (device.empty())
    return primary;
  const std::wstring wide(device.begin(), device.end());  // printable ASCII only (parse_monitor)
  MonitorSearch search{&wide, nullptr};
  EnumDisplayMonitors(nullptr, nullptr, find_monitor, reinterpret_cast<LPARAM>(&search));
  if (search.found)
    return search.found;
  std::fprintf(stderr, "[window] GT4_MONITOR %s is not connected; using the primary monitor\n", device.c_str());
  return primary;
}

// Per-monitor DPI awareness (v2) before any window exists, so window sizes, the monitor rectangle and the swapchain are
// physical pixels on scaled desktops too (the launcher reads the modes in physical pixels).
void declare_dpi_awareness() {
  using SetContext = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
  const HMODULE user32 = GetModuleHandleW(L"user32.dll");
  const auto set_context =
      user32 ? reinterpret_cast<SetContext>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")) : nullptr;
  const bool aware = set_context && set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (!aware)
    SetProcessDPIAware();
  std::fprintf(stderr, "[window] DPI awareness %s\n", aware ? "per-monitor v2" : "system");
}

// IN-520: GT4_INPUT_RECORD=<file> writes the pad state the game receives per VBlank, and F9 ends the recording and closes the
// game. GT4_INPUT_REPLAY=<file> gives the game those states at the same VBlanks instead of reading any input.
gt4::input_recording::Recorder input_recorder;
std::unique_ptr<gt4::input_recording::Player> input_player;
std::uint64_t input_vblank = 0;
// GT4_PACING_FROM_VBLANK=<n>: no waiting before this VBlank, then paced: a bench reaches a race in less time, with the same
// presses, since they count VBlanks.
const std::uint64_t paced_from_vblank = [] {
  const char *text = std::getenv("GT4_PACING_FROM_VBLANK");
  return text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
}();

void apply_pad(const PadState &pad) {
  ps2::set_pad_buttons(pad.buttons);
  for (int axis = 0; axis < 4; ++axis)
    ps2::set_pad_axis(axis, pad.axes[axis]);
  for (int button = 0; button < 16; ++button)
    ps2::set_pad_pressure(button, pad.pressures[button]);
}

gt4::pacing::Counters pacing_counters() {
  gt4::pacing::Counters counters;
  if (frame_pacer)
    counters.late_frames = frame_pacer->late_frames();
  if (audio_output) {
    counters.audio_underruns = audio_output->underruns();
    counters.ring_overflow_frames = audio_output->overflow_frames();
  }
  return counters;
}

gt4::window_mode::Rect to_rect(const RECT &rect) {
  return {rect.left, rect.top, rect.right, rect.bottom};
}

// Borderless fullscreen on the monitor that holds the window. The windowed frame
// and style are kept by the toggle state and restored on the next toggle. The
// swapchain follows the new client size through WM_SIZE.
void set_fullscreen(bool enabled) {
  using gt4::window_mode::Placement;
  if (enabled) {
    RECT windowed{};
    if (!GetWindowRect(game_window, &windowed))
      return;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof monitor;
    if (!GetMonitorInfoW(MonitorFromWindow(game_window, MONITOR_DEFAULTTONEAREST),
                         &monitor))
      return;
    const auto style = static_cast<std::uint32_t>(GetWindowLongW(game_window, GWL_STYLE));
    if (!fullscreen_state.enter(Placement{to_rect(windowed), style}))
      return;
    SetWindowLongW(game_window, GWL_STYLE,
                   static_cast<LONG>(gt4::window_mode::fullscreen_style(style)));
    const RECT area = monitor.rcMonitor;
    SetWindowPos(game_window, HWND_TOP, area.left, area.top,
                 area.right - area.left, area.bottom - area.top,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
    return;
  }
  Placement restored{};
  if (!fullscreen_state.leave(restored))
    return;
  SetWindowLongW(game_window, GWL_STYLE, static_cast<LONG>(restored.style));
  const auto &bounds = restored.bounds;
  SetWindowPos(game_window, nullptr, bounds.left, bounds.top,
               bounds.right - bounds.left, bounds.bottom - bounds.top,
               SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER);
}

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM first,
                                  LPARAM second) {
  if (input_recorder.active() && message == WM_KEYDOWN && first == VK_F9) {
    const std::uint64_t changes = input_recorder.finish(input_vblank);
    std::fprintf(stderr, "[input] recording ended by F9 at VBlank %llu, %llu changes; closing the game\n",
                 static_cast<unsigned long long>(input_vblank), static_cast<unsigned long long>(changes));
    PostMessageW(window, WM_CLOSE, 0, 0);
    return 0;
  }
#ifdef GT4_PERF_PROBE
  if (message == WM_KEYDOWN && (first == VK_F7 || first == VK_F8 || first == VK_F9)) {
    if (first == VK_F7) {
      // The F7/F8 interval starts here: histogram and counter deltas restart.
      counters_at_last_report = pacing_counters();
      pacing_diagnostics.reset();
#ifdef GT4_MTVU_MEASURE
      if ((second & (1ll << 30)) == 0)
        gt4::mtvu::request_interval_reset();
#endif
    }
    const auto request = first == VK_F7 ? gt4::perf::Request::start :
        first == VK_F8 ? gt4::perf::Request::report : gt4::perf::Request::toggle;
    gt4::perf::Probe::shared().request(request, (second & (1ll << 30)) != 0);
    return 0;
  }
#endif
#ifdef GT4_SHAPE_CAPTURE
  if (message == WM_KEYDOWN && first == VK_F8) {
    if (capture) capture->request_toggle((second & (1ll << 30)) != 0);
    return 0;
  }
#endif
  if (message == WM_CLOSE) {
    input_recorder.finish(input_vblank);  // Alt+F4 or the window's X also leave a complete recording
#ifdef GT4_GS_STREAM_CLIENT
    // TASK-038: with GT4_RENDERER=pcsx2 the GS process draws into a child of this window, and DestroyWindow waits for
    // that process's window thread (a stalled GS window thread would freeze the close). Halt the guest instead; the
    // window is destroyed in cleanup(), after gs_stream::finish() has shut the GS process down (or ended it).
    if (gt4::gs_stream::stream_renderer()) {
      closing = true;
      ps2::halt("window closed");
      return 0;
    }
#endif
    DestroyWindow(window);
    return 0;
  }
  // Alt+Enter arrives as WM_SYSKEYDOWN; Alt+Enter and F11 toggle borderless fullscreen.
  // Auto-repeat (previous key state bit set) is ignored.
  const bool repeat = (second & (1ll << 30)) != 0;
  if ((message == WM_SYSKEYDOWN && first == VK_RETURN && !repeat) ||
      (message == WM_KEYDOWN && first == VK_F11 && !repeat)) {
    set_fullscreen(!fullscreen_state.fullscreen());
    return 0;
  }
  if (message == WM_DESTROY) {
    closing = true;
    PostQuitMessage(0);
    return 0;
  }
#ifdef GT4_GS_STREAM_CLIENT
  // TASK-038 (slice195): with GT4_RENDERER=pcsx2 the picture is a child window of another process. Whenever this
  // window is activated (also when gt4-gs.exe gives the foreground back after a click on the picture), the keyboard
  // focus goes to this window, never to the child.
  if (message == WM_ACTIVATE && LOWORD(first) != WA_INACTIVE && gt4::gs_stream::stream_renderer()) {
    SetFocus(window);
    return 0;
  }
#endif
  // TASK-060: over the client area the cursor follows the auto-hide state; elsewhere (frame, title bar) the default.
  if (message == WM_SETCURSOR && reinterpret_cast<HWND>(first) == window && LOWORD(second) == HTCLIENT &&
      cursor_autohide.policy().auto_hide) {
    update_cursor();
    SetCursor(cursor_autohide.hidden() ? nullptr : arrow_cursor);
    return TRUE;
  }
  if (message == WM_KILLFOCUS && controller) {
    controller->stop();
    if (ps2::g_options.presses.empty() && !input_player)
      apply_pad({});
  }
  if (message == WM_MOVE || message == WM_SIZE)
    gt4::achievements::on_window_pos_changed(window);
  // TASK-080: GT4_DISPLAY_ASPECT=auto follows the client area (windowed, fullscreen, Alt+Enter, resize).
  if (message == WM_SIZE)
    gt4::display_aspect::set_window_client_size(LOWORD(second), HIWORD(second));
  if (message == WM_SIZE && surface_attached) {
    ps2::gs::resize_window(LOWORD(second), HIWORD(second));
    return 0;
  }
  return DefWindowProcW(window, message, first, second);
}

// TASK-080 presentation sink (native/display_aspect.hpp, EE thread): CoreGS 1.1 fills a rectangle of the race
// projection ratio; the PCSX2 GS process gets CONTROL display_aspect (protocol 1.3; only a 1.3 consumer is sent it).
void present_race_view(bool fill, double ratio) {
#ifdef GT4_GS_STREAM_CLIENT
  if (gt4::gs_stream::stream_renderer()) {
    gt4::gs_stream::set_display_aspect_mode(fill ? gt4::gs_stream::DisplayAspectMode::fill
                                                 : gt4::gs_stream::DisplayAspectMode::game_aspect,
                                            fill ? gt4::gs_stream::aspect_millionths(ratio) : 0);
    return;
  }
#endif
  gt4::vulkan::set_race_view_presentation(fill, ratio);
}

bool pump_messages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    if (message.message == WM_QUIT) {
      ps2::halt("window closed");
      closing = true;
      return false;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return !closing;
}

// Releases the guest frame. Paced mode waits on the wall-clock grid first, so
// each VBlank comes at the donor's 60 Hz rate and audio never sets the pace.
// The interval histogram is recorded in both modes.
std::int64_t last_release = 0;

void release_frame() {
  if (pacing_mode == gt4::pacing::Mode::paced) {
    if (!frame_pacer->anchored())
      frame_pacer->anchor(frame_clock->now());
    if (const std::int32_t shift = gt4::pacing::grid_shift_us.exchange(0, std::memory_order_relaxed)) {
      frame_pacer->shift(static_cast<std::int64_t>(shift) * static_cast<std::int64_t>(frame_clock->frequency()) / 1000000);
    }
    // TASK-084 step 0: the sleep is its own category, so F8 ee_dispatch and unattributed mean work, not sleep.
    GT4_PERF_SCOPE(gt4::perf::Category::pacing_wait);
    // Bench switch: GT4_PACING_NO_WAIT=1 runs as fast as it can with a window, to compare with a run with no window.
    static const bool no_wait = std::getenv("GT4_PACING_NO_WAIT") != nullptr;
    gt4::pacing::ee_slack_us = static_cast<std::int32_t>(std::clamp<std::int64_t>(
        (frame_pacer->deadline() - frame_clock->now()) * 1000000 / static_cast<std::int64_t>(frame_clock->frequency()), -100000, 100000));
    // IN-519: behind the grid, frames were released at once until it caught up: bursts of 9 ms intervals after a slow stretch,
    // a zigzag in the frametime for no gain. A frame now waits at least this long after the last one, so the lost time comes
    // back at about 1 ms per frame; past a whole period behind, the pacer restarts its grid as before.
    // GT4_PACING_MIN_INTERVAL_US=0 releases at once (A/B).
    static const std::int64_t min_interval = [] {
      const char *text = std::getenv("GT4_PACING_MIN_INTERVAL_US");
      const std::int64_t microseconds = text != nullptr ? std::atoll(text) : 15700;
      return microseconds * static_cast<std::int64_t>(frame_clock->frequency()) / 1000000;
    }();
    // Not waiting, the grid must not run ahead of the clock, or the first paced frame would sleep until the grid came back.
    if (input_vblank + 1 < paced_from_vblank) frame_pacer->anchor(frame_clock->now());
    if (!no_wait && input_vblank + 1 >= paced_from_vblank) {
      const std::int64_t earliest = last_release != 0 ? last_release + min_interval : 0;
      frame_clock->wait_until(std::max(frame_pacer->deadline(), earliest));
    }
  }
  const std::int64_t released = frame_clock->now();
  last_release = released;
  if (pacing_mode == gt4::pacing::Mode::paced)
    frame_pacer->release(released);
  pacing_diagnostics.record_release(released, frame_clock->frequency());
}

void on_vblank() {
#ifdef GT4_PERF_PROBE
  gt4::perf::Probe::shared().frame_boundary(
      {ps2::now(), ps2::primitives_drawn(), ps2::vu1_interpreted_calls()});
#endif
  release_frame();
  ++input_vblank;
  GT4_PERF_SCOPE(gt4::perf::Category::host_vblank);
  if (!pump_messages())
    return;
  update_cursor();
  if (force_480p_enabled)
    gt4::display::force_race_480p_at_vblank(ps2::g_ram, ps2::kRamSize, force_480p_state);
#ifdef GT4_SHAPE_CAPTURE
  if (capture)
    capture->service();
#endif
  std::uint16_t buttons = 0;
  const bool focused = GetForegroundWindow() == game_window;
  if (focused) {
    // Keyboard bindings come from the controls table (defaults match the old fixed map).
    // Alt+Enter toggles fullscreen: while Alt is held, Enter is not Start.
    const bool alt_held = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    for (const auto &binding : keyboard_keys) {
      if (binding.virtual_key == VK_RETURN && alt_held)
        continue;
      if (GetAsyncKeyState(binding.virtual_key) & 0x8000)
        buttons |= binding.button_mask;
    }
  }
  if (input_player) {
    apply_pad(input_player->at(input_vblank));
  } else if (ps2::g_options.presses.empty()) {
    const PadState pad = controller->poll(focused, buttons);
    apply_pad(pad);
    input_recorder.vblank(input_vblank, pad);
    if (focused)
      controller->rumble(ps2::pad_rumble());
  }
  if (audio_output) {
    // Paced mode: move ring data to the device without waiting.
    audio_output->pump();
    pacing_diagnostics.record_queue_frames(audio_output->queued_frames());
    if (audio_output->mode() == gt4::pacing::Mode::paced)
      pacing_diagnostics.record_ratio(audio_output->resampler_ratio());
  }
  gt4::achievements::on_vblank(game_window, ps2::g_ram, ps2::kRamSize, ps2::g_spr, ps2::kScratchpadSize);
  ps2::gs::present();
}
} // namespace

// IN-520: the recording and replay files; false stops the start, since a bench must not run with half its input.
bool open_input_files() {
  const char *record = std::getenv("GT4_INPUT_RECORD");
  const char *replay = std::getenv("GT4_INPUT_REPLAY");
  std::string error;
  if (record != nullptr && replay != nullptr) {
    std::fprintf(stderr, "[input] GT4_INPUT_RECORD and GT4_INPUT_REPLAY together, game not started\n");
    return false;
  }
  if (record != nullptr && !input_recorder.open(record, error)) {
    std::fprintf(stderr, "[input] %s, game not started\n", error.c_str());
    return false;
  }
  if (record != nullptr) std::fprintf(stderr, "[input] recording to %s; F9 ends it and closes the game\n", record);
  if (replay != nullptr) {
    input_player = std::make_unique<gt4::input_recording::Player>();
    if (!input_player->load(replay, error)) {
      std::fprintf(stderr, "[input] %s: %s, game not started\n", replay, error.c_str());
      return false;
    }
    std::fprintf(stderr, "[input] replaying %s: %zu changes up to VBlank %llu\n", replay, input_player->changes(),
                 static_cast<unsigned long long>(input_player->end()));
  }
  return true;
}

bool install_input_replay_without_window() {
  if (std::getenv("GT4_INPUT_RECORD") != nullptr) {
    std::fprintf(stderr, "[input] GT4_INPUT_RECORD needs --window, game not started\n");
    return false;
  }
  if (std::getenv("GT4_INPUT_REPLAY") == nullptr) return true;
  if (!open_input_files()) return false;
  ps2::g_options.on_vblank = [] { apply_pad(input_player->at(++input_vblank)); };
  return true;
}

bool open_window(bool fullscreen) {
  if (!open_input_files()) return false;
  // The launcher passes GT4_FULLSCREEN (its Display setting, on by default). An invalid value stops the start.
  bool start_fullscreen = fullscreen;
  try {
    start_fullscreen = start_fullscreen || gt4::window_mode::parse_start_fullscreen(std::getenv("GT4_FULLSCREEN"));
  } catch (const std::invalid_argument &error) {
    std::fprintf(stderr, "[window] %s, game not started\n", error.what());
    return false;
  }
  // An invalid GT4_CONTROLS_FILE stops the start here, before any window or guest work.
  try {
    active_controls = gt4::controls::load_controls_from_environment();
  } catch (const gt4::controls::ConfigError &error) {
    std::fprintf(stderr, "[controls] invalid controls file, game not started: %s\n",
                 error.what());
    return false;
  }
  keyboard_keys = gt4::controls::keyboard_bindings(active_controls);
  // TASK-080: an invalid GT4_DISPLAY_ASPECT, GT4_OUTPUT_SIZE or GT4_ASPECT_FOV_EXTRA stops the start here (prints and
  // exits).
  gt4::display_aspect::configured_setting();
  const gt4::display_aspect::OutputSize output = gt4::display_aspect::configured_output_size();
  std::string monitor_device;
  try {
    monitor_device = gt4::window_mode::parse_monitor(std::getenv("GT4_MONITOR"));
  } catch (const std::invalid_argument &error) {
    std::fprintf(stderr, "[window] %s, game not started\n", error.what());
    return false;
  }
  try {
    input_backend = gt4::pad::parse_backend(std::getenv("GT4_INPUT"));
  } catch (const std::invalid_argument &error) {
    std::fprintf(stderr, "[input] %s, game not started\n", error.what());
    return false;
  }
  std::fprintf(stderr, "[input] backend %s\n", gt4::pad::backend_name(input_backend));
  const char *controls_file = std::getenv("GT4_CONTROLS_FILE");
  std::fprintf(stderr, "[controls] %s\n",
               controls_file && *controls_file ? controls_file : "default bindings");
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = window_procedure;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = L"GT4NativeVulkanPilot";
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  // TASK-088: a black background, not none. With no brush Windows shows the new window white until the first frame
  // (the start before the Sony logo); the game paints every frame, so only the first frames show it.
  window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  // Icon from the executable resource (gt4vulkan.rc, IDI 1 = game.ico from launcher/assets/brand).
  window_class.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
  arrow_cursor = window_class.hCursor;
  if (!RegisterClassW(&window_class))
    throw std::runtime_error("Cannot register native window class");
  // Without GT4_MONITOR and GT4_OUTPUT_SIZE (headless tools, older launchers): a 960x540 client area where Windows puts
  // it, as before. With them (TASK-080 addendum): a client area of the output size (shrunk with its shape when it does
  // not fit the work area) centred on the chosen monitor; fullscreen covers that monitor (set_fullscreen).
  const bool placed = output.known() || !monitor_device.empty();
  if (placed)
    declare_dpi_awareness();
  RECT window_bounds{0, 0, 960, 540};
  int left = CW_USEDEFAULT, top = CW_USEDEFAULT;
  HMONITOR monitor = nullptr;
  MONITORINFO monitor_info{};
  monitor_info.cbSize = sizeof monitor_info;
  if (placed) {
    monitor = chosen_monitor(monitor_device);
    if (!GetMonitorInfoW(monitor, &monitor_info))
      throw std::runtime_error("Cannot read the chosen monitor");
    RECT frame{0, 0, 0, 0};
    AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const RECT work = monitor_info.rcWork;
    const auto client = gt4::window_mode::fit_client(
        output.known() ? output.width : 960u, output.known() ? output.height : 540u,
        (work.right - work.left) - (frame.right - frame.left), (work.bottom - work.top) - (frame.bottom - frame.top));
    window_bounds = RECT{0, 0, client.width, client.height};
  }
  if (!AdjustWindowRectEx(&window_bounds, WS_OVERLAPPEDWINDOW, FALSE, 0))
    throw std::runtime_error("Cannot size native game window");
  const int window_width = window_bounds.right - window_bounds.left;
  const int window_height = window_bounds.bottom - window_bounds.top;
  if (placed) {
    const auto box = gt4::window_mode::centred(to_rect(monitor_info.rcWork), window_width, window_height);
    left = box.left;
    top = box.top;
  }
  game_window = CreateWindowExW(
      0, window_class.lpszClassName, window_title,
      WS_OVERLAPPEDWINDOW | WS_VISIBLE, left, top, window_width, window_height, nullptr, nullptr,
      window_class.hInstance, nullptr);
  if (!game_window)
    throw std::runtime_error("Cannot create native game window");
  // Start in borderless fullscreen on the window's monitor: the same path as Alt+Enter and F11.
  if (start_fullscreen)
    set_fullscreen(true);
  RECT client{};
  if (GetClientRect(game_window, &client))
    gt4::display_aspect::set_window_client_size(static_cast<std::uint32_t>(client.right),
                                                static_cast<std::uint32_t>(client.bottom));
  std::fprintf(stderr, "[window] %s start (GT4_FULLSCREEN)\n", start_fullscreen ? "fullscreen" : "windowed");
  if (placed) {
    const RECT &area = monitor_info.rcMonitor;
    std::fprintf(stderr, "[window] monitor %s (%ldx%ld at %ld,%ld), output size %ux%u, client %ldx%ld\n",
                 monitor_device.empty() ? "primary" : monitor_device.c_str(), area.right - area.left,
                 area.bottom - area.top, area.left, area.top, output.width, output.height, client.right, client.bottom);
  }
  return true;
}

#ifdef GT4_MEMCARD_FLUSH_OVERLAY
namespace ps2::iop::sio2 {
void flush_cards_now(); // memcard capsule sio2.cpp (scripts/prepare_memcard_flush.py)
}
#endif

int run_window(const std::function<int()> &emulate) {
  ControllerInput input(active_controls, input_backend);
  controller = &input;
  const auto cleanup = [&]() {
    input_recorder.finish(input_vblank);
    gt4::achievements::shutdown();
#ifdef GT4_PERF_PROBE
    gt4::perf::Probe::shared().set_report_section({});
    gt4::perf::Probe::shared().finish();
#endif
    audio_output = nullptr;
    frame_pacer.reset();
    frame_clock.reset();
#ifdef GT4_SHAPE_CAPTURE
    if (capture) {
      capture->finish();
      capture.reset();
    }
#endif
    ps2::set_audio_sink({});
    ps2::g_options.on_vblank = {};
    input.stop();
    controller = nullptr;
    surface_attached = false;
    if (IsWindow(game_window))
      DestroyWindow(game_window);
    game_window = nullptr;
  };
  try {
    force_480p_enabled = gt4::display::parse_force_480p(std::getenv("GT4_FORCE_480P"));
    force_480p_state = {};
    if (force_480p_enabled)
      std::fprintf(stderr, "[display] GT4_FORCE_480P=1: race display index held at 480p (guest VBlank)\n");
    // TASK-079: an invalid GT4_FORCE_WIDESCREEN stops the start here (the hold itself runs at guest VBlank).
    gt4::display::widescreen_hold_enabled();
    pacing_mode = gt4::pacing::parse_mode(std::getenv("GT4_PACING"));
    cursor_autohide = gt4::cursor::AutoHide(gt4::cursor::parse_policy(std::getenv("GT4_CURSOR_AUTOHIDE")));
    std::fprintf(stderr, "[window] cursor auto-hide %s (GT4_CURSOR_AUTOHIDE; %u ms without movement over the picture)\n",
                 cursor_autohide.policy().auto_hide ? "on" : "off", cursor_autohide.policy().hide_delay_ms);
#ifdef GT4_GS_STREAM_CLIENT
    gt4::gs_stream::set_cursor_policy(cursor_autohide.policy().auto_hide, cursor_autohide.policy().hide_delay_ms);
#endif
    frame_clock = std::make_unique<FrameClock>();
    frame_pacer = std::make_unique<gt4::pacing::FramePacer>(
        frame_clock->frequency(), gt4::pacing::guest_vblank_rate);
    pacing_diagnostics.reset();
    counters_at_last_report = {};
    std::fprintf(stderr, "[pacing] mode=%s; guest VBlank %llu Hz; timer=%s\n",
                 gt4::pacing::mode_name(pacing_mode),
                 static_cast<unsigned long long>(gt4::pacing::guest_vblank_rate),
                 frame_clock->high_resolution_timer() ? "high-resolution" : "1 ms");
#ifdef GT4_SHAPE_CAPTURE
    std::array<wchar_t, 32768> output{};
    const auto length = GetEnvironmentVariableW(L"GT4_OBSERVATION_OUTPUT", output.data(),
                                               static_cast<DWORD>(output.size()));
    if (length >= output.size()) throw std::runtime_error("Capture output path exceeds the host limit");
    if (length) {
      capture = std::make_unique<gt4::observation::CaptureControl>(
          gt4::observation::runtime_observer(), std::filesystem::path(output.data()));
    } else {
      std::fputs("[shape-capture] disabled: no explicit GT4_OBSERVATION_OUTPUT\n", stderr);
    }
#endif
    const auto lock_path = ps2::g_options.memcard_path + ".lock";
    HANDLE lock_handle =
        CreateFileA(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock_handle == INVALID_HANDLE_VALUE)
      throw std::runtime_error("Cannot exclusively lock game memory card: " +
                               std::to_string(GetLastError()));
    const std::unique_ptr<void, decltype(&CloseHandle)> card_lock(lock_handle,
                                                                  &CloseHandle);
    ps2::init_graphics();
    RECT bounds{};
    if (!GetClientRect(game_window, &bounds))
      throw std::runtime_error("Cannot inspect native window dimensions");
    if (!ps2::gs::attach_window(3, nullptr,
                                reinterpret_cast<std::uint64_t>(game_window),
                                nullptr, nullptr, bounds.right, bounds.bottom))
      throw std::runtime_error("Native Vulkan presentation refused window");
    surface_attached = true;
    // The presentation is 16:9 pillarbox; TASK-080 fills the screen in the race view at other ratios.
    ps2::gs::gpu::set_widescreen(true);
    gt4::display_aspect::set_presentation_sink(present_race_view);
    std::fprintf(stderr,
                 "[window] real guest GPU output through native Vulkan\n");
    // shortcut: one thread and FIFO presentation, qualify pipelining later.
    ps2::g_options.on_vblank = on_vblank;
    gt4::achievements::initialize(game_window, ps2::g_ram, ps2::kRamSize, ps2::g_spr, ps2::kScratchpadSize);
    AudioOutput audio(pump_messages, pacing_mode);
    audio_output = &audio;
#ifdef GT4_PERF_PROBE
    gt4::perf::Probe::shared().set_report_section([](std::FILE *output) {
      // F8 report: counter differences since the last F7 or report, then totals.
      const gt4::pacing::Counters session = pacing_counters();
      pacing_diagnostics.format(output, pacing_mode,
                                session.since(counters_at_last_report), session);
      if (force_480p_enabled)
        std::fprintf(output, "[display] GT4_FORCE_480P: race display index writes %llu\n",
                     static_cast<unsigned long long>(force_480p_state.writes));
      counters_at_last_report = session;
      pacing_diagnostics.reset();
#ifdef GT4_MTVU_MEASURE
      // TASK-011 step 0: interval counters since the last F7 or report, then a new interval.
      gt4::mtvu::report(output, "interval");
      gt4::mtvu::reset_interval();
#endif
    });
#endif
    ps2::set_audio_sink(
        [&audio](const std::int16_t *samples, std::size_t frames) {
          if (!audio.push(samples, frames))
            ps2::halt("window closed");
        });
    int result = 1;
    try {
      result = emulate();
    } catch (...) {
      ps2::set_audio_sink({});
      throw;
    }
    // Exit path (TASK-025 Part A / TASK-058): a save made less than 50 VBlanks before the window closed, including the
    // GS-process loss path, is written now. emulate() returns normally after halt(), so this runs on every exit.
#ifdef GT4_MEMCARD_FLUSH_OVERLAY
    ps2::iop::sio2::flush_cards_now();
#endif
    ps2::set_audio_sink({});
    audio.close(!closing && result == 0);
    audio_output = nullptr;
    cleanup();
    return result;
  } catch (const std::exception &error) {
    cleanup();
    std::fprintf(stderr, "[host] audio/input/window failure: %s\n",
                 error.what());
    return 1;
  }
}
