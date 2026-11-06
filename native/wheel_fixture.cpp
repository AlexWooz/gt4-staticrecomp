// CTest native_wheel: the racing wheel of the native Driving Force Pro path (native/wheel.hpp, native/dfp_haptics.hpp).
// wheel.toml parsing (mode, the removed pad bridge's keys, refused files), pedal calibration, the DFP input a device
// snapshot gives (steering on the game's range, button bits, hat), the force feedback effect lifecycle on a fake haptic
// sink (create, update only what changed, stop once, one owner thread), and WheelInput on an SDL3 virtual wheel.
#include "wheel.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace gt4::wheel;

void require(bool condition, const char *label) {
  if (!condition)
    throw std::runtime_error(label);
}

void require_rejects(std::string_view toml, const char *label) {
  bool rejected = false;
  try {
    (void)parse_wheel_config(toml, "bad.toml");
  } catch (const ConfigError &) {
    rejected = true;
  }
  require(rejected, label);
}

// ---------------------------------------------------------------------------------------------------- configuration

void test_mode_and_legacy_keys() {
  // No mode key: the DFP path (the only one).
  const WheelConfig plain = parse_wheel_config("[wheel]\nversion = 1\nenabled = true\n", "plain");
  require(plain.enabled, "a file without mode enables the wheel");
  require(parse_wheel_config("[wheel]\nversion = 1\nmode = \"dfp\"\n", "dfp").version == 1, "mode dfp accepted");
  require_rejects("[wheel]\nversion = 1\nmode = \"pad\"\n", "the removed pad bridge mode is refused");
  require_rejects("[wheel]\nversion = 1\nmode = \"other\"\n", "an unknown mode is refused");
  // A file of the 1.2.x launcher: every key of the pad bridge still loads and is ignored.
  const WheelConfig legacy = parse_wheel_config("# 1.2.x\n[wheel]\nversion = 1\nenabled = true\n"
                                                "device_guid = \"\"\ndevice_name = \"\"\npreset = \"logitech_g29\"\n"
                                                "pedal_map_mode = \"both\"\n[steering]\nsource = \"axis:0\"\n"
                                                "rotation_degrees = 900\neffective_degrees = 540\ndeadzone = 0.020\n"
                                                "linearity = \"quadratic\"\nsensitivity = 1.200\ninvert = false\n"
                                                "[ffb]\nenabled = true\nstrength = 0.800\ncentering_spring = 0.250\n"
                                                "damper = 0.150\ncurb_rumble = 1.000\ninvert = false\n"
                                                "[buttons]\nr2 = [\"btn:4\"]\nshift_down = [\"btn:5\"]\n",
                                                "legacy");
  require(legacy.enabled && legacy.steering.rotation_degrees == 900, "1.2.x file loads");
  require(std::abs(legacy.ffb.strength - 0.8) < 1e-9, "1.2.x force strength kept");
  require(legacy.ffb.centering_spring, "a 1.2.x centering_spring amount still parses: nonzero means game");
  require(!parse_wheel_config("[wheel]\nversion = 1\n[ffb]\ncentering_spring = \"off\"\n", "spring-off")
               .ffb.centering_spring,
          "centering_spring off");
  require(parse_wheel_config("[wheel]\nversion = 1\n[ffb]\ncentering_spring = \"game\"\n", "spring-game")
              .ffb.centering_spring,
          "centering_spring game");
  require(parse_wheel_config("[wheel]\nversion = 1\n[ffb]\ncentring_spring = \"game\"\n", "spring-uk")
              .ffb.centering_spring,
          "the British spelling parses too");
  require(!parse_wheel_config("[wheel]\nversion = 1\n[ffb]\ncentering_spring = 0.0\n", "spring-zero")
               .ffb.centering_spring,
          "a 1.2.x zero means off");
  require(!default_wheel_config().ffb.centering_spring, "the game's centring springs are off by default (TASK-192)");
  require_rejects("[wheel]\nversion = 1\n[ffb]\ncentering_spring = \"maybe\"\n", "an unknown spring token is refused");
  require_rejects("[wheel]\nversion = 1\n[ffb]\ncentering_spring = 4\n", "an out-of-range spring amount is refused");
  require(legacy.buttons[10].size() == 1 && legacy.buttons[10][0].index == 5, "shift_down alias fills the l2 slot");
  require_rejects("[wheel]\nversion = 1\nunknown = 1\n", "unknown [wheel] key refused");
  require_rejects("[wheel]\nversion = 2\n", "reject unsupported version");
  require_rejects("[wheel]\nenabled = true\n", "reject missing version");
  require_rejects("[wheel]\nversion = 1\n[steering]\nrotation_degrees = 90\n", "reject rotation < 180");
  require_rejects("[wheel]\nversion = 1\n[throttle]\ndeadzone = 0.40\nsaturation = 0.42\n",
                  "reject saturation too close to deadzone");
  require_rejects("[wheel]\nversion = 1\n[buttons]\ncross = [\"axis:0\"]\n", "reject raw axis without +/- on a button");
  require_rejects("[wheel]\nversion = 1\n[ffb]\nstrength = 3\n", "reject strength above 2");
  require(!parse_wheel_config("[wheel]\nversion = 1\n", "disabled-default").enabled,
          "a file omitting enabled does not activate hardware");
  require(!default_wheel_config().enabled, "default native configuration is opt-in");

  // Round trip through the serializer.
  WheelConfig config = default_wheel_config();
  config.enabled = true;
  config.device_guid = "030000006d0400009bc2000000000000";
  config.device_name = "Logitech G27 Racing Wheel";
  config.preset = "logitech_legacy";
  config.steering.rotation_degrees = 900;
  config.steering.invert = true;
  config.throttle.source = parse_source_ref("axis:2", "x", 1);
  config.clutch.source = parse_source_ref("axis:1", "x", 1);
  config.ffb.strength = 0.65;
  config.ffb.invert = true;
  config.ffb.centering_spring = true;
  config.buttons[4] = {parse_source_ref("dev:0300000000000000000000000000beef:btn:3", "x", 1)};
  const WheelConfig again = parse_wheel_config(serialize_wheel_config(config), "roundtrip");
  require(again.enabled && again.device_guid == config.device_guid && again.device_name == config.device_name,
          "round trip keeps the wheel identity");
  require(again.steering.invert && again.throttle.source == config.throttle.source &&
              again.clutch.source == config.clutch.source && again.buttons[4] == config.buttons[4],
          "round trip keeps sources");
  require(std::abs(again.ffb.strength - 0.65) < 1e-9 && again.ffb.invert, "round trip keeps force settings");
  require(again.ffb.centering_spring, "round trip keeps the centring spring choice");
  require(serialize_wheel_config(config).find("centering_spring = \"game\"") != std::string::npos,
          "the serializer writes the centring spring as a token");
  require(serialize_wheel_config(config).find("mode = \"dfp\"") != std::string::npos, "the serializer writes mode dfp");
}

