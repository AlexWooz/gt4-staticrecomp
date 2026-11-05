#include "usb_ohci.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace gt4::usb {

namespace {

// Register indices (OpenHCI 1.0a, section 7).
enum : unsigned {
  kRevision = 0x00,
  kControl = 0x01,
  kCommandStatus = 0x02,
  kInterruptStatus = 0x03,
  kInterruptEnable = 0x04,
  kInterruptDisable = 0x05,
  kHcca = 0x06,
  kPeriodCurrentEd = 0x07,
  kControlHeadEd = 0x08,
  kControlCurrentEd = 0x09,
  kBulkHeadEd = 0x0A,
  kBulkCurrentEd = 0x0B,
  kDoneHead = 0x0C,
  kFmInterval = 0x0D,
  kFmRemaining = 0x0E,
  kFmNumber = 0x0F,
  kPeriodicStart = 0x10,
  kLsThreshold = 0x11,
  kRhDescriptorA = 0x12,
  kRhDescriptorB = 0x13,
  kRhStatus = 0x14,
  kRhPortStatus = 0x15,
};

// HcControl
constexpr std::uint32_t kPeriodicListEnable = 1u << 2;
constexpr std::uint32_t kControlListEnable = 1u << 4;
constexpr std::uint32_t kBulkListEnable = 1u << 5;
constexpr std::uint32_t kStateMask = 3u << 6;
constexpr std::uint32_t kStateOperational = 2u << 6;
constexpr std::uint32_t kStateSuspend = 3u << 6;
// HcCommandStatus
constexpr std::uint32_t kHostReset = 1u << 0;
constexpr std::uint32_t kControlListFilled = 1u << 1;
constexpr std::uint32_t kBulkListFilled = 1u << 2;
// HcInterruptStatus / Enable
constexpr std::uint32_t kWritebackDoneHead = 1u << 1;
constexpr std::uint32_t kStartOfFrame = 1u << 2;
constexpr std::uint32_t kFrameNumberOverflow = 1u << 5;
constexpr std::uint32_t kRootHubStatusChange = 1u << 6;
constexpr std::uint32_t kMasterInterruptEnable = 1u << 31;
// HcRhPortStatus
constexpr std::uint32_t kConnected = 1u << 0;
constexpr std::uint32_t kEnabled = 1u << 1;
constexpr std::uint32_t kSuspended = 1u << 2;
constexpr std::uint32_t kResetting = 1u << 4;
constexpr std::uint32_t kPowered = 1u << 8;
constexpr std::uint32_t kLowSpeed = 1u << 9;
constexpr std::uint32_t kConnectChange = 1u << 16;
constexpr std::uint32_t kEnableChange = 1u << 17;
constexpr std::uint32_t kSuspendChange = 1u << 18;
constexpr std::uint32_t kResetChange = 1u << 20;
constexpr std::uint32_t kPortChangeMask = 0x001F0000u;

// Condition codes (TD dword 0 bits 28..31).
constexpr unsigned kNoError = 0;
constexpr unsigned kStallCode = 4;
constexpr unsigned kDeviceNotResponding = 5;
constexpr unsigned kDataUnderrun = 9;

constexpr std::uint32_t kHccaFrameNumber = 0x80;
constexpr std::uint32_t kHccaDoneHead = 0x84;
// Bounds a malformed list so a guest bug can never hang the host.
constexpr int kMaxEdsPerList = 64;
constexpr int kMaxTdsPerEd = 64;

} // namespace

Ohci::Ohci(Bus bus) : bus_(std::move(bus)) { reset(); }

void Ohci::reset() {
  regs_.fill(0);
  regs_[kRevision] = 0x110;
  regs_[kControl] = 0;  // UsbReset state after a hardware reset
  regs_[kFmInterval] = 0x27782EDFu;  // FSMPS 10104, FI 11999 (the reset values of section 7.3.1)
  regs_[kLsThreshold] = 0x628;
  regs_[kRhDescriptorA] = 0x00000200u | kPorts;  // NPS: ports always powered, NDP = 2
  done_head_ = 0;
  raised_ = 0;
  for (unsigned port = 0; port < kPorts; ++port) {
    ports_[port].status = kPowered;
    ports_[port].reset_frames = 0;
    if (ports_[port].device)
      connect_port(port);
  }
}

