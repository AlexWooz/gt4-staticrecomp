// TASK-100 S3/S8: compile native/game_target.hpp as the Spec II target and expose the result to game_target_fixture.
//
// GT4_TARGET=spec2 defines GT4_TARGET_SPEC2 for the whole build in CMake; here it is defined locally so one fixture
// binary can check both tables without a second build. S8 filled the table: the values below are the Spec II guest
// addresses found in the derived ELF and confirmed against a runtime RAM dump (docs/SPEC2-PLAN.md, S8). The hook
// write rules run in their own target (native/spec2_hooks_fixture.cpp) with the Spec II table.
#define GT4_TARGET_SPEC2 1
#include "game_target.hpp"

bool spec2_target_has_the_s8_hooks() {
  const gt4::target::Profile &profile = gt4::target::active();
  if (profile.id != gt4::target::Id::spec2)
    return false;
  if (profile.ram_size != 32u << 20)
    return false;
  const gt4::target::HookAddresses &h = profile.hooks;
  const gt4::target::HookAddresses &v = gt4::target::kV200Hooks;
  // The option chain, the published word and the presets.
  if (h.widescreen_owner_pointer != 0x664EDCu || h.widescreen_option_offset != 0x680u ||
      h.widescreen_flag_offset != 0x14u || h.widescreen_record_offset != 0x24u ||
      h.widescreen_record_bytes != 44u || h.widescreen_record_width_offset != 0xCu ||
      h.widescreen_record_height_offset != 0x10u || h.widescreen_global != 0x6655A0u)
    return false;
  if (h.widescreen_preset_4_3 != 0x664E20u || h.widescreen_preset_wide != 0x664E50u ||
      h.widescreen_preset_4_3_ntsc != 0x664E80u || h.widescreen_preset_wide_ntsc != 0x664EB0u)
    return false;
  if (h.video_system_offset != 0u || h.video_system_ntsc != 1u)
    return false;
  // The race display index, the code hooks and the HUD matrix.
  if (h.race_display_index_offset != 0x1718u || h.race_display_index_normal != 0u ||
      h.race_display_index_480p != 1u || h.race_display_index_1080i != 2u)
    return false;
  if (h.init_monitor != 0x26BDB0u || h.camera_call_return != 0x26BE60u || h.race_display_render != 0x1C5040u ||
      h.hud_matrix_offset != 0x70u)
    return false;
  // Our own HUD scratch block is target-independent and must match the v2.00 entry.
  if (h.hud_parameter_block != v.hud_parameter_block || h.hud_parameter_words != v.hud_parameter_words)
    return false;
  // TASK-100 S8b: the license demo guard is mapped (the demo-input virtual adds 0x1A24 to a pointer the loader
  // hands it 0x80 into the body, reaching the same Manager stream at body + 0x1AA4).
  if (h.license_load_demo_inputs != 0x2012C8u || h.license_loader_entry != 0x10CA18u ||
      h.license_loader_return != 0x10CB54u || h.license_replay_loader_entry != 0x10CBA8u ||
      h.license_replay_loader_return != 0x10CCE4u || h.license_crc_table != 0x731DB0u ||
      h.license_manager_add != 0x1A24u || h.license_body_shift != 0x80u)
    return false;
  // TASK-159: the 360-degree chase camera (the spring at 0x002063D8, the pad buffer 0x0089B0C0 + 0x1E, the analog handler
  // at 0x00314870).
  if (h.camera_yaw_call_return != 0x00206490u || h.camera_pad_stick_x != 0x0089B0DEu ||
      h.camera_accelerator_scale_return != 0x00314948u || h.camera_brake_scale_return != 0x00314970u)
    return false;
  // TASK-095: the 2D overlay ortho calls (the menu context at 0x0036D278, the strobe overlay renderer at 0x001FDCC8, whose
  // call from the race monitor 0x001F2E98 is left alone).
  if (h.menu_ortho_call_return != 0x0036D4D8u || h.overlay_ortho_call_return != 0x001FDE18u ||
      h.overlay_draw_end_return != 0x001FDE60u || h.overlay_caller_slot != 0x60u ||
      h.monitor_overlay_call_return != 0x001F2FD4u)
    return false;
  // Not mapped yet: the request struct and the shape observer stay disabled.
  return h.display_request_state == 0 && h.shape_call_pc == 0 && h.shape_target_pc == 0 && h.shape_return_pc == 0;
}

bool spec2_match_addresses_never_match() {
  const gt4::target::HookAddresses &h = gt4::target::active().hooks;
  return gt4::target::match_or_never(h.camera_call_return) == h.camera_call_return &&
         gt4::target::match_or_never(h.race_display_render) == h.race_display_render &&
         gt4::target::match_or_never(h.display_request_state) == gt4::target::kNeverMatch &&
         gt4::target::match_or_never(h.license_loader_return) == h.license_loader_return &&
         gt4::target::match_or_never(0x388B38u) == 0x388B38u;
}
