// TASK-213 Phase 1: the native Wide -> Fullscreen callback contract (native/native_settings.hpp).
//
// The guest side of the contract is simple and fixed (v2.00): the script callback 0x00180108 is called with a0 = the
// returned hValue slot, a2 = argc and a3 = argv; argc > 0 is a set whose argv[0] is an hInt, argc == 0 is a get that
// must write a boxed hInt into the slot. The fixture builds that shape in synthetic RAM, replaces the boxing call with
// a recorder, and checks the rule: the host value is stored, the file is written, the native is skipped (the pc is
// restored to the caller's return label), the guest option field and the 44-byte record are untouched, and outside the
// Options project the call is passed through to the native.
#include "native_settings.hpp"

#include "game_target.hpp"
#include "ps2/cpu.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "native_settings fixture failed: %s\n", message);
    std::exit(1);
  }
}

constexpr std::uint32_t kOwnerPointer = 0x00622F4Cu;
constexpr std::uint32_t kOwner = 0x00300000u;
constexpr std::uint32_t kProjectWord = 0x3A3BCu;
constexpr std::uint32_t kOption = 0x00380000u;
constexpr std::uint32_t kOptionWideFlag = 0x14u;
constexpr std::uint32_t kOptionRecord = 0x24u;
constexpr std::uint32_t kValueObject = 0x00400000u;
constexpr std::uint32_t kArgv = 0x00410000u;
constexpr std::uint32_t kSlot = 0x00420000u;
constexpr std::uint32_t kReturnLabel = 0x00123456u;

void w32(std::vector<std::uint8_t> &ram, std::uint32_t at, std::uint32_t value) {
  std::memcpy(ram.data() + at, &value, 4);
}

std::uint32_t r32(const std::vector<std::uint8_t> &ram, std::uint32_t at) {
  std::uint32_t value = 0;
  std::memcpy(&value, ram.data() + at, 4);
  return value;
}

void set_value_object(std::vector<std::uint8_t> &ram, std::uint32_t vtable, std::int32_t value) {
  w32(ram, kValueObject + 4, vtable);
  w32(ram, kValueObject + 0x10, static_cast<std::uint32_t>(value));
}

void prepare(std::vector<std::uint8_t> &ram) {
  ram.assign(32u << 20, 0);
  w32(ram, kOwnerPointer, kOwner);
  w32(ram, kOwner + kProjectWord, 0x6974706Fu);  // "opti"
  w32(ram, kOption + kOptionWideFlag, 1);        // the guest always stays 16:9
  for (std::uint32_t i = 0; i < 44; i = i + 4) w32(ram, kOption + kOptionRecord + i, 0x40400000u + i);
  set_value_object(ram, gt4::native_settings::guest().hint_vtable, 1);
  w32(ram, kArgv, kValueObject);
  w32(ram, kSlot, 0);
}

ps2::Cpu setter_cpu(std::int32_t argc) {
  ps2::Cpu cpu{};
  cpu.r[4].w[0] = kSlot;
  cpu.r[5].w[0] = 0;
  cpu.r[6].w[0] = static_cast<std::uint32_t>(argc);
  cpu.r[7].w[0] = kArgv;
  cpu.r[31].w[0] = kReturnLabel;
  return cpu;
}

struct BoxCall {
  int calls = 0;
  std::uint32_t slot = 0;
  std::int32_t value = -1;
};

BoxCall g_box;

bool record_box(ps2::Cpu &, std::uint32_t slot, std::int32_t value, std::uint32_t) {
  ++g_box.calls;
  g_box.slot = slot;
  g_box.value = value;
  return true;
}

void check_guest_untouched(const std::vector<std::uint8_t> &ram) {
  require(r32(ram, kOption + kOptionWideFlag) == 1, "the guest wide flag stays 1 (16:9)");
  for (std::uint32_t i = 0; i < 44; i = i + 4)
    require(r32(ram, kOption + kOptionRecord + i) == 0x40400000u + i, "the 44-byte wide record is untouched");
}

