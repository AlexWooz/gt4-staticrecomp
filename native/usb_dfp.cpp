#include "usb_dfp.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace gt4::usb {

namespace {

constexpr std::uint16_t kLogitech = 0x046D;
constexpr std::uint16_t kDrivingForcePro = 0xC298;
constexpr std::uint16_t kDeviceRelease = 0x1106;

// Classic coefficient steps k = 0..7 (Logitech's force feedback command format).
constexpr double kCoefficient[8] = {1.0 / 16, 1.0 / 8, 3.0 / 16, 1.0 / 4, 3.0 / 8, 2.0 / 4, 3.0 / 4, 1.0};

double classic(unsigned k) { return kCoefficient[k & 7u]; }
double fine(unsigned k) { return static_cast<double>(k & 15u) / 15.0; }
double signed_side(double coefficient, unsigned sign_bit) { return (sign_bit & 1u) ? -coefficient : coefficient; }

std::vector<std::uint8_t> string_descriptor(const char *text) {
  std::vector<std::uint8_t> out{0, 3};
  for (const char *c = text; *c; ++c) {
    out.push_back(static_cast<std::uint8_t>(*c));
    out.push_back(0);
  }
  out[0] = static_cast<std::uint8_t>(out.size());
  return out;
}

// GT4_DFP_TEST_STEER=<percent of full travel>: the reported steering follows a triangle wave, one full period every 600
// interrupt IN reports (about 6 s at the 100 reports per second the game polls), starting at full left. It lets a headless
// run show what the game asks for while the wheel is off centre (TASK-192); 0 or unset turns it off.
std::uint16_t swept_steering(std::uint64_t reports, int percent) {
  const double phase = static_cast<double>(reports % 600u) / 600.0;
  const double triangle = phase < 0.5 ? phase * 2.0 : 2.0 - phase * 2.0;  // 0 = full left, 1 = full right
  const double offset = (triangle * 2.0 - 1.0) * (std::clamp(percent, 0, 100) / 100.0);
  return static_cast<std::uint16_t>(std::lround(std::clamp(8191.5 + offset * 8191.0, 0.0, 16383.0)));
}

} // namespace

bool trace_enabled() {
  static const bool on = [] {
    const char *value = std::getenv("GT4_DFP_TRACE");
    return value && *value == '1';
  }();
  return on;
}

// ---------------------------------------------------------------------------------------------------------- bridge

DfpBridge &DfpBridge::shared() {
  static DfpBridge bridge;
  // GT4_DFP=1 plugs the virtual wheel in without a host wheel (headless tests); an enabled wheel.toml does it too.
  static const bool forced = [] {
    const char *value = std::getenv("GT4_DFP");
    if (value && *value == '1')
      bridge.set_enabled(true);
    return true;
  }();
  (void)forced;
  return bridge;
}

void DfpBridge::set_input(const DfpInput &input) {
  std::lock_guard lock(mutex_);
  input_ = input;
}

DfpInput DfpBridge::input() const {
  std::lock_guard lock(mutex_);
  return input_;
}

DfpForces DfpBridge::forces() const {
  std::lock_guard lock(mutex_);
  return forces_;
}

void DfpBridge::set_forces(const DfpForces &forces) {
  std::function<void()> listener;
  {
    std::lock_guard lock(mutex_);
    const std::uint64_t revision = forces_.revision + 1;
    forces_ = forces;
    forces_.revision = revision;
    listener = listener_;
  }
  if (listener)
    listener();
}

void DfpBridge::set_force_listener(std::function<void()> listener) {
  std::lock_guard lock(mutex_);
  listener_ = std::move(listener);
}

bool DfpBridge::configured() const {
  std::lock_guard lock(mutex_);
  return configured_;
}

void DfpBridge::set_configured(bool value) {
  std::lock_guard lock(mutex_);
  configured_ = value;
}

bool DfpBridge::enabled() const {
  std::lock_guard lock(mutex_);
  return enabled_;
}

void DfpBridge::set_enabled(bool value) {
  std::lock_guard lock(mutex_);
  enabled_ = value;
}

// ---------------------------------------------------------------------------------------------------------- decoder

void DfpCommandDecoder::reset() {
  slots_ = {};
  forces_ = DfpForces{};
}

