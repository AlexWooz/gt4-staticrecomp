# Mapping the launcher settings into GT4's own Options menu (TASK-213)

Read-only study, 2026-11-09. Branch `task/213-settings-map` (from `task/212b-spec2-quit` at `70af98d7`).
No code, data, build, pin or test was changed for this document.

Labels: **C** = Confirmed (decoded data, code path or a run), **H** = High confidence, **Hy** = Hypothesis,
**U** = Unknown.

**Short answer.** The 3-choice row is `HiVision` (Normal / 480p / 1080i) on Options → General → Screen; the
aspect row is `Wide` (4:3 / 16:9). Both are no-ops today because the recomp holds 480p and 16:9 at every VBlank.
`HiVision` can be removed now with a **4-byte same-length data patch** of the `"NTSC"` literal in `option.adc`
(the TASK-212 patch pipeline), no script edit. Only **Fullscreen** can change live today; every other launcher
setting is read at start or written into the session `gt4-gs.ini`, so it needs the next start. The fastest route
to many settings is a **hybrid**: remove `HiVision`, put the settings in a host panel over the game, write them
through a `game-changes.toml` the launcher merges, and convert the top rows to native GT4 rows later with a
callback override. Section 6 is the first safe set.

Sources read:

- `docs/tasks/GT4-MENU-QUIT-SETTINGS-STUDY.md` (TASK-212/213 study), `docs/tasks/QUIT-GAME-ROW-RETAIL-SPEC2.md`
  (the data patch + host append + guide), `docs/INGAME-SETTINGS-STUDY.md` (TASK-078).
- The decoded retail v2.00 option data: `private/slice155-spec2-480p/retail_adc/projects_option_option.adc.bin`
  and `projects_option_option.mproject.bin`, plus `option_strings.txt`.
- The Spec II v1.10 (PT-BR) option data, decoded read-only from the user's own ISO with
  `scripts/gt4_volume.py` for this study (`projects/option/option.adc` 288,080 bytes, `option.mproject` 804,881
  bytes raw). Nothing was written to the ISO, the volume or the repository.
- The v2.00 decomp `private/slice151-research/gt4-decomp` (`MOption__*`, `adhoc_methods.txt`), and
  `private/research/ingame-settings-menu-study.md` (the earlier copy of the TASK-078 study).
- The saved Options-page guest states in `private/slice113-video/options-screen3/state.bin` and neighbours.
- The launcher: `launcher/crates/gt4l-core/src/settings.rs`, `launch.rs`, `gs_table.rs`, `config.rs`,
  `display.rs`, `launcher/crates/gt4l-tauri/src/model.rs`, `commands.rs`, `docs/launcher/gs-settings.toml`.
- The game and renderer read paths: `native/window.cpp`, `game_main.cpp`, `frame_skip.cpp`, `race_display.cpp`,
  `race_widescreen.cpp`, `game_target.hpp`, `hw_gs/*`, `native_vulkan.cpp`, `gs_stream/gs_stream_client.cpp`,
  `host_io.cpp`, `trigger_range.hpp`, `display_aspect.cpp`, `data_patch.*`, `menu_quit.*`.
- `docs/GS-LIVE.md`, `docs/GS-STREAM-PROTOCOL.md`, `docs/GS-SETTINGS.md`.

---

## 1. The game's Options pages today

### 1.1 The page table

The Options project is `projects/option/option.{adc,mproject}`. The v2.00 script is Adhoc **v5** (`ADCH005`,
251,715 decoded bytes); Spec II is **v7** (`ADCH007`, 288,080 decoded bytes) and its string table still names
`projects/gt4o_us/option/option.ad`. **C** for the bytes; **H** that the publisher built the page from the GT4
Online option project.

`OptionRoot` builds `OptionPage(name, root, ...)` entries in five groups in v2.00 (Spec II adds a sixth,
`GroupSpecII`). Every name/root pair is a
length-prefixed string in `option.adc`; the v2.00 table sits at `0x8A7C`-`0x99DD`, the Spec II table runs from
`0xCB6D` onward (a second `GroupGeneral` table starts at `0xDBD4`). **C** (decoded data). The page-table
offsets below are the `u16` length word of each entry (the text begins two bytes later); the script/string
offsets elsewhere in this document are content offsets.

| Group | Page | RootWindow | v2.00 `option.adc` | Spec II `option.adc` |
|---|---|---|---|---|
| `GroupGeneral` | Race | `RaceRoot` | `0x8B03` / `0x8B12` | `0xCBE0` / `0xCBF9` |
| | Replay | `ReplayRoot` | `0x8B71` / `0x8B82` | `0xCC4E` / `0xCC5F` |
| | **Screen** | **`ScreenRoot`** | **`0x8BE5` / `0x8BF6`** | **`0xCCB8` / `0xCCC9`** |
| | Color | `ColorRoot` | `0x8C59` / `0x8C69` | `0xCD22` / `0xCD32` |
| | Sound | `SoundRoot` | `0x8CCA` / `0x8CDA` | `0xCD89` / `0xCD99` |
| `GroupArcade` | Arcade | `ArcadeRoot` | `0x8D96` | `0xCE46` |
| | SplitBattle | `SplitBattleRoot` | `0x8E0A` | `0xCEB0` |
| | LAN | `LANPopup` | `0x8E8D` | `0xCF29` |
| | Network | `NetRoot` | `0x8EFA` | (not listed in the first table) |
| | Course | `CourseRoot` | `0x8F69` | `0xCF8C` |
| `GroupPhotoMode` | Album | `AlbumRoot` | `0x903B` | `0xD098` |
| | Dummy x4 | (none in the mproject) | `0x90AC`, `0x90FA`, `0x9148`, `0x9196` | `0xD0FF`… |
| `GroupController` | Controller | `ControllerRoot` | `0x9243` | `0xD27D` / `0xD292` |
| | DS2 port 0/1 | `KeyconfigRoot` | `0x92D7`, `0x938C` | `0xD307`, `0xD3AD` |
| | Steering | `SteeringRoot` | `0x9441` | `0xD453` / `0xD466` |
| | CTR port 0/1 | `KeyconfigRoot` | `0x94BB`, `0x955D` | `0xD4C3`, `0xD556` |
| | CGR port 0/1 | `KeyconfigRoot` | `0x95FF`, `0x96A1` | `0xD5E9`, `0xD67C` |
| | JGR port 0/1 | `KeyconfigRoot` | `0x9743`, `0x97E5` | `0xD70F`, `0xD7A2` |
| `GroupMisc` | Misc | `MiscRoot` | `0x98E0` | `0xD889` / `0xD898` |
| | Unit | `UnitRoot` | `0x994E` | `0xD8ED` / `0xD8FC` |
| | Language | `LanguageRoot` | `0x99BC` | `0xD951` / `0xD964` |
| | Secret (Spec II only in the list) | `SecretRoot` | (tree at line 16479, not listed) | `0xD9C1` / `0xD9D2` |
| | Monitor (Spec II only in the list) | `MonitorRoot` | (tree at line 17308, not listed) | `0xDA2B` / `0xDA3D` |
| `GroupSpecII` (Spec II only) | SpecIISetting | `SpecIISettingRoot` | — | `0xDAEE` / `0xDB06` |

v2.00 has no `Monitor` or `Secret` page in its list (the `MonitorRoot` tree at line 17308 and the `SecretRoot`
tree at line 16479 exist but are not pages; they carry multi-screen rotation and the secret/car-limit options).
Spec II lists `Secret` and `Monitor`, and adds a publisher group `GroupSpecII` with the `SpecIISetting` page.
That page (`SpecIISettingRoot`, Spec II line 8999) is a native settings page made of `OptionMenu` toggles
(`Randomizer`, `TrackRandomizer`, …) and is the publisher's own example of adding a settings page in data. **C**
for the table; **H** that `MonitorRoot` is a LAN/multi-screen helper.

`SpecIISettingRoot` is worth reading before a native host page: it is a v7 page built from the same
`OptionMenu`/`TextFace` primitives, but it is Spec II-only and its modules are the publisher's scripts; it is a
model, not reusable for v2.00 (v5). **C** data, **H** relevance.

The game's own controls and audio are already here: `ControllerRoot`, the eight `KeyconfigRoot` port pages and
`SteeringRoot` for button/steering mapping; `SoundRoot` for BGM/SE volumes and channels; `RaceRoot` for gameplay
options; `MiscRoot`/`UnitRoot`/`LanguageRoot` for saves, units and language. None of them covers a host setting.
**C** (page list and roots; the script behaviour is H from the retail strings and the earlier study).