std::string feedback_text(bool fullscreen) {
  return std::string("version = 1\n\n[changes]\nfullscreen = ") + (fullscreen ? "true\n" : "false\n");
}

} // namespace

int main(int argc, char **argv) {
  require(argc >= 2, "usage: native_settings_fixture <work dir>");
  const std::filesystem::path work = argv[1];
  std::error_code error;
  std::filesystem::remove_all(work, error);
  std::filesystem::create_directories(work, error);

  gt4::native_settings::reset_for_test();
  // Pin the v2.00 contract even in a Spec II build (GT4_TARGET_FIXTURE_V200 fills the same values, but the module's
  // table comes from the library's active target).
  const gt4::target::HookAddresses &v200 = gt4::target::kV200Hooks;
  gt4::native_settings::Guest pinned;
  pinned.wide_mode = v200.option_wide_mode_callback;
  pinned.box_int = v200.script_box_int;
  pinned.release_handle = v200.script_release_handle;
  pinned.hint_vtable = v200.script_hint_vtable;
  pinned.owner_pointer = v200.widescreen_owner_pointer;
  pinned.project_word = v200.option_project_word_offset;
  pinned.option_project = v200.option_project_id;
  gt4::native_settings::set_guest_for_test(pinned);
  const gt4::native_settings::Guest &guest = gt4::native_settings::guest();
  require(guest.wide_mode == 0x00180108u, "v2.00 wide_mode callback");
  require(guest.box_int == 0x002FE278u && guest.release_handle == 0x003285F8u, "v2.00 boxing helpers");
  require(guest.hint_vtable == 0x00674188u, "v2.00 hInt vtable");
  require(guest.owner_pointer == 0x00622F4Cu && guest.project_word == 0x3A3BCu, "v2.00 menu state");
  require(guest.option_project == 0x6974706Fu, "the Options project id is 'opti'");
  require(gt4::native_settings::wide_mode_entry() == 0x00180108u, "the dispatch entry");
  require(!gt4::native_settings::host_fullscreen(), "windowed by default");

  const std::string feedback = (work / "game-changes.toml").string();
  _putenv_s("GT4_SETTINGS_FEEDBACK", feedback.c_str());
  std::vector<std::uint8_t> ram;
  prepare(ram);

  // Setter: On -> the host value, the request and the file; the native is skipped and the pc is restored.
  {
    ps2::Cpu cpu = setter_cpu(1);
    require(gt4::native_settings::on_wide_mode(cpu, ram.data(), ram.size(), 3), "the setter is handled");
    require(gt4::native_settings::host_fullscreen(), "the host value is On");
    require(cpu.pc == kReturnLabel, "the pc is the caller's return label");
    check_guest_untouched(ram);
    std::ifstream file(feedback);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    require(text == feedback_text(true), "the feedback file says fullscreen = true");
    require(!std::filesystem::exists(feedback + ".tmp"), "no temporary file is left behind");
    bool pending = false;
    require(gt4::native_settings::take_fullscreen_request(pending) && pending, "the window gets the On request");
    require(!gt4::native_settings::take_fullscreen_request(pending), "the request is taken once");
  }

  // Setter with the same value: still handled, no duplicate log; the file is rewritten with the same content.
  {
    ps2::Cpu cpu = setter_cpu(1);
    require(gt4::native_settings::on_wide_mode(cpu, ram.data(), ram.size(), 3), "the same value is handled");
    require(gt4::native_settings::host_fullscreen(), "On stays On");
    check_guest_untouched(ram);
  }

  // Getter: boxed host value (1) into the caller's slot, with the guest boxing call replaced by the recorder.
  gt4::native_settings::set_box_int_for_test(record_box);
  {
    ps2::Cpu cpu = setter_cpu(0);
    require(gt4::native_settings::on_wide_mode(cpu, ram.data(), ram.size(), 3), "the getter is handled");
    require(g_box.calls == 1 && g_box.slot == kSlot && g_box.value == 1, "the getter boxes On into the return slot");
    require(cpu.pc == kReturnLabel, "the getter restores the return label");
  }
  // Off too, after a row change back (the file must say false).
  gt4::native_settings::set_host_fullscreen(false);
  {
    ps2::Cpu cpu = setter_cpu(0);
    require(gt4::native_settings::on_wide_mode(cpu, ram.data(), ram.size(), 3), "the Off getter is handled");
    require(g_box.calls == 2 && g_box.value == 0, "the getter boxes Off");
  }
  gt4::native_settings::set_box_int_for_test(nullptr);

  // The four paths that must fall back to the native untouched.
  {
    std::vector<std::uint8_t> other = ram;
    w32(other, kOwner + kProjectWord, 0x6F6D7467u);  // "gtmo": the arcade/GT project, not Options
    ps2::Cpu cpu = setter_cpu(0);
    const std::uint32_t pc = cpu.pc;
    require(!gt4::native_settings::on_wide_mode(cpu, other.data(), other.size(), 3),
            "outside the Options project the native runs");
    require(cpu.pc == pc, "the pc is left to the native");
  }
  {
    std::vector<std::uint8_t> other = ram;
    set_value_object(other, 0x00600000u, 1);  // not an hInt
    ps2::Cpu cpu = setter_cpu(1);
    require(!gt4::native_settings::on_wide_mode(cpu, other.data(), other.size(), 3),
            "an unknown value object is left to the native");
  }
  {
    std::vector<std::uint8_t> other = ram;
    w32(other, kArgv, 0);
    ps2::Cpu cpu = setter_cpu(1);
    require(!gt4::native_settings::on_wide_mode(cpu, other.data(), other.size(), 3),
            "a missing argv is left to the native");
  }
  gt4::native_settings::set_box_int_for_test(record_box);
  {
    ps2::Cpu cpu = setter_cpu(0);
    cpu.r[4].w[0] = 0x7FFFFFF0u;  // outside RAM
    require(!gt4::native_settings::on_wide_mode(cpu, ram.data(), ram.size(), 3),
            "an invalid return slot is left to the native");
  }
  gt4::native_settings::set_box_int_for_test(nullptr);

  // Feedback file rules: exact content, atomic replacement and the unset variable.
  {
    require(gt4::native_settings::write_feedback(false, nullptr, 0), "write Off");
    std::ifstream file(feedback);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    require(text == feedback_text(false), "the file says fullscreen = false");
    _putenv_s("GT4_SETTINGS_FEEDBACK", "");
    char reason[128] = {};
    require(!gt4::native_settings::write_feedback(true, reason, sizeof reason), "no path, no file");
    require(std::strstr(reason, "not set") != nullptr, "the reason names the variable");
    _putenv_s("GT4_SETTINGS_FEEDBACK", feedback.c_str());
  }

  // write_feedback_changes: the Screen rows write resolution and monitor in one file (the value text is TOML as given).
  {
    const char *keys[2] = {"resolution", "monitor"};
    const char *values[2] = {"\"1920x1080\"", "\"\\\\\\\\.\\\\DISPLAY1@1920x1080\""};
    require(gt4::native_settings::write_feedback_changes(keys, values, 2, nullptr, 0), "write two changes");
    std::ifstream file(feedback);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    require(text.find("resolution = \"1920x1080\"") != std::string::npos, "the resolution line");
    require(text.find("monitor = \"") != std::string::npos, "the monitor line");
    require(!std::filesystem::exists(feedback + ".tmp"), "no temporary file after two changes");
  }

  // record_fullscreen stores the value, requests it and writes the file.
  {
    require(gt4::native_settings::record_fullscreen(true, "fixture"), "record On");
    bool pending = false;
    require(gt4::native_settings::take_fullscreen_request(pending) && pending, "record requests On");
    require(gt4::native_settings::host_fullscreen(), "record stores On");
  }

  gt4::native_settings::reset_for_test();
  require(!gt4::native_settings::host_fullscreen(), "reset returns to windowed");
  std::filesystem::remove_all(work, error);
  std::printf("native_settings fixture: ok\n");
  return 0;
}
