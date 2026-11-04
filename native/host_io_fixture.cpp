#include "host_io.hpp"
#include "frame_clock.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <stdexcept>

namespace {
void require(bool value, const char *label) {
  if (!value)
    throw std::runtime_error(label);
}
// This fixture opens only its own virtual device; it does not select or send haptics to physical pads.
void check_standardized_sdl_triggers() {
  require(SDL_InitSubSystem(SDL_INIT_GAMEPAD), "SDL gamepad fixture initialization");
  struct SdlCleanup {
    ~SdlCleanup() { SDL_QuitSubSystem(SDL_INIT_GAMEPAD); }
  } cleanup;
  struct VirtualPad {
    SDL_JoystickID id = 0;
    SDL_Gamepad *pad = nullptr;
    ~VirtualPad() {
      if (pad) SDL_CloseGamepad(pad);
      if (id) SDL_DetachVirtualJoystick(id);
    }
  };
  const auto remap = gt4::controls::parse_controls(
      "schema = 1\n[bindings]\ncross = [\"xinput:RT | deadzone=0\"]\n"
      "square = [\"xinput:LT | deadzone=0\"]\n", "virtual-trigger-remap");
  struct Device { const char *name; Uint16 vendor; Uint16 product; };
  for (const auto &device : {Device{"DualSense Wireless Controller", 0x054c, 0x0ce6},
                             Device{"Xbox Controller", 0x045e, 0x028e}}) {
    VirtualPad owned;
    SDL_VirtualJoystickDesc desc{};
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = device.vendor;
    desc.product_id = device.product;
    desc.name = device.name;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    owned.id = SDL_AttachVirtualJoystick(&desc);
    require(owned.id != 0, "attach own virtual gamepad");
    owned.pad = SDL_OpenGamepad(owned.id);
    require(owned.pad != nullptr, "open own virtual gamepad");
    SDL_Joystick *joystick = SDL_GetGamepadJoystick(owned.pad);
    // Raw signed joystick values are normalized by SDL before the production adapter sees them.
    const Sint16 joystick_values[] = {-32768, -16383, 1, 16385, 32767};
    const std::uint8_t expected_bytes[] = {0, 64, 128, 192, 255};
    for (std::size_t step = 0; step < std::size(joystick_values); ++step) {
      require(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, joystick_values[step]),
              "set virtual left trigger");
      require(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, joystick_values[step]),
              "set virtual right trigger");
      SDL_UpdateJoysticks();
      SDL_UpdateGamepads();
      const auto reading = read_sdl_gamepad(owned.pad);
      require(reading.left_trigger == expected_bytes[step] && reading.right_trigger == expected_bytes[step],
              "PS5 and Xbox standardized SDL rest/quarter/half/three-quarter/full conversion");
      const auto mapped = gt4::pad::map_reading(reading, remap);
      require(mapped.pressures[14] == expected_bytes[step] && mapped.pressures[15] == expected_bytes[step],
              "trigger pressure follows its physical source when rebound to face buttons");
    }
    require(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768), "release virtual LT");
    require(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768), "release virtual RT");
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    const auto released = gt4::pad::map_reading(read_sdl_gamepad(owned.pad), gt4::controls::default_controls());
    require(released.buttons == 0 && released.pressures[8] == 0 && released.pressures[9] == 0,
            "released standardized DualSense and Xbox triggers stay neutral");
  }
  std::fprintf(stderr, "PASS: production SDL adapter with own virtual PS5 and Xbox triggers\n");
}
} // namespace

