#include "race_widescreen.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace gt4::display {
namespace {

std::uint32_t read_le32(const std::uint8_t *bytes) {
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

void write_le32(std::uint8_t *bytes, std::uint32_t value) {
  for (int i = 0; i < 4; ++i)
    bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

float read_le_f32(const std::uint8_t *bytes) {
  const std::uint32_t bits = read_le32(bytes);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

// True when the 32-bit word at guest `address` lies inside guest RAM. 64-bit sum: no wrap.
bool word_in_ram(std::uint64_t address, std::uint32_t ram_size) {
  return address + 4 <= ram_size;
}

// True when `length` bytes at guest `address` lie inside guest RAM.
bool range_in_ram(std::uint64_t address, std::uint64_t length, std::uint32_t ram_size) {
  return address + length <= ram_size;
}

} // namespace

bool parse_force_widescreen(const char *value) {
  if (value == nullptr || std::strcmp(value, "1") == 0)
    return true;
  if (std::strcmp(value, "0") == 0)
    return false;
  throw std::invalid_argument("GT4_FORCE_WIDESCREEN must be 0 or 1");
}

bool widescreen_hold_enabled() {
  static const bool enabled = [] {
    try {
      return parse_force_widescreen(std::getenv("GT4_FORCE_WIDESCREEN"));
    } catch (const std::invalid_argument &error) {
      std::fprintf(stderr, "[display] %s, game not started\n", error.what());
      std::exit(1);
    }
  }();
  return enabled;
}

void widescreen_hold_at_guest_vblank(std::uint8_t *ram, std::uint32_t ram_size) {
  static WidescreenState state;
  if (!widescreen_hold_enabled())
    return;
  force_widescreen_at_vblank(ram, ram_size, state);
}

std::uint32_t widescreen_option_address(std::uint32_t owner_pointer, std::uint32_t ram_size) {
  if (!::gt4::target::available(widescreen_owner_pointer_address))
    return 0;
  if (owner_pointer == 0)
    return 0;
  if (!word_in_ram(widescreen_owner_pointer_address, ram_size))
    return 0;
  const std::uint64_t option = static_cast<std::uint64_t>(owner_pointer) + widescreen_option_object_offset;
  // The furthest word touched is the video system word; the global word and the presets are fixed addresses.
  if (!word_in_ram(option + video_system_offset, ram_size))
    return 0;
  if (!word_in_ram(widescreen_global_address, ram_size))
    return 0;
  if (!range_in_ram(widescreen_preset_wide_ntsc, widescreen_record_bytes, ram_size))
    return 0;
  return static_cast<std::uint32_t>(option);
}

bool record_is_widescreen(const std::uint8_t *record) {
  const float width = read_le_f32(record + widescreen_record_width_offset);
  const float height = read_le_f32(record + widescreen_record_height_offset);
  // Same comparison as func_004390D0: NaN (0 / 0) is not wide.
  return (width / height) >= 1.5f;
}

std::uint32_t widescreen_preset_address(bool wide, bool ntsc) {
  if (wide && ntsc)
    return widescreen_preset_wide_ntsc;
  if (wide)
    return widescreen_preset_wide;
  if (ntsc)
    return widescreen_preset_4_3_ntsc;
  return widescreen_preset_4_3;
}

bool owner_settled(OwnerSettle &settle, std::uint32_t owner_pointer) {
  if (owner_pointer != settle.current) {
    if (settle.current != 0 && owner_pointer != 0)
      ++settle.moves;
    settle.current = owner_pointer;
    settle.stable = 0;
    return false;
  }
  if (owner_pointer == 0)
    return false;
  if (settle.stable < kOwnerLateSettleVblanks)
    ++settle.stable;
  if (settle.stable < kOwnerSettleVblanks)
    return false;
  return settle.moves > 0 || settle.stable >= kOwnerLateSettleVblanks;
}

bool force_widescreen_at_vblank(std::uint8_t *ram, std::uint32_t ram_size, WidescreenState &state) {
  if (!::gt4::target::available(widescreen_owner_pointer_address)) {
    ::gt4::target::note_hook_unavailable("16:9 hold (GT4_FORCE_WIDESCREEN)");
    return false;
  }
  if (!word_in_ram(widescreen_owner_pointer_address, ram_size))
    return false;
  const std::uint32_t owner_pointer = read_le32(ram + widescreen_owner_pointer_address);
  ++state.vblanks;
  if (owner_pointer != state.logged_owner) {
    std::fprintf(stderr, "[display] game-data pointer 0x%08X -> 0x%08X at vblank %llu (moves %u)\n",
                 static_cast<unsigned>(state.logged_owner), static_cast<unsigned>(owner_pointer),
                 static_cast<unsigned long long>(state.vblanks),
                 static_cast<unsigned>(state.settle.moves + (state.logged_owner != 0 && owner_pointer != 0 ? 1 : 0)));
    state.logged_owner = owner_pointer;
  }
  if (!owner_settled(state.settle, owner_pointer))
    return false;
  if (!state.settled_logged) {
    state.settled_logged = true;
    std::fprintf(stderr,
                 "[display] GT4_FORCE_WIDESCREEN: game-data pointer settled on 0x%08X at vblank %llu (moves %u, "
                 "stable %u)\n",
                 static_cast<unsigned>(owner_pointer), static_cast<unsigned long long>(state.vblanks),
                 static_cast<unsigned>(state.settle.moves), static_cast<unsigned>(state.settle.stable));
  }
  const std::uint32_t option = widescreen_option_address(owner_pointer, ram_size);
  if (option == 0)
    return false;

  const std::uint32_t flag_address = option + widescreen_flag_offset;
  const std::uint32_t record_address = option + widescreen_record_offset;
  const std::uint32_t video_address = option + video_system_offset;
  const bool ntsc = read_le32(ram + video_address) == video_system_ntsc;

  // Refuse before any write when the wide preset is not loaded: copying it would put a non-wide record in place.
  const std::uint32_t preset_address = widescreen_preset_address(true, ntsc);
  if (!record_is_widescreen(ram + preset_address)) {
    if (!state.refused_logged) {
      state.refused_logged = true;
      std::fprintf(stderr, "[display] GT4_FORCE_WIDESCREEN: wide preset at guest 0x%08X is not wide; not forced\n",
                   static_cast<unsigned>(preset_address));
    }
    return false;
  }

  bool changed = false;
  if (read_le32(ram + flag_address) != 1u) {
    write_le32(ram + flag_address, 1u);
    changed = true;
  }
  if (!record_is_widescreen(ram + record_address)) {
    std::memcpy(ram + record_address, ram + preset_address, widescreen_record_bytes);
    changed = true;
  }
  const std::uint32_t wide_global = record_is_widescreen(ram + record_address) ? 1u : 0u;
  if (read_le32(ram + widescreen_global_address) != wide_global) {
    write_le32(ram + widescreen_global_address, wide_global);
    changed = true;
  }
  if (!changed)
    return false;

  ++state.writes;
  if (!state.logged) {
    state.logged = true;
    std::fprintf(stderr,
                 "[display] GT4_FORCE_WIDESCREEN: wide flag set to 16:9 at guest 0x%08X (owner 0x%08X, ntsc %d)\n",
                 static_cast<unsigned>(flag_address), static_cast<unsigned>(owner_pointer), ntsc ? 1 : 0);
  }
  return true;
}

} // namespace gt4::display