void test_refused_file_runs_without_wheel() {
  const std::filesystem::path folder = std::filesystem::temp_directory_path() / "gt4-wheel-fixture";
  std::filesystem::create_directories(folder);
  const std::filesystem::path bad = folder / "bad-wheel.toml";
  { std::ofstream(bad) << "[wheel]\nversion = 1\nenabled = true\nmode = \"pad\"\n"; }
  char *inherited = nullptr;
  std::size_t length = 0;
  _dupenv_s(&inherited, &length, "GT4_WHEEL_FILE");
  const std::string saved = inherited ? inherited : "";
  std::free(inherited);
  _putenv_s("GT4_WHEEL_FILE", bad.string().c_str());
  const WheelFileResult refused = load_wheel_from_environment();
  _putenv_s("GT4_WHEEL_FILE", (folder / "missing.toml").string().c_str());
  const WheelFileResult missing = load_wheel_from_environment();
  _putenv_s("GT4_WHEEL_FILE", "");
  const WheelFileResult none = load_wheel_from_environment();
  _putenv_s("GT4_WHEEL_FILE", saved.c_str());
  std::filesystem::remove_all(folder);
  require(!refused.error.empty() && !refused.config.enabled, "a refused file gives an error and no wheel");
  require(refused.error.find("removed") != std::string::npos, "the refusal names the removed pad mode");
  require(!missing.error.empty() && !missing.config.enabled, "a missing GT4_WHEEL_FILE gives an error and no wheel");
  require(none.path.empty() && none.error.empty() && !none.config.enabled, "no file: no wheel, no error");
}

