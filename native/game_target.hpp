#pragma once

// TASK-100 S3: one table of guest addresses per game target.
//
// The host hooks each touch a handful of fixed guest addresses of one game image:
//   - TASK-079 16:9 hold                      (race_widescreen.*)
//   - TASK-080 race display 480p              (race_display.*)
//   - TASK-080 display aspect, race camera
//     and HUD canvas                          (display_aspect.*, scripts/display_aspect_overlay.inc)
//   - TASK-017 license demo guard             (license_guard.*, scripts/license_guard_overlay.inc)
//   - TASK-159 360-degree chase camera        (camera_360.*, scripts/camera360_units.py)
//   - TASK-160 field of view adjustment       (fov_adjust.*, scripts/camera360_units.py)
//   - TASK-095 2D overlays over the race view (display_aspect.*, scripts/camera360_units.py)
//
// Those addresses are properties of one target. Writing a v2.00 address in another target corrupts guest RAM, so no
// module keeps a private copy any more: the numbers live in this header, once, and every hook reads them here.
//
// A zero field means "this target does not have that hook". The hook then disables itself and logs one line, and the
// overlays use `never_match()` so a guest address can never key a hook that does not exist.
//
// TASK-100 S7b: the v2.00 hook fixtures assert the qualified v2.00 guest chains, so a fixture target may define
// GT4_TARGET_FIXTURE_V200 to pin the active table to v2.00 even in a Spec II build (native/CMakeLists.txt). Production
// code never defines it.
//
// Adding a target (S8 for Spec II) means filling its HookAddresses; the v2.00 entries are the Qualified release values
// and must not change. The target is selected at build time by GT4_TARGET (CMake), default v200.

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace gt4::target {

enum class Id { v200, spec2 };

// A guest address that can never match a real PC (32-bit, word aligned, below 0x08000000) or a real return address.
inline constexpr std::uint32_t kNeverMatch = 0xFFFFFFFFu;

struct HookAddresses {
  // TASK-079 16:9 hold.
  std::uint32_t widescreen_owner_pointer = 0;      // word holding the game-data object pointer
  std::uint32_t widescreen_option_offset = 0;      // owner + this = the option object
  std::uint32_t widescreen_flag_offset = 0;        // option + this = wide flag (0 = 4:3, 1 = 16:9)
  std::uint32_t widescreen_record_offset = 0;      // option + this = 44-byte screen aspect record
  std::uint32_t widescreen_record_bytes = 0;
  std::uint32_t widescreen_record_width_offset = 0;
  std::uint32_t widescreen_record_height_offset = 0;
  std::uint32_t video_system_offset = 0;           // option + this = video system word
  std::uint32_t video_system_ntsc = 0;             // value that means NTSC
  std::uint32_t widescreen_global = 0;             // published "display is wide" word
  std::uint32_t widescreen_preset_4_3 = 0;
  std::uint32_t widescreen_preset_wide = 0;
  std::uint32_t widescreen_preset_4_3_ntsc = 0;
  std::uint32_t widescreen_preset_wide_ntsc = 0;

  // TASK-080 race display 480p (the option object is the same one as above).
  std::uint32_t race_display_index_offset = 0;     // option + this = race display index
  std::uint32_t race_display_index_normal = 0;
  std::uint32_t race_display_index_480p = 0;
  std::uint32_t race_display_index_1080i = 0;

  // TASK-080 display aspect: the two guest entries the capsule keys on, and the HUD/request state it reads.
  std::uint32_t init_monitor = 0;                  // RaceBase::initMonitor
  std::uint32_t camera_call_return = 0;            // return label of the camera virtual call
  std::uint32_t race_display_render = 0;           // RaceDisplay::render (the race HUD)
  std::uint32_t display_request_state = 0;         // struct written by func_001074D0
  std::uint32_t display_request_pending_offset = 0;
  std::uint32_t display_request_mode_offset = 0;
  std::uint32_t display_request_row_offset = 0;
  std::uint32_t hud_parameter_block = 0;           // guest block the HUD scale is published through
  std::uint32_t hud_parameter_words = 0;
  std::uint32_t hud_matrix_offset = 0;             // RaceDisplay + this = 16 f32 HUD matrix

  // TASK-017 license demo guard.
  std::uint32_t license_load_demo_inputs = 0;      // RaceSolitaire::loadDemoInputs (vtbl[154])
  std::uint32_t license_loader_entry = 0;          // the loader function that makes the vtbl[154] call
  std::uint32_t license_loader_return = 0;         // return label after the vtbl[154] call
  std::uint32_t license_replay_loader_entry = 0;   // the loader function that makes the vtbl[57] call
  std::uint32_t license_replay_loader_return = 0;  // return label after the vtbl[57] call
  std::uint32_t license_crc_table = 0;             // the game's CRC-32 table
  std::uint32_t license_manager_add = 0;           // demo-input virtual: pointer + this = Manager::load stream
  std::uint32_t license_body_shift = 0;            // demo loader: pointer - header body start (Spec II 0x80)

  // TASK-133 shape observation (diagnostic capture; the observer only reads guest state).
  std::uint32_t shape_call_pc = 0;
  std::uint32_t shape_target_pc = 0;
  std::uint32_t shape_return_pc = 0;

  // TASK-082 frame skipping (native/frame_skip.hpp): the main loop's functors, the two words the flip inverts, and the race.
  std::uint32_t frame_update_functor = 0;
  std::uint32_t frame_render_functor = 0;
  std::uint32_t frame_idle_functor = 0;
  std::uint32_t frame_display_page = 0;
  std::uint32_t frame_display_page_second = 0;
  std::uint32_t frame_race_vtable_first = 0;
  std::uint32_t frame_race_vtable_last = 0;
  std::uint32_t frame_race_dynamics_offset = 0;
  std::uint32_t frame_dynamics_clock_offset = 0;
  std::uint32_t frame_dynamics_bytes = 0;
  std::uint32_t frame_race_flare_offset = 0;
  std::uint32_t frame_flare_track_count_offset = 0;
  std::uint32_t frame_flare_car_count_offset = 0;
  std::uint32_t frame_render_world = 0;  // RacePS2Base::render_world, reached through a vtable

  // TASK-159 360-degree chase camera (native/camera_360.hpp). The translated yaw call of the chase camera's spring calls
  // the host before it (scripts/camera360_units.py names the unit); the host checks ra against this return label and
  // adds the angle of the right stick X byte below to f13.
  std::uint32_t camera_yaw_call_return = 0;  // return label of `jal <angle difference>` in the chase camera spring
  std::uint32_t camera_pad_stick_x = 0;      // right stick X byte of pad port 0 in the game's pad buffer (+0x1E)
  // While the camera is on, the DualShock analog loop hands a right stick Y channel assigned to the accelerator or the brake
  // to the channel handler as 0 (TASK-159b): the return label of that call, the loop's slot table of channel ids, and s0
  // minus the slot at the call.
  std::uint32_t camera_analog_loop_return = 0;
  std::uint32_t camera_analog_channel_table = 0;
  std::uint32_t camera_analog_slot_bias = 0;

  // TASK-160 field of view adjustment (native/fov_adjust.hpp): the projection function's store call gets the zoom factor
  // (its return label) and the port 0 button halfword the game keeps, read at guest VBlank (active low).
  std::uint32_t fov_projection_call_return = 0;
  std::uint32_t fov_pad_buttons = 0;

  // TASK-095 2D overlays over the race view (native/display_aspect.hpp). The menu context (the pre-race screens) and the strobe
  // overlay renderer (pause, photo pause, pit menu, results, race monitor) set their 2D canvas through a direct call of the
  // ortho function (f12 left, f13 right, f14 bottom, f15 top); scripts/camera360_units.py adds a host call before each, and the
  // host checks ra against these return labels before it widens the canvas for the presented ratio. The strobe overlays' text
  // is sized by the text style parser's stores (translated statements, no address here), scaled between the renderer's ortho
  // call and the call after its draw.
  std::uint32_t menu_ortho_call_return = 0;       // menu context render: 0..640 x 480..0, near 0, far 100
  std::uint32_t overlay_ortho_call_return = 0;    // strobe overlay renderer: 0..640 x 480..0
  std::uint32_t overlay_draw_end_return = 0;      // strobe overlay renderer: return label of the call after the draw
  std::uint32_t overlay_caller_slot = 0;          // the renderer's frame: sp + this = its caller's return address
  std::uint32_t monitor_overlay_call_return = 0;  // the race monitor's call of the renderer (left as the game draws it)

  // TASK-213 Phase 1: the native Wide row as a host Fullscreen control (native/native_settings.hpp). The script's
  // MOption.wide_mode callback is one entry for the getter and the setter (argc > 0 = set); the getter returns a boxed
  // hInt through func_002FE278, and the Options project is recognised by the menu state's next-project id.
  std::uint32_t option_wide_mode_callback = 0;  // 0x00180108: MOption.wide_mode get/set
  std::uint32_t script_box_int = 0;             // func_002FE278: box a guest int into the caller's hValue slot
  std::uint32_t script_release_handle = 0;      // func_003285F8: release a RefCounter handle
  std::uint32_t script_hint_vtable = 0;         // hInt vtable (the OptionMenu index object)
  std::uint32_t option_project_word_offset = 0; // owner + this = the next project's four-character id
  std::uint32_t option_project_id = 0;          // the id that means the Options project ("opti")

  // TASK-213 Phase 1c: the Screen page's Resolution/Monitor rows (native/screen_rows.*). The two screen-adjust natives
  // return boxed ints (same shape as wide_mode); the host paints the slider value TextFace (the localized text string)
  // and the engine's insensitive bit, and enumerates the monitor modes/monitors on the host side.
  std::uint32_t screen_adjust_v_callback = 0;   // 0x001802F0: MOption.screen_adjust_v get/set
  std::uint32_t screen_adjust_h_callback = 0;   // 0x00180200: MOption.screen_adjust_h get/set
  std::uint32_t screen_textface_vtable = 0;     // 0x00666370: mTextFace vtable (object vtable at +4)
  std::uint32_t screen_slider_vtable = 0;       // 0x00671D98: mSliderBar vtable
  std::uint32_t screen_optionmenu_vtable = 0;   // 0x0066F5E8: mOptionMenu vtable (the Fullscreen row model)
  std::uint32_t screen_menu_popup_offset = 0;   // optionmenu + this = the popup composite (mComposite)
  std::uint32_t screen_menu_vbox_offset = 0;    // optionmenu + this = the popup's VBox (the choices)
  std::uint32_t screen_menu_index_offset = 0;   // optionmenu + this = the selected index (MOptionMenu::get_index)
  std::uint32_t screen_menu_count_offset = 0;   // optionmenu + this = the choice count (the list's end)
  std::uint32_t screen_menu_open_offset = 0;    // optionmenu + this = 1 while the popup is open (func_002C9098)
  std::uint32_t screen_pane_vtable = 0;         // 0x00672950: the Screen page's `Pane` VBox vtable
  std::uint32_t screen_composite_vtable = 0;    // 0x00662EF8: mComposite vtable (the row composites)
  std::uint32_t screen_text_object_offset = 0;  // textface + this = the mLocalizedText pointer
  std::uint32_t screen_text_value_offset = 0;   // localized + this = the display String (dat pointer)
  std::uint32_t screen_insensitive_offset = 0;  // widget + this = the state word holding the insensitive bit
  std::uint32_t screen_insensitive_bit = 0;     // bit number of that word
  std::uint32_t screen_text_assign = 0;         // 0x005C2630: basic_string::replace (builds/replaces a String)
  std::uint32_t screen_heap_name = 0;           // 0x005C11A8: returns the string allocator's name holder
  std::uint32_t screen_alloc = 0;               // 0x00326750: the engine allocator (size, align, name)
  std::uint32_t screen_free = 0;                // 0x00326798: the engine free (ptr, size, align, name)
  std::uint32_t screen_text_invalidate = 0;     // 0x00245270: invalidate the TextFace's cached layout
  std::uint32_t screen_render_functor = 0;      // the main loop's render functor (the per-frame text service)
  std::uint32_t page_start_callback = 0;        // 0x0022BC20: MRenderContext.startPage script native (Phase 2a)
};

struct Profile {
  Id id = Id::v200;
  const char *name = "";
  std::uint32_t ram_size = 32u << 20;
  HookAddresses hooks = {};
};

// v2.00 (SCUS-97328, retail). Qualified release values; see the headers of each module for the guest chain.
inline constexpr HookAddresses kV200Hooks = {
    /* widescreen_owner_pointer */ 0x622F4Cu,
    /* widescreen_option_offset */ 0x38CB0u,
    /* widescreen_flag_offset */ 0x14u,
    /* widescreen_record_offset */ 0x24u,
    /* widescreen_record_bytes */ 44u,
    /* widescreen_record_width_offset */ 0xCu,
    /* widescreen_record_height_offset */ 0x10u,
    /* video_system_offset */ 0x1388u,
    /* video_system_ntsc */ 1u,
    /* widescreen_global */ 0x6184F0u,
    /* widescreen_preset_4_3 */ 0x622E90u,
    /* widescreen_preset_wide */ 0x622EC0u,
    /* widescreen_preset_4_3_ntsc */ 0x622EF0u,
    /* widescreen_preset_wide_ntsc */ 0x622F20u,
    /* race_display_index_offset */ 0x16B0u,
    /* race_display_index_normal */ 0u,
    /* race_display_index_480p */ 1u,
    /* race_display_index_1080i */ 2u,
    /* init_monitor */ 0x388A80u,
    /* camera_call_return */ 0x388B38u,
    /* race_display_render */ 0x39F498u,
    /* display_request_state */ 0x6184D0u,
    /* display_request_pending_offset */ 0x24u,
    /* display_request_mode_offset */ 0x2Cu,
    /* display_request_row_offset */ 0x30u,
    /* hud_parameter_block */ 0x00007F00u,
    /* hud_parameter_words */ 2u,
    /* hud_matrix_offset */ 0x70u,
    /* license_load_demo_inputs */ 0x3EAD18u,
    /* license_loader_entry */ 0x1100D0u,
    /* license_loader_return */ 0x1101E4u,
    /* license_replay_loader_entry */ 0x110230u,
    /* license_replay_loader_return */ 0x110344u,
    /* license_crc_table */ 0x655358u,
    /* license_manager_add */ 0x1AA4u,
    /* license_body_shift */ 0u,
    /* shape_call_pc */ 0x455A5Cu,
    /* shape_target_pc */ 0x49D308u,
    /* shape_return_pc */ 0x455A64u,
    /* frame_update_functor */ 0x00100EE0u,
    /* frame_render_functor */ 0x00100F50u,
    /* frame_idle_functor */ 0x00100FC0u,
    /* frame_display_page */ 0x00618584u,
    /* frame_display_page_second */ 0x00618600u,
    /* frame_race_vtable_first */ 0x00676000u,
    /* frame_race_vtable_last */ 0x00686000u,
    /* frame_race_dynamics_offset */ 0x70u,
    /* frame_dynamics_clock_offset */ 0xF898u,
    /* frame_dynamics_bytes */ 0x10000u,
    /* frame_race_flare_offset */ 0xDA0u,
    /* frame_flare_track_count_offset */ 0x1848u,
    /* frame_flare_car_count_offset */ 0xA34u,
    /* frame_render_world */ 0x0033DEA8u,
    /* camera_yaw_call_return */ 0x003456D8u,  // func_00345658: jal 0x0036CC40 at 0x003456D0, f13 loaded in its slot
    /* camera_pad_stick_x */ 0x0086F99Eu,      // pad buffer 0x0086F980 (func_00551810) + 0x1E
    /* camera_analog_loop_return */ 0x0043BE74u,    // func_0043BBC0: jal 0x004386F0 at 0x0043BE6C (a2 function, a3 value)
    /* camera_analog_channel_table */ 0x006A64C0u,  // the loop's fp: 02 03 00 01 14 15 16 17 10 11 12 13
    /* camera_analog_slot_bias */ 0u,               // s0 is the slot
    /* fov_projection_call_return */ 0x0037A1C4u,  // func_0037A170: jal 0x00378F18 at 0x0037A1BC (f12, f13, f14 = scales)
    /* fov_pad_buttons */ 0x0064C794u,             // port 0 pad object 0x0064C728 + 0x6C (buttons, then RX RY LX LY)
    /* menu_ortho_call_return */ 0x002151A8u,       // mUpdateContextPS2 render 0x00214FB0: jal 0x004A5A90 at 0x002151A0
    /* overlay_ortho_call_return */ 0x003EC200u,    // func_003EC0B0: jal 0x004A5A90 at 0x003EC1F8
    /* overlay_draw_end_return */ 0x003EC248u,      // func_003EC0B0: jal 0x004A5400 at 0x003EC240, after the draw 0x00475CA0
    /* overlay_caller_slot */ 0x60u,                // func_003EC0B0: sd ra,0x60(sp)
    /* monitor_overlay_call_return */ 0x003DA294u,  // RaceMonitor render func_003DA150: jal 0x003EC0B0 at 0x003DA28C
    /* option_wide_mode_callback */ 0x00180108u,   // MOption::get/set_wide_mode (adhoc_methods.txt, decomp unit_0017FCB0)
    /* script_box_int */ 0x002FE278u,               // box an int: hInt in a RefCounter, then func_00309348 writes the slot
    /* script_release_handle */ 0x003285F8u,        // RefCounter release (func_003285A8 is the retain)
    /* script_hint_vtable */ 0x00674188u,           // gt4/hInt.h: hInt vtable
    /* option_project_word_offset */ 0x3A3BCu,     // TASK-212: the menu state's next-project id ("arca" there)
    /* option_project_id */ 0x6974706Fu,            // "opti": the Options project (RAM state options-screen3)
    /* screen_adjust_v_callback */ 0x001802F0u,    // MOption::get/set_screen_adjust_v (adhoc_methods.txt, decomp)
    /* screen_adjust_h_callback */ 0x00180200u,    // MOption::get/set_screen_adjust_h
    /* screen_textface_vtable */ 0x00666370u,    // gt4/mTextFace.h
    /* screen_slider_vtable */ 0x00671D98u,        // gt4/mSliderBar.h
    /* screen_optionmenu_vtable */ 0x0066F5E8u,    // gt4/mOptionMenu.h (the Fullscreen row)
    /* screen_menu_popup_offset */ 0xC0u,           // mOptionMenu::onInitialize (popup, vbox, shadow)
    /* screen_menu_vbox_offset */ 0xC4u,
    /* screen_menu_index_offset */ 0xD8u,           // MOptionMenu::get_index reads the list at +0xD8
    /* screen_menu_count_offset */ 0xE0u,           // func_0057CE60: +0xE0 - +0xDC
    /* screen_menu_open_offset */ 0xFCu,            // func_002C9098 sets fc = 1; func_002C93C0 clears it
    /* screen_pane_vtable */ 0x00672950u,          // the Screen `Pane` VBox (state 0xF776C0, geometry 160,130,320,150)
    /* screen_composite_vtable */ 0x00662EF8u,     // the row composites (menu_quit)
    /* screen_text_object_offset */ 0x104u,        // mTextFace::unk104 -> mLocalizedText
    /* screen_text_value_offset */ 0xCu,           // mLocalizedText + 0xC = the display String (func_002B71D0)
    /* screen_insensitive_offset */ 0x98u,         // mWidget::unk98 bit 3 = insensitive (func_002661D8 sets the low nibble's 8)
    /* screen_insensitive_bit */ 3u,
    /* screen_text_assign */ 0x005C2630u,          // basic_string::replace(pos, n1, s, n2)
    /* screen_heap_name */ 0x005C11A8u,            // string allocator's name holder (first word = const char*)
    /* screen_alloc */ 0x00326750u,                // engine allocator
    /* screen_free */ 0x00326798u,                 // engine free
    /* screen_text_invalidate */ 0x00245270u,      // clears the TextFace's cached layout
    /* screen_render_functor */ 0x00100F50u,       // the main loop's render functor (frame_render_functor)
    /* page_start_callback */ 0x0022BC20u,        // MRenderContext.startPage (adhoc_methods.txt)
};

// Spec II (SCUS-97436, GT4 Online Public Beta lineage), filled by S8 (2026-10-23). The image is a different build of
// the same game, so the guest *structures* keep their shapes while every address moved; each value below was located
// in the Spec II ELF and then confirmed against a runtime RAM dump of the menus (see docs/SPEC2-PLAN.md, S8).
//
// Option chain (Confirmed by the translated initMonitor at 0x26BDB0 and by the RAM dump):
//   word at 0x664EDC          owner pointer (the game-data object)
//   owner + 0x680             MOption (the same class; the v2.00 owner had it at +0x38CB0)
//   option + 0x14             wide flag
//   option + 0x24             screen aspect record (11 words; width/height f32 at +0xC/+0x10)
//   presets at 0x664E20       4:3, wide, 4:3 NTSC, wide NTSC (0x30 bytes apart; the pairs are byte-identical)
//   word at 0x6655A0          published "display is wide" word (written by MOption::apply at 0x31257C)
//   option + 0x1718           race display index (0 normal, 1 480p, 2 1080i; 2 confirmed by initMonitor)
// The wide setter at 0x3124E4 reads the video-system word from *owner + 0x44*, not from the option, so
// video_system_offset stays 0 here: the hook then reads option word 0 (a pointer, never 1) and picks the non-NTSC
// preset pair, which the runtime dump shows byte-identical to the NTSC pair. Reviewed for this NTSC-U target.
//
// Code hooks: RaceBase::initMonitor 0x26BDB0 with the camera virtual call's return label 0x26BE60 (a1 = sp = matrix);
// RaceDisplay::render 0x1C5040 with the HUD matrix at this+0x70 (the same `addiu a0,sN,0x70` + matrix-load call shape
// as v2.00). The display request struct was not located yet, so the scene policy knows render activity but not photo
// mode.
//
// License demo guard (TASK-100 S8b, Confirmed in the derived ELF unless labelled otherwise):
//   * the two "license/%s" demo loaders start at 0x10CA18 and 0x10CBA8 (both call the path helper 0x1A8BE8 and make
//     one vtable call each: +0x510 -> 0x2012C8 and +0x1C8 -> 0x2012F0, return labels 0x10CB54 and 0x10CCE4);
//   * RaceSolitaire::loadDemoInputs is 0x2012C8 (the RaceSolitaire vtable base 0x6B3130 has the same virtual at
//     +0x510 that v2.00 has at +0x4D8 -> 0x3EAD18) and calls AutomobileControlRecord::Manager::load 0x207AA0;
//   * loadDemoInputs adds 0x1A24 to the pointer it receives (v2.00 adds 0x1AA4). The decrypted demo files on the
//     retail, beta and Spec II discs all keep the single-Recorder stream at body + 0x1AA4, so the loader must hand
//     the virtual a pointer 0x80 past the header's body start (High confidence: 0x1A24 + 0x80 = 0x1AA4; the guard
//     writes at a1 + 0x1A24, exactly where the virtual reads, and evaluates the body from the header's own offset);
//   * the cipher is the same (0x459E90, FT at 0x667048 matching the derived formula, T8 bytes at 0x731C98, standard
//     CRC-32 table at 0x731DB0): CRC-confirmed plaintexts of the Spec II demos pass the v2.00 rule.
inline constexpr HookAddresses kSpec2Hooks = {
    /* widescreen_owner_pointer */ 0x664EDCu,
    /* widescreen_option_offset */ 0x680u,
    /* widescreen_flag_offset */ 0x14u,
    /* widescreen_record_offset */ 0x24u,
    /* widescreen_record_bytes */ 44u,
    /* widescreen_record_width_offset */ 0xCu,
    /* widescreen_record_height_offset */ 0x10u,
    /* video_system_offset */ 0u,
    /* video_system_ntsc */ 1u,
    /* widescreen_global */ 0x6655A0u,
    /* widescreen_preset_4_3 */ 0x664E20u,
    /* widescreen_preset_wide */ 0x664E50u,
    /* widescreen_preset_4_3_ntsc */ 0x664E80u,
    /* widescreen_preset_wide_ntsc */ 0x664EB0u,
    /* race_display_index_offset */ 0x1718u,
    /* race_display_index_normal */ 0u,
    /* race_display_index_480p */ 1u,
    /* race_display_index_1080i */ 2u,
    /* init_monitor */ 0x26BDB0u,
    /* camera_call_return */ 0x26BE60u,
    /* race_display_render */ 0x1C5040u,
    /* display_request_state */ 0u,
    /* display_request_pending_offset */ 0u,
    /* display_request_mode_offset */ 0u,
    /* display_request_row_offset */ 0u,
    /* hud_parameter_block */ 0x00007F00u,
    /* hud_parameter_words */ 2u,
    /* hud_matrix_offset */ 0x70u,
    /* license_load_demo_inputs */ 0x2012C8u,
    /* license_loader_entry */ 0x10CA18u,
    /* license_loader_return */ 0x10CB54u,
    /* license_replay_loader_entry */ 0x10CBA8u,
    /* license_replay_loader_return */ 0x10CCE4u,
    /* license_crc_table */ 0x731DB0u,
    /* license_manager_add */ 0x1A24u,
    /* license_body_shift */ 0x80u,
    /* shape_call_pc */ 0u,
    /* shape_target_pc */ 0u,
    /* shape_return_pc */ 0u,
    /* frame_update_functor */ 0x001007D8u,
    /* frame_render_functor */ 0x00100848u,
    /* frame_idle_functor */ 0x001008B8u,
    /* frame_display_page */ 0x00665634u,
    /* frame_display_page_second */ 0x006656B0u,
    /* frame_race_vtable_first */ 0x006AE000u,
    /* frame_race_vtable_last */ 0x006BF000u,
    /* frame_race_dynamics_offset */ 0x70u,
    /* frame_dynamics_clock_offset */ 0x23960u,
    /* frame_dynamics_bytes */ 0x24000u,
    /* frame_race_flare_offset */ 0xE60u,
    /* frame_flare_track_count_offset */ 0x1848u,
    /* frame_flare_car_count_offset */ 0xA34u,
    /* frame_render_world */ 0x001EA240u,
    /* camera_yaw_call_return */ 0x00206490u,  // spring at 0x002063D8: jal 0x00230F00 at 0x00206488
    /* camera_pad_stick_x */ 0x0089B0DEu,      // pad buffer 0x0089B0C0 (set up at 0x0051BF18) + 0x1E
    /* camera_analog_loop_return */ 0x003190F4u,    // the same loop: jal 0x00314870 at 0x003190EC (a2 function, a3 value)
    /* camera_analog_channel_table */ 0x006F9D10u,  // the loop's s7, the same 12 ids as v2.00
    /* camera_analog_slot_bias */ 1u,               // s0 was already incremented in the loop (0x003190B8)
    /* fov_projection_call_return */ 0x0023E834u,  // projection 0x0023E7E0: jal 0x0023D558 at 0x0023E82C
    /* fov_pad_buttons */ 0x00681344u,             // the same pad object layout (RAM peek: buttons, RX RY LX LY)
    /* menu_ortho_call_return */ 0x0036D4D8u,       // 0x0036D278 (menu context render): jal 0x005A2600 at 0x0036D4D0
    /* overlay_ortho_call_return */ 0x001FDE18u,    // 0x001FDCC8 (strobe overlay renderer): jal 0x005A2600 at 0x001FDE10
    /* overlay_draw_end_return */ 0x001FDE60u,      // 0x001FDCC8: jal 0x005A1E08 at 0x001FDE58, after the draw 0x00341918
    /* overlay_caller_slot */ 0x60u,                // 0x001FDCC8: sd ra,0x60(sp)
    /* monitor_overlay_call_return */ 0x001F2FD4u,  // race monitor render 0x001F2E98: jal 0x001FDCC8 at 0x001F2FCC
};

inline constexpr Profile kV200{Id::v200, "v2.00", 32u << 20, kV200Hooks};
inline constexpr Profile kSpec2{Id::spec2, "Spec II", 32u << 20, kSpec2Hooks};

#if defined(GT4_TARGET_SPEC2) && !defined(GT4_TARGET_FIXTURE_V200)
inline constexpr Id kActiveId = Id::spec2;
#else
inline constexpr Id kActiveId = Id::v200;
#endif

constexpr const Profile &active() { return kActiveId == Id::spec2 ? kSpec2 : kV200; }

// Guest address for a hook, or kNeverMatch when the active target does not have it. constexpr so the overlays keep
// their compile-time comparisons.
constexpr std::uint32_t match_or_never(std::uint32_t address) { return address == 0 ? kNeverMatch : address; }

constexpr bool available(std::uint32_t address) { return address != 0; }

// Logs one line the first time a hook is told it does not exist for the active target. Returns true the first time.
// Header-only with function-local statics: one instance per translation unit is harmless (the line repeats at most once
// per unit that actually calls the hook).
inline bool note_hook_unavailable(const char *hook) {
  static const char *logged[8] = {};
  static std::size_t count = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (logged[i] == hook)
      return false;
  }
  if (count < sizeof logged / sizeof logged[0])
    logged[count++] = hook;
  std::fprintf(stderr, "[target] %s: host hook '%s' has no guest address in this target; disabled\n", active().name,
               hook);
  return true;
}

} // namespace gt4::target