### 1.2 The Screen page tree

`ScreenRoot` (v2.00 line 5812, Spec II line 6616) has `children [6]`: `bg0`, three `ColorFace`s, the `VBox` named
`Pane`, and the `ToolTip` composite. `Pane` has seven children (v2.00 `children [7]` at line 5859, Spec II at
line 6660):

```
ScreenRoot
├ bg0 + ColorFace2/ColorFace/ColorFace3
├ VBox "Pane" (geometry 160 130 320 150)
│ ├ FrameImageFace "bg"
│ ├ Composite "Title"
│ ├ Composite "Wide"        (line 5914; 2 choices: Option4x3 6008, Option16x9 6041)
│ ├ Composite "Vertical"    (line 6081; SliderBar 6101, range -50..50)
│ ├ Composite "Horizontal"  (line 6210; SliderBar 6230, range -50..50)
│ ├ Composite "HiVision"    (line 6339; visible 0; 3 choices: Normal 6434,
│ │                          Mode480p 6467, Mode1080i 6500)
│ └ Composite "Buttons"     (line 6540; Cancel and OK)
└ Composite "ToolTip" (line 6649)
```

Spec II has the same tree at lines 6660/6713/6878/7005/7132/7331, with v7 format differences: `packable digit {1}`
instead of v2.00's `packing digit {1}` on the row composites, and the `Pane` `VBox` has no `packing` line.
**C** (decoded data)

Each row is a `Composite` with a background `ColorFace`, a label `TextFace` (a `key`, e.g. `LabelWide`,
`LabelHiVision`), and one focusable widget: an `OptionMenu` (Wide, HiVision) or a `SliderBar` (Vertical,
Horizontal). The `OptionMenu` carries `has_script digit {1}`, a `tooltip_key`, and a three-level popup:
`popup` → `vbox` → one `Composite` per choice with a dot and a `TextFace` (`key`). **C** (decoded data)

The row label keys resolve in `projects/option/US/rtext.rt2`; the exact labels are the game's own text. **H**

### 1.3 The three-choice resolution row and the aspect row

- **The three-choice row is `HiVision`**: `Normal`, `Mode480p`, `Mode1080i`, one `OptionMenu` with three popup
  items. It is the video-output row the owner described. **C** (decoded data)
- **The aspect row is `Wide`**: `Option4x3` and `Option16x9`, two choices, one `OptionMenu`. **C** (decoded
  data)

Script behaviour, from the retail `option.adc` string table and the earlier TASK-078 study (the v5 script is not
decompiled, so the control flow is H, not C):

- `ScreenRoot.onInitialize` compares `MSystem::GetVideoSystem()` with the literal `"NTSC"` and makes
  `Pane.HiVision` visible only on NTSC. The literal occurs exactly **once** in the decoded v2.00 file, at
  `0x247FB`, preceded by the length word `04 00` and followed (after the operand word) by the `==` opcode; the widget path
  `Pane`/`HiVision`, the property `visible` and the `OptionProject::ScreenRoot::Pane::HiVision::OptionMenu`
  module path are the strings immediately after the compare (`0x24822` onward). **C** for the bytes; **H** for
  the script flow.
- The same row is present and hidden by default in Spec II (`visible digit {0}`), with the same `"NTSC"` compare
  at decoded `0x2A2E5`. **C** (decoded data)
- `reflect` selects the `HiVision` index from `option.race_display_mode`; `apply` writes it back and calls
  `option.apply()` and `main::game.setNeedSave()`; picking 480p or 1080i opens the "Try…" confirmation
  (`TryHiVision`), and 1080i also sets `Wide` to 16:9. **H** (retail strings + TASK-078's OpenAdhoc-derived
  reading; the retail v5 script itself is not decompiled).
- `Wide` writes `option.wide_mode`; the bytecode strings `wide_mode` (`0x24A81`) and
  `OptionProject::ScreenRoot::Pane::Wide::OptionMenu` (`0x24F2C`) are present. **H**

The natives behind both rows (v2.00 decomp, `adhoc_methods.txt`): **C**

| Script attribute | Callback | Decomp | Meaning |
|---|---|---|---|
| `MOption.race_display_mode` get/set | `0x001805D0` | `MOption__get_race_display_mode` | one callback, `argc > 0` = set; string ↔ 0/1/2 |
| `MOption.wide_mode` get/set | `0x00180108` | `MOption__get_wide_mode` | boolean wide flag |
| `MOption.screen_adjust_v` get/set | `0x001802F0` | `MOption__get_screen_adjust_v` | vertical offset |
| `MOption.screen_adjust_h` get/set | `0x00180200` | `MOption__get_screen_adjust_h` | horizontal offset |
| `MOption.applyRaceDisplayMode` | `0x001899F0` | → `func_00436640` | requests the race CRT mode |
| `MOption.applyMenuDisplayMode` | `0x00189A28` | → `func_00436740` | always the 480i menu mode |
| `MSystem.GetVideoSystem` | `0x001AC598` | | returns `"NTSC"` on this disc [H] |

Script natives are reached through `jalr` from `hBuiltinMethod` virtual 14, so a host override in
`ps2::dispatch` can intercept them; the directly called `func_00436640`/`func_00436740` cannot be intercepted
that way. **C** (TASK-078 section 1.5).

### 1.4 What the recomp forces today

| Fixed hold | Where | Effect |
|---|---|---|
| `GT4_FORCE_480P=1` | `launch.rs::add_common` (always), `native/race_display.cpp` | at every guest VBlank, the option word at `owner + 0x38CB0 + 0x16B0` (v2.00; `owner + 0x680 + 0x1718` in Spec II) is written `1` when it is `0` or `2`; the race CRT mode is 480p |
| `GT4_FORCE_WIDESCREEN=1` | `launch.rs::add_common` (always), `native/race_widescreen.cpp` | at every guest VBlank, the wide flag `option + 0x14` is held `1`, the 44-byte record `option + 0x24` is copied from the wide preset, and the published word `0x6184F0` follows |
| `GT4_DISPLAY_ASPECT=auto` | `launch.rs::add_common`, `native/display_aspect.cpp` | the projection/HUD follow the launcher's Resolution shape |
| `input_resolution`, `widescreen`, `aspect_ratio`, `aspect_custom` | `settings::RETIRED_SETTINGS` | never read; no launcher row exists |
| menus | `func_00436740` | menus are always 480i, whatever the option says |

Consequences for the Screen page:

- The `HiVision` choice cannot change the race mode: a `Normal` choice is undone at the next VBlank, and
  `1080i` is also undone (the host writes 1 for index 2). The row is therefore a no-op with a misleading
  label. **C** (code).
- The `Wide` choice cannot change the aspect: the hold writes the wide flag and record back at the next VBlank.
  The row is a no-op too. **C** (code).
- **Removing `HiVision`** removes the only in-game 480i/480p/1080i control. The race stays 480p (the hold), old
  saves that stored `2` are normalized to `1` at the next VBlank, menus stay 480i, and the launcher's Display
  settings remain the only output/resolution control. **C/H**
- **Keeping `Wide`** keeps a second no-op row; **replacing `Wide`** with a two-choice host row (for example
  Fullscreen) is possible with a callback override (section 5.1) and leaves the game's own wide flag at 16:9.
  **H**

### 1.5 Where the values live (the save)

