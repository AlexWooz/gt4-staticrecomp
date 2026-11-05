#include "wheel.hpp"
#include "host_io.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *label) {
  if (!condition)
    throw std::runtime_error(label);
}

void require_rejects(std::string_view toml, const char *label) {
  bool rejected = false;
  try {
    (void)gt4::wheel::parse_wheel_config(toml, "bad.toml");
  } catch (const gt4::wheel::ConfigError &) {
    rejected = true;
  }
  require(rejected, label);
}

void test_presets_and_roundtrip() {
  using namespace gt4::wheel;
  for (const char *preset :
       {"generic", "hori", "logitech_g29", "logitech_dfgt", "thrustmaster", "fanatec_moza", "combined_pedals"}) {
    const WheelConfig cfg = preset_wheel_config(preset);
    const std::string text = serialize_wheel_config(cfg);
    const WheelConfig parsed = parse_wheel_config(text, std::string(preset) + ".toml");
    require(parsed.version == 1, "preset version");
    require(parsed.enabled == cfg.enabled, "preset enabled");
    require(parsed.preset == cfg.preset, "preset name roundtrip");
    require(parsed.pedal_map_mode == cfg.pedal_map_mode, "preset pedal_map_mode roundtrip");
    require(parsed.steering.source == cfg.steering.source, "preset steering source roundtrip");
    require(parsed.steering.rotation_degrees == cfg.steering.rotation_degrees, "preset rotation_degrees roundtrip");
    require(parsed.steering.effective_degrees == cfg.steering.effective_degrees, "preset effective_degrees roundtrip");
    require(parsed.throttle.source == cfg.throttle.source, "preset throttle source roundtrip");
    require(parsed.throttle.mode == cfg.throttle.mode, "preset throttle mode roundtrip");
    require(parsed.brake.source == cfg.brake.source, "preset brake source roundtrip");
    require(parsed.brake.mode == cfg.brake.mode, "preset brake mode roundtrip");
    require(parsed.buttons == cfg.buttons, "preset buttons roundtrip");
  }
}

void test_steering_math() {
  using namespace gt4::wheel;
  SteeringConfig cfg{};
  cfg.rotation_degrees = 900;
  cfg.effective_degrees = 900;
  cfg.deadzone = 0.0;
  cfg.linearity = Curve::linear;
  cfg.sensitivity = 1.0;
  cfg.invert = false;

  require(read_steering(cfg, 0) == 128, "steering center");
  require(read_steering(cfg, -32768) == 0, "steering full left");
  require(read_steering(cfg, 32767) == 255, "steering full right");

  int prev = 0;
  for (int raw = -32768; raw <= 32767; ++raw) {
    const int byte = read_steering(cfg, static_cast<std::int16_t>(raw));
    require(byte >= prev, "steering monotonic across full 16-bit travel");
    prev = byte;
  }

  // Effective rotation lock: 540 deg effective on a 900 deg wheel reaches full lock at 60% physical travel.
  cfg.effective_degrees = 540;
  require(read_steering(cfg, static_cast<std::int16_t>(32767 * 60 / 100)) == 255,
          "effective lock reaches full right at 60% travel");
  require(read_steering(cfg, static_cast<std::int16_t>(-32768 * 60 / 100)) == 0,
          "effective lock reaches full left at 60% travel");

  // Invert flips direction.
  cfg.effective_degrees = 900;
  cfg.invert = true;
  require(read_steering(cfg, -32768) == 255, "inverted steering full left -> 255");
  require(read_steering(cfg, 32767) == 0, "inverted steering full right -> 0");

  // Deadzone holds center.
  cfg.invert = false;
  cfg.deadzone = 0.10;
  require(read_steering(cfg, 3000) == 128, "steering inside deadzone is 128");
  require(read_steering(cfg, 4000) > 128, "steering outside deadzone moves right");
}

