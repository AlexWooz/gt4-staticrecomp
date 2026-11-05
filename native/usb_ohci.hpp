#pragma once
// USB host controller model for the PS2 IOP (OpenHCI 1.0a register set at 0x1F801600, IOP interrupt line 22), written
// from the public OpenHCI specification. It runs the guest USB driver's endpoint and transfer descriptor lists in IOP
// RAM once per 1 ms frame, with a two-port root hub. Devices are host objects behind usb::Device.
//
// TASK-018 (docs/WHEEL-NATIVE-DFP.md): no PCSX2 or Linux driver source was read for this file.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace gt4::usb {

// Result of a device transaction: a byte count, or one of these.
inline constexpr int kNak = -1;
inline constexpr int kStall = -2;

class Device {
public:
  virtual ~Device() = default;
  // Bus reset (port reset): address 0, unconfigured.
  virtual void reset() = 0;
  virtual std::uint8_t address() const = 0;
  // SETUP packet (8 bytes) on endpoint 0. kStall refuses it.
  virtual int setup(std::span<const std::uint8_t, 8> packet) = 0;
  // IN token: fills up to buffer.size() bytes.
  virtual int in(unsigned endpoint, std::span<std::uint8_t> buffer) = 0;
  // OUT token with its data.
  virtual int out(unsigned endpoint, std::span<const std::uint8_t> data) = 0;
  virtual bool low_speed() const { return false; }
};

// What the controller needs from the machine: guest memory and the interrupt line.
struct Bus {
  std::function<std::uint32_t(std::uint32_t)> read32;
  std::function<void(std::uint32_t, std::uint32_t)> write32;
  std::function<std::uint8_t(std::uint32_t)> read8;
  std::function<void(std::uint32_t, std::uint8_t)> write8;
  std::function<void()> raise_interrupt;
};

class Ohci {
public:
  static constexpr unsigned kPorts = 2;
  static constexpr unsigned kRegisterCount = 0x18;

  explicit Ohci(Bus bus);

  void reset();
  // Plugs a device into a root hub port (0 or 1), or unplugs it with nullptr. The device is not owned.
  void attach(unsigned port, Device *device);

  std::uint32_t read(unsigned reg);
  void write(unsigned reg, std::uint32_t value);

  // One 1 ms USB frame. Does nothing unless the guest put the controller in the operational state.
  void frame();

  bool operational() const;
  std::uint64_t frames_run() const { return frames_; }
  std::uint64_t transfers() const { return transfers_; }

private:
  struct Port {
    Device *device = nullptr;
    std::uint32_t status = 0;
    int reset_frames = 0;
  };

  void update_interrupts();
  void set_status(std::uint32_t bits);
  bool process_list(std::uint32_t head, bool periodic, bool *any);
  bool process_ed(std::uint32_t ed);
  Device *find_device(std::uint8_t address);
  void retire(std::uint32_t ed, std::uint32_t td, std::uint32_t next_td, unsigned condition, bool halt);
  void software_reset();
  void connect_port(unsigned port);

  Bus bus_;
  std::array<std::uint32_t, kRegisterCount> regs_{};
  std::array<Port, kPorts> ports_{};
  std::uint32_t done_head_ = 0;
  std::uint32_t raised_ = 0;
  std::uint64_t frames_ = 0;
  std::uint64_t transfers_ = 0;
};

} // namespace gt4::usb