int main() {
  // A missing SDL3.dll must fail the test, not open a system error dialog.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  try {
    check_standardized_sdl_triggers();
    const WORD buttons[] = {XINPUT_GAMEPAD_BACK,
                            XINPUT_GAMEPAD_LEFT_THUMB,
                            XINPUT_GAMEPAD_RIGHT_THUMB,
                            XINPUT_GAMEPAD_START,
                            XINPUT_GAMEPAD_DPAD_UP,
                            XINPUT_GAMEPAD_DPAD_RIGHT,
                            XINPUT_GAMEPAD_DPAD_DOWN,
                            XINPUT_GAMEPAD_DPAD_LEFT,
                            XINPUT_GAMEPAD_LEFT_SHOULDER,
                            XINPUT_GAMEPAD_RIGHT_SHOULDER,
                            XINPUT_GAMEPAD_Y,
                            XINPUT_GAMEPAD_B,
                            XINPUT_GAMEPAD_A,
                            XINPUT_GAMEPAD_X};
    const int bits[] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 15};
    XINPUT_GAMEPAD gamepad{};
    for (int index = 0; index < 14; ++index) {
      gamepad.wButtons = buttons[index];
      const auto pad = map_gamepad(gamepad);
      require(pad.buttons == (1u << bits[index]), "digital button permutation");
      for (int bit = 0; bit < 16; ++bit)
        require(pad.pressures[bit] == (bit == bits[index] ? 255 : 0),
                "digital pressure");
    }
    gamepad.wButtons = 0x0c00;
    require(map_gamepad(gamepad).buttons == 0, "reserved host bits");
    gamepad = {};
    gamepad.bLeftTrigger = 30;
    require(map_gamepad(gamepad).buttons == 0, "trigger deadzone");
    gamepad.bLeftTrigger = 31;
    require(map_gamepad(gamepad).buttons == 256 &&
                map_gamepad(gamepad).pressures[8] == 0,
            "trigger first step: pressed, lowest pressure");
    // TASK-147: L2 (brake) and R2 (throttle) end at GT4's 100%, pressures 84 and 45.
    gamepad.bLeftTrigger = gamepad.bRightTrigger = 255;
    require(map_gamepad(gamepad).buttons == 768 &&
                map_gamepad(gamepad).pressures[8] == 84 &&
                map_gamepad(gamepad).pressures[9] == 45,
            "trigger endpoints");
    gamepad = {};
    gamepad.sThumbLX = -32768;
    gamepad.sThumbLY = 32767;
    gamepad.sThumbRX = 32767;
    gamepad.sThumbRY = -32768;
    require(map_gamepad(gamepad).axes ==
                std::array<std::uint8_t, 4>{255, 255, 0, 0},
            "signed endpoints and Y inversion");
    gamepad = {};
    gamepad.sThumbLX = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    gamepad.sThumbRY = -XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
    require(map_gamepad(gamepad).axes == PadState{}.axes,
            "axis deadzone boundary");
    int previous = 0;
    for (int value = -32768; value <= 32767; ++value) {
      gamepad.sThumbLX = static_cast<SHORT>(value);
      const int axis = map_gamepad(gamepad).axes[2];
      require(axis >= previous, "axis monotonicity");
      previous = axis;
    }
    for (int bit = 0; bit < 16; ++bit) {
      auto pad = map_gamepad(XINPUT_GAMEPAD{});
      const auto keyboard_button = static_cast<std::uint16_t>(1u << bit);
      merge_keyboard(pad, keyboard_button);
      require(pad.buttons == keyboard_button,
              "keyboard bit maps to one digital button");
      for (int pressure = 0; pressure < 16; ++pressure)
        require(pad.pressures[pressure] == (pressure == bit ? 255 : 0),
                "keyboard pressure maps to matching button");
      require(pad.axes == PadState{}.axes,
              "keyboard button leaves analog axes neutral");
    }
    const std::uint16_t directions[] = {
        1u << 4, 1u << 5, 1u << 6, 1u << 7};
    for (const auto direction : directions) {
      auto pad = map_gamepad(XINPUT_GAMEPAD{});
      merge_keyboard(pad, direction);
      require(pad.buttons == direction && pad.axes == PadState{}.axes,
              "keyboard direction emits one digital direction only");
    }
    auto held = map_gamepad(XINPUT_GAMEPAD{});
    merge_keyboard(held, 1u << 7);
    require(held.buttons == (1u << 7), "keyboard direction held");
    auto held_again = map_gamepad(XINPUT_GAMEPAD{});
    merge_keyboard(held_again, 1u << 7);
    require(held_again.buttons == held.buttons &&
                held_again.pressures == held.pressures &&
                held_again.axes == held.axes,
            "keyboard direction remains stable while held");
    auto released = map_gamepad(XINPUT_GAMEPAD{});
    merge_keyboard(released, 0);
    require(released.buttons == 0 && released.axes == PadState{}.axes &&
                released.pressures == PadState{}.pressures,
            "keyboard direction release returns to neutral");
    auto chord = map_gamepad(XINPUT_GAMEPAD{});
    merge_keyboard(chord, (1u << 7) | (1u << 14));
    require(chord.buttons == 0x4080 && chord.pressures[7] == 255 &&
                chord.pressures[14] == 255 && chord.axes == PadState{}.axes,
            "keyboard direction and action chord");
    XINPUT_GAMEPAD analog_gamepad{};
    analog_gamepad.wButtons = XINPUT_GAMEPAD_A;
    analog_gamepad.sThumbLX = 20000;
    analog_gamepad.sThumbLY = -18000;
    analog_gamepad.sThumbRX = -16000;
    analog_gamepad.sThumbRY = 14000;
    auto combined = map_gamepad(analog_gamepad);
    const auto xinput_axes = combined.axes;
    const auto xinput_buttons = combined.buttons;
    merge_keyboard(combined, directions[0] | directions[3]);
    require(combined.axes == xinput_axes,
            "keyboard directions preserve XInput analog axes");
    require((combined.buttons & xinput_buttons) == xinput_buttons,
            "keyboard directions preserve XInput buttons");
    for (const std::uint16_t rumble : {0, 0x0100, 0xff00, 1, 0xff01}) {
      const auto vibration = map_rumble(rumble);
      require(vibration.wLeftMotorSpeed == (rumble >> 8) * 257 &&
                  vibration.wRightMotorSpeed == ((rumble & 1) ? 65535 : 0),
              "rumble channels");
    }
    ControllerInput input;
    const auto neutral = input.poll(false, 0xffff);
    require(neutral.buttons == 0 && neutral.axes == PadState{}.axes &&
                neutral.pressures == PadState{}.pressures,
            "focus loss neutral");
    std::fprintf(stderr, "PASS: XInput mapping, pressures, signed axes, "
                         "digital keyboard, focus, rumble\n");
    AudioOutput audio;
    // Silence: the checks below count frames, timing and queue bounds only, so
    // zeroed PCM keeps the test off the speakers without changing any assertion.
    std::array<std::int16_t, 64> samples{};
    for (int block = 0; block < 81; ++block)
      require(audio.push(samples.data(), 32), "audio fragmented submission");
    audio.close(true);
    require(audio.received_frames() == 2592 && audio.completed_frames() == 2592,
            "audio wrap and partial drain");
    AudioOutput canceled;
    require(canceled.push(samples.data(), 32), "audio partial cancellation");
    canceled.close(false);
    require(canceled.completed_frames() == 0,
            "pending PCM not falsely completed");
    AudioOutput queued_cancel;
    for (int block = 0; block < 64; ++block)
      require(queued_cancel.push(samples.data(), 32),
              "queued PCM before cancel");
    queued_cancel.close(false);
    require(queued_cancel.completed_frames() +
                    queued_cancel.canceled_frames() ==
                2048,
            "queued cancellation ownership accounting");
    queued_cancel.close(false);
    bool rejected = false;
    try {
      queued_cancel.push(samples.data(), 32);
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    require(rejected, "closed sink refuses new samples");
    std::fprintf(stderr, "PASS: real Windows audio completion, ring wrap, "
                         "partial drain and cancel\n");
    // TASK-129: without an output device the sink is silent: it accepts and drops samples in both modes, queues
    // nothing, never waits, and still refuses samples once closed.
    for (const auto mode : {gt4::pacing::Mode::audio, gt4::pacing::Mode::paced}) {
      AudioOutput silent(nullptr, mode, AudioDevice::none);
      require(silent.silent(), "no device gives a silent sink");
      for (int block = 0; block < 600; ++block)
        require(silent.push(samples.data(), 32), "silent sink accepts samples");
      silent.pump();
      require(silent.received_frames() == 19200 && silent.queued_frames() == 0 && silent.completed_frames() == 0,
              "silent sink counts and queues nothing");
      silent.close(true);
      bool silent_rejected = false;
      try {
        silent.push(samples.data(), 32);
      } catch (const std::runtime_error &) {
        silent_rejected = true;
      }
      require(silent_rejected, "closed silent sink refuses new samples");
    }
    require(!audio.silent(), "the system device opens on this machine");
    std::fprintf(stderr, "PASS: silent sink without an output device (audio and paced modes)\n");
    // Paced mode on the real device: three seconds at 60 Hz, one 800-frame
    // block per guest frame. Pushes never wait; the queue stays bounded; the
    // device consumes at wall-clock rate; the resample ratio stays in bounds.
    {
      FrameClock clock;
      gt4::pacing::FramePacer pacer(clock.frequency(), gt4::pacing::guest_vblank_rate);
      AudioOutput paced(nullptr, gt4::pacing::Mode::paced);
      std::array<std::int16_t, 800 * 2> frame_audio{};
      const std::uint64_t bound = gt4::pacing::target_latency_frames + 16 * 256;
      const std::int64_t longest_push = static_cast<std::int64_t>(clock.frequency() / 500);
      std::int64_t worst_push = 0;
      std::uint64_t worst_queue = 0;
      pacer.anchor(clock.now());
      for (int guest_frame = 0; guest_frame < 180; ++guest_frame) {
        clock.wait_until(pacer.deadline());
        pacer.release(clock.now());
        // Silence (zeroed PCM): only timing and queue bounds are asserted.
        std::fill(frame_audio.begin(), frame_audio.end(), std::int16_t{0});
        const std::int64_t before = clock.now();
        require(paced.push(frame_audio.data(), 800), "paced push accepted");
        worst_push = std::max(worst_push, clock.now() - before);
        paced.pump();
        worst_queue = std::max(worst_queue, paced.queued_frames());
      }
      require(worst_push < longest_push, "paced push never waits for the device");
      require(worst_queue <= bound, "paced queue stays bounded");
      require(paced.completed_frames() >= 130000, "device consumes at wall-clock rate");
      require(paced.underruns() <= 2, "paced audio does not starve");
      require(paced.resampler_ratio() >= 0.995 && paced.resampler_ratio() <= 1.005,
              "resample ratio bounded");
      require(paced.overflow_frames() == 0, "ring never overflows at nominal rate");
      std::fprintf(stderr,
                   "PASS: paced audio (3 s, 60 Hz): worst push %.3f ms, max queue %llu "
                   "frames, completed %llu, underruns %llu, ratio %.6f\n",
                   1000.0 * static_cast<double>(worst_push) / static_cast<double>(clock.frequency()),
                   static_cast<unsigned long long>(worst_queue),
                   static_cast<unsigned long long>(paced.completed_frames()),
                   static_cast<unsigned long long>(paced.underruns()), paced.resampler_ratio());
    }
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
