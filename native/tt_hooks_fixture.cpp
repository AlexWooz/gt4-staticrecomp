// CTest native_tt_hooks (Tourist Trophy target, GT4_TARGET=tt): the TT guest-hook table and the license demo guard rule
// at TT's own Manager offset. Built only in a TT build, where the active table is the TT one.

#include "game_target.hpp"
#include "license_guard.hpp"
#include "race_widescreen.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

void put32(std::vector<std::uint8_t> &body, std::uint32_t offset, std::uint32_t value) {
  for (int byte = 0; byte < 4; ++byte) body[offset + byte] = static_cast<std::uint8_t>(value >> (8 * byte));
}

std::uint32_t key_for(const std::vector<std::uint8_t> &body) {
  return (gt4::license_guard::crc32(body.data(), body.size()) & 0x00FFFFFFu) |
         (static_cast<std::uint32_t>(body[0]) << 24);
}

}  // namespace

int main() {
  namespace target = gt4::target;
  namespace guard = gt4::license_guard;
  check(target::active().id == target::Id::tt, "active target is Tourist Trophy");
  const target::HookAddresses &h = target::active().hooks;

  // TASK-079 16:9 hold (docs/TT-STUDY.md: matched from the v2.00 chain, option offset confirmed in guest RAM).
  check(h.widescreen_owner_pointer == 0x5857DCu && h.widescreen_option_offset == 0x680u, "16:9 owner and option");
  check(h.widescreen_flag_offset == 0x14u && h.widescreen_record_offset == 0x24u && h.widescreen_record_bytes == 44u,
        "16:9 flag and record");
  check(h.widescreen_global == 0x579518u, "16:9 published global");
  check(h.widescreen_preset_4_3 == 0x585720u && h.widescreen_preset_wide == 0x585750u &&
            h.widescreen_preset_4_3_ntsc == 0x585780u && h.widescreen_preset_wide_ntsc == 0x5857B0u,
        "16:9 presets");

  // TASK-017 license demo guard.
  check(h.license_loader_entry == 0x10B6D0u && h.license_loader_return == 0x10B810u, "license loader 1");
  check(h.license_replay_loader_entry == 0x10B860u && h.license_replay_loader_return == 0x10B9A0u, "license loader 2");
  check(h.license_load_demo_inputs == 0x386D90u && h.license_crc_table == 0x6191A0u, "demo virtual and CRC table");
  check(h.license_manager_add == 0x1A24u && h.license_body_shift == 0u, "TT Manager add, no body shift");
  check(guard::kManagerOffset == 0x1A24u, "TT Manager offset");
  check(guard::loader_for_return(0x10B810u) == 0x10B6D0u && guard::loader_for_return(0x10B9A0u) == 0x10B860u &&
            guard::loader_for_return(0x1101E4u) == 0u,
        "guarded call sites are TT's, never v2.00's");
  check(guard::manager_address(0x00900040u) == 0x00900040u + 0x1A24u, "guard writes where the TT virtual reads");

  // Race display 480p: option + 0x1718, 0 normal, 1 480p, 2 1080i (TT also has 3 = 480i_frame, never rewritten).
  check(h.race_display_index_offset == 0x1718u && h.race_display_index_normal == 0u && h.race_display_index_480p == 1u &&
            h.race_display_index_1080i == 2u,
        "race display index");

  // 16:9 hold: TT's single game-data object counts as live after kOwnerSettleVblanks of stability (no boot move).
  check(!gt4::display::kOwnerMovesAtBoot, "TT does not wait for a game-data pointer move");
  {
    gt4::display::OwnerSettle settle;
    for (int vblank = 0; vblank < 10; ++vblank) check(!gt4::display::owner_settled(settle, 0u), "no owner yet");
    int settled_at = -1;
    for (int vblank = 0; vblank < 100 && settled_at < 0; ++vblank)
      if (gt4::display::owner_settled(settle, 0x008274B0u)) settled_at = vblank;
    check(settled_at == static_cast<int>(gt4::display::kOwnerSettleVblanks), "TT owner settles after the short wait");
  }

  // Hooks that are still unmapped stay disabled.
  // Frame skipping (TASK-082): functors, the flip's page words and the race cluster of vtables.
  check(h.frame_update_functor == 0x001007D8u && h.frame_render_functor == 0x00100848u && h.frame_idle_functor == 0x001008B8u,
        "frame functors");
  check(h.frame_display_page == 0x005795ACu && h.frame_display_page_second == h.frame_display_page + 0x7Cu, "frame pages");
  check(h.frame_race_vtable_first <= 0x005CB218u && 0x005CB218u <= h.frame_race_vtable_last &&
            h.frame_race_vtable_first <= 0x005D1AB0u && 0x005D4748u <= h.frame_race_vtable_last,
        "the Arcade and license race classes are in the race range");
  check(!(h.frame_race_vtable_first <= 0x005D9E00u && 0x005D9E00u <= h.frame_race_vtable_last),
        "the other DynamicsConductor holder is outside the race range");
  check(h.frame_dynamics_clock_offset == 0x23960u && h.frame_dynamics_clock_offset < h.frame_dynamics_bytes, "race clock");
  // 360-degree camera and field of view (TASK-159 / 159b / 160).
  check(h.camera_yaw_call_return == 0x002E7120u && h.camera_pad_stick_x == 0x00776F1Eu, "camera yaw call and stick X");
  check(h.camera_analog_loop_return == 0x00403ACCu && h.camera_analog_channel_table == 0x005FB078u &&
            h.camera_analog_slot_bias == 1u,
        "analog loop channel handler");
  check(h.fov_projection_call_return == 0x00322DD4u && h.fov_pad_buttons == 0x005986F4u, "field of view");
  check(!target::available(h.frame_render_world) && !target::available(h.race_display_render) &&
            !target::available(h.menu_ortho_call_return),
        "unmapped hooks disabled");

  // The guard rule on a TT-layout body: one Recorder at body + 0x1A24 that ends at the body end.
  std::vector<std::uint8_t> sane(0x1A24u + 4u + 0x110u, 0u);
  for (std::size_t index = 0; index < 0x100u; ++index) sane[index] = static_cast<std::uint8_t>(index * 5u + 1u);
  put32(sane, 0x1A24u, 0u);
  put32(sane, 0x1A28u, 0x5BFu);
  put32(sane, 0x1A2Cu, 0x10Eu);
  guard::Verdict verdict = guard::evaluate(sane.data(), sane.size(), key_for(sane) ^ 0x00010000u);
  check(!verdict.crc_ok && verdict.layout_ok && !verdict.guard, "TT sane layout, CRC fail: kept");
  std::vector<std::uint8_t> insane = sane;
  put32(insane, 0x1A24u, 0x7F7F7F7Fu);
  put32(insane, 0x1A28u, 0x7F7F7F7Fu);
  verdict = guard::evaluate(insane.data(), insane.size(), key_for(sane));
  check(verdict.guard && verdict.count == 0x7F7F7F7Fu, "TT insane count, CRC fail: guarded");
  verdict = guard::evaluate(insane.data(), insane.size(), key_for(insane));
  check(verdict.crc_ok && !verdict.guard, "TT insane count, CRC pass: kept");

  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("native_tt_hooks: ok");
  return 0;
}