void test_pedal_modes() {
  PedalConfig p{};
  p.source = {SourceKind::axis, {}, 1, HatDirection::up};
  p.mode = PedalAxisMode::inverted;
  require(read_pedal(p, 32767) == 0, "inverted pedal rest at +32767");
  require(read_pedal(p, 0) >= 126 && read_pedal(p, 0) <= 129, "inverted pedal half travel at 0");
  require(read_pedal(p, -32768) == 255, "inverted pedal full press at -32768");
  p.mode = PedalAxisMode::normal;
  require(read_pedal(p, -32768) == 0, "normal pedal rest at -32768");
  require(read_pedal(p, 32767) == 255, "normal pedal full press at +32767");
  p.mode = PedalAxisMode::positive;
  require(read_pedal(p, -10000) == 0 && read_pedal(p, 32767) == 255, "positive pedal");
  p.mode = PedalAxisMode::combined_negative;
  require(read_pedal(p, 20000) == 0 && read_pedal(p, -32768) == 255, "combined_negative pedal");
  p.mode = PedalAxisMode::combined_positive;
  require(read_pedal(p, -20000) == 0 && read_pedal(p, 32767) == 255, "combined_positive pedal");
  p.mode = PedalAxisMode::inverted;
  p.curve = Curve::quadratic;
  require(read_pedal(p, 0) < 80, "quadratic curve lowers half travel");
}

void test_identity() {
  require(!is_candidate_wheel_device(false, false, "Logitech F310", "030000006d0400001dc2000000000000"),
          "a Logitech gamepad VID:PID is not a wheel");
  require(is_candidate_wheel_device(false, false, "Unknown", "030000006d0400009bc2000000000000"),
          "the G27 VID:PID is a wheel without a name match");
  require(is_candidate_wheel_device(false, false, "Device", "03000000f51000007770000000000000"),
          "Turtle Beach VelocityOne Race (10F5:7077, not in SDL's list) is a wheel");
  require(is_candidate_wheel_device(false, false, "Gudsen MOZA R5 Base", "03000000" "6e34" "0000" "0400" "000000000000"),
          "MOZA vendor 0x346E is a wheel");
  require(!is_candidate_wheel_device(true, false, "Turtle Beach VelocityOne Multi-Shift"),
          "a shifter is never the primary wheel, even with SDL's wheel type");
  require(!is_candidate_wheel_device(false, false, "USB Pedals"), "pedals are not the primary wheel");
  require(is_candidate_wheel_device(false, true, "HORI Racing Wheel Apex"), "named XInput wheel recognized");
  require(is_known_wheel_vid_pid(0x0483, 0x0522) && is_known_wheel_vid_pid(0x3670, 0x0501) &&
              is_known_wheel_vid_pid(0x11FF, 0x3245),
          "Simagic and PXN IDs from the research table");
  require(!is_known_wheel_vid_pid(0x1209, 0x0000), "0x1209 (generic open hardware) is not MOZA");
  std::uint16_t vid = 0, pid = 0;
  require(extract_guid_vid_pid("030000006d0400009bc2000000000000", vid, pid) && vid == 0x046D && pid == 0xC29B,
          "VID:PID from an SDL GUID");
  require(!extract_guid_vid_pid("short", vid, pid), "a short GUID has no VID:PID");

  DeviceReading pad{};
  pad.guid = "030000005e0400008e02000000007801";
  pad.name = "Xbox 360 Controller";
  pad.is_gamepad = true;
  WheelConfig config = default_wheel_config();
  config.enabled = true;
  require(select_primary_device(config, {pad}) == -1, "auto-detect skips a standard gamepad");
  config.device_guid = pad.guid;
  require(select_primary_device(config, {pad}) == 0, "an explicit GUID selects any device");
  require(select_primary_device(config, {pad, pad}) == -1, "a duplicate explicit GUID is ambiguous");
}

// ---------------------------------------------------------------------------------------------------- DFP input

