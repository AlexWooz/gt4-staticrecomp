// TASK-213 Phase 1: the native Wide row as a host Fullscreen control. See native/native_settings.hpp.
#include "native_settings.hpp"

#include "game_target.hpp"
#include "ps2/generated.hpp"
#include "ps2/system.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace gt4::native_settings {
namespace {

// The hInt fields the contract reads (gt4/hInt.h): the vtable at +4 and the integer at +0x10.
constexpr std::uint32_t kVtableOffset = 4;
constexpr std::uint32_t kValueOffset = 0x10;
constexpr std::uint32_t kHintBytes = 0x14;

std::atomic<bool> g_host_fullscreen{false};
std::atomic<int> g_pending{-1};  // -1 none, 0 Off, 1 On
BoxInt g_box_int_for_test = nullptr;
Guest g_guest_for_test;
bool g_guest_for_test_set = false;

std::uint32_t ld32(const std::uint8_t *ram, std::size_t size, std::uint32_t at) {
  if (at > size || size - at < 4) return 0;
  std::uint32_t value = 0;
  std::memcpy(&value, ram + at, 4);
  return value;
}

bool within(std::uint32_t at, std::uint32_t bytes, std::size_t size) {
  return at != 0 && at <= size && size - at >= bytes;
}

bool option_project_active_guest(const Guest &guest, const std::uint8_t *ram, std::size_t size) {
  if (!guest.owner_pointer || !guest.project_word || !guest.option_project) return false;
  const std::uint32_t owner = ld32(ram, size, guest.owner_pointer);
  if (!within(owner, guest.project_word + 4u, size)) return false;
  return ld32(ram, size, owner + guest.project_word) == guest.option_project;
}

// The value the OptionMenu passes as argv[0]: a guest hInt (the script's int type). Anything else is left to the
// native so an unexpected shape can never corrupt the guest.
bool read_index(const Guest &guest, const std::uint8_t *ram, std::size_t size, std::uint32_t argv, std::int32_t &index) {
  if (!within(argv, 4, size)) return false;
  const std::uint32_t object = ld32(ram, size, argv);
  if (!within(object, kHintBytes, size)) return false;
  if (ld32(ram, size, object + kVtableOffset) != guest.hint_vtable) {
    static bool noted = false;
    if (!noted) {
      noted = true;
      std::fprintf(stderr,
                   "[native-settings] wide_mode value object vtable 0x%08X is not hInt (0x%08X); the Wide row keeps the "
                   "native behaviour\n",
                   ld32(ram, size, object + kVtableOffset), guest.hint_vtable);
    }
    return false;
  }
  index = static_cast<std::int32_t>(ld32(ram, size, object + kValueOffset));
  return true;
}

bool guest_box_int(ps2::Cpu &cpu, std::uint32_t slot, std::int32_t value, std::uint32_t depth) {
  const Guest &g = guest();
  if (!g.box_int || !g.release_handle || !ps2::has_code(g.box_int)) return false;
  cpu.r[4].w[0] = slot;
  cpu.r[5].w[0] = static_cast<std::uint32_t>(value);
  ps2::call(cpu, g.box_int, depth + 1u);
  return true;
}

bool guest_release(ps2::Cpu &cpu, std::uint32_t handle, std::uint32_t depth) {
  const Guest &g = guest();
  if (!g.release_handle || !ps2::has_code(g.release_handle)) return false;
  cpu.r[4].w[0] = handle;
  ps2::call(cpu, g.release_handle, depth + 1u);
  return true;
}

bool box_int(ps2::Cpu &cpu, std::uint32_t slot, std::int32_t value, std::uint32_t depth) {
  if (g_box_int_for_test) return g_box_int_for_test(cpu, slot, value, depth);
  return guest_box_int(cpu, slot, value, depth);
}

bool release_handle(ps2::Cpu &cpu, std::uint32_t handle, std::uint32_t depth) {
  if (g_box_int_for_test) return true;  // the fixture has no guest handle table; nothing to release
  return guest_release(cpu, handle, depth);
}

} // namespace

const Guest &guest() {
  if (g_guest_for_test_set) return g_guest_for_test;
  static const Guest table = [] {
    Guest value;
    const gt4::target::HookAddresses &hooks = gt4::target::active().hooks;
    value.wide_mode = hooks.option_wide_mode_callback;
    value.box_int = hooks.script_box_int;
    value.release_handle = hooks.script_release_handle;
    value.hint_vtable = hooks.script_hint_vtable;
    value.owner_pointer = hooks.widescreen_owner_pointer;
    value.project_word = hooks.option_project_word_offset;
    value.option_project = hooks.option_project_id;
    return value;
  }();
  return table;
}

std::uint32_t wide_mode_entry() { return guest().wide_mode; }

bool host_fullscreen() { return g_host_fullscreen.load(std::memory_order_relaxed); }

void set_host_fullscreen(bool on) { g_host_fullscreen.store(on, std::memory_order_relaxed); }

bool take_fullscreen_request(bool &on) {
  const int pending = g_pending.exchange(-1);
  if (pending < 0) return false;
  on = pending != 0;
  return true;
}

bool record_fullscreen(bool on, const char *source) {
  set_host_fullscreen(on);
  g_pending.store(on ? 1 : 0);
  std::fprintf(stderr, "[native-settings] %s: fullscreen %s; the guest wide flag and the save stay 16:9\n", source,
               on ? "On" : "Off");
  char error[160] = {};
  if (write_feedback(on, error, sizeof error)) return true;
  if (error[0] != '\0') std::fprintf(stderr, "[native-settings] feedback not written: %s\n", error);
  return false;
}