void test_pedal_modes() {
  using namespace gt4::wheel;
  PedalConfig p{};
  p.source = {SourceKind::axis, {}, 1, HatDirection::up};
  p.deadzone = 0.02;
  p.saturation = 0.98;
  p.curve = Curve::linear;

  // 1. Inverted (+32767 rest -> -32768 full)
  p.mode = PedalAxisMode::inverted;
  require(read_pedal(p, 32767) == 0, "inverted pedal rest at +32767");
  require(read_pedal(p, 0) >= 126 && read_pedal(p, 0) <= 129, "inverted pedal half travel at 0");
  require(read_pedal(p, -32768) == 255, "inverted pedal full press at -32768");

  // 2. Normal (-32768 rest -> +32767 full)
  p.mode = PedalAxisMode::normal;
  require(read_pedal(p, -32768) == 0, "normal pedal rest at -32768");
  require(read_pedal(p, 0) >= 126 && read_pedal(p, 0) <= 129, "normal pedal half travel at 0");
  require(read_pedal(p, 32767) == 255, "normal pedal full press at +32767");

  // 3. Positive (0 rest -> +32767 full)
  p.mode = PedalAxisMode::positive;
  require(read_pedal(p, -10000) == 0, "positive pedal ignores negative half");
  require(read_pedal(p, 0) == 0, "positive pedal rest at 0");
  require(read_pedal(p, 32767) == 255, "positive pedal full at +32767");

  // 4. Combined negative / positive (single shared axis)
  p.mode = PedalAxisMode::combined_negative;
  require(read_pedal(p, 20000) == 0, "combined_negative ignores positive half");
  require(read_pedal(p, -32768) == 255, "combined_negative full at -32768");

  p.mode = PedalAxisMode::combined_positive;
  require(read_pedal(p, -20000) == 0, "combined_positive ignores negative half");
  require(read_pedal(p, 32767) == 255, "combined_positive full at +32767");
}

void test_multi_device_and_pedal_map_modes() {
  using namespace gt4::wheel;

  const std::string base_guid = "030000006d0400004fc2000000000000";
  const std::string pedals_guid = "03000000eb0300000120000000000000";
  const std::string shifter_guid = "030000004f04000060b6000000000000";

  const std::string toml =
      "[wheel]\n"
      "version = 1\n"
      "enabled = true\n"
      "device_guid = \"" + base_guid + "\"\n"
      "pedal_map_mode = \"both\"\n"
      "[steering]\n"
      "source = \"axis:0\"\n"
      "rotation_degrees = 900\n"
      "effective_degrees = 900\n"
      "[throttle]\n"
      "source = \"dev:" + pedals_guid + ":axis:0\"\n"
      "mode = \"normal\"\n"
      "[brake]\n"
      "source = \"dev:" + pedals_guid + ":axis:1\"\n"
      "mode = \"normal\"\n"
      "[handbrake]\n"
      "source = \"dev:" + shifter_guid + ":axis:0\"\n"
      "mode = \"positive\"\n"
      "[buttons]\n"
      "shift_up = [\"btn:4\", \"dev:" + shifter_guid + ":btn:0\"]\n"
      "shift_down = [\"btn:5\", \"dev:" + shifter_guid + ":btn:1\"]\n"
      "look_back = [\"hat:0:down\"]\n";

  const WheelConfig cfg = parse_wheel_config(toml, "multi.toml");

  DeviceReading wheelbase{};
  wheelbase.guid = base_guid;
  wheelbase.name = "Logitech G29 Driving Force Racing Wheel";
  wheelbase.is_wheel = true;
  wheelbase.axes = {16384}; // ~50% right turn
  wheelbase.buttons = {false, false, false, false, false, false};
  wheelbase.hats = {0x04}; // hat down -> look_back (L1, bit 10)

  DeviceReading pedals{};
  pedals.guid = pedals_guid;
  pedals.name = "USB Load Cell Pedals";
  pedals.is_wheel = false;
  pedals.axes = {32767, 0}; // full throttle, ~50% brake

  DeviceReading shifter{};
  shifter.guid = shifter_guid;
  shifter.name = "USB Shifter + Handbrake";
  shifter.is_wheel = false;
  shifter.axes = {32767}; // full handbrake
  shifter.buttons = {true, false}; // shift_up pressed on external shifter

  bool active = false;
  const PadState state = map_wheel_devices(cfg, {wheelbase, pedals, shifter}, &active);
  require(active, "multi-device wheel state active");
  require(state.axes[2] > 180 && state.axes[2] < 200, "wheelbase steering mapped to lstick_x");
  require((state.buttons & (1u << 14)) != 0 && state.pressures[14] == 255, "external pedal full throttle -> Cross");
  require((state.buttons & (1u << 9)) != 0 && state.pressures[9] == 255, "external pedal full throttle + shift_up -> R2");
  require((state.buttons & (1u << 15)) != 0 && state.pressures[15] >= 120 && state.pressures[15] <= 135,
          "external pedal half brake -> Square pressure");
  require((state.buttons & (1u << 8)) != 0 && state.pressures[8] >= 120 && state.pressures[8] <= 135,
          "external pedal half brake -> L2 pressure");
  require((state.buttons & (1u << 13)) != 0 && state.pressures[13] == 255,
          "external handbrake axis -> Circle pressure");
  require((state.buttons & (1u << 10)) != 0 && state.pressures[10] == 255,
          "hat down -> L1 look_back");

  // Also check rstick_y pedal map mode:
  WheelConfig rstick_cfg = cfg;
  rstick_cfg.pedal_map_mode = PedalMapMode::rstick_y;
  pedals.axes = {32767, -32768}; // full throttle, zero brake
  const PadState rstick_throttle = map_wheel_devices(rstick_cfg, {wheelbase, pedals, shifter});
  require(rstick_throttle.axes[1] == 0, "rstick_y mode full throttle -> axis 1 == 0");
  pedals.axes = {-32768, 32767}; // zero throttle, full brake
  const PadState rstick_brake = map_wheel_devices(rstick_cfg, {wheelbase, pedals, shifter});
  require(rstick_brake.axes[1] == 255, "rstick_y mode full brake -> axis 1 == 255");
}