void Ohci::software_reset() {
  // HostControllerReset: registers to their reset values, state UsbSuspend; root hub ports keep their connection.
  const auto ports = ports_;
  reset();
  regs_[kControl] = kStateSuspend;
  for (unsigned port = 0; port < kPorts; ++port)
    ports_[port].status = (ports[port].status & (kConnected | kPowered | kLowSpeed)) | kPowered |
                          (ports[port].device ? kConnectChange : 0);
}

void Ohci::connect_port(unsigned port) {
  Port &p = ports_[port];
  p.status = kPowered | kConnected | kConnectChange | (p.device && p.device->low_speed() ? kLowSpeed : 0);
}

void Ohci::attach(unsigned port, Device *device) {
  if (port >= kPorts)
    return;
  ports_[port].device = device;
  if (device) {
    device->reset();
    connect_port(port);
  } else {
    ports_[port].status = kPowered | kConnectChange;
  }
  set_status(kRootHubStatusChange);
}

bool Ohci::operational() const { return (regs_[kControl] & kStateMask) == kStateOperational; }

std::uint32_t Ohci::read(unsigned reg) {
  if (reg >= kRegisterCount)
    return 0;
  if (reg == kInterruptDisable)
    return regs_[kInterruptEnable];
  if (reg == kFmRemaining)
    return regs_[kFmInterval] & 0x3FFFu;  // a frame is atomic here: always a full frame remaining
  if (reg >= kRhPortStatus && reg < kRhPortStatus + kPorts)
    return ports_[reg - kRhPortStatus].status;
  return regs_[reg];
}

void Ohci::write(unsigned reg, std::uint32_t value) {
  if (reg >= kRegisterCount)
    return;
  switch (reg) {
  case kRevision:
  case kFmRemaining:
  case kFmNumber:
  case kRhDescriptorA:
    return;  // read-only here
  case kCommandStatus:
    if (value & kHostReset) {
      software_reset();
      return;
    }
    regs_[kCommandStatus] |= value & (kControlListFilled | kBulkListFilled | (1u << 3));
    return;
  case kInterruptStatus:
    regs_[kInterruptStatus] &= ~value;  // write 1 to clear
    raised_ &= ~value;
    return;
  case kInterruptEnable:
    regs_[kInterruptEnable] |= value;
    update_interrupts();
    return;
  case kInterruptDisable:
    regs_[kInterruptEnable] &= ~value;
    return;
  case kRhStatus:
    // LPSC / LPS and the remote wakeup bits have no effect with NPS set; OCIC is write-1-to-clear.
    regs_[kRhStatus] &= ~(value & (1u << 17));
    return;
  default:
    break;
  }
  if (reg >= kRhPortStatus && reg < kRhPortStatus + kPorts) {
    Port &p = ports_[reg - kRhPortStatus];
    p.status &= ~(value & kPortChangeMask);
    const bool connected = (p.status & kConnected) != 0;
    if (value & (1u << 0))
      p.status &= ~kEnabled;  // ClearPortEnable
    if ((value & (1u << 1)) && connected)
      p.status |= kEnabled;  // SetPortEnable
    if ((value & (1u << 2)) && connected)
      p.status |= kSuspended;
    if (value & (1u << 3)) {
      if (p.status & kSuspended)
        p.status |= kSuspendChange;
      p.status &= ~kSuspended;
    }
    if ((value & (1u << 4)) && connected) {
      p.status |= kResetting;  // SetPortReset: completes after 10 frames (the root hub's reset time)
      p.reset_frames = 10;
    }
    return;
  }
  regs_[reg] = value;
  if (reg == kControl && (value & kStateMask) != kStateOperational)
    done_head_ = 0;
}

void Ohci::set_status(std::uint32_t bits) {
  regs_[kInterruptStatus] |= bits;
  update_interrupts();
}

