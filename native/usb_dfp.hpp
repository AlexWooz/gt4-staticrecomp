#pragma once
// Virtual Logitech Driving Force Pro on the emulated USB bus (TASK-018, docs/WHEEL-NATIVE-DFP.md).
//
// The device identity and the report and command layouts follow the prose facts in docs/research/DFP-PCSX2-FACTS.md
// (itself a secondary source) and Logitech's published classic force feedback command format; the descriptors and all
// code here are written fresh. No PCSX2 or Linux driver source was read.
//
// Threads: the IOP side (Dfp, via the controller) and the host side (the wheel, at VBlank) share one DfpBridge.

#include "usb_ohci.hpp"

#include <array>
#include <functional>
#include <map>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace gt4::usb {

// Host -> guest: what the wheel currently reports.
struct DfpInput {
  std::uint16_t steering = 0x1FFF;  // 14 bits, 0 = full left, 0x1FFF centre, 0x3FFF full right
  std::uint16_t buttons = 0;        // 14 bits: cross, square, circle, triangle, R1, L1, R2, L2, select, start, R3, L3
  std::uint8_t hat = 8;             // 0 up, clockwise in 45-degree steps; 8 released
  std::uint8_t throttle = 0;        // 0 released .. 255 fully pressed (inverted on the wire)
  std::uint8_t brake = 0;
};

// One force slot as the game last set it. `order` is the order of the slot's last download or refresh, so an effect that
// more than one slot can carry (the spring) follows the newest command, as the single host effect it becomes does.
struct DfpSlot {
  bool active = false;
  std::uint8_t type = 0xFF;
  std::array<std::uint8_t, 5> params{};
  std::uint64_t order = 0;
};

// Guest -> host: the forces the game asks for, in device-independent units.
//
// There is no default centring spring here (TASK-194): the DFP's "default spring on" command is accepted and ignored, as
// PCSX2 ignores it, and nothing brings one back, not even a USB reset. GT4's own spring effects arrive as slot forces.
struct DfpForces {
  double constant = 0.0;        // -1 .. +1 (positive = towards the right)
  bool spring = false;
  double spring_left = 0.0;     // coefficient 0..1 on each side
  double spring_right = 0.0;
  double spring_center = 0.0;   // -1 .. +1
  double spring_deadband = 0.0; // 0 .. 2 (full travel)
  double spring_saturation = 1.0;
  bool damper = false;
  double damper_left = 0.0;
  double damper_right = 0.0;
  double damper_saturation = 1.0;
  bool friction = false;
  double friction_left = 0.0;   // the two coefficient bytes, each 0..1 (0x80/255 on the left, 1.0 on the right for GT4)
  double friction_right = 0.0;
  double friction_saturation = 1.0;
  int range_degrees = 200;       // a DFP starts in the compatible 200-degree mode
  std::uint64_t revision = 0;    // bumped on every change
};

class DfpBridge {
public:
  static DfpBridge &shared();

  void set_input(const DfpInput &input);
  DfpInput input() const;
  DfpForces forces() const;
  void set_forces(const DfpForces &forces);
  // True once the guest has configured the device (the game's driver found it).
  bool configured() const;
  void set_configured(bool value);
  // The virtual wheel is plugged in (wheel.toml enabled = true, or GT4_DFP=1); without it no device is plugged in and the
  // USB capsule leaves the donor register stub answering.
  bool enabled() const;
  void set_enabled(bool value);
  // Called after every force change, outside the lock, on the thread that changed them (the emulation thread).
  void set_force_listener(std::function<void()> listener);

private:
  std::function<void()> listener_;
  mutable std::mutex mutex_;
  DfpInput input_;
  DfpForces forces_;
  bool configured_ = false;
  bool enabled_ = false;
};

// Decodes one 7- or 8-byte force feedback command into slots and forces. Pure; used by Dfp and the fixture.
class DfpCommandDecoder {
public:
  void reset();
  // Returns false for a command it does not understand (logged by the caller).
  bool apply(const std::uint8_t *command, std::size_t length);
  const DfpForces &forces() const { return forces_; }
  const std::array<DfpSlot, 4> &slots() const { return slots_; }

private:
  void recompute();
  std::array<DfpSlot, 4> slots_{};
  DfpForces forces_{};
  std::uint64_t sequence_ = 0;
};

class Dfp final : public Device {
public:
  explicit Dfp(DfpBridge &bridge);
  // The census of the session's force commands goes to game.log here, without the trace switch (TASK-192).
  ~Dfp() override;
  void reset() override;
  std::uint8_t address() const override { return address_; }
  int setup(std::span<const std::uint8_t, 8> packet) override;
  int in(unsigned endpoint, std::span<std::uint8_t> buffer) override;
  int out(unsigned endpoint, std::span<const std::uint8_t> data) override;

  // The 8-byte interrupt IN report for an input state.
  static std::array<std::uint8_t, 8> encode_report(const DfpInput &input);
  static const std::vector<std::uint8_t> &device_descriptor();
  static const std::vector<std::uint8_t> &configuration_descriptor();
  static const std::vector<std::uint8_t> &report_descriptor();

private:
  void command(const std::uint8_t *data, std::size_t length);
  void log_census() const;

  DfpBridge &bridge_;
  DfpCommandDecoder decoder_;
  std::uint8_t address_ = 0;
  std::uint8_t pending_address_ = 0;
  bool address_pending_ = false;
  std::uint8_t configuration_ = 0;
  std::vector<std::uint8_t> response_;
  std::size_t response_offset_ = 0;
  bool expect_report_data_ = false;
  std::uint64_t commands_ = 0;
  std::uint64_t reports_ = 0;
  DfpInput last_reported_{};  // the input the last interrupt IN report carried (the steering the game last saw)
  std::map<unsigned, std::pair<std::uint64_t, std::array<std::uint8_t, 8>>> census_;
};

// The machine's USB controller and its virtual wheel, created on first use by the IOP hardware glue.
Ohci &machine_controller(const Bus &bus);
// Logs the decoded commands (GT4_DFP_TRACE=1).
bool trace_enabled();

} // namespace gt4::usb
