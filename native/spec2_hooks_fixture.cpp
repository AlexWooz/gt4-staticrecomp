// TASK-100 S8: the Spec II hook table and its write rules, checked in synthetic guest RAM.
//
// This target compiles with GT4_TARGET_SPEC2, in v2.00 builds too, so the rules below always run against the Spec II
// table (the same pattern as gt4_quickfix_v200_fixture for the v2.00 hooks). The record/preset bytes are the values a
// menu RAM dump of the Spec II image holds (docs/SPEC2-PLAN.md, S8).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

#include "game_target.hpp"
#include "license_guard.hpp"
#include "race_display.hpp"
#include "race_widescreen.hpp"

namespace {

int g_failures = 0;

void expect(bool condition, const char *what) {
  if (condition) return;
  std::fprintf(stderr, "spec2_hooks: FAILED %s\n", what);
  ++g_failures;
}

void put32(std::vector<std::uint8_t> &ram, std::uint32_t address, std::uint32_t value) {
  std::memcpy(ram.data() + address, &value, 4);
}

void putf(std::vector<std::uint8_t> &ram, std::uint32_t address, float value) {
  std::memcpy(ram.data() + address, &value, 4);
}

std::uint32_t get32(const std::vector<std::uint8_t> &ram, std::uint32_t address) {
  std::uint32_t value = 0;
  std::memcpy(&value, ram.data() + address, 4);
  return value;
}

// The 11-word screen record as the game writes it (wide 425x240, 4:3 275x205; +0xC width, +0x10 height).
void write_record(std::vector<std::uint8_t> &ram, std::uint32_t address, float width, float height) {
  const std::uint32_t words[7] = {0u, 0x3F800000u, 0u, 0u, 0u, 0x3F7AE147u, 0x3F800000u};
  for (int i = 0; i < 7; ++i) put32(ram, address + 4u * static_cast<std::uint32_t>(i), words[i]);
  putf(ram, address + 0xCu, width);
  putf(ram, address + 0x10u, height);
}

bool record_is(const std::vector<std::uint8_t> &ram, std::uint32_t address, float width, float height) {
  float got_width = 0.0f, got_height = 0.0f;
  std::memcpy(&got_width, ram.data() + address + 0xCu, 4);
  std::memcpy(&got_height, ram.data() + address + 0x10u, 4);
  return got_width == width && got_height == height;
}

using namespace gt4::target;

void check_table() {
  expect(std::is_same_v<std::remove_cv_t<decltype(kSpec2Hooks)>, HookAddresses>, "Spec II table type");
  expect(kSpec2Hooks.widescreen_owner_pointer == 0x664EDCu, "owner pointer");
  expect(kSpec2Hooks.widescreen_option_offset == 0x680u, "option offset");
  expect(kSpec2Hooks.widescreen_flag_offset == 0x14u, "wide flag offset");
  expect(kSpec2Hooks.widescreen_record_offset == 0x24u, "record offset");
  expect(kSpec2Hooks.widescreen_record_bytes == 44u, "record size");
  expect(kSpec2Hooks.widescreen_record_width_offset == 0xCu, "record width offset");
  expect(kSpec2Hooks.widescreen_record_height_offset == 0x10u, "record height offset");
  expect(kSpec2Hooks.widescreen_global == 0x6655A0u, "published wide word");
  expect(kSpec2Hooks.widescreen_preset_4_3 == 0x664E20u, "4:3 preset");
  expect(kSpec2Hooks.widescreen_preset_wide == 0x664E50u, "wide preset");
  expect(kSpec2Hooks.widescreen_preset_4_3_ntsc == 0x664E80u, "4:3 NTSC preset");
  expect(kSpec2Hooks.widescreen_preset_wide_ntsc == 0x664EB0u, "wide NTSC preset");
  expect(kSpec2Hooks.race_display_index_offset == 0x1718u, "race display index offset");
  expect(kSpec2Hooks.race_display_index_normal == 0u, "normal index");
  expect(kSpec2Hooks.race_display_index_480p == 1u, "480p index");
  expect(kSpec2Hooks.race_display_index_1080i == 2u, "1080i index");
  expect(kSpec2Hooks.init_monitor == 0x26BDB0u, "RaceBase::initMonitor");
  expect(kSpec2Hooks.camera_call_return == 0x26BE60u, "camera call return label");
  expect(kSpec2Hooks.race_display_render == 0x1C5040u, "RaceDisplay::render");
  expect(kSpec2Hooks.hud_matrix_offset == 0x70u, "HUD matrix offset");
  expect(kSpec2Hooks.hud_parameter_block == kV200Hooks.hud_parameter_block, "HUD scratch block (ours)");
  expect(kSpec2Hooks.hud_parameter_words == kV200Hooks.hud_parameter_words, "HUD scratch words (ours)");
  // Presets sit 0x30 bytes apart and the owner word follows the last one by 0x2C, exactly as in v2.00.
  expect(kSpec2Hooks.widescreen_preset_wide - kSpec2Hooks.widescreen_preset_4_3 == 0x30u, "preset stride");
  expect(kSpec2Hooks.widescreen_owner_pointer - kSpec2Hooks.widescreen_preset_wide_ntsc == 0x2Cu,
         "owner word after the last preset");
  // Not mapped yet: the request struct and the shape observer disable themselves safely.
  expect(match_or_never(kSpec2Hooks.display_request_state) == kNeverMatch, "request state disabled");
  expect(match_or_never(kSpec2Hooks.license_loader_return) == kSpec2Hooks.license_loader_return,
         "license guard enabled");
  expect(match_or_never(kSpec2Hooks.shape_call_pc) == kNeverMatch, "shape observer disabled");
}

// TASK-100 S8b: the Spec II license demo guard values (derived ELF; see docs/SPEC2-PLAN.md).
void check_license_guard_table() {
  expect(kSpec2Hooks.license_load_demo_inputs == 0x2012C8u, "loadDemoInputs");
  expect(kSpec2Hooks.license_loader_entry == 0x10CA18u, "license loader entry");
  expect(kSpec2Hooks.license_loader_return == 0x10CB54u, "license loader return label");
  expect(kSpec2Hooks.license_replay_loader_entry == 0x10CBA8u, "replay loader entry");
  expect(kSpec2Hooks.license_replay_loader_return == 0x10CCE4u, "replay loader return label");
  expect(kSpec2Hooks.license_crc_table == 0x731DB0u, "CRC table");
  expect(kSpec2Hooks.license_manager_add == 0x1A24u, "Manager add");
  expect(kSpec2Hooks.license_body_shift == 0x80u, "body shift");
  // Only the two Spec II return labels key the guard; other return addresses (including the v2.00 labels) do not.
  expect(gt4::license_guard::loader_for_return(0x10CB54u) == 0x10CA18u, "first loader keys the guard");
  expect(gt4::license_guard::loader_for_return(0x10CCE4u) == 0x10CBA8u, "second loader keys the guard");
  expect(gt4::license_guard::loader_for_return(0x10CB58u) == 0u, "another label does not key the guard");
  expect(gt4::license_guard::loader_for_return(0x1101E4u) == 0u, "the v2.00 label does not key the guard");
  expect(gt4::license_guard::loader_for_return(0x10CCE8u) == 0u, "a later label does not key the guard");
  // The virtual's add plus the loader's shift land on the Manager stream the layout rule reads.
  expect(gt4::license_guard::kManagerAdd + gt4::license_guard::kBodyShift == gt4::license_guard::kManagerOffset,
         "add plus shift reaches the Manager stream");
  const std::uint32_t body = 0x01D0E540u;
  expect(gt4::license_guard::body_pointer_matches(body, body), "unshifted body pointer accepted");
  expect(gt4::license_guard::body_pointer_matches(body, body + 0x80u), "shifted body pointer accepted");
  expect(!gt4::license_guard::body_pointer_matches(body, body + 0x40u), "other body pointer rejected");
  expect(gt4::license_guard::manager_address(body) == body + 0x1A24u, "manager address is the virtual's read");
  expect(gt4::license_guard::manager_address(body + 0x80u) == body + 0x1AA4u, "shifted pointer manager is the stream");
}

// The guard rule itself on synthetic bodies (the same checks native_license_guard runs on v2.00).
void check_license_guard_rule() {
  std::vector<std::uint8_t> body(0x1BB8u, 0u);
  for (std::size_t index = 0; index < 0x100u; ++index) body[index] = static_cast<std::uint8_t>(index * 7u + 3u);
  put32(body, 0x1AACu, 0x10Eu);  // Recorder size; the end must be 0x1BB8
  put32(body, 0x1AA8u, 0x5BFu);  // frames
  put32(body, 0x1AA4u, 0u);      // count n
  const std::uint32_t key = (gt4::license_guard::crc32(body.data(), body.size()) & 0x00FFFFFFu) |
                            (static_cast<std::uint32_t>(body[0]) << 24);
  const gt4::license_guard::Verdict sane = gt4::license_guard::evaluate(body.data(), body.size(), key ^ 0x10000u);
  expect(!sane.crc_ok && sane.layout_ok && !sane.guard, "a sane layout with a failing CRC is left alone");
  const gt4::license_guard::Verdict crc = gt4::license_guard::evaluate(body.data(), body.size(), key);
  expect(crc.crc_ok && !crc.guard, "a passing CRC is never guarded, whatever the layout");
  put32(body, 0x1AA4u, 0x7F7F7F7Fu);
  const gt4::license_guard::Verdict broken = gt4::license_guard::evaluate(body.data(), body.size(), key ^ 0x10000u);
  expect(!broken.crc_ok && !broken.layout_ok && broken.count == 0x7F7F7F7Fu && broken.guard,
         "an insane count with a failing CRC is guarded");
}

void check_widescreen_hold() {
  std::vector<std::uint8_t> ram(32u << 20, 0);
  write_record(ram, 0x664E20u, 275.0f, 205.0f);  // 4:3
  write_record(ram, 0x664E50u, 425.0f, 240.0f);  // wide
  write_record(ram, 0x664E80u, 275.0f, 205.0f);  // 4:3 NTSC
  write_record(ram, 0x664EB0u, 425.0f, 240.0f);  // wide NTSC
  expect(gt4::display::record_is_widescreen(ram.data() + 0x664E50u), "wide preset reads wide");
  expect(!gt4::display::record_is_widescreen(ram.data() + 0x664E20u), "4:3 preset reads 4:3");

  const std::uint32_t owner = 0x100000u;
  const std::uint32_t option = owner + kSpec2Hooks.widescreen_option_offset;
  put32(ram, kSpec2Hooks.widescreen_owner_pointer, owner);
  put32(ram, option + kSpec2Hooks.widescreen_flag_offset, 0u);
  write_record(ram, option + kSpec2Hooks.widescreen_record_offset, 275.0f, 205.0f);
  put32(ram, kSpec2Hooks.widescreen_global, 0u);

  gt4::display::WidescreenState state;
  // The hold only writes the settled live object: one move then 40 stable VBlanks.
  state.settle.current = 0x900000u;
  state.settle.moves = 1;
  for (int i = 0; i < 40; ++i) gt4::display::owner_settled(state.settle, owner);
  expect(state.settle.current == owner && state.settle.stable >= 30, "owner settled on the live object");
  expect(gt4::display::force_widescreen_at_vblank(ram.data(), static_cast<std::uint32_t>(ram.size()), state),
         "wide hold wrote");
  expect(get32(ram, option + kSpec2Hooks.widescreen_flag_offset) == 1u, "wide flag forced");
  expect(record_is(ram, option + kSpec2Hooks.widescreen_record_offset, 425.0f, 240.0f), "wide record copied");
  expect(get32(ram, kSpec2Hooks.widescreen_global) == 1u, "published wide word set");
}

void check_480p_hold() {
  std::vector<std::uint8_t> ram(32u << 20, 0);
  const std::uint32_t owner = 0x100000u;
  const std::uint32_t index = owner + kSpec2Hooks.widescreen_option_offset + kSpec2Hooks.race_display_index_offset;
  put32(ram, kSpec2Hooks.widescreen_owner_pointer, owner);
  put32(ram, index, kSpec2Hooks.race_display_index_normal);
  gt4::display::Force480pState state;
  expect(gt4::display::force_race_480p_at_vblank(ram.data(), static_cast<std::uint32_t>(ram.size()), state),
         "480p hold wrote on the normal index");
  expect(get32(ram, index) == kSpec2Hooks.race_display_index_480p, "480p index forced");
  // TASK-078: a 1080i save races at 480p too.
  put32(ram, index, kSpec2Hooks.race_display_index_1080i);
  expect(gt4::display::force_race_480p_at_vblank(ram.data(), static_cast<std::uint32_t>(ram.size()), state),
         "480p hold wrote on the 1080i index");
  expect(get32(ram, index) == kSpec2Hooks.race_display_index_480p, "1080i index forced to 480p");
}

}  // namespace

int main() {
  check_table();
  check_license_guard_table();
  check_license_guard_rule();
  check_widescreen_hold();
  check_480p_hold();
  if (g_failures) {
    std::fprintf(stderr, "spec2_hooks: %d failure(s)\n", g_failures);
    return 1;
  }
  std::fprintf(stderr, "spec2_hooks: all checks passed\n");
  return 0;
}