bool DfpCommandDecoder::apply(const std::uint8_t *command, std::size_t length) {
  if (length < 7)
    return false;
  if (command[0] == 0xF8) {  // extended commands
    switch (command[1]) {
    case 0x01:  // native DFP mode
    case 0x09:  // change mode
    case 0x0A:  // revert identity
    case 0x06:  // sent once by GT4's driver after configuration (meaning unknown; no observed effect)
      return true;
    case 0x02:
      forces_.range_degrees = 200;
      return true;
    case 0x03:
      forces_.range_degrees = 900;
      return true;
    case 0x81:  // explicit range in degrees
      forces_.range_degrees = std::clamp(command[2] | (command[3] << 8), 40, 900);
      return true;
    default:
      return false;
    }
  }
  const unsigned operation = command[0] & 0x0Fu;
  const unsigned mask = command[0] >> 4;
  switch (operation) {
  case 0x0:  // download
  case 0x1:  // download and play
  case 0xC:  // refresh (update a playing force)
    for (unsigned slot = 0; slot < 4; ++slot) {
      if (!(mask & (1u << slot)))
        continue;
      slots_[slot].type = command[1];
      std::memcpy(slots_[slot].params.data(), command + 2, 5);
      if (operation == 0x1)
        slots_[slot].active = true;
    }
    break;
  case 0x2:  // play
    for (unsigned slot = 0; slot < 4; ++slot)
      if (mask & (1u << slot))
        slots_[slot].active = true;
    break;
  case 0x3:  // stop
    for (unsigned slot = 0; slot < 4; ++slot)
      if (mask & (1u << slot))
        slots_[slot].active = false;
    break;
  case 0x4:  // default spring on
    forces_.autocenter = true;
    break;
  case 0x5:  // default spring off
    forces_.autocenter = false;
    break;
  case 0xE:  // set default spring
    forces_.autocenter_strength = std::max(classic(command[2]), classic(command[3])) * (command[4] / 255.0);
    break;
  case 0x8:  // normal mode
  case 0x9:  // LED
  case 0xB:  // raw mode
  case 0xF:  // dead band
    return true;
  default:
    return false;
  }
  recompute();
  return true;
}

void DfpCommandDecoder::recompute() {
  DfpForces &f = forces_;
  f.constant = 0.0;
  f.spring = f.damper = f.friction = false;
  for (unsigned slot = 0; slot < 4; ++slot) {
    const DfpSlot &s = slots_[slot];
    if (!s.active)
      continue;
    const auto &p = s.params;
    switch (s.type) {
    case 0x00:  // constant: this slot's force byte, 128 = none
      f.constant += (static_cast<double>(p[slot]) - 128.0) / 127.0;
      break;
    case 0x08:  // variable force: level L1 (and L2 for the slot pair's second force), 128 = none. With zero step
                // parameters it holds its level, which is how GT4 drives the wheel while racing (observed
                // 2026-11-05: about 100 "11 08 <L1> 80 00 00 00" per second on slot 0, L1 following the car).
      f.constant += (static_cast<double>(slot >= 2 ? p[1] : p[0]) - 128.0) / 127.0;
      break;
    case 0x01:  // spring
    case 0x0B: {  // high-resolution spring
      const bool hi = s.type == 0x0B;
      const unsigned k1 = p[2] & 0x0Fu, k2 = p[2] >> 4;
      f.spring = true;
      f.spring_left = signed_side(hi ? fine(k1) : classic(k1), p[3]);
      f.spring_right = signed_side(hi ? fine(k2) : classic(k2), p[3] >> 4);
      f.spring_center = ((p[0] + p[1]) / 2.0 - 127.5) / 127.5;
      f.spring_deadband = std::abs(static_cast<int>(p[1]) - static_cast<int>(p[0])) / 255.0 * 2.0;
      f.spring_saturation = p[4] / 255.0;
      break;
    }
    case 0x02:  // damper
    case 0x0C: {  // high-resolution damper
      const bool hi = s.type == 0x0C;
      f.damper = true;
      f.damper_left = signed_side(hi ? fine(p[0]) : classic(p[0]), p[1]);
      f.damper_right = signed_side(hi ? fine(p[2]) : classic(p[2]), p[3]);
      f.damper_saturation = s.type == 0x0C ? p[4] / 255.0 : 1.0;
      break;
    }
    case 0x03:  // auto-centre as a slot force
    case 0x0D:
      f.spring = true;
      f.spring_left = f.spring_right =
          (s.type == 0x0D ? std::max(fine(p[0]), fine(p[1])) : std::max(classic(p[0]), classic(p[1])));
      f.spring_center = 0.0;
      f.spring_deadband = 0.0;
      f.spring_saturation = p[2] / 255.0;
      break;
    case 0x0E:  // friction
      f.friction = true;
      f.friction_coefficient = std::max(p[0], p[1]) / 255.0;
      f.friction_saturation = p[2] / 255.0;
      break;
    default:  // periodic shapes, ramp: not reproduced
      break;
    }
  }
  f.constant = std::clamp(f.constant, -1.0, 1.0);
}