DeviceReading wheel_reading() {
  DeviceReading wheel{};
  wheel.guid = "030000006d0400009bc2000000000000";
  wheel.name = "Logitech G27 Racing Wheel";
  wheel.is_wheel = true;
  wheel.axes = {0, 32767, 32767, 32767};
  wheel.buttons = std::vector<bool>(24, false);
  wheel.hats = {0};
  return wheel;
}

void test_dfp_input() {
  WheelConfig config = default_wheel_config();
  config.enabled = true;
  config.steering.rotation_degrees = 900;
  DeviceReading wheel = wheel_reading();
  const auto input_for = [&](int range) { return map_dfp_input(config, {wheel}, 0, range); };

  // Steering: 14 bits, 0 full left, 0x1FFF centre, 0x3FFF full right; 1:1 on the game's 900 degrees.
  require(input_for(900).steering == 0x1FFF, "centred wheel is 0x1FFF");
  wheel.axes[0] = -32768;
  require(input_for(900).steering == 0, "full left is 0");
  wheel.axes[0] = 32767;
  require(input_for(900).steering == 0x3FFF, "full right is 0x3FFF");
  wheel.axes[0] = 16384; // 225 degrees right on a 900-degree wheel
  require(std::abs(static_cast<int>(input_for(900).steering) - (0x1FFF + 0x1000)) <= 1, "half right is a quarter");
  // On the DFP's 200-degree mode the wheel reaches full lock at 100 degrees.
  wheel.axes[0] = static_cast<std::int16_t>(std::lround(32767.0 * 100.0 / 450.0));
  require(input_for(200).steering == 0x3FFF, "100 degrees right is full lock on a 200-degree range");
  wheel.axes[0] = static_cast<std::int16_t>(std::lround(32767.0 * 50.0 / 450.0));
  require(std::abs(static_cast<int>(input_for(200).steering) - (0x1FFF + 0x1000)) <= 2, "50 of 100 degrees is half");
  // A 270-degree wheel on 900 degrees: full lock turns the DFP 135 degrees.
  config.steering.rotation_degrees = 270;
  wheel.axes[0] = 32767;
  require(std::abs(static_cast<int>(input_for(900).steering) - (0x1FFF + 2458)) <= 1, "270-degree wheel on 900");
  config.steering.invert = true;
  require(input_for(900).steering < 0x1FFF, "inverted steering turns the other way");
  config.steering.invert = false;
  config.steering.rotation_degrees = 900;
  wheel.axes[0] = 0;

  // Pedals: pressed 0..255 (the device model inverts them on the wire).
  wheel.axes[1] = -32768;
  wheel.axes[2] = 0;
  const gt4::usb::DfpInput pedals = input_for(900);
  require(pedals.throttle == 255, "throttle fully pressed");
  require(pedals.brake >= 126 && pedals.brake <= 129, "brake half pressed");
  wheel.axes[1] = wheel.axes[2] = 32767;

  // Buttons: default bindings btn 0..11 onto the DFP's bits. The shift slots are the paddles (DFP R1/L1).
  const struct {
    int host_button;
    int dfp_bit;
    const char *label;
  } expected[] = {{0, 0, "btn 0 Cross"}, {1, 1, "btn 1 Square"}, {2, 2, "btn 2 Circle"}, {3, 3, "btn 3 Triangle"},
                  {4, 4, "btn 4 right paddle: DFP R1"}, {5, 5, "btn 5 left paddle: DFP L1"}, {6, 6, "btn 6 DFP R2"},
                  {7, 7, "btn 7 DFP L2"}, {8, 8, "btn 8 Select"}, {9, 9, "btn 9 Start"}, {10, 10, "btn 10 R3"},
                  {11, 11, "btn 11 L3"}};
  for (const auto &item : expected) {
    std::fill(wheel.buttons.begin(), wheel.buttons.end(), false);
    wheel.buttons[static_cast<std::size_t>(item.host_button)] = true;
    require(input_for(900).buttons == (1u << item.dfp_bit), item.label);
  }
  std::fill(wheel.buttons.begin(), wheel.buttons.end(), false);
  require(input_for(900).buttons == 0 && input_for(900).hat == 8, "nothing pressed: no bits, hat released (8)");
  wheel.hats[0] = 0x01 | 0x02;
  require(input_for(900).hat == 1, "hat up-right is 1");
  wheel.hats[0] = 0x08;
  require(input_for(900).hat == 6, "hat left is 6");
  wheel.hats[0] = 0;

  // The handbrake pedal presses Circle past half travel.
  config.handbrake.source = parse_source_ref("axis:3", "x", 1);
  wheel.axes[3] = -32768;
  require((input_for(900).buttons & (1u << 2)) != 0, "handbrake presses Circle");
  wheel.axes[3] = 32767;
  require(input_for(900).buttons == 0, "released handbrake presses nothing");

  // A disabled configuration or no primary device is neutral.
  config.enabled = false;
  require(input_for(900).steering == 0x1FFF && input_for(900).buttons == 0, "disabled wheel is neutral");
  config.enabled = true;
  require(map_dfp_input(config, {wheel}, -1, 900).buttons == 0, "no primary device is neutral");
}