- `0x00622F4C` holds the game-data object pointer (the save-data object). The option object is
  `owner + 0x38CB0` in v2.00 and `owner + 0x680` in Spec II (Spec II's owner word is `0x664EDC`). **C**
  (`native/game_target.hpp`, decomp).
- `option + 0x16B0` (v2.00) / `option + 0x1718` (Spec II): the race display index, `0` normal, `1` 480p,
  `2` 1080i. **C**
- `option + 0x14`: the wide flag (`0` 4:3, `1` 16:9). `option + 0x24`: the 44-byte screen-aspect record, with
  the width/height `f32` at `+0xC`/`+0x10`. **C**
- The option object is inside the GT4 save; a value written there is persisted when the game saves. A host
  setting must never be written there (a real PS2 cannot use it). **C** design rule; the save path itself is H.

---

## 2. Removing the Screen row

### 2.1 The recommended edit: the `"NTSC"` byte patch (data only)

The same-length data patch of TASK-212 already serves any decoded byte range; no GT4.VOL TOC write and no
v5 script edit are needed. Replace the four bytes of the `"NTSC"` literal in `option.adc` with a non-`NTSC`
four-byte value (for example `PAL `). The compare then fails, the script never makes `HiVision` visible, and the
page uses the layout it already uses on a non-NTSC console. **H** (bytes C; the runtime result is the acceptance
gate).

| Target | File | Kind | Stored / decoded | `"NTSC"` offset | Expect | Replace |
|---|---|---|---|---|---|---|
| v2.00 SCUS-97328 | `projects/option/option.adc` | 2 (PS2ZIP) | 55,353 / 251,715 | `0x247FB` (149,499) | `4E 54 53 43` | `50 41 4C 20` (`PAL `) |
| Spec II SCUS-97436 | `projects/option/option.adc` | 2 (PS2ZIP) | 58,422 / 288,080 | `0x2A2E5` (172,773) | `4E 54 53 43` | `50 41 4C 20` |

- The literal is unique in each file (one hit in each decoded file). Any four bytes other than `NTSC` work;
  `PAL ` is chosen because it is also false for a real PAL console and reads as a region, not a hack. **C**
  bytes, **H** effect.
- The edit keeps the decoded file at exactly 251,715 / 288,080 bytes, so the PS2ZIP re-encode only has to fit
  the original **stored** extent (55,353 / 58,422). The builder checks that and refuses growth. **C** (builder
  code; the 4-byte change cannot plausibly grow the stream past the extent, but the build's report is the proof).
- Spec II's `option.mproject` is a raw (kind 1) 804,881-byte record, so any later **text** edit of it must pad
  to exactly that decoded size (the builder does, with newlines); the byte edit of `option.adc` does not touch
  it. **C** (record table).
- The recipe is private and derived from the user's disc. The patch container is built during the launcher's
  "Preparing game files" step (the TASK-212 plan, not implemented yet) or by hand for a test kit:

```
python scripts/gt4_data_patch.py --iso "<the user's disc>" --recipe <private recipe>.json --output <fresh private dir>
```

  The recipe shape is `{"files": [{"catalogue_path": "projects/option/option.adc",
  "byte_replacements": [{"offset": <offset>, "length": 4, "expect_hex": "<original>", "hex": "<new>"}]}]}`.
  The numbers stay in the private recipe; the table above is for verification. **C** (script `build_replacement`,
  `byte_replacements` at `gt4_data_patch.py:101-114`).
- The patch can carry several files in one container: the `option.adc` byte edit and the TASK-212
  `arcade.mproject` row (and, later, a `text_replacements` edit of `option.mproject`) are one `recipe["files"]`
  list per target. **C** (builder loops `recipe["files"]`).
- The runtime is the existing capsule: `GT4_DATA_PATCH` names the container, `native/data_patch.cpp` verifies each
  original range's SHA-256 against the image and rewrites the overlapping bytes in `disc::read`
  (`data_patch.cpp:156`). **C**

**Why not the `.mproject` structural edit.** Deleting the `HiVision` child (`Pane` `children [7]` → `[6]`, the
block at v2.00 lines 6339-6539 / Spec II 7132-7330) would remove the widget, but the script still looks it up by
path: the v2.00 `option.adc` has 21 `HiVision` string hits, including `Pane`/`HiVision`,
`OptionProject::ScreenRoot::Pane::HiVision::OptionMenu`, `visible`, `ModeString` and `TryHiVision`. A missing
widget is a script error, and GT4 has no recovery from a script exception. **H** (strings C; the failure is H, not
run). The structural edit only becomes safe after a v5 script round trip that also removes the script
references.

**Why not patch `MSystem.GetVideoSystem`.** Overriding `0x001AC598` to report a non-NTSC system would hide the
row, but the study already warns that other code may use the video system for timing or region decisions
(**U**), and a global override is a much wider change than one literal. Not recommended.

### 2.2 The host-hide alternative (no data patch)

If the launcher cannot build a patch (the TASK-212 "Preparing game files" integration is still pending), the row
can be hidden at runtime:

1. Find the `ScreenRoot` page tree in guest RAM after the page script's `onInitialize`.
2. Find the `HiVision` `Composite` (row `MComposite` vtable `0x00662EF8` in v2.00, geometry `{0,116,320,32}`,
   name `HiVision`) and clear its `visible` word while the Screen page is current (or remove it from the `Pane`
   child list, as `menu_quit` removes the hidden template).
3. Re-hide if the page reloads; the script's `visible = 1` runs per page initialize, so a VBlank watcher must run
   after it.

Evidence that the tree is readable: the saved Options → Screen state
`private/slice113-video/options-screen3/state.bin` is a `PS2STATE` v2 file with guest RAM at file offset 1460; in
that state the float geometry `{0,116,320,32}` occurs nine times, row composites carry vtable `0x00662EF8`, and
the strings `HiVision`, `LabelHiVision`, `Mode480p` and `Mode1080i` are all present in RAM. **C** (state scan,
this study). The exact `visible` field offset is **U**; deriving it needs a state where the row is visible and a
state where it is not, or a headless toggle. The effort is a new watcher module (like `menu_quit`) plus a page
gate, about one slice; the risk is a one-frame flash and a Spec II gate that does not exist yet. **H/U**

The host-hide writes no game data and works with a disc-independent build; it is the fallback, not the preferred
path once the patch builder ships.

### 2.3 The save and the existing values

- The removal writes **no** option field. The existing hold continues to write `1` to the race index (`0`/`2`
  only) and to the wide flag/record at VBlank; that is the current "fixed at 480p / 16:9" behaviour, not a host
  value. **C**
- An old save with index `2` becomes `1` in memory at the first VBlank after the option object is settled, and
  the game may persist `1` on its next save. `1` is a value a real PS2 accepts. **C/H**
- A host row (section 5) must not write `option + 0x16B0` or `option + 0x14`; it writes only the host feedback
  file. **C** design rule.
- The game may mark the save dirty when the player changes an option (`setNeedSave()` in the script). A native
  host row that swallows the guest setter still lets the script call `setNeedSave()`; the option values stay
  `1`/`1`, so the save is re-written with valid values, not contaminated. **H**

### 2.4 Acceptance for the removal

- Build the patch from the v2.00 ISO; the report must say kind 2, 55,353 stored / 251,715 decoded, one byte
  replacement at `0x247FB`, and the runtime must print the verified range line. **C** target.
- Headless from `options-screen3/state.bin` (or a fresh boot to Options): a frame of Options → General → Screen
  shows `Wide`, `Vertical`, `Horizontal` and the buttons, with no `HiVision` row and no gap; all other General
  pages still open; the log has no script error. **C** target, not run for this document.
- A race still runs 480p; menus still draw; the launcher report is unchanged. **C** target.
- Save check: copy the card; after the visit, `option + 0x16B0` is `1` and `option + 0x14` is `1`; the card
  still loads on a real PS2 (owner). **H** target.
- Spec II: the same byte edit at `0x2A2E5`; a Spec II Options-page state does not exist yet (the TASK-212b kit
  has a top-menu state), so the headless page test is a next check. **U** for the Spec II state, **C** for the
  bytes.

---

## 3. The settings table

The table below maps every candidate launcher setting to its consumer and the point where the value is read.
"Live" means the running game changes without a restart; "restart" means the next start of the game (the
launcher's Play, or a relaunch after a normal close). Every row also says where it would live in the game menu
and how the value would be written through. **No audio setting exists in the launcher today**; GT4's own
`SoundRoot` page is the only audio control, and no row is invented here. **C**

Evidence codes used in the table (the full code path is in section 4):

- **E1** `launch.rs::add_common` writes `GT4_FULLSCREEN`; `window.cpp:781-897` reads it at window start;
  `window.cpp:369` is `set_fullscreen`, and `window.cpp:448-453` toggles it live on Alt+Enter/F11.
- **E2** `launch.rs::pcsx2_environment`/`coregs20_environment` choose the renderer; the game reads
  `GT4_RENDERER` in `hw_gs_dispatch.cpp:10` (CoreGS) or `gs_stream/gs_stream_client.cpp` (GSdx) at start.
- **E3** `GT4_HW_SCALE` is parsed in `hw_gs_dispatch.cpp`/`hw_gs_options.cpp:226-230` when the CoreGS renderer is
  constructed; GSdx's `upscale_multiplier` is in the session `gt4-gs.ini`, read by gt4-gs at process start.
- **E4** `GT4_OUTPUT_SIZE` and `GT4_MONITOR` are read once at start (`display_aspect.cpp:490`,
  `window.cpp:815`).
- **E5** `GT4_PRESENT_MODE` is read at renderer/window construction (`native_vulkan.cpp:248`,
  `hw_gs_backend.cpp:1770`); GSdx's `VsyncEnable`/`DisableMailboxPresentation` are in the session INI.
- **E6** `frame_skip::start_from_environment` (`frame_skip.cpp:177`, called from `game_main.cpp:309`) reads
  `GT4_FRAME_SKIP_FORCE`/`GT4_FRAME_SKIP` at start.
- **E7** `GT4_CORE2_PERF_FLARES`, `GT4_CORE2_PERF_ROAD_SHEEN` and `GT4_CORE2_EFFECTS_1X` are `const` members of
  the CoreGS renderer (`core2_renderer.cpp:571-592`, `core2_gpu.cpp:2787-2790`), set at construction.
- **E8** `GT4_GS_HIDE_FLARES`/`GT4_GS_READBACK_LOCAL` are latched when the stream client configures
  (`gs_stream_client.cpp:713-714`) and in a `static` in `hw_gs_stream_sink.cpp:939`.
- **E9** GS table rows are written into the session `gt4-gs.ini` (`launch.rs::gs_config_text`) and read by
  gt4-gs at process start; `CONTROL kind 4 reload_config` is answered "unsupported" (`GS-LIVE` section 2).
- **E10** `GT4_HW_HUD` is a `const` at backend construction (`hw_gs_backend.cpp:348-350`).
- **E11** `GT4_HW_ADAPTER` is read at device selection (`hw_gs_backend.cpp:752`).
- **E12** `GT4_CORE2_FSR_SHARPNESS` is a `const` at renderer construction (`core2_renderer.cpp:471-489`).
- **E13** `GT4_HW_PRESENT_SHARED_QUEUE` is read in the backend's init (`hw_gs_backend.cpp:788`).
- **E14** `GT4_PAD` is read in the `ControllerInput` constructor (`host_io.cpp:87`).
- **E15** `GT4_TRIGGER_FULL_RANGE` is "read once (start)" (`trigger_range.hpp:30`), configured in
  `game_main.cpp:285/312`.
- **E16** `GT4_PERF` is read when the perf probe starts (`perf_probe.cpp:121`).
- **E17** `GT4_VU1_VERIFY`/`GT4_VU0_VERIFY` are read at start (`launch.rs::add_common`).
- **E18** `GT4_HW_POSTFX`/`GT4_HW_TEXTURES` are CoreGS options parsed at construction; GSdx's `fxaa` /
  `LoadTextureReplacements` are INI rows.
- **E19** launcher-only: no game consumer.

### 3.1 Display and window

| Setting (id) | Consumer / when read | Live or restart (evidence, confidence) | Game menu home / row type | Write-through | Risk / test |
|---|---|---|---|---|---|
| **Fullscreen** `fullscreen` | game window; `GT4_FULLSCREEN` at start, `set_fullscreen` any time | **Live** (E1, C). Alt+Enter/F11 already toggles it; the GS child follows via `CONTROL resize` | No game page today; panel row "Fullscreen", toggle On/Off; optionally repurpose the `Wide` 2-choice row later | game writes `fullscreen = on/off` to the feedback file | low: attract timer and pad mask; test both renderers, windowed and fullscreen |
| **Renderer** `renderer` | launcher chooses the game start path; `GT4_RENDERER=hw` or `pcsx2` + `gt4-gs.exe` | **Restart** (E2, C). CoreGS lives in the game executable; GSdx is a separate process; the choice is a start path, not a runtime switch | Graphics page of the panel, list "GSdx / CoreGS 2.0" | `renderer = "pcsx2"` or `"coregs20"`; the merge should refuse CoreGS on a card without Vulkan (the check exists for the post-GSdx-failure offer) | medium: a PC without Vulkan must keep GSdx; test switching both ways, restart notice |
| **Render scale** `render_scale` | CoreGS `GT4_HW_SCALE`; GSdx `upscale_multiplier` | **Restart** (E3, C). CoreGS parses it when the renderer is constructed; gt4-gs reads the INI at process start; `--prepare-shaders` bakes the scale into the variant cache | Graphics page, steps 50–200% in 25% steps (the launcher's range) | `render_scale = "50".."200"`; the launcher resolves it against the output and the renderer caps | medium: shader cache/prepare scale, GSdx fractional steps only with gt4-gs 1.3.0; test 100/150/200 on both |
| **Resolution** `resolution` | game window size + the display plan; `GT4_OUTPUT_SIZE` | **Restart** (E4, C) | Display page, list of the monitor's modes | `resolution = "desktop"` or `"WxH"`; the launcher falls back to the desktop mode if unsupported | medium: ultrawide/16:10 shapes and the projection correction; test 16:9/21:9 |
| **Monitor** `monitor` | window placement; `GT4_MONITOR` | **Restart** (E4, C) | Display page, list of connected monitors | `monitor = "<device>@WxH"`; a monitor that is gone falls back to primary | low/medium: device names, two-monitor test |
| **VSync** `vsync` | `GT4_PRESENT_MODE`; GSdx INI `VsyncEnable`/`DisableMailboxPresentation` | **Restart** (E5, C); CoreGS draws "Fast" as On | Display page, list On/Fast/Off (Fast is not offered under CoreGS) | `vsync = on/fast/off` | low: driver modes; test each value, tearing check |
| **Graphics card** `graphics_card` | CoreGS adapter selection; `GT4_HW_ADAPTER` | **Restart** (E11, C) | Advanced page, list of cards + Automatic | `graphics_card = "<name>"` or `auto` | low: a removed card falls back to the best one |
| **Presentation thread** `present_thread` | CoreGS `GT4_HW_PRESENT_SHARED_QUEUE=0` (only the off value is sent) | **Restart** (E13, C) | Advanced page (CoreGS only), toggle | `present_thread = on/off` | low: one-queue GPUs only; no visual change expected |

### 3.2 Graphics and effects

| Setting (id) | Consumer / when read | Live or restart (evidence, confidence) | Game menu home / row type | Write-through | Risk / test |
|---|---|---|---|---|---|
| **Sharpening (FSR1)** `coregs_sharpening` | CoreGS `GT4_CORE2_FSR_SHARPNESS` | **Restart** (E12, C) | Graphics page (CoreGS only), list Off/Mild/Medium/Strong | `coregs_sharpening = off/mild/medium/strong` | low: visual only |
| **Sun glare and light flares** `effect_flares` | game-side flare table; CoreGS `GT4_CORE2_PERF_FLARES`, GSdx `GT4_GS_HIDE_FLARES` + `GT4_GS_READBACK_LOCAL` | **Restart** (E7/E8, C) | Graphics page, toggle (both renderers) | `[gs] effect_flares = true/false` | medium: night-race visual check and the F8 cost; the first effects row |
| **Road sheen** `effect_road_sheen` | CoreGS `GT4_CORE2_PERF_ROAD_SHEEN` | **Restart** (E7, C) | Graphics page (CoreGS only), toggle | `[gs] effect_road_sheen = true/false` | low/medium: wet-road visual check |
| **Small effects** `effect_targets` | CoreGS `GT4_CORE2_EFFECTS_1X` | **Restart** (E7, C) | Graphics page (CoreGS only), list Full resolution/Lower resolution | `[gs] effect_targets = full/1x` | low: glow/reflection visual check |
| **Blending accuracy** `blending` | gt4-gs `accurate_blending_unit` (GSdx only; CoreGS has no such setting) | **Restart** (E9, C) | Graphics page (GSdx only), list High/Minimum | `[gs] blending = high/minimum` | medium: translucent effects; test |
| **Readbacks** `hw_download_mode` | gt4-gs `HWDownloadMode` (GSdx only) | **Restart** (E9, C); the launcher accepts Accurate and No readbacks today | Graphics page (GSdx only), list | `[gs] hw_download_mode = accurate/no-readbacks` | medium: car icons/loading picture; test |
| **Anti-aliasing** `fxaa` | CoreGS `GT4_HW_POSTFX=fxaa`; GSdx `fxaa` | **Restart** (E18, C) | Graphics page, toggle (both) | `[gs] fxaa = true/false` | low: visual |
| **Sharpening (CAS)** `cas_mode` / `cas_sharpness` | gt4-gs `CASMode`/`CASSharpness` (GSdx only) | **Restart** (E9, C) | Graphics page (GSdx only), list + slider | `[gs] cas_mode`, `[gs] cas_sharpness` | low: visual |
| **Load custom textures** `texture_load` | CoreGS `GT4_HW_TEXTURES`; GSdx `LoadTextureReplacements` | **Restart** (E18/E9, C) | Graphics page, toggle (both) | `[gs] texture_load = true/false` | low: a missing folder is harmless |

### 3.3 Performance

| Setting (id) | Consumer / when read | Live or restart (evidence, confidence) | Game menu home / row type | Write-through | Risk / test |
|---|---|---|---|---|---|
| **Frame rate (experimental)** `frame_rate` | CoreGS `GT4_FRAME_SKIP_FORCE=2` (30) / `GT4_FRAME_SKIP=1` (60) | **Restart** (E6, C). Frame skipping in a race; the physics stay 60 Hz; menus stay 60 | Performance page (CoreGS only), list 60/30 | `frame_rate = "60"/"30"` | medium: smoothness/tearing; test a race at 30 and 60; no game-speed change |
| **Performance HUD** `performance_hud` | CoreGS `GT4_HW_HUD=1`/`fps` | **Restart** (E10, C) | Advanced page (CoreGS only), list Off/FPS only/Full | `performance_hud = off/fps/full` | low; a live toggle would need a renderer API (Hy) |
| **Performance overlay** `perf_overlay` | gt4-gs `GT4GS/PerfOverlay` (GSdx only) | **Restart** (E9, C) | Advanced page (GSdx only), toggle | `[gs] perf_overlay = true/false` | low |
| **Performance report** `performance_report` | game `GT4_PERF=1` | **Restart** (E16, C) | no player row (RC diagnostic) | launcher `settings.toml` only | low |
| **Check compiled VU code** `check_vu_code` | game `GT4_VU1_VERIFY`/`GT4_VU0_VERIFY` | **Restart** (E17, C) | no player row (RC diagnostic) | launcher `settings.toml` only | low |

### 3.4 Controls

| Setting (id) | Consumer / when read | Live or restart (evidence, confidence) | Game menu home / row type | Write-through | Risk / test |
|---|---|---|---|---|---|
| **Controller** `controller` | game host input; `GT4_PAD` | **Restart** (E14, C). SDL opens the chosen pad at start | Controls page, list of connected pads + Automatic | `controller = "<guid>[/<serial>]"` or `auto` | medium: a removed pad falls back to pad 1; hot-plug test |
| **Full trigger range** `trigger_full_range` | game `GT4_TRIGGER_FULL_RANGE` | **Restart** (E15, C) | Controls page, toggle | `trigger_full_range = on/off` | low: throttle/brake feel; test |
| **Game button mapping** (GT4's own) | game script `ControllerRoot`/`KeyconfigRoot` | **Live** (the game's own `apply()`, writes the save) | GT4 Options → Controller | none (game save) | keep as is; no host row duplicates it |
| **Deadzones/curves** (`controls.toml`) | game host input; `GT4_CONTROLS_FILE` at start | **Restart** (`controls.cpp:556`, `standalone.cpp:537`) | would need a host row; not in the first set | launcher writes `controls.toml`; a change in game would need a feedback contract for a second file | medium: separate file, hot-plug |
| **Audio** | none in the launcher; GT4 `SoundRoot` is the game's own | n/a | GT4 Options → Sound (game save) | none | do not invent a launcher audio setting |

### 3.5 Launcher-only settings

| Setting (id) | Consumer | Why not in the game |
|---|---|---|
| **Language** `language` | launcher UI | changes the launcher's language, not the game's |
| **Channel** `update_channel` | launcher updater | a launcher update policy |
| **Discord Rich Presence** `discord_rich_presence` | launcher Discord IPC | the game does not run Discord; the launcher owns the presence |
| **Performance report** `performance_report`, **Check compiled VU code** `check_vu_code` | game, at start | RC diagnostics; the launcher hides them in final builds (`RC_ONLY_ROWS`) |

### 3.6 Live/restart summary

| Today | Settings |
|---|---|
| **Live without a restart** | **Fullscreen only** (`set_fullscreen`; Alt+Enter/F11 already proves the path). The game's own Options pages are live because their script calls `option.apply()`, but that is the game, not a host setting. |
| **Next start of the game** | Renderer, Render scale, Resolution, Monitor, VSync, Graphics card, Presentation thread, FSR1 sharpening, all three effect toggles, Blending, Readbacks, FXAA, CAS, textures, Frame rate, Performance HUD/overlay, Controller, Trigger range, `controls.toml` |
| **Launcher restart or next update** | Language, Channel, Discord Rich Presence |
| **Could become live with new code (Hy)** | Performance HUD (recreate/toggle the HUD layer), effects (read an atomic per batch), frame rate (reconfigure frame skip), controller (SDL device switch), VSync (recreate the swapchain); **not** renderer/scale/resolution/GSdx INI rows without renderer or gt4-gs work |

---

## 4. Live vs restart: the evidence

### 4.1 The two renderers

- **GSdx** is `gt4-gs.exe`, a separate process started by the game's stream client with `GT4_GS_PROCESS`,
  `GT4_GS_PROCESS_SHA256` and `GT4_GS_CONFIG` (`launch.rs` passes the first, the game spawns it). Its whole
  configuration (upscale, blending, readbacks, vsync, FXAA, CAS, texture loading, overlay) is the session
  `gt4-gs.ini`, read at process start. **C** (`docs/GS-LIVE.md` section 1, `launch.rs::gs_config_text`).
- **CoreGS 2.0** is inside the game executable (`GT4_RENDERER=hw`, `GT4_HW_CORE=2`). Its scale, effects, FSR1,
  HUD, adapter and present mode come from environment variables parsed when the renderer/backend is constructed
  (`hw_gs_dispatch.cpp:10`, `hw_gs_options.cpp:226`, `core2_renderer.cpp` const members,
  `hw_gs_backend.cpp:348/752/788/1770`). **C**
- No renderer has a runtime configuration API that these settings use. The stream protocol defines
  `CONTROL kind 4 reload_config`, but gt4-gs answers "unsupported"; `STATE_SAVE`/`STATE_LOAD` are reserved and
  unused. **C** (`docs/GS-STREAM-PROTOCOL.md` section 6.1, `docs/GS-LIVE.md` section 2).
- Restarting **only** `gt4-gs.exe` is not possible today: the GS state lives in that process, the game owns the
  IPC and the stream starts at power-on; a new GS process would need a state save/load and a resync, neither
  exists. **C** (protocol and code); a future fork could add it, not this task.

### 4.2 What is read at start vs every VBlank

- **Once at start (or renderer construction):** `GT4_RENDERER`, `GT4_HW_SCALE`, `GT4_CORE2_PERF_*`,
  `GT4_CORE2_EFFECTS_1X`, `GT4_CORE2_FSR_SHARPNESS`, `GT4_HW_HUD`, `GT4_HW_ADAPTER`,
  `GT4_HW_PRESENT_SHARED_QUEUE`, `GT4_PRESENT_MODE`, `GT4_FULLSCREEN` (the start value), `GT4_MONITOR`,
  `GT4_OUTPUT_SIZE`, `GT4_DISPLAY_ASPECT`, `GT4_PAD`, `GT4_TRIGGER_FULL_RANGE`, `GT4_FRAME_SKIP*`, `GT4_PERF`,
  `GT4_VU1_VERIFY`/`GT4_VU0_VERIFY`, `GT4_GS_HIDE_FLARES`, `GT4_GS_READBACK_LOCAL`, the `gt4-gs.ini` rows. **C**
- **Every VBlank:** the two fixed holds (`GT4_FORCE_480P`, `GT4_FORCE_WIDESCREEN`), the menu/quit watcher, the
  achievements overlay, the performance HUD bookkeeping. These are not settings; they are the fixed behaviour
  and the overlay. **C**
- The launcher writes `settings.toml`, the session `gt4-gs.ini`, `game.ini` (for a direct start) and the
  `GT4_*` environment **before** Play; changing a setting while the game runs does not reach the running
  process. **C** (`launch.rs`, `commands.rs::play`).

### 4.3 Fullscreen, resolution, monitor, VSync, HUD

- **Fullscreen** is the one setting whose action already exists at runtime: `set_fullscreen` changes the window
  style/placement, `WM_SIZE` recreates the swapchain, and the GS child follows with a `CONTROL resize`. **C**
  (window code); Alt+Enter/F11 is the live proof.
- **Resolution** is a start value (`GT4_OUTPUT_SIZE`) that also feeds `display::display_plan` (the internal
  multiplier); changing the window size live would not change the stored setting or the render scale. **C**
- **Monitor** is a start placement (`GT4_MONITOR`); moving the window live is not implemented as a setting. **C**
- **VSync** is the swapchain present mode chosen at construction (CoreGS) or the INI row (GSdx); a live change
  would need a swapchain recreation or a gt4-gs reload. **C**
- **HUD** is **not** live today, contrary to the first expectation: `GT4_HW_HUD` is read into a `const` mode at
  backend construction (`hw_gs_backend.cpp:348-350`), and there is no runtime setter. Changing it live would need
  a new layer/toggle. **C** for restart, **Hy** for a cheap live toggle.

### 4.4 Frame rate 30/60

`frame_rate` is **not** a physics change. The game's physics and timing stay at the fixed 1/60 s. The 30 FPS mode
passes `GT4_FRAME_SKIP_FORCE=2`, which draws one frame in two in a running race; the 60 FPS mode passes
`GT4_FRAME_SKIP=1`, which leaves frames out only when the PC is behind, so the game keeps its full speed
(`frame_skip.hpp:19-25`, `launch.rs::coregs20_environment`). Menus stay at 60. **C**

The frame-skip controller is configured once in `start_from_environment`; a live switch would need a
reconfiguration entry point (not present). **C**

---

## 5. Implementation routes

### 5.1 Extending GT4's Options page (the Quit Game pipeline, and its limit)

What transfers from TASK-212:

- the **same-length data patch** (`scripts/gt4_data_patch.py` + `native/data_patch.cpp` + the `GT4_DATA_PATCH`
  capsule) for any text or byte edit of the option files; **C**
- the **widget style** (the game's `Composite`/`TextFace`/`OptionMenu`/`SliderBar` templates, `hb_24`/`hm_16`
  fonts, the row geometry) and the **pad masking** (hide Cross/Circle from the guest while a host-only row is
  focused); **C**
- the **VBlank watcher** pattern (`menu_quit`): find a known tree by vtable/geometry, inject once, track the
  focused row, act on a pad edge, reuse the cached tree until the vtable check fails; **C**
- the **exit/persistence pattern**: a host action, not a guest call. **C**

What does **not** transfer:

- The arcade main menu is a `SelectBox` with index-based navigation and a fixed `button_list`; the host append
  works because the `SelectBox` owns the rows and the index. **C**
- The Options root and the Screen page have **no `SelectBox`**: `OptionRoot` is a `ScrollWindow` whose `space`
  is filled by the script with `hidden::Item` cards, and `ScreenRoot` is a `VBox` whose rows are script-focused
  (`OptionMenu`/`SliderBar` with `has_script`). (The arcade `CourseRoot` page does contain a `SelectBox`, but it
  is not the page this task targets.) Appending a widget to `Pane` does not add it to the script's focus list,
  so the game's own navigation would never reach it. **H** (tree C; focus behaviour H).
- Therefore "add a row to the Options screen exactly like Quit Game" is **not** the same cheap pipeline. A host
  row in `Pane` would need a second watcher that (a) finds the page, (b) injects the row, (c) intercepts
  Up/Down/Cross before the guest, (d) feeds the guest a neutral pad, and (e) handles the row's value drawing.
  That is a new pattern, about 2-3 slices, with a script-focus risk. **H**

The native route that fits the existing page best: **repurpose an existing row** and override its option
callback, so the script, focus and layout stay the game's. This is an alternative to removing the row: the first
slice removes `HiVision` (owner's request); a later phase can repurpose `Wide` (and, if the owner prefers a
control over a deletion, `HiVision` too).

| Row | Choices | Host setting it could carry | Callback to override |
|---|---|---|---|
| `HiVision` (3) | Normal / 480p / 1080i | Render scale presets (100/150/200%) or Frame rate (60/30/… only 2) | `MOption.race_display_mode` `0x001805D0` |
| `Wide` (2) | 4:3 / 16:9 | Fullscreen On/Off, or Renderer GSdx/CoreGS 2.0 | `MOption.wide_mode` `0x00180108` |
| `Vertical`/`Horizontal` (slider, -50..50) | a number | Render scale as a slider (50–200), or an effect strength | `MOption.screen_adjust_v` `0x001802F0` / `screen_adjust_h` `0x00180200` |

How the override works (the TASK-078 P2 idea, now with the data patch already in place):

1. The script's setter is reached by `jalr`; a host table in `ps2::dispatch` maps `0x001805D0` (or `0x00180108`)
   to a host function. **C** path.
2. On a set, the host stores the chosen host value (render scale preset, fullscreen, renderer) and **does not
   call the original setter**, so the guest option field stays `1` (480p) / `1` (16:9). On a get, the host
   returns the boxed host value so the row shows the selection. Returning a value needs the game's boxing
   helpers (`func_002FE278` for an int, `func_00314B20` for a string); the exact callback contract is only
   partly derived. **Hy**
3. The labels: replace the popup item `key`s with literal `text string {...}` in `option.mproject` (the Quit
   widget proved a literal `TextFace` text works) or edit the `US/rtext.rt2` values. A text edit is same-length
   after minification; the byte patch path already exists. **H**
4. The host writes the value to the feedback file (section 5.4); the guest calls `option.apply()` and
   `main::game.setNeedSave()` unchanged, which may re-write the save with the same `1`/`1` values. **H**
5. Acceptance: the row shows our label and value; changing it does not change the race CRT mode or the wide
   flag; a save from the session still loads on a real PS2; every other option page still opens. **C** target.

Limits: only as many native rows as there are repurposable widgets (four on the Screen page); each override is a
callback-contract risk; the script is v5 and not decompiled, so every assumption must be checked on a headless
frame. **H**

### 5.2 A native overlay panel (host UI over the game)

Reuse the achievements overlay, which is already an owned, layered, non-activating window over the game and
survives both renderers (`achievements_overlay.cpp`; ticked from `window.cpp:723`). **C**

- Draw a settings panel with GDI+ (rows, values, selection), in the launcher's style; it is clearly ours, not
  GT4's page.
- Read the pad in `window.cpp`'s VBlank path before `apply_pad`; while the panel is open, feed the guest a
  neutral pad (the `menu_quit::guest_buttons` pattern) and consume Up/Down/Left/Right/Cross/Circle/Triangle.
- Open it with a **global pad combo** (for example L3+R3+Select, to be checked against GT4's own bindings; the
  Quit combo is L3+R3+Start) or with **F1** on the keyboard. A page-scoped trigger on the Options pages needs a
  page gate (v2.00 `[0x622F4C]+0x3A39C` is the menu project, but the page name is not in a known word; Spec II
  has no reliable menu word). **H/U**
- No game data, no script, no save; works on both games and both renderers; covers every row in section 3.
- Risks: the layered window over the `gt4-gs.exe` child (the achievements overlay proves the window part, not an
  interactive panel); the attract timer if the panel is opened on the arcade top menu (the Options page likely
  has no auto-demo — **U**); pad conflicts with the game's own menu navigation (the panel consumes the pad, so
  the game sees nothing until it closes).

Effort: about 1.5-2 slices for the panel, the row table and the feedback writer. This is the route that gives
"as many launcher functions as possible" soonest.

### 5.3 The hybrid (recommended first)

1. Remove `HiVision` with the `"NTSC"` byte patch (or host-hide if the patch builder is not in the build).
2. Put the host settings in the overlay panel, opened from the game (combo) and, later, from a native row.
3. Write every change through the `game-changes.toml` feedback file; the launcher merges it.
4. In a later phase, convert the top rows to native rows with the repurpose + callback override (section 5.1).

This keeps the first slice small, proves the option-file patch on the Options data, and does not put a host
value in the save.

### 5.4 The `game-changes.toml` feedback contract

Keep the launcher the **only writer of `settings.toml`**. The game writes a separate, small file; the launcher
validates and merges it.

- New launcher-to-game variable: `GT4_SETTINGS_FEEDBACK=<config dir>\game-changes.toml` (the same config folder
  as `settings.toml`, `paths.rs:config_file`). The game writes it atomically (temporary file + rename, like
  `gs_table::write_ini_atomic`). **H** design.
- Format (versioned, no game data):

```toml
version = 1
# one line per change; the launcher validates each id and value
[changes]
fullscreen = "on"
render_scale = "150"
effect_flares = "false"
```

- The launcher merges on the exit watcher (`commands.rs::play`'s `wait_for_exit` thread) **and** at startup, so a
  launcher crash does not lose the change; it then deletes the file. **H** design (the watcher exists; the merge
  is new).
- Validation: a normal id goes through `settings::definition` + `validate` + `set_by_user`; a GS id goes through
  `gs_table::row` + `check_user_value` + `set_gs`. Unknown ids, invalid values and ids the build does not offer
  are logged and dropped; the file is never trusted blindly. **C** (the validators).
- Values are the launcher's stored forms, not the env forms: `render_scale` is `"50".."200"`, not
  `GT4_HW_SCALE`'s multiplier; `effect_flares` is `true`/`false`, not `on`/`off`; `renderer` is `pcsx2`/
  `coregs20`. **C** (`settings.rs`, `gs_table.rs`).
- `renderer = coregs20` should be refused (kept GSdx, with a log) when the PC has no Vulkan-capable card; the
  launcher is the authority, not the game. The check exists in `use_coregs20_after_gsdx_failure`; the merge would
  reuse it. **C** for the check, **H** for the merge rule.
- The launcher shows the restart notice itself if it wants (home message), but the notice the player sees while
  changing is drawn by the panel ("applies at the next start"). The launcher never restarts the game by itself.
  **H**
- The game's save is untouched: `settings.toml` and `game-changes.toml` are host files; the guest option struct
  is never written by a host row. **C** design rule.

### 5.5 Route comparison

| Route | What the player sees | Coverage | Effort | Risk | Save |
|---|---|---|---|---|---|
| **HiVision byte patch** | the row is gone | one page | 0.5-1 slice | low (data only) | no write |
| **Repurpose a row + callback override** | native GT4 row and page | 2-4 rows (the repurposable widgets) | 2-3 slices | medium (callback contract, script semantics) | safe with the swallow; the script may dirty the save |
| **Host append into Pane** | native GT4 row | few rows | 2-3 slices | medium-high (no `SelectBox`; focus bypass) | safe (host-only row) |
| **Overlay panel** | our panel over the game | every row in section 3 | 1.5-2 slices | low-medium (input, layered window over the GS child) | no write |
| **Hybrid (recommended)** | row gone + our panel first, native rows later | all, improving | 2.5-4 slices for the first set, 2-4 more for the native phase | low first, medium later | safe |

---

## 6. The first safe set (rc17 or the next build)

Ordered by effort/risk. Each step is its own commit/capsule if needed; the owner tests the windowed result.

| # | Change | Vehicle | Effort | Risk | What it proves / acceptance |
|---|---|---|---|---|---|
| 1 | **Remove the `HiVision` row** | `option.adc` `"NTSC"` byte patch (same-length); fallback host-hide if the launcher patch build is not in the release | 0.5-1 slice | low | the option-file data patch works on the Options data; Screen page has no video-output row; all pages and a race still work; save fields stay `1`/`1` |
| 2 | **Host settings panel + feedback** | overlay (achievements-style layered window) + `game-changes.toml` + launcher merge | 1.5-2 slices | low-medium | a panel opens from the game (combo), consumes the pad, writes a change, the launcher merges it on exit and uses it next start; **Fullscreen is live** |
| 3 | **Renderer** and **Render scale** rows | same panel; restart notice | +0.5 slice | medium | the two top owner asks; a restart is required and clearly announced; GSdx/CoreGS both start with the new value; a PC without Vulkan keeps GSdx |
| 4 | **First effects row: Sun glare and light flares** (`effect_flares`) | same panel; restart notice | +0.25 slice | medium | the shared effect works under both renderers; a night-race visual and F8 cost check; then road sheen/targets (CoreGS only) |
| 5 | Later | native repurpose for Fullscreen/scale; blending/readbacks (GSdx); frame rate; HUD; monitor/resolution/vsync; controller/trigger | 2-4 slices | medium | the native look and the remaining rows |

Reasoning:

- The removal is independent of the panel and is the smallest proof of a patch to `option.adc`; it can ship even
  if the panel slips.
- Fullscreen is the only setting whose live action already exists; it is the honest "live" row and validates the
  input/feedback plumbing.
- Renderer and Render scale are the owner's explicit asks and the reason the restart notice exists; putting them
  in the first panel makes the restart rule visible early.
- Flares is the cheapest effect (one shared toggle, a large measurable gain) and the first of the three effect
  toggles the owner named.
- Blending/readbacks are GSdx-only and need the gt4-gs reload that does not exist; they are not first.

---

## 7. How to add another row or setting

A repeatable recipe, extending `QUIT-GAME-ROW-RETAIL-SPEC2.md` section 3.

0. **Choose the vehicle.** Overlay row (host only, any setting) for the first slice; native repurpose (an
   existing row + a callback override) for the GT4 look; host append only after a focus-navigation study. Choose
   the target (v2.00 / Spec II) and whether the setting is shared (one implementation) or per renderer.
1. **Find the data anchor (native only).** For a text edit, an anchor that occurs exactly once in the decoded
   file; for a byte edit, the exact offset, length and `expect_hex`. Build the patch from the user's disc; the
   builder refuses growth and the runtime verifies every original range. Never commit the recipe or the patch.
2. **Define the row** in the panel's row table (one source for the panel, the feedback writer and the tests):
   `id`, player label (en-US and pt-BR), row type (toggle / list / steps), allowed values, the consumer, live or
   restart, the feedback id and the save rule (`host-only`). For a native row, also the widget and the callback
   address.
3. **Draw and input.** Panel: a row per definition, selection highlight, value text, "next start" marker on a
   restart-only row. Native: the game's own widget; the host draws only the value if the widget cannot.
4. **Apply.** Live rows call the host action (Fullscreen → `set_fullscreen`; later HUD/effects if a runtime
   setter exists). Restart rows do nothing to the running game; they write the feedback value and show the
   notice. A host row never writes the guest option struct.
5. **Write through.** The game writes `game-changes.toml` atomically; the launcher validates and merges on exit
   and at startup; the launcher is the only writer of `settings.toml`.
6. **Protect the save.** No host value in the option object; the guest fields stay `1` (480p) and `1` (16:9); a
   swallowed setter must still let the script's `apply()`/`setNeedSave()` run (the values are unchanged).
7. **Test.** A unit fixture for the row table and the feedback file; a Python patch test for every byte/text
   anchor; a headless run through the page (frame + log); a race; a save-field check; the owner's windowed test in
   both games.
8. **Docs.** Update `TASKS.md` (state and result), `docs/STATUS.md`, the daily journal, and this mapping when a
   rule changes. Record the exact addresses/hashes in the task, not in a committed recipe.

Shared vs target-specific:

| Piece | Shared | Target-specific |
|---|---|---|
| `gt4_data_patch.py`, `data_patch.cpp`, `GT4_DATA_PATCH` | yes | the recipe anchors and the target detection |
| Widget text/style (fonts, skew, shadow) | yes | the page tree and the hidden template anchors |
| Overlay panel, row table, input mask | yes | the page gate (v2.00 menu word; Spec II structural) |
| `game-changes.toml` and the launcher merge | yes | none |
| Callback overrides | the addresses are per target | v2.00 `0x001805D0`/`0x00180108`/…; Spec II needs its own addresses (not derived here) |
| Tests | the algorithm | the fixture tree and the headless state |

---

## 8. Unknowns and next checks

1. **`HiVision` byte patch in the running game.** The bytes are confirmed; the Screen page still needs a headless
   frame (v2.00 then Spec II) to prove the row is gone and the layout has no gap. Next: run with
   `options-screen3/state.bin` (v2.00) and a new Spec II Options state.
2. **Spec II Options state and page gate.** No Spec II Options-page runtime state exists (the TASK-212b kit has a
   top-menu state only); a page-scoped trigger and a host-hide watcher need one. **U**
3. **Host-hide `visible` offset.** The row object is in the state (vtable `0x00662EF8`, geometry
   `{0,116,320,32}`), but the exact `visible` field is not derived; a visible/hidden state pair or a headless
   toggle is needed. **U**
4. **Structural `.mproject` removal.** High confidence that deleting the `HiVision` node breaks the script
   (21 string references, missing widget is fatal); not run. Only after a v5 script round trip. **H/U**
5. **Native repurpose contract.** The setter/getter boxing and the `argc > 0` convention for `0x001805D0`,
   `0x00180108`, `0x00180200/0x001802F0` are partly derived; the exact returned `hString`/`hInt` object and the
   script's `reflect`/`apply` order need a headless test. **Hy**
6. **Overlay over the GSdx child.** The achievements overlay proves a layered window over both renderers, but an
   interactive panel (focus, input, fullscreen) is not tested over `gt4-gs.exe`. **U**
7. **Launcher patch build.** The "Preparing game files" integration that builds `GT4_DATA_PATCH` from the user's
   disc and invalidates it on a disc change is planned (`QUIT-GAME-ROW-RETAIL-SPEC2.md` section 1.4), not
   implemented. Without it, the row removal needs the host-hide. **C** design, **U** implementation.
8. **Feedback-file location/permissions.** A config-folder file is writable in the normal install; a portable or
   read-only install may not be. The session folder is the fallback (the launcher keeps 20 sessions). **U**
9. **Merge timing.** The exit watcher exists (`commands.rs::play`), but merging at exit and at startup, atomic
   replacement and the "reject a value the build does not offer" rules are a design, not code. **H/U**
10. **Restart UX.** The panel can show "applies at the next start"; the launcher does not restart the game. A
    one-click "restart now" would need a relaunch contract with the launcher (out of scope). **U**
11. **`frame_rate` 30/60.** It is frame skipping, not physics; a 30 FPS race test and a smoothness/tearing check
    are the owner's gate. **C** semantics, **U** the visual result on the owner's PC.
12. **Audio.** The launcher has no audio setting; GT4's `SoundRoot` is the game's own. Do not invent one. **C**
13. **`Live` HUD/effects/frame rate.** They need new renderer/controller setters; the effort is a later slice,
    and the effect flags are currently `const`. **Hy**
14. **`Wide` replacement.** The two-choice row could carry Fullscreen; the `wide_mode` callback and the label
    edit are not tested. **Hy**
15. **Controller hot-plug.** `GT4_PAD` is opened in the `ControllerInput` constructor; a live switch needs an SDL
    device reopen. **Hy**
16. **Game-side deadzones.** `controls.toml` is read at start; a game row for deadzones/curves would need a
    second feedback file or a merge into `controls.toml`, which the launcher owns. **U**
17. **Spec II `SpecIISettingRoot`.** The publisher's `GroupSpecII` page (`SpecIISettingRoot`, line 8999) is a
    native settings page of `OptionMenu` toggles. Whether it can host one more host row without a v7 script edit
    is a next check; it is the closest existing model for the native phase. **U**

---

## 9. Address quick reference

| Thing | v2.00 (SCUS-97328) | Spec II (SCUS-97436) | Conf. |
|---|---|---|---|
| Options project | `projects/option/option.adc` 251,715 (kind 2, 55,353 stored) | 288,080 (kind 2, 58,422 stored) | C |
| Options widgets | `option.mproject` 681,977 (kind 2, 22,449 stored) | 804,881 raw (kind 1) | C |
| Volume TOC | `0x10AC800` | `0x115B800` (odd page parity) | C |
| Page table | `option.adc` `0x8A7C`-`0x99DD` (length words) | `0xCB6D` onward, second `GroupGeneral` `0xDBD4` | C |
| `ScreenRoot` | mproject line 5812, `children [6]` | line 6616, `children [6]` | C |
| `Pane` | line 5852, `children [7]` line 5859 | line 6656, `children [7]` line 6660 | C |
| `Wide` | line 5914, items 6008/6041 | line 6713, items 6805/6838 | C |
| `Vertical` / `Horizontal` | 6081 / 6210, sliders 6101 / 6230 | 6878 / 7005, sliders 6896 / 7023 | C |
| `HiVision` | line 6339, `visible 0`, items 6434/6467/6500 | line 7132, items 7225/7258/7291 | C |
| `"NTSC"` literal | `option.adc` `0x247FB` (149,499) | `0x2A2E5` (172,773) | C |
| HiVision script path | `0x24822`, `0x252E6`; `visible` `0x24832` | (not re-derived) | C/U |
| `Wide` script path | `0x24F2C`; `wide_mode` `0x24A81` | (not re-derived) | C/U |
| `TryHiVision` | `0x254A1` | (not re-derived) | C/U |
| Option object | owner `0x622F4C` + `0x38CB0` | owner `0x664EDC` + `0x680` | C |
| Race display index | option + `0x16B0` | option + `0x1718` | C |
| Wide flag / record | option + `0x14` / + `0x24` (44 bytes, w/h f32 +0xC/+0x10) | same offsets | C |
| Wide presets / published word | `0x622E90`…`0x622F20`, `0x6184F0` | `0x664E20`…`0x664EB0`, `0x6655A0` | C |
| `MSystem.GetVideoSystem` | `0x001AC598` | (same class, address not derived) | C/U |
| `MOption.race_display_mode` | `0x001805D0` | not derived | C/U |
| `MOption.wide_mode` | `0x00180108` | not derived | C/U |
| `MOption.screen_adjust_v/h` | `0x001802F0` / `0x00180200` | not derived | C/U |
| `MOption.applyRaceDisplayMode` / `applyMenuDisplayMode` | `0x001899F0` / `0x00189A28` | not derived | C/U |
| `func_00436640` / `func_00436740` | race mode / menu 480i | not derived | C/U |
| Menu state word | `[[0x00622F4C]+0x3A39C]` last, `+0x3A3BC` next | no reliable word (structural gate) | C/H |
| Race 480p hold | `native/race_display.cpp` | same | C |
| Wide hold | `native/race_widescreen.cpp` | same | C |
| Fullscreen live | `window.cpp:369` `set_fullscreen`, `448-453` | same | C |
| GS child follows resize | `CONTROL resize` (GS-LIVE) | same | C |
| `reload_config` | answered "unsupported" | same | C |
| Launcher settings | `settings.toml` schema 9 | same | C |
| GS table | `docs/launcher/gs-settings.toml` | same | C |
| GS INI | session `gt4-gs.ini` (`GT4_GS_CONFIG`) | same | C |
| Data patch | `GT4_DATA_PATCH`, `data_patch.cpp:156` | same | C |
| Options-page state | `private/slice113-video/options-screen3/state.bin` (RAM at 1460) | none saved | C/U |

---

## 10. Resumo para o dono (pt-BR)

- A linha de 3 escolhas que você quer tirar é a **HiVision** (Normal / 480p / 1080i) da página **Tela**
  (Options → General → Screen). A linha de proporção é a **Wide** (4:3 / 16:9). As duas hoje não fazem efeito:
  o recomp já fixa 480p e 16:9 a cada VBlank; os menus continuam em 480i.
- Dá para tirar a HiVision **sem editar script**: trocamos 4 bytes da constante `"NTSC"` no `option.adc` (a
  mesma técnica de patch do Quit Game, que já existe). A linha some como num console não-NTSC. É a primeira
  coisa segura; o teste é uma captura da página Tela e uma corrida.
- O resto não cabe de graça na página Tela: as páginas de Options não têm a lista SelectBox do menu principal;
  acrescentar linha ali exige script v5 (que ainda não fecha) ou reaproveitar uma linha existente com um
  override no jogo. Por isso, para o próximo build, o caminho é um **painel nosso desenhado sobre o jogo**
  (o mesmo tipo de janela do overlay de conquistas), com todos os ajustes, e o arquivo `game-changes.toml` que o
  launcher junta no `settings.toml`.
- **Ao vivo, hoje, só Tela cheia.** Renderizador, escala, resolução, monitor, VSync, efeitos, blending,
  readbacks, HUD e 30/60 FPS precisam reiniciar o jogo: são lidos na inicialização (ou no `gt4-gs.ini`), e o
  `reload_config` do GS responde "unsupported". O modo 30 FPS é só pular quadros; a física continua 60 Hz.
- Primeiro conjunto: tirar a HiVision; painel com Tela cheia (ao vivo); Renderizador e Escala de render
  (aviso de "vale no próximo início"); depois o primeiro efeito, os flares (funciona nos dois renderizadores).
- Nada disso grava valor de host no save do GT4: o save continua 480p/16:9 e abre num PS2 de verdade.