// ---------------------------------------------------------------------------------------------------------- device

const std::vector<std::uint8_t> &Dfp::report_descriptor() {
  // HID report descriptor for the 8-byte input report and the 7-byte vendor output report.
  static const std::vector<std::uint8_t> descriptor = {
      0x05, 0x01,             // Usage Page (Generic Desktop)
      0x09, 0x04,             // Usage (Joystick)
      0xA1, 0x01,             // Collection (Application)
      0xA1, 0x02,             //   Collection (Logical)
      0x95, 0x01, 0x75, 0x0E, //     Report Count 1, Report Size 14
      0x15, 0x00, 0x26, 0xFF, 0x3F, // Logical Minimum 0, Logical Maximum 16383
      0x35, 0x00, 0x46, 0xFF, 0x3F, // Physical Minimum 0, Physical Maximum 16383
      0x09, 0x30, 0x81, 0x02, //     Usage (X), Input (Data, Variable, Absolute)
      0x95, 0x0E, 0x75, 0x01, //     Report Count 14, Report Size 1
      0x25, 0x01, 0x45, 0x01, //     Logical Maximum 1, Physical Maximum 1
      0x05, 0x09, 0x19, 0x01, 0x29, 0x0E, // Usage Page (Button), Usage Minimum 1, Usage Maximum 14
      0x81, 0x02,             //     Input (Data, Variable, Absolute)
      0x05, 0x01, 0x95, 0x01, 0x75, 0x04, // Usage Page (Generic Desktop), Report Count 1, Report Size 4
      0x25, 0x07, 0x46, 0x3B, 0x01, 0x65, 0x14, // Logical Maximum 7, Physical Maximum 315, Unit (degrees)
      0x09, 0x39, 0x81, 0x42, //     Usage (Hat Switch), Input (Data, Variable, Absolute, Null State)
      0x65, 0x00, 0x75, 0x08, //     Unit (None), Report Size 8
      0x26, 0xFF, 0x00, 0x46, 0xFF, 0x00, // Logical Maximum 255, Physical Maximum 255
      0x81, 0x01,             //     Input (Constant): reserved byte
      0x09, 0x31, 0x81, 0x02, //     Usage (Y): throttle
      0x09, 0x35, 0x81, 0x02, //     Usage (Rz): brake
      0x81, 0x01,             //     Input (Constant): marker byte
      0xC0,                   //   End Collection
      0x06, 0x00, 0xFF,       //   Usage Page (Vendor Defined)
      0x09, 0x01, 0x95, 0x07, 0x75, 0x08, // Usage (1), Report Count 7, Report Size 8
      0x91, 0x02,             //   Output (Data, Variable, Absolute): force feedback commands
      0xC0,                   // End Collection
  };
  return descriptor;
}

const std::vector<std::uint8_t> &Dfp::device_descriptor() {
  static const std::vector<std::uint8_t> descriptor = {
      18, 1, 0x10, 0x01,  // bcdUSB 1.10
      0, 0, 0, 8,         // class per interface, EP0 max packet 8
      kLogitech & 0xFF, kLogitech >> 8, kDrivingForcePro & 0xFF, kDrivingForcePro >> 8,
      kDeviceRelease & 0xFF, kDeviceRelease >> 8,
      3, 1, 0,            // manufacturer string 3, product string 1, no serial
      1,                  // one configuration
  };
  return descriptor;
}