// ---------------------------------------------------------------------------------------------------- force feedback

struct FakeSink final : HapticSink {
  struct Log {
    std::mutex mutex;
    std::vector<std::string> calls;
    std::array<EffectParams, effect_kind_count> last{};
    std::vector<std::thread::id> threads;
    int stops = 0;
    int releases = 0;
    void add(std::string call) {
      std::lock_guard lock(mutex);
      calls.push_back(std::move(call));
      threads.push_back(std::this_thread::get_id());
    }
    std::vector<std::string> take() {
      std::lock_guard lock(mutex);
      std::vector<std::string> out;
      out.swap(calls);
      return out;
    }
  };
  Log &log;
  bool friction_supported;
  FakeSink(Log &target, bool friction = true) : log(target), friction_supported(friction) {}
  bool create(EffectKind kind, const EffectParams &params) override {
    if (kind == EffectKind::friction && !friction_supported)
      return false;
    log.add(std::string("create ") + effect_kind_name(kind));
    std::lock_guard lock(log.mutex);
    log.last[static_cast<std::size_t>(kind)] = params;
    return true;
  }
  bool update(EffectKind kind, const EffectParams &params) override {
    log.add(std::string("update ") + effect_kind_name(kind));
    std::lock_guard lock(log.mutex);
    log.last[static_cast<std::size_t>(kind)] = params;
    return true;
  }
  bool run(EffectKind kind) override {
    log.add(std::string("run ") + effect_kind_name(kind));
    return true;
  }
  void stop_all() override {
    log.add("stop");
    std::lock_guard lock(log.mutex);
    ++log.stops;
  }
  void set_gain(int percent) override { log.add("gain " + std::to_string(percent)); }
  void release() override {
    log.add("release");
    std::lock_guard lock(log.mutex);
    ++log.releases;
  }
};

std::string joined(const std::vector<std::string> &calls) {
  std::string out;
  for (const auto &call : calls)
    out += (out.empty() ? "" : ", ") + call;
  return out;
}

void require_calls(FakeSink::Log &log, const std::string &expected, const char *label) {
  const std::string actual = joined(log.take());
  if (actual != expected) {
    std::fprintf(stderr, "  expected: %s\n  actual:   %s\n", expected.c_str(), actual.c_str());
    require(false, label);
  }
}

gt4::usb::DfpForces racing_force(double level) {
  gt4::usb::DfpForces forces;
  forces.autocenter = false;
  forces.constant = level;
  forces.range_degrees = 900;
  return forces;
}

