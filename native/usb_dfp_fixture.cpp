// usb_dfp_fixture: the OHCI controller model and the virtual Driving Force Pro, driven the way a guest USB driver
// drives them (root hub reset, enumeration on the control list, interrupt IN on the periodic list, force commands on
// interrupt OUT), against a fake guest memory. Also checks the force feedback command decoder.
#include "usb_dfp.hpp"
#include "usb_ohci.hpp"

#include <algorithm>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace gt4::usb;

namespace {

int failures = 0;
void require(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

struct Memory {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x10000);
  std::uint32_t read32(std::uint32_t a) const {
    std::uint32_t v;
    std::memcpy(&v, &bytes[a], 4);
    return v;
  }
  void write32(std::uint32_t a, std::uint32_t v) { std::memcpy(&bytes[a], &v, 4); }
};

// Guest layout in the fake memory.
constexpr std::uint32_t kHcca = 0x1000;
constexpr std::uint32_t kControlEd = 0x2000;
constexpr std::uint32_t kInterruptEd = 0x2040;
constexpr std::uint32_t kOutEd = 0x2080;
constexpr std::uint32_t kTd = 0x3000;      // TDs at 0x3000 + 16 * n
constexpr std::uint32_t kBuffer = 0x4000;  // data at 0x4000 + 0x100 * n

struct Guest {
  Memory memory;
  Ohci ohci;
  int interrupts = 0;
  explicit Guest()
      : ohci(Bus{[this](std::uint32_t a) { return memory.read32(a); },
                 [this](std::uint32_t a, std::uint32_t v) { memory.write32(a, v); },
                 [this](std::uint32_t a) { return memory.bytes[a]; },
                 [this](std::uint32_t a, std::uint8_t v) { memory.bytes[a] = v; }, [this] { ++interrupts; }}) {}

  std::uint32_t td(int n) const { return kTd + 16u * static_cast<std::uint32_t>(n); }
  std::uint32_t buffer(int n) const { return kBuffer + 0x100u * static_cast<std::uint32_t>(n); }

  // A general TD: direction pid 0 SETUP, 1 OUT, 2 IN; toggle -1 = from the ED.
  void make_td(int n, unsigned pid, int toggle, std::uint32_t length, std::uint32_t next) {
    std::uint32_t control = (1u << 18) | (pid << 19) | (7u << 21) | (15u << 28);
    if (toggle >= 0)
      control |= (2u | static_cast<std::uint32_t>(toggle)) << 24;
    memory.write32(td(n), control);
    memory.write32(td(n) + 4, length ? buffer(n) : 0);
    memory.write32(td(n) + 8, next);
    memory.write32(td(n) + 12, length ? buffer(n) + length - 1 : 0);
  }
  void make_ed(std::uint32_t ed, std::uint8_t address, unsigned endpoint, unsigned direction, std::uint32_t head,
               std::uint32_t tail) {
    memory.write32(ed, address | (endpoint << 7) | (direction << 11) | (8u << 16));
    memory.write32(ed + 4, tail);
    memory.write32(ed + 8, head);
    memory.write32(ed + 12, 0);
  }
  unsigned condition(int n) const { return memory.read32(td(n)) >> 28; }
  void frames(int count) {
    for (int i = 0; i < count; ++i)
      ohci.frame();
  }