const std::vector<std::uint8_t> &Dfp::configuration_descriptor() {
  static const std::vector<std::uint8_t> descriptor = [] {
    const auto report_length = static_cast<std::uint16_t>(report_descriptor().size());
    std::vector<std::uint8_t> d = {
        9, 2, 41, 0, 1, 1, 0, 0xC0, 40,          // configuration 1, self-powered, 80 mA
        9, 4, 0, 0, 2, 3, 0, 0, 0,               // interface 0: HID, two endpoints
        9, 0x21, 0x00, 0x01, 0, 1, 0x22,         // HID 1.00, one report descriptor
        static_cast<std::uint8_t>(report_length & 0xFF), static_cast<std::uint8_t>(report_length >> 8),
        7, 5, 0x81, 3, 8, 0, 10,                 // EP1 interrupt IN, 8 bytes, 10 ms
        7, 5, 0x02, 3, 8, 0, 10,                 // EP2 interrupt OUT, 8 bytes, 10 ms
    };
    return d;
  }();
  return descriptor;
}

std::array<std::uint8_t, 8> Dfp::encode_report(const DfpInput &input) {
  std::uint64_t word = 0;
  word |= static_cast<std::uint64_t>(input.steering & 0x3FFFu);
  word |= static_cast<std::uint64_t>(input.buttons & 0x3FFFu) << 14;
  word |= static_cast<std::uint64_t>(input.hat & 0xFu) << 28;
  word |= static_cast<std::uint64_t>(255u - input.throttle) << 40;
  word |= static_cast<std::uint64_t>(255u - input.brake) << 48;
  word |= static_cast<std::uint64_t>(0x11u) << 56;
  std::array<std::uint8_t, 8> report{};
  for (std::size_t i = 0; i < 8; ++i)
    report[i] = static_cast<std::uint8_t>(word >> (8 * i));
  return report;
}

Dfp::Dfp(DfpBridge &bridge) : bridge_(bridge) {}

Dfp::~Dfp() { log_census(); }

void Dfp::log_census() const {
  if (commands_ == 0)
    return;
  std::fprintf(stderr, "[dfp] census: %llu force commands this session, %zu command types:\n",
               static_cast<unsigned long long>(commands_), census_.size());
  for (const auto &[key, value] : census_) {
    std::fprintf(stderr, "[dfp]   %02X %02X x%llu latest", key >> 8, key & 0xFF,
                 static_cast<unsigned long long>(value.first));
    for (const std::uint8_t byte : value.second)
      std::fprintf(stderr, " %02X", byte);
    std::fprintf(stderr, "\n");
  }
  const DfpForces &f = decoder_.forces();
  std::fprintf(stderr,
               "[dfp]   last decoded: constant %.3f, spring %s %.2f/%.2f centre %.2f dead band %.2f saturation %.2f, "
               "damper %s %.2f/%.2f saturation %.2f, friction %s %.2f saturation %.2f, default spring %s, range %d\n",
               f.constant, f.spring ? "on" : "off", f.spring_left, f.spring_right, f.spring_center, f.spring_deadband,
               f.spring_saturation, f.damper ? "on" : "off", f.damper_left, f.damper_right, f.damper_saturation,
               f.friction ? "on" : "off", f.friction_coefficient, f.friction_saturation,
               f.autocenter ? "on" : "off", f.range_degrees);
}

void Dfp::reset() {
  address_ = pending_address_ = 0;
  address_pending_ = false;
  configuration_ = 0;
  response_.clear();
  response_offset_ = 0;
  expect_report_data_ = false;
  decoder_.reset();
  bridge_.set_configured(false);
  bridge_.set_forces(decoder_.forces());
}