void test_effect_plan() {
  FfbConfig config;
  config.strength = 0.5;
  config.damper = 0.2;
  gt4::usb::DfpForces forces = racing_force(1.0);
  EffectPlan plan = plan_dfp_effects(forces, config);
  require(plan[0].level == 16384, "strength 0.5 halves the constant force");
  config.invert = true;
  require(plan_dfp_effects(forces, config)[0].level == -16384, "invert reverses the constant force");
  config.invert = false;
  require(plan[2].left_coeff == static_cast<std::int16_t>(std::lround(0.2 * 32767)), "damper floor not scaled by strength");
  require(plan[1].left_coeff == 0, "no game spring and no default spring: spring off");
  forces.autocenter = true;
  forces.autocenter_strength = 0.5;
  plan = plan_dfp_effects(forces, config);
  require(plan[1].left_coeff == 0, "the DFP default spring is off by default (TASK-192)");
  config.centering_spring = true;
  plan = plan_dfp_effects(forces, config);
  require(plan[1].left_coeff == static_cast<std::int16_t>(std::lround(0.25 * 32767)),
          "centering_spring game: the default spring at strength");
  forces.spring = true;
  forces.spring_left = forces.spring_right = 1.0;
  forces.spring_center = 0.5;
  forces.spring_deadband = 0.2;
  forces.spring_saturation = 1.0;
  plan = plan_dfp_effects(forces, config);
  require(plan[1].center == static_cast<std::int16_t>(std::lround(0.5 * 32767)) &&
              plan[1].deadband == static_cast<std::uint16_t>(std::lround(0.1 * 65535)),
          "the game's spring replaces the default spring");
  // TASK-192: with the setting off the game's spring is planned as nothing, and nothing else changes.
  config.centering_spring = false;
  gt4::usb::DfpForces menu = forces;
  menu.constant = 0.25;
  menu.damper = true;
  menu.damper_left = menu.damper_right = 0.3;
  menu.damper_saturation = 1.0;
  menu.friction = true;
  menu.friction_coefficient = 0.5;
  menu.friction_saturation = 0.125;
  const EffectPlan suppressed = plan_dfp_effects(menu, config);
  config.centering_spring = true;
  const EffectPlan played = plan_dfp_effects(menu, config);
  require(suppressed[1] == EffectParams{}, "centering_spring off: the spring effect carries no force");
  require(played[1].left_coeff != 0, "centering_spring game: the same forces carry the spring");
  require(suppressed[0] == played[0] && suppressed[2] == played[2] && suppressed[3] == played[3],
          "the centring spring choice touches neither the racing force, the damper nor the friction");
  require(suppressed[0].level == static_cast<std::int16_t>(std::lround(0.25 * 0.5 * 32767)),
          "strength still scales the game's racing force with the spring off");
  config.strength = 0.0;
  plan = plan_dfp_effects(forces, config);
  require(plan[0].level == 0 && plan[2].left_coeff == 0, "zero strength: no force and no damping floor");
}

void test_player_lifecycle() {
  FakeSink::Log log;
  FakeSink sink(log, /*friction=*/false);
  DfpHapticPlayer player(sink);
  FfbConfig config;
  const auto plan = [&](double level) { return plan_dfp_effects(racing_force(level), config); };

  player.apply(plan(0.1), true);
  require_calls(log, "create constant, create spring, create damper, gain 100, run constant, run spring, run damper",
                "first force creates the supported effects with their values and starts them once");
  require(!player.supported(EffectKind::friction), "an effect the device lacks is not supported");
  player.apply(plan(0.2), true);
  require_calls(log, "update constant", "a new force level updates only the constant force");
  player.apply(plan(0.2), true);
  require_calls(log, "", "the same forces send nothing");
  gt4::usb::DfpForces menu = racing_force(0.2);
  menu.friction = true;
  menu.friction_coefficient = 0.5;
  menu.friction_saturation = 1.0;
  player.apply(plan_dfp_effects(menu, config), true);
  require_calls(log, "", "friction is never sent to a device without it");
  player.apply(plan(0.2), false);
  require_calls(log, "stop, gain 0", "focus loss stops once");
  player.apply(plan(0.3), false);
  player.stop();
  require_calls(log, "", "further focus loss and stop calls send nothing");
  player.apply(plan(0.3), true);
  require_calls(log, "update constant, gain 100, run constant, run spring, run damper",
                "focus regained: the changed force, then the effects start again");
  require(player.updates_sent() == 2 && player.starts() == 2, "update and start counters");
  player.release();
  require_calls(log, "stop, gain 0, release", "release stops and closes");
  player.apply(plan(0.4), true);
  require_calls(log, "", "nothing after release");
}