void Ohci::update_interrupts() {
  const std::uint32_t pending = regs_[kInterruptStatus] & regs_[kInterruptEnable] & ~kMasterInterruptEnable;
  if (!(regs_[kInterruptEnable] & kMasterInterruptEnable))
    return;
  // The line is asserted for each newly pending cause; the guest clears causes by writing HcInterruptStatus.
  if (pending & ~raised_) {
    raised_ |= pending;
    if (bus_.raise_interrupt)
      bus_.raise_interrupt();
  }
}

Device *Ohci::find_device(std::uint8_t address) {
  for (Port &p : ports_) {
    if (p.device && (p.status & kConnected) && (p.status & kEnabled) && !(p.status & kSuspended) &&
        p.device->address() == address)
      return p.device;
  }
  return nullptr;
}

void Ohci::retire(std::uint32_t ed, std::uint32_t td, std::uint32_t next_td, unsigned condition, bool halt) {
  std::uint32_t control = bus_.read32(td);
  control = (control & ~0xFC000000u) | (condition << 28);  // CC, ErrorCount 0
  bus_.write32(td, control);
  const std::uint32_t head = bus_.read32(ed + 8);
  bus_.write32(ed + 8, (next_td & ~0xFu) | (head & 2u) | (halt ? 1u : 0u));
  bus_.write32(td + 8, done_head_);
  done_head_ = td;
  ++transfers_;
}

// Runs the general TDs queued on one endpoint descriptor. Returns true when at least one TD completed or NAKed.
bool Ohci::process_ed(std::uint32_t ed) {
  bool worked = false;
  for (int count = 0; count < kMaxTdsPerEd; ++count) {
    const std::uint32_t info = bus_.read32(ed);
    const std::uint32_t tail = bus_.read32(ed + 4) & ~0xFu;
    const std::uint32_t head_word = bus_.read32(ed + 8);
    const std::uint32_t td = head_word & ~0xFu;
    if ((info & (1u << 14)) || (head_word & 1u) || td == tail || td == 0)
      return worked;  // skipped, halted or empty
    if (info & (1u << 15))
      return worked;  // isochronous: no isochronous device on this bus

    const std::uint8_t address = static_cast<std::uint8_t>(info & 0x7Fu);
    const unsigned endpoint = (info >> 7) & 0xFu;
    const unsigned ed_direction = (info >> 11) & 3u;
    const std::uint32_t control = bus_.read32(td);
    const std::uint32_t buffer = bus_.read32(td + 4);
    const std::uint32_t next_td = bus_.read32(td + 8);
    const std::uint32_t buffer_end = bus_.read32(td + 12);
    const unsigned direction = (ed_direction == 1 || ed_direction == 2) ? ed_direction : ((control >> 19) & 3u);
    const bool rounding = (control & (1u << 18)) != 0;
    const unsigned toggle_field = (control >> 24) & 3u;
    const unsigned toggle = (toggle_field & 2u) ? (toggle_field & 1u) : ((head_word >> 1) & 1u);
    const std::size_t length = buffer == 0 ? 0 : static_cast<std::size_t>(buffer_end - buffer + 1);
    if (length > 0x2000) {  // two 4 KiB pages at most (section 4.3.1.3.1)
      retire(ed, td, next_td, kDataUnderrun, true);
      worked = true;
      continue;
    }

    Device *device = find_device(address);
    if (!device) {
      retire(ed, td, next_td, kDeviceNotResponding, true);
      worked = true;
      continue;
    }
    std::vector<std::uint8_t> data(length);
    int result = 0;
    if (direction == 0) {  // SETUP
      if (length != 8) {
        retire(ed, td, next_td, kStallCode, true);
        worked = true;
        continue;
      }
      std::array<std::uint8_t, 8> packet{};
      for (std::size_t i = 0; i < 8; ++i)
        packet[i] = bus_.read8(buffer + static_cast<std::uint32_t>(i));
      result = device->setup(packet);
    } else if (direction == 1) {  // OUT
      for (std::size_t i = 0; i < length; ++i)
        data[i] = bus_.read8(buffer + static_cast<std::uint32_t>(i));
      result = device->out(endpoint, data);
    } else {  // IN
      result = device->in(endpoint, data);
      if (result > 0)
        for (int i = 0; i < result; ++i)
          bus_.write8(buffer + static_cast<std::uint32_t>(i), data[static_cast<std::size_t>(i)]);
    }
    if (result == kNak)
      return true;  // the TD stays queued; the endpoint is retried in a later frame
    if (result == kStall) {
      retire(ed, td, next_td, kStallCode, true);
      worked = true;
      continue;
    }

    // Success: advance the data toggle in the TD and the endpoint's carry.
    const unsigned next_toggle = toggle ^ 1u;
    std::uint32_t new_control = (control & ~(3u << 24)) | ((2u | next_toggle) << 24);
    bus_.write32(td, new_control);
    bus_.write32(ed + 8, (bus_.read32(ed + 8) & ~2u) | (next_toggle << 1));

    unsigned condition = kNoError;
    bool halt = false;
    const auto moved = static_cast<std::size_t>(result);
    if (direction == 2 && moved < length) {
      if (rounding) {
        bus_.write32(td + 4, buffer + static_cast<std::uint32_t>(moved));
      } else {
        condition = kDataUnderrun;
        halt = true;
        bus_.write32(td + 4, buffer + static_cast<std::uint32_t>(moved));
      }
    } else {
      bus_.write32(td + 4, 0);
    }
    retire(ed, td, next_td, condition, halt);
    worked = true;
    if (direction == 2 && moved < length)
      continue;
  }
  return worked;
}