int Dfp::setup(std::span<const std::uint8_t, 8> p) {
  const std::uint8_t request_type = p[0], request = p[1];
  const unsigned value = p[2] | (p[3] << 8), index = p[4] | (p[5] << 8), length = p[6] | (p[7] << 8);
  response_.clear();
  response_offset_ = 0;
  expect_report_data_ = false;
  const auto reply = [&](const std::vector<std::uint8_t> &data) {
    response_.assign(data.begin(), data.begin() + std::min<std::size_t>(data.size(), length));
    return 0;
  };
  if (trace_enabled())
    std::fprintf(stderr, "[dfp] setup %02X %02X value %04X index %04X length %u\n", request_type, request, value, index,
                 length);
  if ((request_type & 0x60) == 0x00) {  // standard
    switch (request) {
    case 0x00:  // GET_STATUS
      return reply({0, 0});
    case 0x01:  // CLEAR_FEATURE
    case 0x03:  // SET_FEATURE
    case 0x0B:  // SET_INTERFACE
      return 0;
    case 0x05:  // SET_ADDRESS: takes effect after the status stage
      pending_address_ = static_cast<std::uint8_t>(value & 0x7F);
      address_pending_ = true;
      return 0;
    case 0x06: {  // GET_DESCRIPTOR
      const unsigned type = value >> 8, which = value & 0xFF;
      if (type == 1)
        return reply(device_descriptor());
      if (type == 2)
        return reply(configuration_descriptor());
      if (type == 3) {
        if (which == 0)
          return reply({4, 3, 0x09, 0x04});
        if (which == 1)
          return reply(string_descriptor("Logitech Driving Force Pro"));
        if (which == 3)
          return reply(string_descriptor("Logitech"));
        return kStall;
      }
      if (type == 0x21)
        return reply(std::vector<std::uint8_t>(configuration_descriptor().begin() + 18,
                                               configuration_descriptor().begin() + 27));
      if (type == 0x22)
        return reply(report_descriptor());
      return kStall;
    }
    case 0x08:  // GET_CONFIGURATION
      return reply({configuration_});
    case 0x09:  // SET_CONFIGURATION
      configuration_ = static_cast<std::uint8_t>(value & 0xFF);
      bridge_.set_configured(configuration_ != 0);
      std::fprintf(stderr, "[dfp] the game's USB driver configured the virtual Driving Force Pro (configuration %u)\n",
                   configuration_);
      return 0;
    case 0x0A:  // GET_INTERFACE
      return reply({0});
    default:
      return kStall;
    }
  }
  if ((request_type & 0x60) == 0x20) {  // HID class
    switch (request) {
    case 0x01: {  // GET_REPORT
      const auto report = encode_report(bridge_.input());
      return reply(std::vector<std::uint8_t>(report.begin(), report.end()));
    }
    case 0x09:  // SET_REPORT: the data stage carries a command
      expect_report_data_ = true;
      return 0;
    case 0x0A:  // SET_IDLE
    case 0x0B:  // SET_PROTOCOL
      return 0;
    default:
      return kStall;
    }
  }
  return kStall;
}

int Dfp::in(unsigned endpoint, std::span<std::uint8_t> buffer) {
  if (endpoint == 0) {
    if (buffer.empty() && address_pending_) {  // status stage of SET_ADDRESS
      address_ = pending_address_;
      address_pending_ = false;
      return 0;
    }
    const std::size_t count = std::min(buffer.size(), response_.size() - response_offset_);
    std::copy_n(response_.begin() + static_cast<std::ptrdiff_t>(response_offset_), count, buffer.begin());
    response_offset_ += count;
    return static_cast<int>(count);
  }
  if (endpoint == 1) {
    ++reports_;
    if (trace_enabled() && (reports_ <= 3 || reports_ % 1000 == 0))
      std::fprintf(stderr, "[dfp] input report %llu read by the game\n", static_cast<unsigned long long>(reports_));
    DfpInput input = bridge_.input();
    // Headless check (GT4_DFP_TEST_PULSE=<button bit 0..13>): taps that button for 0.5 s in every second of reports.
    static const int test_bit = [] {
      const char *value = std::getenv("GT4_DFP_TEST_PULSE");
      return value && *value ? std::atoi(value) : -1;
    }();
    if (test_bit >= 0 && test_bit < 14 && (reports_ / 50) % 2 == 1)
      input.buttons |= static_cast<std::uint16_t>(1u << test_bit);
    // Headless check (GT4_DFP_TEST_STEER=<percent>): turns the virtual wheel, so a bench sees the game's forces off centre.
    static const int test_steer = [] {
      const char *value = std::getenv("GT4_DFP_TEST_STEER");
      return value && *value ? std::atoi(value) : 0;
    }();
    if (test_steer > 0)
      input.steering = swept_steering(reports_, test_steer);
    last_reported_ = input;
    const auto report = encode_report(input);
    const std::size_t count = std::min<std::size_t>(buffer.size(), report.size());
    std::copy_n(report.begin(), count, buffer.begin());
    return static_cast<int>(count);
  }
  return kStall;
}