void test_haptic_thread(bool threaded) {
  FakeSink::Log log;
  int opened = 0;
  void *opened_device = nullptr;
  int token = 0;
  {
    DfpHapticThread haptics(FfbConfig{}, [&](void *device) -> std::unique_ptr<HapticSink> {
      ++opened;
      opened_device = device;
      return std::make_unique<FakeSink>(log);
    }, threaded);
    haptics.attach(&token);
    require(opened == 1 && opened_device == &token, "attach opens the device's sink");
    haptics.set_active(true);
    for (int step = 1; step <= 200; ++step) {
      gt4::usb::DfpForces forces = racing_force(step / 200.0);
      forces.revision = static_cast<std::uint64_t>(step);
      haptics.set_forces(forces);
    }
    haptics.wait_idle();
    {
      std::lock_guard lock(log.mutex);
      require(log.last[0].level == 32767, "the latest force set is the one on the wheel");
      if (threaded) {
        const auto main_thread = std::this_thread::get_id();
        require(std::all_of(log.threads.begin(), log.threads.end(),
                            [&](std::thread::id id) { return id != main_thread && id == log.threads.front(); }),
                "every device call comes from the one haptics thread");
      }
    }
    const DfpHapticThread::Stats stats = haptics.stats();
    require(stats.force_sets == 200, "every force set counted");
    require(stats.forces_played >= 1 && stats.forces_played <= 200, "game forces reached the wheel");
    // Every level differs: at most 200 updates. The first force set no longer changes the spring either, which is off by
    // default since TASK-192; the thread drops the levels it has not reached yet.
    require(stats.updates_sent <= 200, "never more updates than changes");
    if (!threaded)
      require(stats.updates_sent == 200, "without the thread every change is one update");
    haptics.set_active(false);
    haptics.set_active(false);
    haptics.wait_idle();
    {
      std::lock_guard lock(log.mutex);
      require(log.stops == 1, "focus loss stops the effects once");
    }
    haptics.detach();
    {
      std::lock_guard lock(log.mutex);
      require(log.releases == 1, "detach releases the device before returning");
    }
  }
  std::lock_guard lock(log.mutex);
  require(log.releases == 1 && log.stops == 1, "shutdown after detach sends nothing more");
}

// ---------------------------------------------------------------------------------------------------- SDL3 virtual wheel

struct VirtualWheel {
  SDL_JoystickID id = 0;
  SDL_Joystick *joystick = nullptr;
  VirtualWheel(const char *name, SDL_JoystickType type, Uint16 product) {
    SDL_VirtualJoystickDesc description{};
    SDL_INIT_INTERFACE(&description);
    description.type = static_cast<Uint16>(type);
    description.name = name;
    description.vendor_id = 0xdead;
    description.product_id = product;
    description.naxes = 4;
    description.nbuttons = 16;
    description.nhats = 1;
    id = SDL_AttachVirtualJoystick(&description);
    require(id != 0, "attach virtual wheel");
    joystick = SDL_OpenJoystick(id);
    require(joystick != nullptr, "open virtual wheel");
    for (int axis = 1; axis < 4; ++axis)
      SDL_SetJoystickVirtualAxis(joystick, axis, 32767); // pedals at rest
  }
  std::string guid() const {
    char text[64]{};
    SDL_GUIDToString(SDL_GetJoystickGUID(joystick), text, sizeof(text));
    return text;
  }
  void detach() {
    if (joystick)
      SDL_CloseJoystick(joystick);
    joystick = nullptr;
    if (id)
      SDL_DetachVirtualJoystick(id);
    id = 0;
  }
  ~VirtualWheel() { detach(); }
};