void test_opt_in_and_device_identity() {
  using namespace gt4::wheel;
  const char *inherited = std::getenv("GT4_WHEEL_FILE");
  const std::string saved = inherited ? inherited : "";
  _putenv_s("GT4_WHEEL_FILE", "");
  bool explicit_file = true;
  const auto disabled = load_wheel_from_environment(&explicit_file);
  _putenv_s("GT4_WHEEL_FILE", saved.c_str());
  require(!disabled.enabled && !explicit_file, "no explicit wheel file means disabled; never read APPDATA");
  require(!default_wheel_config().enabled, "default native configuration is opt-in");
  require(!parse_wheel_config("[wheel]\nversion = 1\n", "disabled-default").enabled,
          "configuration omitting enabled does not implicitly activate hardware");
  for (const char *name : {"USB Gamepad", "DualSense Wireless Controller", "Thrustmaster HOTAS", "Logitech F310", "USB Pedals"}) {
    require(!is_candidate_wheel_device(false, false, name, "030000006d0400001dc2000000000000"),
            "vendor, six axes and non-gamepad joystick status do not establish wheel identity");
  }
  require(is_candidate_wheel_device(false, true, "HORI Racing Wheel Apex"), "named XInput wheel recognized");
  require(is_candidate_wheel_device(true, false, "Unknown USB Device"), "SDL wheel type recognized");
  DeviceReading unknown{};
  unknown.guid = "11111111111111111111111111111111";
  unknown.name = "USB Gamepad";
  unknown.is_gamepad = true;
  unknown.axes = std::vector<std::int16_t>(6, 0);
  auto cfg = default_wheel_config();
  cfg.enabled = true;
  require(select_primary_device(cfg, {unknown}) == -1, "six-axis gamepad never automatically selected");
  cfg.device_guid = unknown.guid;
  require(select_primary_device(cfg, {unknown}) == 0, "explicit GUID permits manually configured generic device");
  require(select_primary_device(cfg, {unknown, unknown}) == -1, "duplicate explicit GUID is ambiguous, no primary target");
  cfg.steering.source.device_guid = unknown.guid;
  cfg.throttle.source.device_guid = unknown.guid;
  cfg.brake.source = {};
  for (auto &slot : cfg.buttons) slot.clear();
  const auto ambiguous = map_wheel_devices(cfg, {unknown, unknown});
  require(ambiguous.buttons == 0 && ambiguous.axes[2] == 128, "duplicate auxiliary GUID reads neutral");
}