int Dfp::out(unsigned endpoint, std::span<const std::uint8_t> data) {
  if (endpoint == 0) {
    if (expect_report_data_ && !data.empty()) {
      command(data.data(), data.size());
      expect_report_data_ = false;
    }
    return static_cast<int>(data.size());
  }
  if (endpoint == 2) {
    for (std::size_t at = 0; at + 7 <= data.size(); at += 8)
      command(data.data() + at, std::min<std::size_t>(8, data.size() - at));
    return static_cast<int>(data.size());
  }
  return kStall;
}

void Dfp::command(const std::uint8_t *data, std::size_t length) {
  const bool known = decoder_.apply(data, length);
  ++commands_;
  // Force recording with GT4_DFP_TRACE=1 (dfp-forces.csv in the working folder): host time, the game's force level and
  // the steering the game last saw, one row per command.
  {
    static std::FILE *csv = [] {
      std::FILE *file = trace_enabled() ? std::fopen("dfp-forces.csv", "w") : nullptr;
      if (file)
        std::fprintf(file, "ms,constant,steering,command,type,level\n");
      return file;
    }();
    if (csv) {
      using clock = std::chrono::steady_clock;
      static const clock::time_point start = clock::now();
      const double ms = std::chrono::duration<double, std::milli>(clock::now() - start).count();
      std::fprintf(csv, "%.2f,%.4f,%u,%02X,%02X,%u\n", ms, decoder_.forces().constant, last_reported_.steering,
                   data[0], data[1], data[2]);
      if (commands_ % 200 == 0)
        std::fflush(csv);
    }
  }
  // Census of the commands the game sends: per (command byte, type byte), a count and the latest bytes, printed every
  // 600 commands (about every 6 s while driving).
  {
    const unsigned key = (static_cast<unsigned>(data[0]) << 8) | data[1];
    auto &entry = census_[key];
    ++entry.first;
    std::memcpy(entry.second.data(), data, std::min<std::size_t>(length, 8));
    if (trace_enabled() && commands_ % 600 == 0) {
      std::fprintf(stderr, "[dfp] census after %llu commands:\n", static_cast<unsigned long long>(commands_));
      for (const auto &[k, value] : census_) {
        std::fprintf(stderr, "[dfp]   %02X %02X x%llu latest", k >> 8, k & 0xFF, static_cast<unsigned long long>(value.first));
        for (const std::uint8_t byte : value.second)
          std::fprintf(stderr, " %02X", byte);
        std::fprintf(stderr, "\n");
      }
    }
  }
  if (trace_enabled() || !known || commands_ <= 8) {
    std::fprintf(stderr, "[dfp] command%s", known ? "" : " (not understood)");
    for (std::size_t i = 0; i < length; ++i)
      std::fprintf(stderr, " %02X", data[i]);
    std::fprintf(stderr, "\n");
  }
  if (known)
    bridge_.set_forces(decoder_.forces());
}

// ---------------------------------------------------------------------------------------------------------- machine

Ohci &machine_controller(const Bus &bus) {
  static std::unique_ptr<Ohci> controller;
  static std::unique_ptr<Dfp> wheel;
  if (!controller) {
    controller = std::make_unique<Ohci>(bus);
    DfpBridge &bridge = DfpBridge::shared();
    if (bridge.enabled()) {
      // GT4_DFP_PORT=1|2 picks the root hub port. Port 2 by default: GT4 v2.00 reads the wheel's input only from the
      // second root hub port (headless check 2026-11-05: wheel Cross presses answer the first-start dialog on port 2
      // and are ignored on port 1, although the driver configures and polls the device on both).
      unsigned port = 1;
      if (const char *chosen = std::getenv("GT4_DFP_PORT"); chosen && *chosen == '1')
        port = 0;
      wheel = std::make_unique<Dfp>(bridge);
      controller->attach(port, wheel.get());
      std::fprintf(stderr, "[dfp] virtual Driving Force Pro plugged into USB port %u\n", port + 1);
    }
  }
  return *controller;
}

} // namespace gt4::usb