bool Ohci::process_list(std::uint32_t head, bool periodic, bool *any) {
  std::uint32_t ed = head & ~0xFu;
  for (int count = 0; ed != 0 && count < kMaxEdsPerList; ++count) {
    if (process_ed(ed) && any)
      *any = true;
    if (periodic) {
      // Interrupt endpoints move at most one TD per frame in hardware; the queue simply carries the rest over.
    }
    ed = bus_.read32(ed + 12) & ~0xFu;
  }
  return true;
}

void Ohci::frame() {
  // Root hub port resets complete on the frame clock even before the controller is operational.
  for (Port &p : ports_) {
    if (p.reset_frames > 0 && --p.reset_frames == 0) {
      p.status &= ~kResetting;
      p.status |= kEnabled | kResetChange | kEnableChange;
      if (p.device)
        p.device->reset();
      set_status(kRootHubStatusChange);
    }
    if ((p.status & kPortChangeMask) && !(regs_[kInterruptStatus] & kRootHubStatusChange))
      set_status(kRootHubStatusChange);
  }
  if (!operational())
    return;
  ++frames_;
  const std::uint32_t frame_number = (regs_[kFmNumber] + 1) & 0xFFFFu;
  regs_[kFmNumber] = frame_number;
  const std::uint32_t hcca = regs_[kHcca] & ~0xFFu;
  if (hcca != 0) {
    const std::uint32_t old = bus_.read32(hcca + kHccaFrameNumber);
    bus_.write32(hcca + kHccaFrameNumber, (old & 0xFFFF0000u) | frame_number);
  }
  std::uint32_t status = kStartOfFrame;
  if ((frame_number & 0x7FFFu) == 0)
    status |= kFrameNumberOverflow;

  const std::uint32_t control = regs_[kControl];
  if ((control & kPeriodicListEnable) && hcca != 0)
    process_list(bus_.read32(hcca + 4 * (frame_number & 31u)), true, nullptr);
  if ((control & kControlListEnable) && (regs_[kCommandStatus] & kControlListFilled)) {
    bool any = false;
    process_list(regs_[kControlHeadEd], false, &any);
    if (!any)
      regs_[kCommandStatus] &= ~kControlListFilled;
  }
  if ((control & kBulkListEnable) && (regs_[kCommandStatus] & kBulkListFilled)) {
    bool any = false;
    process_list(regs_[kBulkHeadEd], false, &any);
    if (!any)
      regs_[kCommandStatus] &= ~kBulkListFilled;
  }

  // Done queue write-back: only when the guest has consumed the previous one (WDH clear).
  if (done_head_ != 0 && hcca != 0 && !(regs_[kInterruptStatus] & kWritebackDoneHead)) {
    bus_.write32(hcca + kHccaDoneHead, done_head_);
    regs_[kDoneHead] = 0;
    done_head_ = 0;
    status |= kWritebackDoneHead;
  }
  set_status(status);
}

} // namespace gt4::usb