struct VirtualDevice {
  SDL_JoystickID id = 0;
  SDL_Joystick *joystick = nullptr;
  VirtualDevice(const char *name, SDL_JoystickType type, Uint16 product) {
    SDL_VirtualJoystickDesc description{};
    SDL_INIT_INTERFACE(&description);
    description.type = type;
    description.name = name;
    description.vendor_id = 0xdead;
    description.product_id = product;
    description.naxes = SDL_GAMEPAD_AXIS_COUNT;
    description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    description.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    description.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    id = SDL_AttachVirtualJoystick(&description);
    require(id != 0, "attach owned virtual device");
    joystick = SDL_OpenJoystick(id);
    require(joystick != nullptr, "open owned virtual device");
  }
  std::string guid() const {
    char text[64]{};
    SDL_GUIDToString(SDL_GetJoystickGUID(joystick), text, sizeof(text));
    return text;
  }
  void detach() {
    if (id) SDL_DetachVirtualJoystick(id);
    id = 0;
  }
  ~VirtualDevice() {
    if (joystick) SDL_CloseJoystick(joystick);
    if (id) SDL_DetachVirtualJoystick(id);
  }
};

void test_owned_routing_disconnect_and_haptic_stop() {
  using namespace gt4::wheel;
  require(SDL_InitSubSystem(SDL_INIT_GAMEPAD), "virtual routing SDL initialization");
  struct Cleanup { ~Cleanup() { SDL_QuitSubSystem(SDL_INIT_GAMEPAD); } } cleanup;
  VirtualDevice auxiliary("GT4 fixture standalone USB pedals", SDL_JOYSTICK_TYPE_UNKNOWN, 101);
  VirtualDevice hori("GT4 fixture HORI Racing Wheel", SDL_JOYSTICK_TYPE_GAMEPAD, 102);
  VirtualDevice xbox("GT4 fixture Xbox Controller", SDL_JOYSTICK_TYPE_GAMEPAD, 103);
  require(SDL_IsGamepad(hori.id) && SDL_IsGamepad(xbox.id), "owned virtual wheel and second controller both SDL gamepads");
  SDL_SetJoystickVirtualAxis(hori.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
  SDL_SetJoystickVirtualAxis(hori.joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
  SDL_SetJoystickVirtualAxis(xbox.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
  SDL_SetJoystickVirtualAxis(xbox.joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
  SDL_SetJoystickVirtualButton(xbox.joystick, SDL_GAMEPAD_BUTTON_EAST, true);

  auto config = default_wheel_config();
  config.enabled = true;
  config.device_guid = hori.guid();
  config.throttle.source = {SourceKind::axis, {}, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER};
  config.throttle.mode = PedalAxisMode::normal;
  config.brake.source = {SourceKind::axis, {}, SDL_GAMEPAD_AXIS_LEFT_TRIGGER};
  config.brake.mode = PedalAxisMode::normal;
  config.handbrake.source.device_guid = auxiliary.guid();
  config.handbrake.source.index = 0;
  config.handbrake.mode = PedalAxisMode::positive;
  for (auto &slot : config.buttons) slot.clear();

  int gain = 0, center = 0, stops = 0, rumble_stops = 0;
  std::uint16_t low = 0, high = 0;
  HapticOutput output;
  output.gain = [&](int value) { gain = value; };
  output.autocenter = [&](int value) { center = value; };
  output.stop_all = [&] { ++stops; };
  output.stop_rumble = [&] { ++rumble_stops; };
  output.rumble = [](float) {};
  output.joystick_rumble = [&](std::uint16_t a, std::uint16_t b) { low = a; high = b; };
  WheelInput wheel(config, true, output);
  // Local port: the SDL backend is the default since TASK-075; the XInput-to-SDL switch of the 1.2.x bridge is not kept.
  ControllerInput controller(gt4::controls::default_controls(), gt4::pad::Backend::sdl);
  wheel.prepare(true);
  require(wheel.primary_instance_id() == hori.id, "haptic primary uses owned wheel instance ID");
  auto exclusions = wheel.owned_device_ids();
  require(std::find(exclusions.begin(), exclusions.end(), hori.id) != exclusions.end(), "production ownership contains the virtual HORI");
  // Filter every non-fixture gamepad without opening or commanding it.
  int count = 0;
  auto *ids = SDL_GetGamepads(&count);
  for (int i = 0; ids && i < count; ++i)
    if (ids[i] != xbox.id && ids[i] != hori.id && ids[i] != auxiliary.id) exclusions.push_back(ids[i]);
  SDL_free(ids);
  controller.set_excluded_devices(exclusions);
  SDL_SetJoystickVirtualAxis(hori.joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
  wheel.prepare(true);
  auto pad = controller.poll(true, static_cast<std::uint16_t>(1u << 3));
  wheel.merge_prepared(pad);
  require(pad.pressures[14] == 255, "owned wheel throttle drives Cross");
  require(pad.pressures[13] == 255, "separate Xbox gamepad remains routed through SDL");
  require((pad.buttons & (1u << 3)) != 0, "keyboard remains available with wheel ownership");
  require(pad.pressures[8] == 0 && pad.pressures[9] == 0,
          "wheel trigger never also generates parallel gamepad shift buttons");

  wheel.rumble(true, 0);
  require(center > 0 && gain > 0, "focused centering can be active with raw game rumble zero");
  wheel.prepare(false);
  require(center == 0 && gain == 0 && low == 0 && high == 0 && stops > 0 && rumble_stops > 0,
          "focus loss stops autocenter, gain, effects and both rumble outputs even with raw zero");
  wheel.prepare(true);
  wheel.rumble(true, 0xff01);
  require(center > 0 && low > 0 && high > 0, "focused resume restores configured feedback through fake sink");
  wheel.stop();
  require(center == 0 && gain == 0 && low == 0 && high == 0, "explicit stop clears nonzero feedback");

  auto zero_config = config;
  zero_config.ffb.strength = 0.0;
  {
    WheelInput zero_strength(zero_config, true, output);
    zero_strength.prepare(true);
    zero_strength.rumble(true, 0xff01);
    require(center == 0 && gain == 0 && low == 0 && high == 0,
            "zero master strength produces no gain, centering or rumble through production output policy");
  }
  auxiliary.detach();
  wheel.prepare(true); // Before the periodic two-second scan, filtered snapshot is shorter.
  require(wheel.primary_instance_id() == hori.id, "earlier-device disconnect cannot retarget the wheel");
  hori.detach();
  wheel.prepare(true);
  require(!wheel.has_active_wheel(), "wheel detach clears primary immediately between scans");
  require(center == 0 && gain == 0 && low == 0 && high == 0, "detach clears all feedback through the production stop path");
  require(wheel.owned_device_ids().empty(), "detached wheel and auxiliary own no gamepad instances");
  controller.set_excluded_devices({});
  require(controller.backend() == gt4::pad::Backend::sdl, "SDL backend unchanged after ownership ends");
  VirtualDevice reconnected("GT4 fixture HORI Racing Wheel", SDL_JOYSTICK_TYPE_GAMEPAD, 102);
  require(reconnected.guid() == config.device_guid, "same virtual model reconnects under its persisted GUID");
  SDL_SetJoystickVirtualAxis(reconnected.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
  SDL_SetJoystickVirtualAxis(reconnected.joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
  SDL_Delay(2010); // Exercise the normal production rescan interval, not a test-only scan hook.
  wheel.prepare(true);
  require(wheel.primary_instance_id() == reconnected.id, "reconnect acquires the new instance ID");
  wheel.rumble(true, 0);
  require(center > 0, "focused reconnect restores only the newly selected wheel through the fake sink");
  wheel.stop();
}

void test_gamepad_exclusion_and_validation() {
  using namespace gt4::wheel;

  // A standard Xbox/PlayStation gamepad is not auto-selected when device_guid is empty.
  DeviceReading xbox_pad{};
  xbox_pad.guid = "030000005e0400008e02000000007801";
  xbox_pad.name = "Xbox 360 Controller";
  xbox_pad.is_wheel = false;
  xbox_pad.is_gamepad = true;
  xbox_pad.axes = {32767, -32768, -32768};

  WheelConfig auto_cfg = default_wheel_config();
  auto_cfg.enabled = true;
  require(!is_candidate_wheel_device(false, true, "Xbox 360 Controller"),
          "standard gamepad is not a candidate wheel");
  require(select_primary_device(auto_cfg, {xbox_pad}) == -1,
          "auto-detect skips standard gamepad");

  // Validation errors:
  require_rejects("[wheel]\nversion = 2\n", "reject unsupported version");
  require_rejects("[wheel]\nenabled = true\n", "reject missing version");
  require_rejects("[wheel]\nversion = 1\n[steering]\nrotation_degrees = 90\n", "reject rotation < 180");
  require_rejects("[wheel]\nversion = 1\n[throttle]\ndeadzone = 0.40\nsaturation = 0.42\n",
                  "reject saturation too close to deadzone");
  require_rejects("[wheel]\nversion = 1\n[buttons]\ncross = [\"axis:0\"]\n",
                  "reject raw axis without +/- threshold on button");
}

void test_live_sdl_virtual_wheel() {
  using namespace gt4::wheel;

  require(SDL_InitSubSystem(SDL_INIT_JOYSTICK), "SDL_InitSubSystem(SDL_INIT_JOYSTICK)");
  SDL_VirtualJoystickDesc desc{};
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_WHEEL;
  desc.naxes = 4;
  desc.nbuttons = 12;
  desc.nhats = 1;
  desc.name = "GT4 Virtual Racing Wheel";

  const SDL_JoystickID virt_id = SDL_AttachVirtualJoystick(&desc);
  require(virt_id != 0, "SDL_AttachVirtualJoystick succeeded");

  SDL_Joystick *virt_joy = SDL_OpenJoystick(virt_id);
  require(virt_joy != nullptr, "SDL_OpenJoystick on virtual wheel");

  // Set initial rest state: steering centered (0), inverted pedals at rest (+32767).
  SDL_SetJoystickVirtualAxis(virt_joy, 0, 0);
  SDL_SetJoystickVirtualAxis(virt_joy, 1, 32767);
  SDL_SetJoystickVirtualAxis(virt_joy, 2, 32767);
  SDL_SetJoystickVirtualAxis(virt_joy, 3, 32767);
  SDL_UpdateJoysticks();

  char guid_buf[64]{};
  SDL_GUIDToString(SDL_GetJoystickGUID(virt_joy), guid_buf, sizeof(guid_buf));

  WheelConfig cfg = default_wheel_config();
  cfg.enabled = true;
  cfg.device_guid = guid_buf;
  WheelInput wheel_input(cfg, true);

  PadState pad{};
  wheel_input.poll_and_merge(true, pad);
  require(wheel_input.has_active_wheel(), "WheelInput connected to virtual wheel");
  require(pad.axes[2] == 128 && pad.buttons == 0, "virtual wheel rest state is neutral");

  // Turn wheel full left (-32768), press throttle full (-32768), press right paddle (btn 4 -> R2).
  SDL_SetJoystickVirtualAxis(virt_joy, 0, -32768);
  SDL_SetJoystickVirtualAxis(virt_joy, 1, -32768);
  SDL_SetJoystickVirtualButton(virt_joy, 4, true);
  SDL_SetJoystickVirtualHat(virt_joy, 0, SDL_HAT_UP);

  pad = {};
  wheel_input.poll_and_merge(true, pad);
  require(pad.axes[2] == 0, "virtual wheel full left steering -> lstick_x = 0");
  require((pad.buttons & (1u << 14)) != 0 && pad.pressures[14] == 255,
          "virtual wheel full throttle -> Cross pressure 255");
  require((pad.buttons & (1u << 9)) != 0 && pad.pressures[9] == 255,
          "virtual wheel right paddle -> R2 Shift Up");
  require((pad.buttons & (1u << 4)) != 0 && pad.pressures[4] == 255,
          "virtual wheel hat up -> D-Pad Up");

  // Unfocused window leaves pad untouched and stops rumble.
  PadState unfocused{};
  wheel_input.poll_and_merge(false, unfocused);
  require(unfocused.buttons == 0 && unfocused.axes[2] == 128, "unfocused poll leaves pad neutral");

  SDL_CloseJoystick(virt_joy);
  SDL_DetachVirtualJoystick(virt_id);
  SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}

} // namespace

int main() {
  try {
    test_opt_in_and_device_identity();
    test_owned_routing_disconnect_and_haptic_stop();
    test_presets_and_roundtrip();
    test_steering_math();
    test_pedal_modes();
    test_multi_device_and_pedal_map_modes();
    test_gamepad_exclusion_and_validation();
    test_live_sdl_virtual_wheel();
    std::fprintf(stderr, "PASS: wheel presets, steering lock/linearity, 5 pedal modes, multi-device mapping, "
                         "SDL3 virtual wheel\n");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