void test_wheel_input_on_virtual_wheel() {
  require(SDL_InitSubSystem(SDL_INIT_GAMEPAD), "SDL initialization");
  struct Cleanup {
    ~Cleanup() { SDL_QuitSubSystem(SDL_INIT_GAMEPAD); }
  } cleanup;
  VirtualWheel wheel("GT4 fixture racing wheel", SDL_JOYSTICK_TYPE_WHEEL, 201);
  FakeSink::Log log;
  void *haptic_device = nullptr;
  WheelConfig config = default_wheel_config();
  config.enabled = true;
  config.device_guid = wheel.guid();
  gt4::usb::DfpBridge &bridge = gt4::usb::DfpBridge::shared();
  {
    WheelInput input(config, [&](void *device) -> std::unique_ptr<HapticSink> {
      haptic_device = device;
      return std::make_unique<FakeSink>(log);
    });
    require(bridge.enabled(), "an enabled wheel plugs the virtual DFP in");
    input.prepare(true);
    require(input.has_active_wheel() && input.primary_instance_id() == wheel.id, "the virtual wheel is the primary");
    require(haptic_device != nullptr, "the wheel's haptics were opened from its joystick");
    const auto &owned = input.owned_device_ids();
    require(std::find(owned.begin(), owned.end(), wheel.id) != owned.end(), "the gamepad path never reads the wheel");

    SDL_SetJoystickVirtualAxis(wheel.joystick, 0, 32767);
    SDL_SetJoystickVirtualAxis(wheel.joystick, 1, -32768);
    SDL_SetJoystickVirtualButton(wheel.joystick, 4, true);
    SDL_SetJoystickVirtualHat(wheel.joystick, 0, SDL_HAT_UP);
    input.prepare(true);
    gt4::usb::DfpInput seen = bridge.input();
    require(seen.steering == 0x3FFF, "the DFP sees full right lock");
    require(seen.throttle == 255, "the DFP sees the throttle");
    require(seen.buttons == (1u << 4), "the right paddle is the DFP's R1");
    require(seen.hat == 0, "the hat reaches the DFP");

    // The game's force command reaches the wheel through the bridge's listener.
    gt4::usb::DfpForces forces = racing_force(0.5);
    bridge.set_forces(forces);
    input.wait_haptics_idle();
    {
      std::lock_guard lock(log.mutex);
      require(log.last[0].level == static_cast<std::int16_t>(std::lround(0.5 * 32767)), "the game's force on the wheel");
    }
    require(input.haptic_stats().forces_played >= 1, "the summary counts the force as played");
    require(input.force_summary().find("reached the wheel yes") != std::string::npos, "summary line says yes");
    require(input.force_summary().find("centering spring off") != std::string::npos,
            "summary line names the centring spring setting");

    // Focus loss: neutral input; the forces stop once however often VBlank and the window call it.
    input.prepare(false);
    input.prepare(false);
    input.stop();
    input.wait_haptics_idle();
    seen = bridge.input();
    require(seen.steering == 0x1FFF && seen.buttons == 0 && seen.throttle == 0, "no focus: neutral DFP input");
    {
      std::lock_guard lock(log.mutex);
      require(log.stops == 1, "focus loss stopped the forces once");
    }
    input.prepare(true);
    input.wait_haptics_idle();
    require(bridge.input().steering == 0x3FFF, "focus regained: input again");

    // Unplugging the wheel releases the haptic handle before its joystick closes.
    wheel.detach();
    input.prepare(true);
    {
      std::lock_guard lock(log.mutex);
      require(log.releases == 1, "unplugged wheel: haptics released");
    }
    require(!input.has_active_wheel() && input.owned_device_ids().empty(), "unplugged wheel owns nothing");
  }
  bridge.set_force_listener({});
  bridge.set_enabled(false);
  bridge.set_input({});
}

void test_disabled_wheel_touches_nothing() {
  gt4::usb::DfpBridge &bridge = gt4::usb::DfpBridge::shared();
  bridge.set_enabled(false);
  int opened = 0;
  {
    WheelInput input(default_wheel_config(), [&](void *) -> std::unique_ptr<HapticSink> {
      ++opened;
      return nullptr;
    });
    input.prepare(true);
    input.stop();
    require(!input.has_active_wheel() && input.owned_device_ids().empty(), "disabled wheel owns nothing");
  }
  require(!bridge.enabled() && opened == 0, "a disabled wheel never plugs the DFP in nor opens haptics");
}

} // namespace

int main() {
  try {
    test_mode_and_legacy_keys();
    test_refused_file_runs_without_wheel();
    test_pedal_modes();
    test_identity();
    test_dfp_input();
    test_effect_plan();
    test_player_lifecycle();
    test_haptic_thread(true);
    test_haptic_thread(false);
    test_disabled_wheel_touches_nothing();
    test_wheel_input_on_virtual_wheel();
    std::fprintf(stderr, "PASS: wheel.toml (mode dfp, 1.2.x keys, refused files), pedals, identity, DFP input, "
                         "force effects (changed-only updates, stop once, one haptics thread), SDL3 virtual wheel\n");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
