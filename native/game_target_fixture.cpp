// TASK-100 S3: the per-target guest-hook table (native/game_target.hpp).
//
// The v2.00 half checks every documented address, so using the table instead of the per-module constants provably keeps
// the release values. The Spec II half is compiled by game_target_spec2_fixture.cpp with GT4_TARGET_SPEC2, to prove an
// empty table disables every hook and that the overlays' match addresses can never hit.

#include "game_target.hpp"

#include <cstdio>

bool spec2_target_has_the_s8_hooks();
bool spec2_match_addresses_never_match();

namespace {

int failures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "game_target_fixture: FAILED %s\n", what);
    ++failures;
  }
}

void check_v200_values() {
  using namespace gt4::target;
  const Profile &profile = active();
  check(profile.id == Id::v200, "active target is v2.00 by default");
  check(profile.ram_size == 32u << 20, "v2.00 guest RAM is 32 MiB");
  const HookAddresses &h = profile.hooks;
  // TASK-079 16:9 hold.
  check(h.widescreen_owner_pointer == 0x622F4Cu, "widescreen owner pointer");
  check(h.widescreen_option_offset == 0x38CB0u, "widescreen option offset");
  check(h.widescreen_flag_offset == 0x14u, "widescreen flag offset");
  check(h.widescreen_record_offset == 0x24u, "widescreen record offset");
  check(h.widescreen_record_bytes == 44u, "widescreen record size");
  check(h.widescreen_record_width_offset == 0xCu, "widescreen record width");
  check(h.widescreen_record_height_offset == 0x10u, "widescreen record height");
  check(h.video_system_offset == 0x1388u, "video system offset");
  check(h.video_system_ntsc == 1u, "video system NTSC value");
  check(h.widescreen_global == 0x6184F0u, "published wide word");
  check(h.widescreen_preset_4_3 == 0x622E90u, "4:3 preset");
  check(h.widescreen_preset_wide == 0x622EC0u, "wide preset");
  check(h.widescreen_preset_4_3_ntsc == 0x622EF0u, "4:3 NTSC preset");
  check(h.widescreen_preset_wide_ntsc == 0x622F20u, "wide NTSC preset");
  // TASK-080 race display 480p.
  check(h.race_display_index_offset == 0x16B0u, "race display index offset");
  check(h.race_display_index_normal == 0u, "race display normal index");
  check(h.race_display_index_480p == 1u, "race display 480p index");
  check(h.race_display_index_1080i == 2u, "race display 1080i index");
  // TASK-080 display aspect.
  check(h.init_monitor == 0x388A80u, "RaceBase::initMonitor");
  check(h.camera_call_return == 0x388B38u, "camera call return label");
  check(h.race_display_render == 0x39F498u, "RaceDisplay::render");
  check(h.display_request_state == 0x6184D0u, "display request state");
  check(h.display_request_pending_offset == 0x24u, "display request pending offset");
  check(h.display_request_mode_offset == 0x2Cu, "display request mode offset");
  check(h.display_request_row_offset == 0x30u, "display request row offset");
  check(h.hud_parameter_block == 0x7F00u, "HUD parameter block");
  check(h.hud_parameter_words == 2u, "HUD parameter words");
  check(h.hud_matrix_offset == 0x70u, "HUD matrix offset");
  // TASK-017 license demo guard.
  check(h.license_load_demo_inputs == 0x3EAD18u, "loadDemoInputs");
  check(h.license_loader_entry == 0x1100D0u, "first loader entry");
  check(h.license_loader_return == 0x1101E4u, "first loader return label");
  check(h.license_replay_loader_entry == 0x110230u, "second loader entry");
  check(h.license_replay_loader_return == 0x110344u, "second loader return label");
  check(h.license_crc_table == 0x655358u, "CRC table");
  check(h.license_manager_add == 0x1AA4u, "Manager add");
  check(h.license_body_shift == 0u, "body shift");
  // TASK-133 shape observation (diagnostic).
  check(h.shape_call_pc == 0x455A5Cu, "shape call pc");
  check(h.shape_target_pc == 0x49D308u, "shape target pc");
  check(h.shape_return_pc == 0x455A64u, "shape return pc");
  // TASK-159 360-degree chase camera.
  check(h.camera_yaw_call_return == 0x3456D8u, "chase camera yaw call return label");
  check(h.camera_pad_stick_x == 0x86F99Eu, "right stick X byte of pad port 0");
  check(h.camera_analog_loop_return == 0x43BE74u, "DualShock analog loop's handler call return label");
  check(h.camera_analog_channel_table == 0x6A64C0u && h.camera_analog_slot_bias == 0u, "analog loop slot table, slot in s0");
  // TASK-160 field of view adjustment.
  check(h.fov_projection_call_return == 0x37A1C4u, "projection store call return label");
  check(h.fov_pad_buttons == 0x64C794u, "port 0 button halfword");
  // TASK-095 2D overlays over the race view.
  check(h.menu_ortho_call_return == 0x2151A8u, "menu context ortho return label");
  check(h.overlay_ortho_call_return == 0x3EC200u, "strobe overlay renderer ortho return label");
  check(h.overlay_draw_end_return == 0x3EC248u, "strobe overlay renderer end-of-draw return label");
  check(h.overlay_caller_slot == 0x60u, "strobe overlay renderer saved ra slot");
  check(h.monitor_overlay_call_return == 0x3DA294u, "race monitor's call of the strobe renderer");
  // Both v2.00 tables (the release one and the one the modules compile against) agree.
  check(kV200Hooks.widescreen_owner_pointer == h.widescreen_owner_pointer, "kV200Hooks is the active table");
}

void check_helpers() {
  using namespace gt4::target;
  check(available(0x100000u), "available() on a real address");
  check(!available(0u), "available() on an absent hook");
  check(match_or_never(0u) == kNeverMatch, "absent hook becomes the sentinel");
  check(match_or_never(0x388B38u) == 0x388B38u, "present hook keeps its address");
  check(kNeverMatch != 0u && (kNeverMatch & 3u) == 3u, "sentinel is not word aligned, so no PC can equal it");
  // One log line per hook: the first call reports, later ones stay quiet.
  check(note_hook_unavailable("fixture hook"), "first note reports");
  check(!note_hook_unavailable("fixture hook"), "second note is quiet");
  check(note_hook_unavailable("another fixture hook"), "a different hook reports once");
}

} // namespace

int main() {
  check_v200_values();
  check_helpers();
  check(spec2_target_has_the_s8_hooks(), "Spec II table has the S8 hooks and disables the rest");
  check(spec2_match_addresses_never_match(), "Spec II overlay keys are the sentinel");
  if (failures != 0) {
    std::fprintf(stderr, "game_target_fixture: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("game_target_fixture: ok\n");
  return 0;
}