  // One control transfer: SETUP, optional IN data stage of `length`, status stage. Returns the IN data.
  std::vector<std::uint8_t> control_in(std::uint8_t address, const std::uint8_t setup[8], std::uint32_t length) {
    std::memcpy(&memory.bytes[buffer(0)], setup, 8);
    const std::uint32_t tail = td(3);
    make_td(0, 0, 0, 8, td(1));
    if (length) {
      make_td(1, 2, 1, length, td(2));
      make_td(2, 1, 1, 0, tail);  // status OUT
    } else {
      make_td(1, 2, 1, 0, tail);  // status IN
    }
    make_ed(kControlEd, address, 0, 0, td(0), tail);
    ohci.write(0x08, kControlEd);  // HcControlHeadED
    ohci.write(0x02, 1u << 1);     // CLF
    frames(2);
    std::vector<std::uint8_t> out;
    if (length) {
      const std::uint32_t end = memory.read32(td(1) + 4);
      const std::uint32_t got = end == 0 ? length : end - buffer(1);
      out.assign(memory.bytes.begin() + buffer(1), memory.bytes.begin() + buffer(1) + got);
    }
    return out;
  }
};

void test_decoder() {
  DfpCommandDecoder decoder;
  const std::uint8_t constant_play[7] = {0x11, 0x00, 0xC0, 0x80, 0x80, 0x80, 0x00};  // slot 0, constant, 0xC0
  require(decoder.apply(constant_play, 7), "constant download-and-play understood");
  require(std::abs(decoder.forces().constant - (0xC0 - 128.0) / 127.0) < 1e-9, "constant force level from slot 0's byte");
  const std::uint8_t stop[7] = {0x13, 0, 0, 0, 0, 0, 0};
  decoder.apply(stop, 7);
  require(decoder.forces().constant == 0.0, "stop clears the slot's force");
  const std::uint8_t spring[7] = {0x21, 0x01, 0x70, 0x90, 0x77, 0x00, 0xFF};  // slot 1, spring, k1=k2=7
  decoder.apply(spring, 7);
  require(decoder.forces().spring && decoder.forces().spring_left == 1.0 && decoder.forces().spring_right == 1.0,
          "spring coefficients from k nibbles");
  require(std::abs(decoder.forces().spring_center) < 0.01, "spring centred between its dead band bytes");
  const std::uint8_t range900[7] = {0xF8, 0x03, 0, 0, 0, 0, 0};
  decoder.apply(range900, 7);
  require(decoder.forces().range_degrees == 900, "extended command sets the 900-degree range");
  // GT4's race force: variable force on slot 0 (observed "11 08 9F 80 00 00 00"), stopped with "13 08".
  const std::uint8_t variable[7] = {0x11, 0x08, 0x9F, 0x80, 0x00, 0x00, 0x00};
  decoder.apply(variable, 7);
  require(std::abs(decoder.forces().constant - (0x9F - 128.0) / 127.0) < 1e-9, "variable force level L1 drives the wheel");
  const std::uint8_t variable_stop[7] = {0x13, 0x08, 0x7F, 0x80, 0, 0, 0};
  decoder.apply(variable_stop, 7);
  require(decoder.forces().constant == 0.0, "stopping the variable force releases the wheel");
  const std::uint8_t spring_off[7] = {0x05, 0, 0, 0, 0, 0, 0};
  decoder.apply(spring_off, 7);
  require(!decoder.forces().autocenter, "default spring off");
}

void test_report() {
  DfpInput input;
  input.steering = 0x1FFF;
  input.buttons = 1u << 4;  // R1
  input.hat = 8;
  input.throttle = 255;
  input.brake = 0;
  const auto report = Dfp::encode_report(input);
  std::uint64_t word = 0;
  for (int i = 7; i >= 0; --i)
    word = (word << 8) | report[static_cast<std::size_t>(i)];
  require((word & 0x3FFF) == 0x1FFF, "steering centre in bits 0..13");
  require(((word >> 14) & 0x3FFF) == (1u << 4), "R1 in the button field");
  require(((word >> 28) & 0xF) == 8, "hat released = 8");
  require(((word >> 40) & 0xFF) == 0, "throttle fully pressed is 0 on the wire");
  require(((word >> 48) & 0xFF) == 255, "brake released is 255 on the wire");
  require((word >> 56) == 0x11, "marker byte 0x11");
  require(Dfp::configuration_descriptor().size() == 41, "41-byte configuration descriptor");
}

void test_bus() {
  DfpBridge bridge;
  Dfp wheel(bridge);
  Guest guest;
  guest.ohci.attach(0, &wheel);
  require((guest.ohci.read(0x15) & 1u) != 0, "port 1 shows a connected device");
  require((guest.ohci.read(0x16) & 1u) == 0, "port 2 is empty");

  // Bring the controller up as a driver does.
  guest.ohci.write(0x06, kHcca);                         // HcHCCA
  guest.ohci.write(0x04, 0x80000000u | (1u << 1) | (1u << 6));  // MIE, WDH, RHSC
  guest.ohci.write(0x01, (2u << 6) | (1u << 4) | (1u << 2));    // operational, control + periodic lists
  guest.ohci.write(0x15, 1u << 4);                       // SetPortReset
  guest.frames(12);
  require((guest.ohci.read(0x15) & (1u << 1)) != 0, "port enabled after reset");
  require((guest.ohci.read(0x15) & (1u << 20)) != 0, "reset change reported");
  require(guest.interrupts > 0, "root hub status change raised the interrupt");
  guest.ohci.write(0x15, 0x001F0000u);
  guest.ohci.write(0x03, 0xFFFFFFFFu);

  const std::uint8_t get_device[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 18, 0};
  const auto device = guest.control_in(0, get_device, 18);
  require(device.size() == 18 && device[8] == 0x6D && device[9] == 0x04 && device[10] == 0x98 && device[11] == 0xC2,
          "device descriptor: Logitech 046D:C298 at address 0");
  require(guest.condition(0) == 0 && guest.condition(1) == 0 && guest.condition(2) == 0, "enumeration TDs succeeded");
  require(guest.memory.read32(kHcca + 0x84) != 0, "done queue written back to the HCCA");

  const std::uint8_t set_address[8] = {0x00, 0x05, 0x05, 0x00, 0x00, 0x00, 0, 0};
  guest.ohci.write(0x03, 0xFFFFFFFFu);
  guest.control_in(0, set_address, 0);
  require(wheel.address() == 5, "SET_ADDRESS takes effect after its status stage");

  const std::uint8_t get_config[8] = {0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 41, 0};
  guest.ohci.write(0x03, 0xFFFFFFFFu);
  const auto config = guest.control_in(5, get_config, 41);
  require(config.size() == 41 && config[1] == 2 && config[14] == 3, "configuration descriptor with a HID interface");
  const std::uint8_t set_config[8] = {0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0, 0};
  guest.ohci.write(0x03, 0xFFFFFFFFu);
  guest.control_in(5, set_config, 0);
  require(bridge.configured(), "SET_CONFIGURATION marks the wheel configured");

  // Interrupt IN on the periodic list: every slot of the HCCA table points at one ED.
  DfpInput input;
  input.steering = 0x3000;
  input.throttle = 200;
  bridge.set_input(input);
  guest.make_td(10, 2, -1, 8, guest.td(11));
  guest.make_ed(kInterruptEd, 5, 1, 2, guest.td(10), guest.td(11));
  for (std::uint32_t slot = 0; slot < 32; ++slot)
    guest.memory.write32(kHcca + 4 * slot, kInterruptEd);
  guest.ohci.write(0x03, 0xFFFFFFFFu);
  guest.frames(2);
  require(guest.condition(10) == 0, "interrupt IN completed");
  std::uint64_t word = 0;
  for (int i = 7; i >= 0; --i)
    word = (word << 8) | guest.memory.bytes[guest.buffer(10) + static_cast<std::uint32_t>(i)];
  require((word & 0x3FFF) == 0x3000, "the guest receives the host steering value");
  require(((word >> 40) & 0xFF) == 255 - 200, "the guest receives the inverted throttle");

  // Interrupt OUT: a constant force command reaches the bridge.
  const std::uint8_t command[8] = {0x11, 0x00, 0x40, 0x80, 0x80, 0x80, 0x00, 0x00};
  std::memcpy(&guest.memory.bytes[guest.buffer(12)], command, 8);
  guest.make_td(12, 1, -1, 8, guest.td(13));
  guest.make_ed(kOutEd, 5, 2, 1, guest.td(12), guest.td(13));
  for (std::uint32_t slot = 0; slot < 32; ++slot)
    guest.memory.write32(kHcca + 4 * slot, kOutEd);
  guest.ohci.write(0x03, 0xFFFFFFFFu);
  guest.frames(2);
  require(guest.condition(12) == 0, "interrupt OUT completed");
  require(std::abs(bridge.forces().constant - (0x40 - 128.0) / 127.0) < 1e-9, "the game's constant force reaches the host");

  // A wrong address gets no answer.
  guest.make_td(14, 2, -1, 8, guest.td(15));
  guest.make_ed(kInterruptEd, 9, 1, 2, guest.td(14), guest.td(15));
  for (std::uint32_t slot = 0; slot < 32; ++slot)
    guest.memory.write32(kHcca + 4 * slot, kInterruptEd);
  guest.ohci.write(0x03, 0xFFFFFFFFu);
  guest.frames(2);
  require(guest.condition(14) == 5, "no device at address 9: device not responding");
}

} // namespace

int main() {
  test_decoder();
  test_report();
  test_bus();
  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("PASS: OHCI root hub, enumeration, interrupt IN/OUT, DFP report and force commands\n");
  return 0;
}