bool on_wide_mode(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth) {
  const Guest &g = guest();
  if (!g.wide_mode || !g.box_int || !g.release_handle || !g.hint_vtable) {
    gt4::target::note_hook_unavailable("native settings (Wide -> Fullscreen)");
    return false;
  }
  if (!ram || !option_project_active_guest(g, ram, size)) return false;
  const std::uint32_t return_label = cpu.r[31].w[0];
  if (cpu.r[6].w[0] > 0) {
    // Setter: the script ran `option.wide_mode = Pane::Wide::OptionMenu.index`.
    std::int32_t index = -1;
    if (!read_index(g, ram, size, cpu.r[7].w[0], index)) return false;
    if (index != 0 && index != 1) {
      static bool noted = false;
      if (!noted) {
        noted = true;
        std::fprintf(stderr, "[native-settings] wide_mode set %d is outside the two choices; the native runs\n", index);
      }
      return false;
    }
    const bool on = index == 1;
    if (on != host_fullscreen()) {
      record_fullscreen(on, "Wide row");
    } else {
      std::fprintf(stderr, "[native-settings] Wide row set %s (already %s)\n", on ? "On" : "Off", on ? "On" : "Off");
    }
    cpu.pc = return_label;
    return true;
  }
  // Getter: `ScreenRoot.reflect` reads `option.wide_mode` into the row's index.
  const std::uint32_t slot = cpu.r[4].w[0];
  if (!within(slot, 4, size)) {
    static bool noted = false;
    if (!noted) {
      noted = true;
      std::fprintf(stderr, "[native-settings] wide_mode getter slot 0x%08X is outside RAM; the native runs\n", slot);
    }
    return false;
  }
  // Both guest calls must exist before the old handle is released: after a release the native must not run as well, or
  // the handle would be released twice.
  const bool boxer_ready =
      g_box_int_for_test != nullptr || (ps2::has_code(g.box_int) && ps2::has_code(g.release_handle));
  if (!boxer_ready) {
    static bool noted = false;
    if (!noted) {
      noted = true;
      std::fprintf(stderr, "[native-settings] wide_mode getter has no translated boxing call in this target; the native "
                           "runs\n");
    }
    return false;
  }
  const std::uint32_t old = ld32(ram, size, slot);
  if (old != 0 && !release_handle(cpu, old, depth)) {
    std::fprintf(stderr, "[native-settings] wide_mode getter could not release the old handle 0x%08X\n", old);
    return false;
  }
  if (!box_int(cpu, slot, host_fullscreen() ? 1 : 0, depth)) {
    std::fprintf(stderr, "[native-settings] wide_mode getter boxing call failed after the old handle was released\n");
    cpu.pc = return_label;
    return true;
  }
  cpu.pc = return_label;
  return true;
}

bool write_feedback(bool fullscreen, char *error, std::size_t error_size) {
  const char *key = "fullscreen";
  const char *value = fullscreen ? "true" : "false";
  return write_feedback_changes(&key, &value, 1, error, error_size);
}

bool write_feedback_changes(const char *const *keys, const char *const *values, std::size_t count, char *error,
                            std::size_t error_size) {
  const auto fail = [error, error_size](const char *text) {
    if (error && error_size > 0) std::snprintf(error, error_size, "%s", text);
    return false;
  };
  const char *path = std::getenv(kFeedbackVariable);
  if (path == nullptr || *path == '\0') return fail("GT4_SETTINGS_FEEDBACK is not set");
  if (count == 0 || keys == nullptr || values == nullptr) return fail("no change to write");
  const std::string target(path);
  const std::string temporary = target + ".tmp";
  std::string text = "version = 1\n\n[changes]\n";
  for (std::size_t i = 0; i < count; ++i) {
    text += keys[i];
    text += " = ";
    text += values[i];
    text += "\n";
  }
  std::FILE *file = std::fopen(temporary.c_str(), "wb");
  if (!file) return fail("cannot create the temporary feedback file");
  const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
  const bool flushed = std::fflush(file) == 0;
  std::fclose(file);
  if (written != text.size() || !flushed) {
    std::remove(temporary.c_str());
    return fail("cannot write the temporary feedback file");
  }
  if (!MoveFileExA(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::remove(temporary.c_str());
    return fail("cannot replace the feedback file");
  }
  std::fprintf(stderr, "[native-settings] feedback %s:", target.c_str());
  for (std::size_t i = 0; i < count; ++i) std::fprintf(stderr, " %s = %s", keys[i], values[i]);
  std::fprintf(stderr, "\n");
  return true;
}

bool option_project_active(const std::uint8_t *ram, std::size_t size) {
  return ram != nullptr && option_project_active_guest(guest(), ram, size);
}

bool read_hint_value(const std::uint8_t *ram, std::size_t size, std::uint32_t argv, std::int32_t &value) {
  return read_index(guest(), ram, size, argv, value);
}

void set_box_int_for_test(BoxInt fn) { g_box_int_for_test = fn; }

void set_guest_for_test(const Guest &guest) {
  g_guest_for_test = guest;
  g_guest_for_test_set = true;
}

void reset_for_test() {
  g_host_fullscreen.store(false);
  g_pending.store(-1);
  g_box_int_for_test = nullptr;
  g_guest_for_test = Guest{};
  g_guest_for_test_set = false;
}

} // namespace gt4::native_settings
