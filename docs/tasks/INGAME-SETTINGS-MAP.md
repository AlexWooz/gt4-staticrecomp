# Mapping the launcher settings into GT4's own Options menu (TASK-213)

Read-only study, 2026-11-09. Branch `task/213-settings-map` (from `task/212b-spec2-quit` at `70af98d7`).
No code, data, build, pin or test was changed for this document.

> **Update (2026-11-09, TASK-213 Phase 1):** sections 11-13 were added on `task/213-native-settings` (from
> `a3d7f54b`). They record the native implementation that shipped in the test build (HiVision removal, `Wide` ->
> Fullscreen with the host callback and the `game-changes.toml` feedback) and the owner's page-extension plan (more
> rows on the Screen page, or a new graphics page inside Options). The study sections 1-10 are unchanged and remain
> the reference for the data and the live/restart rules.

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

## 11. Phase 1 as implemented: HiVision removed, Wide -> Fullscreen (2026-11-09)

Branch `task/213-native-settings` (from `task/213-settings-map` at `a3d7f54b`). v2.00 only; Spec II is Phase 2.
This section replaces the "host panel first" recommendation of section 6 for the native look: the owner chose
**all-native in-game rows, no floating host panel** (2026-11-09).

### 11.1 The two data edits (private, from the owner's disc)

| Target | File | Edit | Anchor | Result |
|---|---|---|---|---|
| v2.00 | `projects/option/option.adc` | 4 bytes `4E 54 53 43` -> `50 41 4C 20` (`"NTSC"` -> `"PAL "`) | `0x247FB`, unique | `GetVideoSystem() == "NTSC"` is false, `Pane.HiVision.visible` stays `0` |
| v2.00 | `projects/option/option.mproject` | `key string {"LabelWide"}` -> `text string {"Fullscreen"}`; `key string {"Item4x3"}` -> `text string {"Off"}`; `key string {"Item16x9"}` -> `text string {"On"}`; the Wide `OptionMenu` `tooltip digit {1}` -> `{0}` | each anchor unique; text edit | row label and both choices read from literal text, no `rtext.rt2` edit; the stale 4:3/16:9 tooltip is suppressed |

- **No minification** (`"minify": false`): the patch keeps every byte of whitespace, so only the label bytes differ.
  The edited decoded file is 681,971 bytes (6 shorter), padded with 6 newlines to exactly 681,977, and its PS2ZIP
  payload compresses to **19,965** bytes against the 22,449-byte stored extent (headroom 2,484). **C** (builder
  report; measured with `gt4_data_patch.ps2zip`).
- The container has two ranges (77,986-byte container, 77,802 payload bytes), SHA-256
  `e83ecb2390c9065ae7362f6752ad64d7b76c5baa2bdd93447f874ca95e19ed96`; the runtime prints
  `[data-patch] ...: 2 range(s), 77802 payload bytes, verified against the image` before the first byte is used. **C**
  (headless logs).
- Why literal text and not `US/rtext.rt2`: the RT2 is another volume file (its values may be encrypted in RT05; GT4 is
  RT04), and a key shared with another page would change everywhere. A literal `TextFace.text` is exactly what the
  TASK-212 Quit widget proved. **C** (Quit row) / **H** (no other user of the keys).
- The `text string {"Fullscreen"}` fits the 134-px label box in `hm_16` and matches the page's style; `align 3` keeps
  the right alignment. The choices' `hm_16` labels `Off`/`On` are shorter than the old `4:3`/`16:9`. **C** (frames).

### 11.2 The callback contract (v2.00, Confirmed on the headless page)

`MOption.wide_mode` is one script native, `0x00180108`, registered as both the getter and the setter
(`config/adhoc_methods.txt`; decomp `MOption__get_wide_mode`, unit `0x0017FCB0`). The script calls it through
`hBuiltinMethod` virtual 14, i.e. a `jalr`, so the recomp's `ps2::dispatch` sees the whole call. **C**

| Register / slot | Meaning |
|---|---|
| `a0` | the caller's `hValue` return slot (the hidden structure-return pointer) |
| `a1` | the method context; the native ignores it |
| `a2` | `argc`: `> 0` = set, `0` = get |
| `a3` | `argv`: `argv[0]` = a guest `hInt` (the OptionMenu index), vtable `0x00674188`, value at `+0x10` |
| return | for a get, the native boxes an `hInt` with `func_002FE278` (allocate 0x14-byte RefCounter, `hInt` ctor, write the slot, retain) and releases the old handle with `func_003285F8`; the result is not in `v0` |

`native/native_settings.*` + `scripts/native_settings_overlay.inc` (capsule `slice424-native-settings-license-guard-runtime`):

- **Dispatch:** `if (pc == 0x00180108 && on_wide_mode(...)) return;` - handled calls set `c.pc = ra` and return, so the
  native body never runs; every other dispatch, and the same address in another project, is untouched.
- **Page gate:** only while `[[0x00622F4C] + 0x3A3BC] == "opti"` (the menu state's next project id; `0x6974706F`). The
  arcade TopRoot reads `option.wide_mode` for its 4:3/16:9 layout (`arcade/TopRoot.ad` in the OpenAdhoc v7 source, line
  19); outside the Options project the call is passed through, so that menu keeps seeing the guest's `1`. **C** (RAM
  state `options-screen3`: the word is `opti`; fixture: `gtmo` -> passthrough).
- **Setter:** read `argv[0]` as an `hInt` (vtable guard; any other object is left to the native), accept only 0/1,
  store the host value, write the feedback file, request the window change, and **do not call the native**. The guest
  `option + 0x14` and the 44-byte record are never written.
- **Getter:** box the host value (0 Off, 1 On) into `a0` with the game's own `func_002FE278`, after releasing a
  non-null old handle with `0x003285F8`; the guest call runs through `ps2::call(func, depth + 1)` and the hook restores
  `c.pc = ra` afterwards. `ScreenRoot.reflect` calls `Pane::Wide::OptionMenu.setIndex(context, option.wide_mode)`, so
  the row shows the host value. **C** (run A: host 0 -> Off while the guest is 1; run B: host 1 -> On).
- **Cancel reverts correctly:** `onInitialize` snapshots `sWide = option.wide_mode`, which is now the host value; Cancel
  runs `applyOrg` and writes that snapshot back through the same setter, so a cancel restores the value the page opened
  with (and writes the file again only when it differs). **H** (script source of the publisher's v7 twin; the v5 script
  is not decompiled) - the page test did not press Cancel; it is part of the owner test.
- **The live window:** `window.cpp` applies the pending value at the next VBlank with the same `set_fullscreen` path as
  Alt+Enter/F11 (borderless fullscreen, `WM_SIZE` recreates the swapchain, the GS child follows with `CONTROL resize`).
  Alt+Enter/F11 also writes the feedback file now, so the row and the keyboard stay in agreement. A headless run has no
  window and only logs the request. **C** (code + log); the live window is the owner's test.
- **Target table:** the addresses are in `native/game_target.hpp` (`option_wide_mode_callback`, `script_box_int`,
  `script_release_handle`, `script_hint_vtable`, `option_project_word_offset`, `option_project_id`). Spec II leaves them
  0, so the hook logs one `[target] ... disabled` line and the native runs; the Spec II derivation is Phase 2. **C**

Risk of the guest-call getter: it runs the game's allocator and `hInt` ctor from inside a dispatch hook. `ps2::call`
recurses into `dispatch` exactly as any guest call does and returns when the callee's translated body returns; the
depth is the caller's + 1. If the boxer address had no translated entry, `ps2::has_code` refuses first and the native
runs unchanged (no halt). **H** (design; every headless getter call exercised it - the page reflects the value on every
open).

### 11.3 The feedback file (launcher is still the only writer of `settings.toml`)

- `GT4_SETTINGS_FEEDBACK=<config folder>\game-changes.toml` is passed by the launcher (`commands.rs::prepare_play`,
  `extra`). The game writes, atomically (temporary file + `MoveFileEx` replace), exactly:
  ```toml
  version = 1

  [changes]
  fullscreen = true
  ```
  on every host change (the row or Alt+Enter/F11). The write is immediate, so a crash after the change loses nothing.
  **C** (files `game-changes-a.toml`/`-b.toml`).
- `gt4l-core::game_changes` (new module) validates: `version` must be 1; a normal id goes through
  `settings::definition` + `Settings::set_by_user`; a GS id goes through `gs_table::row` + `Settings::set_gs`; a TOML
  boolean is accepted for a toggle only; unknown ids, invalid values, a missing/newer version and a malformed file are
  logged and dropped. The file is deleted after the merge either way, so it is never applied twice. **C** (14 unit
  tests).
- The merge runs **at game exit** (`commands.rs::play`'s exit watcher, after the card harvest) and **at the next
  launcher start** (`state.rs`, after `config::load`, before the first Play), and `save_config` persists it. A
  crash-left file is therefore merged exactly once. **C** (unit tests + code).
- Values are the launcher's stored forms, not the env forms. Only Fullscreen exists in Phase 1; the same contract
  carries the restart-only rows of section 12 unchanged.

### 11.4 Acceptance evidence (v2.00, headless, 2026-11-09)

| Check | Result |
|---|---|
| Page without HiVision | frame `run-a-frame_02300.png`: "Screen Settings", rows Fullscreen / Vertical Adjust / Horizontal Adjust, Cancel/OK; no HiVision row, no gap |
| Wide label and choices | "Fullscreen" with `Off`/`On` in the popup (`run-a-frame_02450.png`), closed row shows the selected word |
| Getter = host value | run A `GT4_FULLSCREEN=0` -> Off; run B `GT4_FULLSCREEN=1` -> On, before any change |
| Setter = host value | run A Off -> On (`[native-settings] Wide row: fullscreen On`, `fullscreen = true`); run B On -> Off (`fullscreen = false`) |
| Native setter skipped | run B state: wide flag `1`, and the 44-byte record equals the 16:9 preset at `0x622EC0` (425 x 240), not the 4:3 preset at `0x622E90`; had the native run for Off it would have written `0` and the 4:3 preset |
| Feedback file | exact `version = 1` + `[changes] fullscreen = true|false`; no temporary file left |
| Live window | not in a headless run (`[native-settings] fullscreen On requested; this run has no window`); owner's windowed test |
| Save | the hook writes no option field; the card is a copy; the wide flag/record in both states are the game's own 16:9 |

Labels: **C** for the page capture, the labels, both directions, the file and the untouched guest fields; **H** that the
v5 script's Cancel path behaves like the v7 twin; **U** on the owner's window, the attract timer and the GS child resize.

## 12. Extending the Screen page or adding a graphics page (owner clarification, 2026-11-09)

The owner's goal is not only to repurpose the existing rows: **add options to Options > General > Screen carrying the
launcher's graphics settings, or create a new graphics page inside Options**. This section is the plan; it also states
which part is blocked and why. The floating host panel is **out** (owner decision); a host-native page built from the
game's own widgets and reached from Options is the accepted fallback only if the script route is impossible.

### 12.1 The page as it stands (and the "3 rows")

What the owner calls "the 3 original rows" are the three **visible interactive rows** of the page: `Wide` (now
Fullscreen), `Vertical` (-50..50), `Horizontal` (-50..50), each packed 32 px high in the `Pane` VBox. The page actually
has a **fourth interactive row**, `HiVision` (3 choices), hidden by the script unless `GetVideoSystem() == "NTSC"`; with
our patch it stays hidden. There are also the `Title` (20 px), the `Buttons` row (Cancel/OK, 34 px) and the page-level
`ToolTip` (y 438). **C** (mproject lines 5812-6649; frames).

```
ScreenRoot (page, full 640x480)
  bg0 + three ColorFaces
  VBox "Pane" (160,130,320,150), packing 1, clip 0
    FrameImageFace "bg" (2,2,320,150, 4R_* round frame)
    Composite "Title"   (packing, 320x20)          TextFace key TitleScreen
    Composite "Wide"    (packing, 0,20,320,32)      label 9,0,134,32; OptionMenu 150,4,160,24, has_script
    Composite "Vertical"   (packing, 0,52,320,32)   label; SliderBar, range -50..50, popup
    Composite "Horizontal" (packing, 0,84,320,32)   label; SliderBar, range -50..50, popup
    Composite "HiVision" (packing, visible 0, 0,116,320,32)  3-choice OptionMenu, has_script
    Composite "Buttons"  (packing, 0,116,320,34)    Cancel (40,1) / OK (160,1), can_focus
  Composite "ToolTip" (0,438,640,42)  ToolTipFace
```

The VBox packs only visible children, so with HiVision hidden the heights add up exactly (20 + 32 + 32 + 32 + 34 = 150).
`ROOT.setFocus(Pane::Wide::OptionMenu)` in `onInitialize` is the only focus instruction in the script; moving between
rows is engine focus traversal inside the VBox (the publisher's v7 twin has no per-row key code either). **H** for
traversal (the page works that way today; not instrumented); **C** for the tree and geometry.

### 12.2 Adding rows to the Screen page (data-only, no script)

The row list is the `Pane` widget's `children` array in `option.mproject`; the page table in `option.adc` lists **pages**,
not rows, so a row needs no script and no page-table entry. The steps:

1. `Pane children [7]` -> `[8]` (or more), and one `Composite` per row with `packing digit {1}` and a 32-px geometry.
2. The label is a literal `TextFace` (`text string {"Render scale"}`, font `hm_16`, `align 3`), or a `key` if an unused
   text-table key exists; a literal avoids the `rtext.rt2` file entirely (Quit and Fullscreen prove it).
3. The widget: an `OptionMenu` with N `Composite` choices (each a `TextFace` literal; a `dot` `ImageFace` for the
   selection), a `SliderBar` with `range`, or a static `TextFace` for a note. The row type decides the input: a 2-choice
   row costs ~15 padded lines of mproject text (the Fullscreen row is the model).
4. **`has_script` must be omitted** on a host row unless a script module exists: `has_script 1` makes the engine call
   `Pane::<NewRow>::OptionMenu` and a missing module is a script error (same rule that rejected deleting HiVision).
   Without it the widget still owns its index and popup drawing; the host reads the index from the widget object each
   VBlank and writes the feedback file.
5. **Focus:** the new child must be focusable and reachable by the VBox traversal. The existing row composites have no
   `can_focus` themselves; their `OptionMenu`/`SliderBar` are focusable by type. A new `OptionMenu` must be proven
   reachable (down/up from Wide) before any setting is wired; this is the first gate of the next slice. **U**
6. **Layout:** the `Pane` is 150 px tall with the 4R_* frame image; two more 32-px rows need the `Pane` geometry and
   `minimum_height` grown to 214, the `bg` `FrameImageFace` to `320x214`, and the `ToolTip` left at 438 (the page has
   room: 130 + 214 = 344 < 438). No script constant refers to the height. **C** (geometry) / **H** (packing).
7. **The stored-extent limit is not the blocker:** with `minify: true` the whole mproject (681,977 decoded, 22,449
   stored) compresses to 14,168 bytes, and one extra OptionMenu row (1,102 minified bytes) adds ~103 compressed bytes -
   8,178 bytes of headroom for the stored extent and 218 KB below the decoded size. The full data patch fits. **C**
   (measured with the builder's code). Minification changes every byte but not the token stream; TASK-212 already
   shipped a minified `arcade.mproject`.
8. **Host side:** a poller finds the new row's widget by a unique anchor (name or geometry) once, reads its
   `OptionMenu.index`/`SliderBar.index` field, debounces a change, applies the host value, writes
   `game-changes.toml`, and re-renders. This is the same scan/cache pattern as `menu_quit`, but read-only and page-gated
   (`"opti"` + the page's root vtable/geometry). It does not need to trap the pad: the engine moves the selection and
   the script is not involved. **H** (field offsets to derive from the state; the OptionMenu index is the same object
   the script reads, so its offset is discoverable by comparing two states).

### 12.3 A new page inside Options (script route) and the v5 gate

A **page** is not data: `OptionRoot.ad` builds `OptionPage(name, root, ...)` entries and the page list; opening one runs
`start_page(context, Root)` and the page module's `onInitialize`/`reflect`/`apply`/`onCancel`; the `Screen` tile in the
Options root is one of those page entries. So a new graphics page needs a script edit:

- a new `GraphicsRoot.ad` (or an extra module) with the row modules and the callbacks;
- a new `OptionPage` line in `OptionRoot.ad` and, if the tile list is a fixed layout, a new tile widget in
  `option.mproject` (the page tiles are `hidden::Item` cards in the Options project, `OptionRoot` is a `ScrollWindow`);
- a new page-table entry in the compiled `option.adc` (the name/root string pair) and the module names in the string
  table.

**The v5 toolchain.** The retail `option.adc` is Adhoc **v5** (magic `ADCH005`, 251,715 decoded / 55,353 stored); Spec II
is v7. OpenAdhoc and GTAdhocToolchain 1.4.0 list GT4 as "Adhoc Version 7 (v5-7 compatible)" and the compiler takes
`-v 5`, so a v5 **build** is supported by the toolchain. What is not proven is the **round trip**: decompile the retail
v5 bytecode, add a page, rebuild, and keep the exact string/page-table offsets the game's loader expects, inside the
55,353-byte stored extent (the same-length rule; a larger adc would need a new GT4.VOL TOC, which neither the builder nor
the release has). The repository has no v5 decompiler in its toolchain and the publisher's v5 source for this disc is not
public. Therefore: **script page extension is U, not scheduled**, and it must not be attempted by editing the adc
bytecode in place (the page table is an offset table; a shifted string breaks every page). The honest next step for that
route is a separate research task: a v5 adc reader/writer round trip on a scratch copy, with the file hash and size
unchanged, before any script edit.

**The accepted fallback (host-native page with the game's widgets).** If the v5 round trip fails, the page can be built
at runtime from the game's own widgets, reached from Options, not a floating overlay:

1. Ship the page as a hidden `Composite` template tree in `option.mproject` (data patch; the TextFaces/SliderBars are
   the game's own, so it looks native), attached to the Options root or to a hidden template list (the Quit pattern).
2. The host clones/attaches the template as a child of the Options page container when the player activates a trigger on
   the Options root (a pad combo on that page, or an extra invisible tile), and removes it on close. The engine draws
   the widgets; no GDI/font work.
3. The host owns focus and input for the page (it can reuse the page's VBox for traversal by setting the focus to the
   first row, or set the focus itself per press), and masks Cross/Circle while a host row is focused (the `menu_quit`
   mask) so a missing script module is never dispatched.
4. Values change by writing the widget's own `index` and forcing a redraw (a host value set on the widget), or by
   drawing the value text from the host into the existing `TextFace` (set `text` and dirty the widget). The first is
   preferred; it needs the index field and a redraw entry point. **U** (the redraw entry is not derived yet).
5. Close returns to the Options root by re-attaching the original tree. **U** (the same attach/detach bookkeeping).

Trade-off: the real GT4 look and the real Options entry, but the host owns the page's navigation, value drawing and the
open trigger, and it is a new 2-3 slice module (tree clone, focus, input mask, redraw, close). The data-only rows of
section 12.2 stay the cheaper path if the engine focus traversal reaches a `has_script`-less row; that is the first
thing to prove, because it removes the host navigation entirely (the engine focuses the row, the host only polls it).

### 12.4 First rows to add, live/restart and write-through

Ordered by value and risk, reusing the section 3 table. Every row is **restart-only except Fullscreen**; the game writes
only `game-changes.toml`, and the launcher merges it on exit/at the next start (section 11.3).

| # | Row (launcher id) | Row type | Values | Consumer / restart | Notes |
|---|---|---|---|---|---|
| 1 | Fullscreen (`fullscreen`) | done | Off/On | live (`set_fullscreen`) | Phase 1, this branch |
| 2 | Render scale (`render_scale`) | 2-4 choices (`100%`, `150%`, `200%`) | stored text `"100".."200"` | CoreGS `GT4_HW_SCALE`, GSdx `upscale_multiplier`; **restart** | the launcher resolves it against the output; a choice row matches the 2-4 `OptionMenu` cost |
| 3 | Renderer (`renderer`) | 2 choices (`GSdx`, `CoreGS 2.0`) | `pcsx2` / `coregs20` | launcher start path (`GT4_RENDERER`); **restart** | refuse `coregs20` on a PC without Vulkan (the launcher is the authority; the check exists) |
| 4 | Sun glare and light flares (`effect_flares`) | toggle | `true`/`false` | CoreGS `GT4_CORE2_PERF_FLARES`, GSdx `GT4_GS_HIDE_FLARES`; **restart** | shared by both renderers; the first effects row |
| 5 | Road sheen (`effect_road_sheen`) | toggle | `true`/`false` | CoreGS `GT4_CORE2_PERF_ROAD_SHEEN`; **restart** | CoreGS only |
| 6 | Small effects (`effect_targets`) | 2 choices (`Full`, `1x`) | `full`/`1x` | CoreGS `GT4_CORE2_EFFECTS_1X`; **restart** | CoreGS only |
| 7 | Blending (`blending`) | 2 choices (`High`, `Minimum`) | `high`/`minimum` | gt4-gs INI `accurate_blending_unit`; **restart** | GSdx only |
| 8 | Readbacks (`hw_download_mode`) | 2 choices (`Accurate`, `No readbacks`) | `accurate`/`no-readbacks` | gt4-gs INI `HWDownloadMode`; **restart** | GSdx only; the gt4-gs version check applies |

- **Restart notice:** the game has no notification UI for a native row, and the owner rejected a host overlay. The
  write-through file is the notice: the launcher already shows its own restart/notice messages; the merge logs
  `settings from the game: <id> = <value>` and the launcher's home can say "some settings apply the next time you play"
  (its own message, not the game's). A row label can also carry a short suffix (`Render scale*`) if the owner wants the
  in-game hint; the asterisk explanation would be one more static `TextFace` row. **H** design; the owner decides.
- A native row never writes the save's option struct; only `game-changes.toml` (host file) changes. The guest option
  fields stay `1`/`1` as in Phase 1. **C** (Phase 1 states).

### 12.5 What was implemented in this run and the smallest next step

Implemented: section 11 (HiVision removal, Wide -> Fullscreen live host row, feedback merge). Not implemented: any extra
row or page, because the first gate is not the file size (section 12.2 item 7 proves it fits) but the **engine focus
traversal into a `has_script`-less row**: if the VBox does not hand focus to it, the row cannot be used without the
host-owned navigation of 12.3, which is a different (bigger) slice.

Smallest next step (one slice, read-only until the headless frame): add **one inert row** to `Pane` (literal label
"Render scale", an `OptionMenu` without `has_script`, two literal choices), minify-patch the mproject, and answer three
questions on a headless frame: (a) does Down from `Wide` reach the row and does the popup open/select; (b) which object
field holds its index (compare two saves); (c) does the page still fit the `Pane` at 150/182 px. If (a) and (b) pass,
the host poller + the first real setting (`render_scale`) follows; if (a) fails, the route is 12.3's host-native page
(or a v5 script round trip), decided by the owner.

## 13. Resumo para o dono (pt-BR) - Phase 1 e o plano de páginas

- **HiVision saiu.** A página Options > General > Screen agora mostra só **Fullscreen**, **Vertical Adjust**,
  **Horizontal Adjust** e Cancel/OK, sem buraco e sem a linha inútil de 3 escolhas. É um patch de dados do seu disco
  (4 bytes da constante "NTSC" + os textos da linha), nada de script.
- **A linha Wide virou Fullscreen (Off/On).** O jogo pergunta ao host o valor (Off com o jogo em janela, On em tela
  cheia) e, quando você troca, o host guarda a escolha e muda a janela na hora - sem reiniciar. O campo do save e o
  registro 16:9 **não** são tocados: o save continua igual (comprovado nos dois estados).
- **O launcher fica sabendo.** O jogo grava um arquivo pequeno (`game-changes.toml`) e o launcher junta no
  `settings.toml` ao fechar o jogo (e na próxima abertura, se o launcher tiver caído). Alt+Enter/F11 também grava.
  Teste com janela fica com você.
- **Mais opções na página (seu pedido).** O plano está na seção 12: dá para **acrescentar linhas** na página Screen com
  um patch de dados (o arquivo comporta: sobra espaço de sobra depois de compactar; a linha nova usa os widgets do
  próprio jogo). O que falta provar primeiro é se o **foco do jogo chega** numa linha nova sem script; é o próximo
  passo pequeno (uma linha inerte e uma captura headless). Se não chegar, a alternativa é uma **página nativa montada
  com os widgets do jogo**, aberta pelo menu Options (não é painel flutuante), com o host cuidando do foco/entrada -
  mais trabalho (2-3 fatias).
- **Uma página nova de verdade** (um item novo no menu Options) exige editar o **script Adhoc v5** do `option.adc`. A
  ferramenta (OpenAdhoc/GTAdhocToolchain 1.4.0) compila v5, mas o **ciclo completo** (descompilar o arquivo original,
  acrescentar a página e recompilar mantendo os mesmos offsets, dentro do tamanho original) não está provado neste
  repositório; sem isso a página nova fica bloqueada e a rota acima (linhas na página existente / página montada pelo
  host) é a que vale.
- **Primeiras linhas sugeridas:** Render scale, Renderer, flares, road sheen, small effects, blending e readbacks -
  todas **valem no próximo início** (só Fullscreen é ao vivo). O aviso de "vale no próximo início" fica com o launcher,
  que já mostra esse tipo de aviso; o jogo não desenha notificação própria.


## 14. Phase 1b: the Screen rows and the Graphics menu decision (2026-11-09)

Branch `task/213-native-settings`, after Phase 1 (`85ef6e40`, accepted) and the test-record commit (`1c6b4bf9`).
Part A (Screen rows) and Part B (Graphics menu) are design/evidence here; no new game code shipped in this slice.

### 14.1 Owner acceptance of Phase 1

The owner ran `private\slice424-kit\play.cmd` with `--window`: Options > General > Screen shows no `HiVision`, the
`Wide` row is `Fullscreen` with `Off`/`On`, and the toggle changed the window live. The kit's `game-changes.toml` from
that run (19:24) says `fullscreen = false`. **C** (owner report + file). Recorded in TASKS, STATUS and the journal.

### 14.2 Screen page: Resolution (Vertical) and Monitor (Horizontal)

**What was done (data only, diagnostic):** a new private patch (`private/slice425-screen-labels`) relabels
`LabelVertical` -> literal `Resolution` and `LabelHorizontal` -> literal `Monitor` (plus the Phase 1 NTSC/Fullscreen
edits; 6 text replacements, 77,986-byte container, no minification). Headless frames
(`private/slice425-run/png/`): `frame_02300.png` shows the page with **Fullscreen Off / Resolution / Monitor**, the
values **0** and **0**; `frame_02450.png` (Resolution focused) and `frame_02750.png` (Monitor focused) keep the values
**0**, and the tooltips still say "Adjusts the vertical/horizontal position of the screen". **C**

**The exact value-display gate.** Each row's `SliderBar` has two TextFaces: the row label (key `LabelVertical` /
`LabelHorizontal`) and the slider's own value `label` (`{10,1,150,24}`), plus `popup.min`, `popup.max` and
`popup.pinch.value`. (Correction to §1.2/§9: the v2.00 `Vertical` slider range is **-20..50**, not -50..50; the
`Horizontal` range is -50..50.) The page script's `setupSlider` (v7 twin, `ScreenRoot.ad`; the v5 bytecode strings name
it too) runs on page open and after every slider change and writes **numbers** into all of them:
`slider["label"].text = slider.index * bias`, `popup["pinch"]["value"].text = ...`, `popup["min"/"max"].text = ...`.
The native `MOption.screen_adjust_v` (`0x001802F0`) / `screen_adjust_h` (`0x00180200`) getter returns a boxed **int**
(the same shape as `wide_mode`); the script formats it. Therefore a native override can change the *index* and the
stored host value but **cannot keep a resolution/monitor name on the slider**: the script rewrites the label to a number
on open and after each change. **C** (decomp + script source + frames).

**What the widget can do (the parts that exist):**
- `MTextFace.set_text` `0x00243BF0` (getter `get_text` `0x00243B08`, `get_value` `0x00243A68`): the value TextFace
  can display any string; the script itself writes a string in the `none` case of `setupSlider`.
- `MWidget.set_insensitive` `0x00256F30` (`get_insensitive` `0x00256E90`): the engine's real greyed/not-selectable
  state. The slider already carries the greyed art (`FrameImageFace "bgi"`, `visible_condition 8`,
  `visible_compare 8`), so setting insensitive gives the game's own greyed look, not a fake. The network page uses
  `.insensitive` the same way. **C** (data + natives) / **H** (the engine picks `bgi` from that state).
- The value TextFace is a child of the SliderBar, itself a child of the row `Composite` (v2.00 `MComposite` vtable
  `0x00662EF8`, row geometry `{0,52,320,32}` / `{0,84,320,32}`, slider `{150,4,160,24}`, value label
  `{10,1,150,24}`), so the host can find it with the same geometry scan as `menu_quit`. **C** (tree) / **H** (scan).

**The gate for the owner's target.** To show the monitor's modes and the monitor list on these rows the host must
overwrite the script-written texts *after* the script (a VBlank watcher), i.e. derive and prove the host text
injection: build a guest `String` (basic_string) and assign it to the value TextFace, or call `MTextFace.set_text`
with the reader/boxed-string path; then set `insensitive` when Fullscreen is Off and ignore the slider setter while
Off. The alternative is a v5 script edit of `setupSlider`/`onValueChanged` (blocked, 14.3). **H/U**. The smallest next
step is one headless proof of the host text injection on the value TextFace (no resolver logic yet); if that frame
shows a mode name, the two rows follow. **Do not ship a numeric list.**

**Lists and write-through (ready once the display gate is passed).** The game can enumerate the current monitor's
modes (`EnumDisplaySettingsW`) and the monitors (`EnumDisplayMonitors`/`GetMonitorInfoW`); the launcher's stored forms
are `resolution = "WxH"` or `desktop`, and `monitor = "<device>@<WxH>"` or `primary`
(`launcher/crates/gt4l-core/src/settings.rs`, `display.rs`). Both are **restart** values (the mapping §3.1/§4.3:
`GT4_OUTPUT_SIZE` is read once and the display plan/render scale is built from it; a live window resize is not a
settings change). The rows write `game-changes.toml` (`resolution`, `monitor`), the launcher validates and merges
(`game_changes` -> `set_by_user`), and the guest `option + 0x18` (h) / `+0x1C` (v) fields and the save stay untouched by
hooking `screen_adjust_v/h` and skipping the native. Greyed state: `insensitive` while Fullscreen is Off; the setter is
ignored then. **C** for the validators/merge, **H** for the rows.

### 14.3 The Graphics menu: route decision

The owner wants a new **Graphics** menu inside Options, not a floating panel, carrying renderer, render scale, effects,
GSdx blending/readbacks, frame rate and HUD with restart notices.

**Route 1 - Adhoc v5 script/TOC: blocked.** GTAdhocToolchain (MIT) is present as **source only** in
`private/slice151-research/nenkai/GTAdhocToolchain`: its CLI has `build`, `disassembly-repl`, `pack`/`unpack`,
`mproject-to-bin`/`-to-text`; it supports bytecode **v5** compilation and a **disassembler**, but there is **no
assembler from its disassembly** and **no `.ad` source for the retail v5 option project** (OpenAdhoc's GT4 `option`
project is v7; OpenAdhoc is GPL-3.0 and is a reference only). The page list is script data (`OptionRoot.ad`'s
`options` array and `start_page`), so a new page is a script edit. A v5 rebuild changes the `option.adc` layout and
size, which needs a **new GT4.VOL TOC**; `scripts/gt4_data_patch.py` only does same-length replacements and refuses
growth, and no TOC writer exists. The owner would also have to accept a GPL build tool in the private flow. **C** for
the tool/commands/licence, **C** for the TOC gate. Smallest step if this route is chosen: build the MIT CLI, test
`build -v 5` + disassembly on a scratch copy, then write a TOC writer; weeks, not a slice.

**Route 2 - host-native page with the game's widgets: feasible pieces, unproven page.** The engine exposes
`MRenderContext.startPage` `0x0022BC20` / `getCurrentPage` `0x0022BE00` / `closePage` `0x0022BD98`,
`MComposite.appendChild` `0x00205420` / `insertChild` `0x00205530` / `getChild` `0x00205980`,
`MTextFace.set_text` `0x00243BF0`, `MWidget.set_insensitive` `0x00256F30`. The TASK-212 Quit row proves a host-parsed
widget tree renders and can be appended; the menu_quit mask proves host-owned input. What is **not** proven: building a
valid page root/vtable and render context, attaching it to the Options root (the `OptionRoot` ScrollWindow and its
`hidden::Item` tiles are script-filled), the focus list (the script's `setFocus`/page modules), the back transition and
the restart-notice text drawing. That is the 2-3 slice module of §12.3; the same host text/insensitive/scan gate as
14.2 is its first step. **C** for the functions; **U** for the page.

**Candidate 3 - repurpose an existing page (Network):** the Options root has a reachable `Network` tile (page table
`0x8EFA`; `NetRoot` at `0x8EFA`), so a data-only "Graphics" page could be the existing Network page with its tile/rows
relabelled and its natives overridden. The v7 `NetRoot.ad` shows 10 network rows (`AutoConnect`, `ConnectIdx`,
`UseProxy`, `ProxyAddr`, `ProxyPort`, `UseUPnP`, `UDPBindPortSetting`, `UDPBindPort`, `EntranceAddr`, `EntrancePort`)
with `reflect`/`apply`/`sensitive` and `setNeedSave`; repurposing means hiding/overriding all of them and relabelling
the tile, and it removes the Network page (likely unused in the recomp, but an owner decision). **H/U**; not
implemented.

**Decision for this slice:** no shell. The route with the least licence/toolchain risk is the host-native page using
the game's widgets, but its first gate is the host text/insensitive injection, which is also the gate for the Screen
rows. The smallest next step is the same for both: one headless proof that the host can set a value TextFace string and
the insensitive state on the live Screen page; then the Resolution/Monitor rows and, with the clone/attach proof, the
Graphics page shell.

### 14.4 Graphics page content (ordered, restart rules, write-through)

From §12.4 and §3: renderer `renderer` (GSdx/CoreGS 2.0, restart), render scale `render_scale` (`100/150/200`, restart),
effects `effect_flares` (restart, shared by both renderers), `effect_road_sheen` (restart, CoreGS), `effect_targets`
(restart, CoreGS), GSdx blending `blending` (restart), readbacks `hw_download_mode` (restart), frame rate `frame_rate`
(30/60, restart, frame skipping only), performance HUD `performance_hud` (restart). Every row writes `game-changes.toml`
only; the launcher merges (`game_changes` -> `set_by_user`/`set_gs`) and stays the only writer of `settings.toml`. A
restart row must show "applies at next start": the game has no notification UI, so the row's own value/label text (or a
static note row) carries it; the launcher can also show its message after the merge. No host value enters the save.

### 14.5 Evidence

- Diagnostic patch: `private/slice425-screen-labels/patch/gt4-data.patch` (container
  `bd3f41d9b99c13763217211934a56165cb15f3c9f0a8cbceb5c0cc9903f7f415`, 77,986 bytes; report in the same folder).
- Frames: `private/slice425-run/png/frame_02300.png` (Resolution/Monitor labels, values 0, Fullscreen Off),
  `frame_02450.png` (Resolution focused, stale tooltip), `frame_02750.png` (Monitor focused). Log
  `private/slice425-run/run.log` (`[data-patch] ... verified against the image`, no errors), state
  `private/slice425-run/state.bin` (wide 1, screen_adjust_h 0, screen_adjust_v 0, project `"opti"`).
- Route evidence: GTAdhocToolchain `Program.cs` commands and README (v5 compile + disassembler), OpenAdhoc LICENSE
  (GPL-3.0), `gt4_data_patch.py` growth refusal, the `MRenderContext`/`MComposite`/`MTextFace`/`MWidget` natives above.


## 15. Resumo para o dono (pt-BR) - Phase 1b

- **A Phase 1 foi aceita por você** (teste com janela): a página Tela não tem HiVision, a linha Wide é **Fullscreen
  Off/On** e a troca mudou a janela ao vivo. O `game-changes.toml` do kit registrou a mudança. Obrigado!
- **Resolution / Monitor (linhas Vertical/Horizontal):** os rótulos já podem virar **Resolution** e **Monitor** por
  patch de dados (provado numa captura headless: a página mostra Fullscreen Off, Resolution 0, Monitor 0). O problema
  é o **valor**: o script da página reescreve o texto do slider com um **número** (`slider.index`) ao abrir e a cada
  mudança; o controle nativo devolve um inteiro, não um texto. O widget em si aceita texto e tem o estado
  "insensitive" (cinza/desabilitado) do próprio jogo, mas para mostrar o nome da resolução/monitor o host precisa
  escrever no texto do widget **depois** do script (ou editar o script v5, que está bloqueado). Isso é o próximo passo
  pequeno: provar a escrita de texto do host numa captura headless; depois as duas linhas com as listas do monitor e o
  cinza quando Fullscreen está Off. **Não foi entregue uma lista falsa num slider numérico.**
- **Menu Graphics:** a rota por script **v5** está bloqueada: a ferramenta GTAdhocToolchain é MIT e está só como
  código-fonte (compila v5 e desmonta, mas não remonta o desmonte, não há o código-fonte v5 do projeto original e não
  existe escritor do TOC do GT4.VOL; o patch de dados recusa crescimento). A rota de **página nativa com os widgets do
  jogo** tem as funções do motor mapeadas (iniciar página, anexar widget, texto, insensitive), mas ainda não foi
  provada; é o mesmo portão da escrita de texto. A terceira ideia, reaproveitar a página **Network** como Graphics, é
  possível por dados mas mexe na página de rede e precisa da sua decisão. Nada disso foi inventado como pronto.
- **Conteúdo do menu Graphics (quando a rota fechar):** renderizador, escala de render, flares, road sheen, small
  effects, blending/readbacks, 30/60 FPS e HUD; tudo **vale no próximo início** (só Fullscreen é ao vivo); o jogo grava
  `game-changes.toml`, o launcher valida e junta, e nada entra no save.


## 16. Phase 1c: host text and insensitive on the Screen rows (2026-11-09, implemented; the slider rows were replaced by OptionMenus and Resolution became live in Phase 1d, section 20)

Branch `task/213-native-settings`, after Phase 1b. The owner accepted Phase 1. This slice proves the text injection and
the insensitive state on the game's own slider rows, and ships the **Resolution** and **Monitor** controls.

### 16.1 What was built

- **Data patch (private, `private/slice426-screen-rows`):** the Phase 1 edits, plus the `Vertical`/`Horizontal` labels
  -> literal `Resolution`/`Monitor`, the two stale tooltips off, the Vertical slider range `-20..50` -> `0..31` and
  Horizontal `-50..50` -> `0..7`. 10 text replacements and the `"NTSC"` byte edit; container
  `1818f9622d1f34eb15a09b12df1b4e42f8ef58f9aeb35d5f0cd92ecef2aa50f8`, 77,986 bytes, no growth.
- **`native/screen_rows.*`** (in the runtime, capsule `slice426-screen-rows-license-guard-runtime`, pin
  `docs/inputs/slice426-screen-rows-license-guard-runtime.json`):
  - two dispatch hooks for `MOption.screen_adjust_v` `0x001802F0` and `screen_adjust_h` `0x00180200`, same shape as
    `wide_mode`: the getter boxes the host list index, the setter reads the slider `.index`, clamps it to the list, stores
    it, writes `game-changes.toml` and **skips the native**;
  - a per-frame service at the main loop render functor (`0x00100F50`), which paints the slider's value TextFace, the
    popup pinch/min/max and sets the engine's insensitive state.
- **Lists:** `EnumDisplaySettingsW` for the current monitor's modes (29 on the test machine, current 1920x1080 from
  `GT4_OUTPUT_SIZE`), `EnumDisplayMonitors` for the monitors (1 here: `\\.\DISPLAY1`, desktop 3440x1440). The labels are
  the mode `WxH` and the monitor `index: short name`; the feedback uses the launcher's stored forms
  (`resolution = "WxH"`, `monitor = "<device>@<WxH>"`). Both are **restart** values (`GT4_OUTPUT_SIZE`/`GT4_MONITOR` are
  read once); the launcher merges the feedback on exit/at the next start. The game shows no notice of its own.
- **Greyed/not selectable:** the engine's insensitive state. Fullscreen Off: the sliders take the engine's greyed `bgi`
  art (measured background 97,97,101 vs 131,131,137 active) and a Down press **skips both rows to the OK button**; the
  page closes through the button (frame `off-2500-...`). Fullscreen On: Down focuses Resolution, the popup opens and the
  value changes (`on-2500/2600/2650`), with `resolution = "1920x1200"` written to the feedback file.

### 16.2 The text path (exact)

- The slider's value is a normal `mTextFace` (vtable `0x00666370`). Its `+0x104` points to an `mLocalizedText`; **+0x8 is
  the key** (what `get_key` returns) and **+0xC is the display String** (what `func_002B71D0`/`get_text` reads). The page
  script's `setupSlider` writes a **number** into the +0xC string on open and after every change; the native
  `screen_adjust_v/h` getter is an `hInt`.
- **Proof of injection:** the host builds guest `basic_string`s with the engine's own `func_005C2630` (seeded from the
  current text String) and points the TextFace's `+0xC` slot at them with a reference increment. The renderer reads the
  slot at every draw, so the names appear. One string per list entry is built **once** when the page is first found
  (29 mode names + 1 monitor name + `0`/list max), and the per-frame service is a **memory pointer swap only**.
- **The failure that was fixed:** building a new string **on the render frame** (when the script had just repainted its
  number) crashed with `0xC0000005` inside `ps2::kernel::handle_syscall` at the allocator lock; the old string was also
  freed while the TextFace's render cache still pointed at it. The shipped design (prebuilt strings, no free of the
  replaced script strings) has no guest calls on the render frame and does not crash (two runs, exit 0).
- The popup `min`/`max` boxes show the list bounds (`0`/`28`); the pinch shows the current name; the row labels are the
  data-patch literals. The old text strings are intentionally leaked (a handful per page visit) because the engine's
  layout cache can still reference them; the prebuilt strings are held by the host cache.

### 16.3 The insensitive path (exact)

- `mWidget`'s state word at **+0x98**: **bit 3 (`0x8`)** is the insensitive flag (`func_002661D8` sets the low nibble's
  8 and preserves bits 0-2). Bit 13 (`func_00265D98`) is a **different** flag; the first implementation used it and the
  rows stayed selectable and un-greyed. The host sets/clears bit 3 on the SliderBar and, recursively, on its composite
  children (the greyed `bgi` frame is a child). The engine's focus traversal skips the insensitive sliders.
- Fullscreen Off/On drives the state through `native_settings::host_fullscreen()`; the row names stay visible while
  greyed. The row label text itself stays black (the control is greyed); the value text is the same TextFace.

### 16.4 Acceptance evidence (headless, v2.00)

| Check | Evidence |
|---|---|
| Names on the rows | `private/slice426-kit/frames/off-2300-greyed-names.png` (Resolution 1920x1080, Monitor 1: DISPLAY1), `on-2300-active-names.png` |
| Greyed, not selectable | `off-2500-focus-skips-to-ok.png` (Down from Fullscreen lands on OK); slider background 97 vs 131 active; log `row greyed insensitive=1` |
| Active | `on-2500-resolution-focused.png` (Down focuses Resolution), `on-2600-resolution-popup.png`, `on-2650-resolution-changed.png` (1920x1200) |
| Feedback | `game-changes-on.toml`: `version = 1`, `[changes] resolution = "1920x1200"`; no file in the Off run (no change) |
| Save untouched | `state-off.bin`/`state-on.bin`: wide 1, `screen_adjust_h/v` **0**, `"opti"` |
| Exit | both runs exit 0; no `[mem]` unmapped reads, no crash |

### 16.5 Risks and unknowns

- **One monitor on this machine:** the Monitor row lists and paints its single entry and the feedback code is the same as
  Resolution, but a monitor change could not be exercised headless. A two-monitor owner test is the gate.
- **String cache leak:** replaced strings are not freed (the engine's layout cache can reference them). A handful of
  short strings per page visit; bounded by the session. A proper fix needs the cache invalidation path (the earlier
  `func_00245270` call with the TextFace made the engine read a null localized pointer; the correct handle is not
  derived).
- **Prebuild timing:** the strings are built on the first render frame the page is seen; if that frame's guest context
  ever holds the allocator lock, the build could still race. The two runs (Off/On) built 30 strings successfully at
  frame 1663.
- **Monitor label width:** long friendly names are replaced by the short device (`DISPLAY1`); the full name is not shown.
- **Restart notice:** the game has no notice; the launcher's message is the only one. The row value changes immediately,
  but the running game keeps its output size until the next start.
- **Spec II:** the addresses (`screen_adjust_v/h`, vtable `0x00666370`, bit 3 at +0x98, the string helpers) are v2.00;
  Spec II is Phase 2.
- **The `GT4_SCREEN_ROWS_TEXT`/`GT4_SCREEN_ROWS_INSENSITIVE` debug switches** exist for the isolation runs; both default
  on and are not sent by the launcher.
- The slider range is 0..31 / 0..7; the list length can be smaller (the host clamps and paints the bounds). A mode count
  above 32 or a monitor list above 8 would clamp; typical monitors are below both.

### 16.6 Build/kit

- Capsule `private/slice426-screen-rows-license-guard-runtime` (receipt `7bb369a...`), pin
  `docs/inputs/slice426-screen-rows-license-guard-runtime.json`.
- Test build in `build/slice424-native-settings-build` (the existing build dir; the capsule changed):
  `gt4vulkan.exe` 90,227,200 bytes, SHA-256 `5a91309aee6ad31a2c2079e402cb4cb112fd4389e3f7872f1fae1f5da2835ffd`.
- Kit `private/slice426-kit` (exe, SDL3, ELF, card, patch/recipe/report, both states/logs, the On feedback, the six
  frames, README). Test only.
- Targeted tests: `native_native_settings` 1/1 (the multi-key feedback writer added), `cargo test -p gt4l-core --lib`
  310 passed / 0 failed (the two resolution/monitor merge tests), the two headless runs exit 0.

## 17. Resumo para o dono (pt-BR) - Phase 1c

- **Resolution e Monitor estão funcionando na página Tela.** As linhas mostram o nome da resolução atual
  (ex.: **1920x1080**) e o monitor (ex.: **1: DISPLAY1**), com as listas tiradas do Windows (29 modos e 1 monitor nesta
  máquina). O texto do jogo foi provado: o host escreve o nome no próprio campo do jogo e o script não consegue mais
  deixar o número na tela.
- **Fullscreen Off:** as duas linhas ficam **cinzas e não selecionáveis** (o cursor pula direto para OK); Fullscreen On:
  elas voltam e podem ser mudadas com o controle. A troca grava o `game-changes.toml` e **vale no próximo início**
  (o jogo não muda a janela na hora); o launcher junta no `settings.toml` ao fechar/abrir.
- **O save não muda:** os campos internos de ajuste de tela continuam 0; o valor do host fica só no arquivo do launcher.
- **Teste com dois monitores** é o que falta para o seu caso de escolher o monitor; nesta máquina só há um. O resto
  (nomes, listas, cinza, não selecionável, arquivo de aviso) está provado em capturas headless.


## 18. Phase 2a: host-native page, route and gate (2026-11-09, design; not proven)

The owner chose the **host-native page with the game's own widgets, reached from Options** and excluded the Adhoc v5/TOC
route and the Network-page repurpose. The target is an empty page with the game's row style, correct focus, input and
back transition, reached from the Options root, then renderer/render-scale rows.

**What the engine gives (Confirmed, code/data).**

- The page transition is **script-event-driven**. `share/page_util.ad`'s `start_page(context, next_page)` pushes an
  `MFunctionEvent(_start_page, context, next_page)`; `_start_page` runs `context.transition.panOutIn(context)`,
  `setCursor(context, "load")` (the `CursorRoot` common page), `context.startPage(next_page)`, restores the cursor and
  calls `context.flushKeyEvent()`. Back is `_start_page(context, nil)` -> transition out, `context.finish()`.
- The natives behind it: `MRenderContext.startPage` `0x0022BC20`, `closePage` `0x0022FA08` (class method) /
  `0x0022BD98` (script), `getCurrentPage` `0x0022BE00`, `pushPage` `0x0022BCE0`, `pushEvent` `0x0022B9F0`,
  `finish` `0x0022BEA0`, `sync` `0x0022C138`; the script `root.setFocus(widget)` and `MComposite` child helpers
  (`appendChild` `0x00205420`, `insertChild` `0x00205530`).
- The live objects in the Screen state: one `mRenderContextPS2` at **`0x01021FF0`** (vtable `0x00663BE0`); 26
  `mRootWindow` page roots (vtable `0x00665418`), including the ScreenRoot at **`0x00F77200`** (its `Pane` VBox is
  `0x00F776C0`); the Options root is the `mScrollWindow` (vtable `0x006704A8`) at **`0x01FDA8A0`** (parent
  `0x01FD08F0`), with the other two ScrollWindows at `0x00F3EC70`/`0x01FCDA30`. The hidden `MonitorRoot` tree (its
  `TitleMonitor`/`monitor position`/`num of monitor` widgets) exists but is not in the v2.00 page list. The page list
  and the page modules are script data in `option.adc`; `OptionRoot.ad`'s `options` array and the tiles are the list.
- A page root is a real `mRootWindow`; the ScreenRoot page already proves the engine renders a root with the game's
  widgets (that is how the current Screen page is drawn).

**The gate (Unknown, not proven).** `startPage` and `closePage` are **script natives**: their arguments are script
`hValue` handles for the context and the page root (`getCurrentPage` and `startPage` use the RefCounter handle
conversion `func_0022AD20`/`func_0022FB50`/`func_00232BF0`), not the raw engine objects listed above. The handle/wrapper
relation (which RefCounter object wraps `mRenderContextPS2` `0x01021FF0`, and which wraps a page root such as the hidden
`MonitorRoot` `mRootWindow`) is **not derived**. Passing the engine objects directly would be a guess and can crash. *Resolved in section 22: the "handle" is the raw engine object and the script passes the address of a slot that holds it; the probe call returns, but the hidden page is not a registered page and stalls the loop.*
The focus, the page stack, the cursor widget and the back transition are also script state (`MFunctionEvent`,
`context.transition`, `root.setFocus`, `last_focus`); a host-started page bypasses `onInitialize`/`reflect`/`apply` and
would have to own focus, input and back itself.

**Smallest next step (one bounded experiment, no code shipped until it passes).**

1. Find the **context handle**: scan RAM for a RefCounter/hValue object whose payload points at `0x01021FF0` (the
   `mRenderContextPS2`), or read the handle the widget's `get_context` native (`0x00256730`) returns; confirm by
   comparing the handle to the script's context (the page scripts receive it).
2. Find the **page-root handle**: the same scan for the hidden `MonitorRoot` root (an `mRootWindow`, not in the page
   list) or a host-built root; the existing hidden tree is the cheapest empty shell.
3. Call `MRenderContext.startPage` (`0x0022BC20`) from a dispatch hook with the two handles (same convention as the
   Wide row: a0 = return slot, a1 = context, a2 = 1, a3 = argv with the page root), or push the equivalent
   `MFunctionEvent`; capture a headless frame. If the MonitorRoot tree renders, the page root/vtable and the attach are
   proven; then `startPage(context, OptionRoot_handle)` (or `closePage`) for the back, and the host pad mask/focus for
   input.
4. If the handles cannot be derived or the call does not render, the route is blocked; the fallback (a host tree
   attached inside the Options root with host focus/input, the Quit-row pattern) is **not a new page** and needs the
   owner's decision; no floating overlay.

No page was implemented or faked in this slice; the only Phase 2a work here is the route evidence and the gate. The
Page 1c rows are complete and committed.

## 19. Resumo para o dono (pt-BR) - Phase 2a

- A página nativa com os widgets do jogo **ainda não foi provada**: o motor abre páginas por **eventos de script**
  (`start_page` -> `context.startPage` com a transição, o cursor e o foco), e a chamada nativa precisa de **handles de
  script** para o contexto e para a raiz da página. Os objetos do motor existem na RAM (contexto `0x01021FF0`, raízes
  `mRootWindow`, a raiz do Options), mas a relação handle/objeto **não foi derivada**; chamar com os objetos crus seria
  um chute e pode travar.
- **Próximo passo pequeno:** achar o handle do contexto e o handle de uma raiz existente (a árvore escondida
  `MonitorRoot` serve de página vazia), chamar `startPage` e capturar um frame headless. Se renderizar, a página existe;
  depois vêm o foco, a entrada e o voltar. Se não, a rota está bloqueada e a alternativa (uma árvore do host dentro da
  raiz do Options, sem sobreposição flutuante) precisa da sua decisão.
- Nada foi falsificado: nenhuma página foi inventada neste passo.

## 20. Phase 1d: the Screen rows as OptionMenus and a live Resolution (2026-11-10, implemented, test build)

The owner tested the Phase 1c kit: the labels/values worked, but confirming Resolution changed only the row and the
feedback file, never the screen; and the sliders were the wrong widget for both rows. This slice fixes both.

### 20.1 Why the Phase 1c row did not change the screen (Confirmed)

- The launcher's **Resolution** is an **output size** (`GT4_OUTPUT_SIZE`, read once at start in
  `display_aspect.cpp:490` and `window.cpp`), not a display mode. In windowed mode it is the client area; in fullscreen
  the borderless window always covers the monitor (`set_fullscreen` uses the monitor `rcMonitor`, not the output size),
  so the monitor stayed at its desktop mode. The value only fed the aspect ratio and the launcher's internal GS scale
  (`GT4_HW_SCALE`, fixed when the renderer is constructed).
- The Phase 1c row wrote `game-changes.toml` and was restart-only; the kit launches `gt4vulkan.exe` directly, so no
  launcher merged the file, and even through the launcher `GT4_OUTPUT_SIZE` is a start value. Hence the row value
  changed while the screen did not.
- The three candidate behaviours, decided from the code and the launcher's own model:
  - **borderless window resize in fullscreen:** the window covers the monitor; shrinking it would reveal the desktop,
    not change the screen. Not correct for fullscreen.
  - **render-scale change:** changes the internal GS multiplier only; CoreGS 1.1 sets `GT4_HW_SCALE` at construction and
    GSdx is a separate process with its INI read at start. Not live today, and not the row's stored meaning.
  - **true display-mode change:** the only way the actual monitor resolution changes while fullscreen. Chosen.

### 20.2 Implemented behaviour (windowed and fullscreen)

- **Fullscreen:** `window.cpp::apply_display_mode` enumerates the current monitor's modes (`EnumDisplaySettingsW`,
  preferring the current refresh rate), saves the original `DEVMODE` once, and calls
  `ChangeDisplaySettingsExW(device, &mode, nullptr, CDS_FULLSCREEN, nullptr)`. The borderless window is then set to the
  new `rcMonitor`; `WM_SIZE` rebuilds the swapchain and the GS child follows with its resize. The original desktop mode
  is restored by `restore_display_mode` when fullscreen is left (Alt+Enter/F11 or the Fullscreen row), on `WM_DESTROY`
  and in the cleanup path. A refused mode (`DISP_CHANGE_*`) is logged and the current mode is kept.
- **Start:** a fullscreen start applies `GT4_OUTPUT_SIZE` to the monitor (so the Phase 1c kit's 1920x1080 row now
  matches a real 1920x1080 screen). "Desktop resolution" is the current mode and is a no-op; an unsupported size is
  logged and the desktop mode stays.
- **Windowed:** `apply_output_size` computes the frame with `AdjustWindowRectEx` and calls `SetWindowPos` for the chosen
  client area, centred on the current monitor. The row is greyed while Fullscreen is Off (the owner's rule), so this
  path is implemented but not reachable from the page yet.
- **Aspect:** `display_aspect::set_live_output_size` stores the chosen size; `current_ratio` prefers it over the start
  `GT4_OUTPUT_SIZE` ratio (a fixed `GT4_DISPLAY_ASPECT` still wins). The next race camera and the HUD parameter block
  use the new ratio; the menu page keeps its layout.
- **Feedback:** the row still writes `resolution = "WxH"` and the launcher merges it; the guest save is untouched (the
  hidden sliders' setters are skipped).

### 20.3 The dropdown rows (data-only, no script)

- The page script owns the stock `Vertical`/`Horizontal` `SliderBar` rows (`setupSlider` writes their numeric value and
  popup), so the row type cannot be changed under the same module. The data-only route of section 12.2 was used:
  - the ScreenRoot `Pane` stays `320x150`: the hidden stock sliders leave the pack and the two new rows replace them (20 + 3*32 + 34 = 150);
  - `Vertical` and `Horizontal` get `visible digit {0}` (their script modules still exist and still run; the engine
    skips them in the VBox packing and focus traversal);
  - two new `Composite` rows with literal labels (`Resolution`, `Monitor`) and an `OptionMenu` each are inserted before
    `HiVision`, with **no `has_script`** (the engine still owns the index, popup, focus and drawing; no script module is
    called);
  - the Resolution popup has the 29 modes of `\\.\DISPLAY1` as `Composite` choices with literal `TextFace`s (the first
    is 1920x1080, the kit's start, so the closed row is right at page open); the Monitor popup has the connected
    monitor(s), here `1: DISPLAY1`;
  - the private patch is minified (`minify: true`), stored extent 22,449 bytes / decoded 681,977 bytes; container
    `4d1068828fc9976e5e9bdaef732f22a0b47482373ea6d373c303b769d17f6344`, 77,986 bytes.
- The engine's `mOptionMenu` keeps the selected index at **`+0xD8`**, the count at `+0xE0` and the popup-open flag at
  **`+0xFC`** (`func_002C9098` sets `fc=1` when the popup opens, `func_002C93C0` clears it; `func_002C98F0` scrolls
  the popup when the selected item leaves the window). The host reads these at the render functor `0x00100F50`, and
  applies only on the closed transition (Cross confirm; Circle restores the pre-open index via `f100`, so nothing is
  applied). The popup itself scrolls for the 29-item list (frame evidence below).
- **No host text injection on these rows:** the engine's closed-label clone shares the localized text object with the
  popup choice; a pointer swap to a host-built string crashed the engine (`no translated code at 0x00617D04`). The data
  literals are the game's own text; the host text path from Phase 1c remains proven but is not needed here.
- **Insensitive:** the host sets `mWidget +0x98` bit 3 on the two OptionMenus and their children while Fullscreen is
  Off; the engine greys the `bgi` art and the focus traversal skips the rows (Down lands on OK).
- **Controller path:** the engine's own focus traversal + `mOptionMenu` key handler; the host does not trap the pad. The
  hidden sliders' `screen_adjust_v/h` natives still return 0 and skip the setter, so the guest fields and the save stay
  0 (`state-on.bin`/`state-off.bin`: `screen_adjust_v/h` 0, wide 1, project `opti`).

### 20.4 Evidence (headless, v2.00)

| Check | Evidence |
|---|---|
| Initial row | `private/slice430-kit/frames/on-2300-initial-1920x1080.png` (Fullscreen On, Resolution 1920x1080, Monitor 1: DISPLAY1) |
| Popup and confirm | `on-2800-popup-1280x720.png`, `on-2950-selected-1280x720.png`; log `resolution = "1280x720"` |
| Long list scroll | `scroll-3500-popup-3024x1296.png`, `scroll-3600-selected-3024x1296.png`; log `resolution = "3024x1296"` |
| Greyed + skip | `off-2300-greyed.png`, `off-2500-focus-ok.png` (Down lands on OK); log `greyed row insensitive=1` |
| Save untouched | states: `screen_adjust_v/h` 0, wide 1, `opti` |
| Exit | the three runs exit 0; no unmapped reads, no crash |

- Tests: `native_native_settings` 1/1 (the output-request and window-present checks), `native_display_aspect` 1/1 (the
  live output size overrides the start ratio), `cargo test -p gt4l-core --lib` 310 passed, `test_gt4_data_patch.py` 16
  OK, `test_license_guard.py` 21 OK.
- Kit `private/slice430-kit`: exe `d0a75027...` (90,234,880 bytes), patch `4d106882...`, states/logs/feedback/frames/
  README. Test build only.

### 20.5 Risks and unknowns

- **The display-mode change is global and not testable headless.** It changes the desktop for every window; the game
  restores the original on fullscreen Off/exit, but a crash or kill leaves the chosen mode. The owner's windowed test is
  the gate; a refused mode is logged and the current mode is kept.
- **Fixed mode list:** the patch lists `\\.\DISPLAY1`'s 29 modes at build time. A different monitor/driver or the agent's
  remote `\\.\DISPLAY22` leaves stale names; the window enumerates the real monitor for the apply and refuses a mode it
  does not support. A product build needs a per-machine list or a script setter for the OptionMenu text (the shared
  text object makes a pointer swap unsafe).
- **First closed-row value:** the engine fills the closed row from the first choice at page build; the patch orders
  1920x1080 first (the kit start). A start at another resolution shows 1920x1080 until a choice is confirmed; fixed-data
  limitation.
- **Monitor is restart-only:** the row lists the connected monitors (1 here) and writes `monitor = "<device>@<WxH>"`;
  moving a running window is not implemented.
- **GS internal scale** stays at the start `GT4_HW_SCALE`; Resolution changes the output (window/display) size and the
  aspect, not the internal multiplier. A lower mode supersamples, a higher one upscales.
- **Aspect timing:** the new ratio applies at the next race camera init; the menu page keeps its layout.
- **Fullscreen start changes the desktop mode** from `GT4_OUTPUT_SIZE` (the Phase 1c kit did not). This is the fix; the
  normal exit restores it.
- **Spec II** addresses remain Phase 2; this slice is v2.00 only.

## 21. Resumo para o dono (pt-BR) - Phase 1d

- **Por que a Resolution não mudava a tela:** o valor era só um "tamanho de saída" lido no início; em tela cheia a
  janela cobre o monitor inteiro, então o monitor continuava no modo do Windows. O arquivo `game-changes.toml` só valia
  no próximo início, e o kit abre o jogo direto (sem launcher para juntar).
- **Agora, em tela cheia, a Resolution muda o monitor de verdade:** `ChangeDisplaySettingsExW` troca o modo do monitor e
  a janela cobre o novo modo; o modo original é restaurado ao sair da tela cheia ou fechar o jogo. No início, o
  `GT4_OUTPUT_SIZE` também é aplicado (o kit começa em 1920x1080; para começar nativo, use 3440x1440). Em janela, o
  caminho redimensiona a janela, mas a linha continua cinza com Fullscreen Off (sua regra).
- **As duas linhas agora são os menus suspensos do próprio jogo** (o mesmo widget da Fullscreen), com a lista de 29
  modos e o monitor; a lista rola quando é longa. O controle funciona pelo foco do motor do jogo; o host só lê a escolha
  quando o menu fecha e grava o `game-changes.toml`. O save não muda (os campos internos continuam 0).
- **O que testar:** tela cheia, Options > General > Screen, abrir Resolution, escolher 3440x1440 e confirmar; o monitor
  deve mudar na hora. Escolher 1920x1080 volta. Fechar o jogo restaura o desktop. Com Fullscreen Off as linhas ficam
  cinzas e o cursor pula para OK.
- **Riscos:** a troca de modo é global (se o jogo for morto à força, o desktop fica no modo escolhido); a lista de modos
  é fixa para o monitor desta máquina; o monitor é só reinício; e a escala interna do GS continua a do início. O teste
  na sua janela é o que falta para fechar.

## 22. Phase 2a probe: the hValue relation and the hidden-page gate (2026-11-10, evidence; page not reachable)

This section resolves the Phase 2a gate with a live dispatch probe. The probe is gated by `GT4_PAGE_PROBE` (log the
game's own `startPage` arguments) and `GT4_PAGE_LAUNCH_FRAME` (call `startPage` with the derived handles for the hidden
`MonitorRoot` at one frame). A normal run has neither variable set; the native and the game are unchanged.

### 22.1 The handle relation (Confirmed)

The Phase 2a report assumed the `startPage`/`closePage` arguments were script `hValue` wrappers and that the
wrapper-to-object relation was unknown. The probe and the disassembly show the opposite: **the "handle" is the raw
engine object itself**, and the script argument is the **address of a 4-byte slot holding that object pointer**.

- `MRenderContext.startPage` `0x0022BC20` (disassembly):
  - `a1` = the address of the context slot; `*a1` = the raw `mRenderContextPS2` object (vtable `0x00663BE0`).
  - `a2` = argc (1); `a3` = the argv array of page-root pointers; `*a3` = the raw `mRootWindow` object
    (vtable `0x00665418`).
  - `0x22BC48 jal 0x22AD20` converts the context slot to the raw context (a gcc `__dynamic_cast` through
    `GT4MC__FileGT4GameData__loadInstance` + `RefCounter` retain); `0x22BC80 jal 0x232C78` converts each page-root slot
    to the raw `mRootWindow`; `0x22F890` is the raw `mRenderContext::startPage(context, page_root)`.
- The raw method checks `func_00265F38(page_root)` (bit 20 of `mWidget+0x98`), calls the page root's vtable `+0x2D0`
  initialization, and inserts the page root into the context's page list at `context+0x38`; the current page is
  `*(context+0x38)` (`func_0022FB50`).
- **Live evidence** (`private/slice432-launch/run.log`, probe build): the game's own startPage calls show the slots and
  the raw objects: `a1=0x00E14C7C` with `*a1=0x00F681A0` (vtable `0x00663BE0`), `a3=0x00F0B464` with
  `*a3=0x01FC2A90` (vtable `0x00665418`). The same page root appears in several calls; the context values are the live
  `mRenderContextPS2`. There is no hidden wrapper object between the slot and the engine object.

**Consequence:** a dispatch hook can call `startPage` by writing the raw context and page-root pointers into guest
scratch slots and passing their addresses. This is what the probe does; it does not guess.

### 22.2 The hidden MonitorRoot (live, but not a registered page)

- The v2.00 `option.mproject` has a `MonitorRoot` `RootWindow` (`has_script digit {1}`, line 17310) with the
  `TitleMonitor`, `num of monitor`, `monitor position` and `monitor type` widgets. The page is **not in the v2.00 page
  list** (`option.adc`), but the project loader instantiates the root: in the probe state the hidden page root is
  **`0x00FD8090`** (vtable `0x00665418`, Pane `0x00FD8550`, `TitleMonitor` TextFace `0x00FD89D0`).
- Calling `startPage(context=0x0102AFB0, page=0x00FD8090)` at frame 2300 goes through the native and returns:
  `[page-launch] startPage(MonitorRoot context 0x0102AFB0, page 0x00FD8090) returned`.
- **It does not render the page.** The context's page-list slot stays `0x00D73610 -> 0x00F77200` (the ScreenRoot); the
  guest frame counter **stops at 2300** (the frame tick log has no line after 2300; the frame files after 2300 are the
  same Screen Settings picture). The hidden page root is live, but its page initialization/page-list insert is not
  accepted; the not-registered `has_script 1` page leaves the update/render loop waiting. The page root's own state
  (`0x45045000`, bit 20 = 0) passes the `func_00265F38` check, so the block is after the page root's `+0x2D0`
  initialization / the page-list insert, not the handle conversion.
- **Exact failing call:** `MRenderContext.startPage(0x0102AFB0, 0x00FD8090)` at frame 2300; the native returns without
  changing `context+0x38`, and the guest frame loop does not advance. The page list/registration is the blocker, not the
  hValue relation.

Evidence: `private/slice432-launch/run.log` (the game's own startPage calls, the MonitorRoot launch, the return, the
stop), `state.bin` (context `0x0102AFB0`, current page slot `0xD73610 -> 0xF77200`, MonitorRoot `0xFD8090` state
`0x45045000`), `png/frame_02300.png` and `png/frame_02500.png` (the Screen Settings page before and after; no
MonitorRoot frame). Probe build: capsule `private/slice432-page-launch-license-guard-runtime`, receipt
`cead3c642991531df3b0f6010ed532af56107ad22e2fa2af8d96c7302a3bdc66`, pin
`docs/inputs/slice432-page-launch-license-guard-runtime.json`; exe
`a54ed940f9bb02c4fbd37a1e1a3fff7cd46432f154e24729ce33e461fd1a7320` (90,238,976 bytes). The earlier
`slice431-page-probe` capsule (receipt `dd4e3263...`) is the log-only probe.

### 22.3 The smallest next step

1. **Scriptless page root (the recommended smallest test).** Patch `MonitorRoot`'s `has_script digit {1}` to `{0}` in a
   fresh private data patch (or add a new `RootWindow` with `has_script 0` and an empty Pane), rebuild, find its raw
   `mRootWindow` the same way (the `TitleMonitor` key / the new label), and call `startPage` with the same slots. If it
   renders, the page route is proven for a scriptless page and the empty Graphics shell can be a **new scriptless
   RootWindow** with the game's widgets. This is data-only and does not need the option page list. The page root must
   still be instantiated by the project loader (the hidden MonitorRoot is; a new RootWindow should be checked).
2. **Registered page root / host tree inside the Options root (fallback).** Use a registered page root whose script is
   disabled or replace its children with the host Graphics tree; or inject a host tile into the Options root's scroll
   content and handle its Cross press. The exact attach/focus risks:
   - the Options root is a `mScrollWindow` (`0x01FDA8A0` in the state); its content is a tile grid and the script may
     rebuild it when the Options root is entered/left, so a host child must be appended after the update and
     re-appended every time;
   - the Screen page's Pane VBox focus traversal reaches the host OptionMenu rows (Phase 1d proved it), but the Options
     root has no `SelectBox`: a host tile is reachable only if it is a child of the scrolled focus container, and its
     Cross press must be masked from the script (the `menu_quit` pattern);
   - `startPage`/`closePage` is still needed for the page transition/back; a host tree inside the Options root avoids
     the page list but not the scroll/focus rebuild.
   The smallest proof for this route: append a **single game `TextFace`/`Composite`** to the Screen page's Pane (a
   registered page, known focus) and capture a frame; then a focusable `OptionMenu` row, then a Cross handler. The
   owner's "no floating panel" rule stays: the tree is a child of the real page/scroll content.

The probe does not continue to the Graphics rows: the page is not reachable, and a host tree is a new design step for
the owner's decision.

## 23. Resumo para o dono (pt-BR) - Phase 2a

- **A relação do handle foi descoberta:** o "handle" não é um objeto escondido; é o **próprio objeto do motor**
  (`mRenderContextPS2` e `mRootWindow`). O script passa o **endereço de um slot de 4 bytes** com o ponteiro do objeto;
  as funções `func_0022AD20`/`func_00232C78` são só o cast de RTTI e a contagem de referências. O probe mostrou isso
  nas chamadas do próprio jogo (contexto `0xF681A0`, página `0x01FC2A90` etc.).
- **A `MonitorRoot` existe viva, mas não é uma página registrada:** a raiz `0x00FD8090` está na RAM com o Pane e o
  `TitleMonitor`, mas não está na lista de páginas do `option.adc` e tem `has_script 1`. Chamei
  `startPage(contexto 0x0102AFB0, página 0x00FD8090)` no frame 2300: a chamada **retorna**, mas a página atual continua
  a ScreenRoot (`0xF77200`) e o **contador de frames do jogo para no 2300**; o frame depois disso é a mesma tela de
  Screen. Ou seja, a página não renderiza e a transição trava o loop. O ponto exato é a inicialização/registro da página,
  não o handle.
- **Próximo passo menor:** um teste **data-only** com a `MonitorRoot` sem script (`has_script 0`) ou uma nova
  `RootWindow` scriptless vazia; se ela abrir com a mesma chamada, a página vazia do Graphics é uma `RootWindow`
  scriptless com os widgets do jogo. Se não abrir, a alternativa é a árvore do host **dentro da raiz do Options**
  (o `ScrollWindow`), sem sobreposição flutuante; os riscos são o conteúdo ser reconstruído pelo script e o foco do
  tile/scroll. O menor teste é pendurar um único `Composite`/`TextFace` na Pane da página Screen e capturar um frame.
- O probe é um build de teste (cápsula `slice432`, exe `a54ed940...`), com as variáveis `GT4_PAGE_PROBE` e
  `GT4_PAGE_LAUNCH_FRAME`; sem elas o jogo normal não muda.

## 24. Phase 2a scriptless-root experiment: still rejected, the page list is the gate (2026-11-10, evidence)

The recommended smallest experiment was the `MonitorRoot` scriptless root. The result is negative: the page still does
not render and the guest frame loop still stalls, so the page is **not reachable** and no Graphics shell was built.

### 24.1 The data patch (same-length, verified)

- Fresh private patch `private/slice433-scriptless` from the accepted `slice430` recipe, plus one text replacement:
  `RootWindow { name string {"MonitorRoot"} has_script digit {1}` -> `... digit {0}`. The old/new strings are the same
  length (69 bytes) and the replacement is unique; `minify: true` keeps the stored extent.
- Container `dc6dafbe20ca768e826cb48e9e5b3deeb7720c70362876bcc94dc06c990bb6cf` (77,986 bytes); report
  `projects/option/option.mproject` replacement stored SHA-256
  `66553dd9215ce10c0638184ca400cf6f48cd40d3d1f5c52397178277a237b4f6`, 10 text replacements. The builder's
  round-trip verification passed; the runtime log shows `[data-patch] ... verified against the image`.
- The probe build is the same `slice432-page-launch` capsule; the only code change is the probe's post-launch frame
  log (native/page_probe.cpp). Exe
  `95887cb77277018a0a3232e916b6b2e7df224e95df2129144d011b90f8c0a529` (90,238,976 bytes).

### 24.2 The result (stall)

Run: `GT4_DATA_PATCH=slice433-scriptless/patch/gt4-data.patch GT4_PAGE_PROBE=1 GT4_PAGE_LAUNCH_FRAME=2300`, frames
2600 (the frame limit is the safety: the run exits 0 after 2600 host frames even when the guest frame counter stops).
Probe log:

```
[page-launch] MonitorRoot 0x00FD8090 (TitleMonitor 0x00FD89D0)
[page-launch] frame 2300 MonitorRoot: context 0x0102AFB0, page 0x00FD8090
[page-launch] startPage(MonitorRoot context 0x0102AFB0, page 0x00FD8090) returned
[page-launch] after frame 2300
[ee] stopped after 2600 frames ...: frame limit reached
```

- The post-launch frame log shows **only 2300**; no 2301+ line. The guest frame counter does not advance.
- The saved state at 2500: context `0x0102AFB0` (vtable `0x00663BE0`), current page slot `0xD73610 -> 0xF77200`
  (the ScreenRoot), hidden root `0xFD8090` state `0x45045000` (bit 20 = 0), unchanged.
- The frames after 2300 are the same Screen Settings picture; no MonitorRoot frame.
- **Exact call/state:** `MRenderContext.startPage(context=0x0102AFB0, page=0x00FD8090)` at frame 2300 returns, but the
  context page list/current page is not updated and the update/render loop waits. The scriptless flag alone does not
  make the hidden root an accepted page; the **`option.adc` page list/registration** is the blocker (the script was not
  the only gate).
- Evidence: `private/slice433-launch/run.log`, `state.bin`, `frames/` PNGs; the `slice430-kit` and the accepted Screen
  page are untouched.

### 24.3 Reachability decision: blocked

The hidden `MonitorRoot` is a live `mRootWindow` tree, but `startPage` with it is not accepted even when scriptless; the
page list (compiled `option.adc`, script) is required, or a host tree is needed. **No empty Graphics page, no rows, no
floating panel was added.** The owner's Screen page and `private/slice430-kit` remain as accepted.

### 24.4 The fallback: a host tree inside the Options root (not a floating panel)

- **Route:** append a host `Composite`/`OptionMenu` to the Options root `mScrollWindow` content (state address
  `0x01FDA8A0`) or, for the smallest proof, to the **Screen page's Pane** (a registered page; the Phase 1d OptionMenu
  rows prove the VBox focus traversal). The Graphics menu is a real child of the page/scroll content.
- **Attach/focus risk (exact):**
  - the Options root is a `mScrollWindow` and its tile/scroll content can be rebuilt by the script on enter/leave; a
    host child must be appended after the script update and re-appended every time;
  - the Options root has no `SelectBox`; a host tile is reachable only as a child of the scrolled focus container, and
    its Cross press must be masked from the script (the `menu_quit` pattern);
  - the Screen page's Pane is the known focus path, but it is the Screen page, not a new page; a separate Graphics
    page still needs a registered page root or an Options-root tile that calls the host page.
- **Smallest proof:** append one game `TextFace`/`Composite` to the Screen page's Pane and capture a frame; then a
  focusable `OptionMenu` row; then the Graphics rows (renderer/render scale, restart-only, writing only
  `game-changes.toml`). The launcher stays the only `settings.toml` writer; the save stays untouched.
- **Page registration alternative:** if a separate page is required, a page-table entry (compiled script) or a
  registered page root whose content is replaced is needed; the hidden MonitorRoot alone is not enough. The Adhoc
  v5/TOC route and the Network-page repurpose remain out.

## 25. Resumo para o dono (pt-BR) - scriptless root

- **Teste da raiz sem script:** troquei só `has_script 1 -> 0` na `MonitorRoot` (patch privado `slice433`, mesmo
  tamanho, verificado). A chamada `startPage(contexto 0x0102AFB0, página 0x00FD8090)` no frame 2300 **continua
  retornando sem abrir a página**: a página atual fica na ScreenRoot (`0xF77200`), o contador de frames do jogo para no
  2300 e o frame seguinte é a mesma tela de Screen. O limite de 2600 frames encerra o run sem travar o processo.
- **Decisão:** a página **não é alcançável** só com a raiz scriptless; falta o **registro na lista de páginas**
  (`option.adc`, script). Não adicionei a página Graphics nem as linhas; nada foi falsificado. A página Screen aceita e
  o kit `slice430` continuam iguais.
- **Próximo passo:** a árvore do host **dentro da raiz do Options** (o `ScrollWindow`), sem sobreposição flutuante; o
  caminho de foco conhecido é a Pane da página Screen. O menor teste é pendurar um único `Composite`/`TextFace` na Pane
  da Screen e capturar um frame; depois uma linha `OptionMenu` focável e as linhas Graphics (renderer/render scale,
  restart-only, só `game-changes.toml`). Os riscos são o script reconstruir o conteúdo do scroll e o foco do tile; a
  Cross do tile precisa ser mascarada.

## 26. Phase 2a: the Recomp settings hub on the arcade main menu (2026-11-10, implemented, test build)

The owner replaced the separate Graphics page with a new **Recomp settings** menu: **Graphics, Controls, Display, Audio,
System**. The entry decision, the hub, the first submenu and the shells are implemented as a host row/menu on the
game's own main-menu `SelectBox`; no page-list edit, no Adhoc v5/TOC, no Network repurpose, no floating panel. The
accepted Screen page and `private/slice430-kit` are unchanged.

### 26.1 The entry decision (evidence)

| Candidate | Evidence | Decision |
|---|---|---|
| **Main-menu row via the Quit Game pipeline** | the hidden Quit template is appended to the arcade `SelectBox` (v2.00 vtable `0x00671898`, rows `+0xA0/+0xA4/+0xA8`, index `+0xBC`, end `+0xC4`) and the row is focused/activated (`private/slice421-kit`, TASK-212); the SelectBox owns drawing/focus/scroll | **chosen**: the Recomp settings row is appended before Quit (the host row pipeline is proven, the menu is a real game list) |
| Tile/row in the Options root (`mScrollWindow`) | the script may rebuild the tile/scroll content on enter/leave; the Options root has no `SelectBox`; the host child must be re-appended and its Cross masked | not chosen: more attach/focus risk for the same goal |
| Section inside the Screen page Pane | the VBox focus path is proven (Phase 1d rows), but it is the Screen page, not a new menu | not chosen: not a separate menu |

### 26.2 The host menu (game widgets, no floating panel)

- The private data patch (`private/slice436-recomp-labels`) combines the accepted option patch (`slice430`) with the arcade
  Quit patch (`slice421`) and adds a hidden `recomp` composite with 20 row templates next to the hidden Quit
  composite. The TopRoot children count grows 8 -> 9; the composite is `visible 0`; the rows are the game's
  `Composite`/`TextFace` widgets (hb_24, skew, shadow) with literal labels. Container
  `19eab22cecdaf72b6b1a9054e2f2a1a03783ef4152224110f11d7b7b9d84fa63` (91,712 bytes, 3 ranges).
- The host (`native/recomp_menu.*`) scans the arcade `SelectBox` and the hidden `recomp` composite (child count 20),
  detaches the rows into a host array, appends the **Recomp settings** row (index 6, before Quit), and swaps the
  SelectBox row list on navigation:
  - hub: title row r14 "Recomp settings" + rows r1..r5 **Graphics, Controls, Display, Audio, System**;
  - Graphics: title r15 "Graphics" + the current renderer row (r6 GSdx / r7 CoreGS 2.0) + the current scale row
    (r8..r13, 0.50x..2.00x; stored 50..200%). The rows show the "x" form because the game font `hb_24` has no
    percent glyph: a "125%" label drew as "125 0" in the first build;
  - shells: the title rows r16..r19 (Controls, Display, Audio, System).
  The engine draws the SelectBox, the rows and the focus; the host owns the list fields, the focus index, the Cross/
  Circle actions and the pad masking. The title rows are insensitive (bit 3) and start at index 0; the first selectable
  row is index 1.
- **Focus/up/down:** the host moves the focus index on the Up/Down edges in the host lists (the game's SelectBox
  otherwise restores its own index); the render functor re-applies the index after the page update. **Cross** enters;
  **Circle** backs (Graphics -> hub -> main menu). The `menu_quit` watcher pauses while a host list is open so it cannot
  append Quit inside it. The main menu's own rows keep their normal behavior outside the Recomp/Quit rows.
- **Graphics:** Cross on the Renderer row cycles GSdx/CoreGS 2.0; Cross on the Render scale row cycles 50/75/100/125/
  150/200%. Both are restart-only and write only `game-changes.toml`:
  `renderer = "pcsx2"` or `"coregs20"`, `render_scale = "125"`. The host accumulates the changed keys so both survive in
  one file (the first version overwrote the file with the last change). The launcher is the only `settings.toml`
  writer; the guest save stays untouched (states: screen_adjust_v/h 0, wide 1, project `arca`).
- The initial renderer comes from `GT4_RENDERER` (`hw` = CoreGS 2.0, otherwise GSdx); the initial render scale shows
  100% because the launcher passes the internal multiplier (`GT4_HW_SCALE`), not the percentage. The row writes a value
  only when the player changes it. The renderer value is re-read from `GT4_RENDERER` when the menu tree reloads, so a
  pending change made in the same session shows after the restart, not before it.

### 26.3 Evidence (headless, v2.00)

| Check | Evidence |
|---|---|
| Entry row | `private/slice436-kit/frames/main-12950-recomp-settings-row.png` (Recomp settings above Quit Game) |
| Hub | `hub-13100-graphics-controls.png`, `hub-13150-display-audio-system.png` (all five rows in order) |
| Focus/up/down | the `[recomp-menu] focus index` lines (hub 1->2->1, Graphics 1->2) and the frames |
| Graphics | `graphics-13200-gsdx-1.00x.png`, `graphics-13400-coregs-1.25x.png`; log `renderer = "coregs20"`, `render_scale = "125"` |
| Back | `[recomp-menu] back to the hub` / `back to the main menu`; the frames after the Circles |
| Shells | `controls-13600-shell.png`, `display-13800-shell.png`, `audio-13200-shell.png`, `system-13400-shell.png` |
| Feedback | `game-changes.toml`: `renderer = "coregs20"`, `render_scale = "125"` |
| Save untouched | `state-graphics.bin`/`state-shells.bin`: screen_adjust_v/h 0, wide 1, project `arca` |
| Exit | both headless runs exit 0; no unmapped reads, no crash |

- Kit `private/slice436-kit`: exe `8a6f33cd...` (90,249,728 bytes), patch `19eab22c...`, states/logs/frames/README;
  test build only.
- Tests: `native_game_target` 1/1, `native_native_settings` 1/1, `native_display_aspect` 1/1; `cargo test -p
  gt4l-core --lib` 311 passed (the new `renderer_and_render_scale_are_stored`); `test_license_guard.py` 21 OK;
  `test_gt4_data_patch.py` 16 OK.

### 26.4 Risks and limitations

- **The host row swap depends on the v2.00 arcade `SelectBox` layout** and the hidden composite; a menu reload re-parses
  the template and the host re-injects. The game's SelectBox update otherwise restores its own index; the render hook
  re-applies the host index. The menu_quit pause prevents a Quit re-append inside a host list.
- **The initial render scale is 100%** even if the launcher's stored scale is another step (only the internal
  multiplier reaches the game); the renderer initial comes from `GT4_RENDERER`. The values are written only on change.
- **The Recomp row is in the arcade main menu only** (the main menu whose last major menu is Arcade), not the GT Mode
  menu or the Options root. The state-arcade shortcut or an arcade save is required for the owner test.
- **The shells are title-only** (Controls, Display, Audio, System) by design; Display does not open the Screen page.
- **The launcher remains the only `settings.toml` writer**; the game writes `game-changes.toml`; the save untouched.
- **Spec II** is not covered by the v2.00 SelectBox profile/data patch.
- The host row/menu uses the game's SelectBox; the `SelectBox` script's per-row modules are not used by the host rows,
  but the engine's focus/scroll/draw are. The main menu's normal rows outside Recomp/Quit keep their script behavior.

## 27. Resumo para o dono (pt-BR) - Recomp settings

- **Entrada escolhida:** a linha **Recomp settings** foi anexada ao **menu principal do Arcade**, logo acima de Quit
  Game, usando o mesmo caminho da linha Quit Game (o SelectBox do jogo). Não usei o menu Options (scroll) nem uma seção
  na página Screen; não há página nova, Adhoc v5/TOC, Network ou painel flutuante.
- **Hub:** abre com o título **Recomp settings** e as linhas **Graphics, Controls, Display, Audio, System** na ordem
  que você aprovou. O foco sobe/desce, Cross entra, Circle volta (Graphics -> hub -> menu principal). O menu é o próprio
  SelectBox do jogo (desenho, foco e scroll); o host só troca a lista e trata os botões.
- **Graphics:** Renderer (GSdx/CoreGS 2.0) e Render scale (0.50x..2.00x; gravado 50..200%) com Cross; os dois são **restart-only** e gravam
  só `game-changes.toml` (`renderer = "coregs20"`, `render_scale = "125"`); o launcher continua o único a escrever
  `settings.toml` e o save não muda.
- **Controls/Display/Audio/System:** abrem telas de título vazias por enquanto, na ordem; voltam com Circle.
- **Teste:** o kit `private/slice436-kit` tem o estado que entra no menu Arcade, o patch combinado e as capturas; o
  README tem o comando. O renderer inicial vem de `GT4_RENDERER`; a escala inicial mostra 100% mesmo que o launcher
  esteja em outro passo (só o multiplicador interno chega ao jogo), então o valor só é gravado quando você muda.
- **Limitações:** a linha Recomp só existe no menu principal do Arcade (não no menu GT Mode nem no Options); as telas
  Controls/Display/Audio/System são vazias por enquanto; Spec II não coberto.

## 28. Phase 2a: the four Dummy page entries are bytecode with a nil root (2026-11-10, investigation; no page shipped)

The owner asked whether a `Dummy` entry in the v2.00 `option.adc` page table could be repurposed **same-length** to a real
page (the hidden `MonitorRoot` or another empty root) and reached from the Options menu. The answer is **no**: the page
list is not a table of `(name, root)` records but the compiled Adhoc **v5 top-level code frame** that constructs each
page; the Dummy pages are constructed with a **nil** root and no root strings, and the four blocks have no slack.

### 28.1 What the bytes are

- `projects/option/option.adc` (v2.00) is `ADCH005`, 251,715 decoded bytes. The region `0x8A7C`-`0x99DD` is the top-level
  frame's page-construction code, not a data table: `0x1D` is the v5/v7 `STRING_CONST` opcode and `0x16` is
  `NIL_CONST` in the public Adhoc instruction enum (MIT reference `private/slice151-research/nenkai/GTAdhocToolchain`).
  **H** (bytecode interpretation; the v5 opcode map is not disassembled in this repository).
- Page blocks begin with the `OptionPage` class strings and a 78-113-byte body. Measured (decoded offsets, header-to-
  header): `Album` at `0x9015` is 113 bytes with `STRING_CONST "Album"` (5) and the root pair
  `"AlbumRoot"` x2 (9 each); `Dummy` at `0x9086`, `0x90D4`, `0x9122`, `0x9170` are **78 bytes each**, with
  `STRING_CONST "Dummy"` (5) and a **single `0x16` nil in the root slot**, no root strings. The four Dummies are
  back-to-back in `GroupPhotoMode`; `Album` + 4 Dummies = 425 bytes.
- Any real page needs a root pair: the Album block is already **+35 bytes** over a Dummy; a `MonitorRoot` (11 chars)
  pair is 4 bytes longer again. The decoded adc size is fixed at 251,715 by the patch builder's round trip
  (`verify_round_trip`), so a same-length edit must find 35+ bytes inside the four 78-byte blocks; there is no slack
  (no padding, no unused string) and the blocks are contiguous. Even the label alone ("Recomp settings", 15 chars vs
  "Dummy", 5) needs 10 more bytes. **C** (measured bytes and builder rule).
- The Dummy blocks are hidden by name: the Options root screenshot at frame 2000 shows **Photo Mode = Photo Lab only**
  (`private/slice437-option-root/png/frame_02000.png`), and the script's tile code compares the title to the literal
  `"Dummy"` (the string sits next to `title` and `==` in the frame at `0xA795`). Renaming a Dummy **same length** to
  `Color` or `Zzzzz` makes a tile appear in Photo Mode: `Color` resolves to the existing rtext label
  "Picture Quality", `Zzzzz` falls back to the page name (`private/slice437-root-rename/png/frame_02000.png`,
  `private/slice437-root-unknown/png/frame_02000.png`). So the filter is name-based and the label is rtext(name) with a
  name fallback. **C** (frames) / **H** (the compare site is bytecode).

### 28.2 Activation needs a root and the page module

- Pressing Cross on the renamed rootless Dummy halts the guest with `no translated code at 0x00000000 (ra 0x0030B450)`
  (`private/slice437-rootless-open/run.log`): a page with a nil root cannot start.
- Repurposing the visible **SplitBattle** page entry to the new root `RecompPageR13` (a scriptless `RootWindow` added to
  `option.mproject`, `children [20] -> [21]`) and the name **"Recomp settings"** (the adc block rewrite keeps 57 bytes:
  name 11 -> 15, roots 15 x2 -> 13 x2) produces the tile **"Recomp settings"** in Arcade Mode and the matching tooltip
  (`private/slice437-page-open/png/frame_02000.png`), but Cross halts at `no translated code at 0x00000000
  (ra 0x0031ABEC)` (`private/slice437-page-open/run.log`). The page entry alone is not enough: the engine also needs the
  page's script module (the crash address differs from the nil-root crash), so the page **name is the module key** (H)
  or the root must be a registered page root with its module. **C** (frames/logs) / **H** (module key).
- A pointer-only link of the existing `Monitor` page literal (`OptionPage("Monitor", MonitorRoot)`, code at `0xBED8`)
  into `GroupPhotoMode` (group head `0xB4 -> 0x178`, Monitor next `0x178 -> 0xB4`) changed nothing in the Options tile
  list (`private/slice437-root-monitor/png/frame_02000.png`): the list is built by the page-construction code, not by a
  group pointer. **C**.
- The Monitor/Secret page objects exist in the adc code (Secret at `0xBD5E`, Monitor at `0xBED8`) but the v2.00 list
  ends at Miscellaneous (Misc/Units) on screen; they are not appended (`private/slice437-root-scroll/png/frame_04100.png`).

### 28.3 The visible-page label gate (rtext)

Keeping the page name for its module means the tile label comes from `projects/option/US/rtext.rt2`; the SplitBattle
value is "2 Player Battle" (15 chars), exactly the length of **"Recomp settings"** (15). A same-length byte relabel was
built at each of the six occurrences; every build is **27-35 compressed bytes over the 13,526-byte stored extent**
(13,553-13,561), even when the replacement is shorter and zero-padded. The builder's zlib is already ~28 bytes worse than
the original packer on this file, so the rtext cannot be patched with the current builder without freeing compressed
headroom first (a long unused string blanked, or a better compressor). **C** (six builds refused) / **H** (the
compressor gap).

### 28.4 The gate and the smallest next step

**Gate:** the four Dummy entries are code, not data; each has a nil root and 78 bytes, and a real page needs 35+ more
bytes (root pair) plus 10 more for the label, inside a fixed 251,715-byte decoded file with no slack; the Dummies are
also filtered out of the tile list by name. Same-length repurposing is **not possible**. A name-only rename makes a tile
but the page cannot start without its module; a root swap on a visible page starts neither.

**Next step needs the owner's decision (no page was implemented, nothing was repurposed silently):**
1. **Repurpose a visible page with its module and root** (SplitBattle is the best fit: its label is 15 chars), then
   attach the Recomp rows to that page's Pane with the host (the accepted Screen-page row pattern) and hide/neutralise
   the page's own widgets; the tile label needs the rtext headroom above or the page name fallback (which changes the
   module key — not viable).
2. **Keep the Arcade-main-menu hub** and the `slice436-kit` as the shipped fallback while a v5 module is considered;
   v5/TOC is out for now.
3. Network stays vetoed; no floating panel.

Private evidence: `private/slice437-option-root` (Options root frames), `slice437-rename-dummy`, `slice437-rename-unknown`,
`slice437-rootless-open`, `slice437-monitor-link`, `slice437-root-monitor`, `slice437-recomp-page-probe`,
`slice437-page-open`, `slice437-root-scroll`, `slice437-rtext-relabel`, `slice437-rtext-single`, `slice437-rtext-short`.
No repository source or data changed; the accepted Screen page, the Arcade hub and `slice430/436-kit` are untouched.

## 29. Resumo para o dono (pt-BR) - páginas Dummy

- **A resposta curta: não dá para reaproveitar uma página Dummy mantendo o mesmo tamanho.** As quatro entradas
  `Dummy` do `option.adc` não são uma tabela de dados: são **código compilado** (Adhoc v5) que cria a página com
  **raiz nula** (`nil`), sem os textos da raiz. Cada bloco tem 78 bytes; uma página real (Album) tem 113 bytes, e o
  arquivo decodificado não pode crescer (251.715 bytes fixos). Faltam pelo menos ~35 bytes para a raiz e ~10 para o
  nome, e não há espaço sobrando nos quatro blocos.
- **As Dummies também ficam escondidas:** o menu Options mostra só "Photo Lab" em Photo Mode. Renomeando uma Dummy com
  o mesmo tamanho, a linha aparece (testei "Color" e "Zzzzz"), então o filtro é pelo nome "Dummy". Mas apertar Cross
  numa página sem raiz derruba o jogo (`no translated code at 0x00000000`).
- **Testei a ideia mais próxima do que você quer:** troquei a página **2 Player Battle (SplitBattle)** por uma página
  nova "Recomp settings" (mesmo tamanho no arquivo). A linha **"Recomp settings" aparece no menu Options**, com o
  tooltip certo, mas ao abrir o jogo cai (`0x0031ABEC`): a página precisa também do **script/módulo** dela, que não
  existe para a página nova. A lista também não é montada por um ponteiro de grupo (tentei ligar a página Monitor
  existente e nada mudou).
- **O rótulo:** manter o nome SplitBattle (para o módulo funcionar) e trocar só o texto exibido exigiria editar o
  `rtext.rt2`; a troca de mesmo tamanho ("2 Player Battle" -> "Recomp settings") **não cabe** no espaço comprimido
  (13.554 > 13.526 bytes) nem com o texto encurtado. Precisa liberar compressão ou um compressor melhor.
- **O que eu preciso de você:** escolher se podemos **reaproveitar uma página visível** (a melhor é 2 Player Battle,
  cujo rótulo tem exatamente 15 caracteres) mantendo o módulo/raiz dela e desenhando as linhas Recomp por cima com o
  host, ou se seguramos o menu do Arcade como está. **Não escolhi nenhuma página sozinho e não ship nenhuma página.**
  Network continua vetado; v5/TOC fora por enquanto. O kit `slice436-kit` e a página Screen aceita continuam intactos.

## 30. Phase 2a: Options > Miscellaneous cannot host a scriptless OptionMenu row (2026-11-10, gate)

The owner's fallback was a **Recomp settings** entry in **Options > Miscellaneous** (no new page/root binding, no existing
page sacrificed). The Misc page is `MiscRoot` with a `VBox Pane` of **7 children** (bg, Title, OpeningMovie, AutoDemo,
DemoMovieInterval, AutoSave, Buttons; 182 px tall). Two data-only probes were built on the retail `option.mproject`
(host row only; the accepted Screen patch was tested separately):

- **Label-only row (safe):** an 8th child `Composite "Recomp"` with a ColorFace and a literal `TextFace
  "Recomp settings"`, Pane 182 -> 214 and bg 182 -> 214, loads and renders: the Misc page shows the row between
  Autosave and Cancel/OK (`private/slice440-run-label-misc/png/frame_04500.png`), and a 5,000-frame run exits 0. **C**.
- **OptionMenu row (blocked):** the same row with an `OptionMenu` of 16 choices (12 Graphics combos + Controls,
  Display, Audio, System) loads the page data but the **Options root crashes at load**: `no translated code at
  0x00000000 (ra 0x0026986C)` (`private/slice440-run/run.log`, `slice440-run-noscreen`, `slice440-misc-dup2`). The
  Pane-height-only change is safe (`slice440-run-height`), and naming the row `OpeningMovie` (a module that exists) does
  not help. The Misc page script initializes its Pane's OptionMenu children and a scriptless one calls a null module
  (H; the crash is at project load, before the page is opened). **C** (logs) / **H** (cause).
- So the interactive row cannot be added as a scriptless OptionMenu child of the Misc Pane without breaking the page
  script. The static label row is safe but does not open anything.

**Smallest alternatives (no page sacrificed, no fake page, no floating panel):**
1. **Pad combo on the Misc page + host tree over the page**: the host shows a hidden `Composite` hub attached to
   `MiscRoot` (not the Pane, so the page script's child init is untouched), owns focus/index/Cross/Circle, and the
   label row is the visible hint. This is the closest to the owner's entry.
2. **Focusable Composite row + host focus detection**: add `can_focus 1` to the label row (safe), and derive the
   engine's focused-widget pointer so the host can open the hub on Cross; the focus field is not derived yet.
3. **v5 module** for a real scripted Misc row (out for now).

No repository source or data changed; the Arcade hub and `slice436-kit`, the accepted Screen page and `slice430-kit` are
untouched. Probes: `private/slice440-misc-row` (OptionMenu, crash), `slice440-misc-row-noscreen`, `slice440-misc-height`
(safe), `slice440-misc-dup2` (crash), `slice440-misc-label` (safe), `slice440-run*` logs/frames.

## 31. Resumo para o dono (pt-BR) - Options > Miscellaneous

- A linha **"Recomp settings"** cabe na página Miscellaneous como **texto** (sem controle): a página abre normal, com a
  linha entre Autosave e Cancel/OK (frame 4500 do probe), e o save/páginas existentes não mudam.
- Mas a mesma linha com um **OptionMenu** (para abrir o hub) **derruba o jogo ao entrar no Options**
  (`no translated code at 0x00000000`, ra 0x0026986C): o script da página Miscellaneous inicializa os OptionMenus
  filhos e um sem módulo chama função nula. Só a altura da página ou uma linha estática não quebram.
- Ou seja: **a Misc não aceita a linha interativa sem script**. Não sacrifiquei nenhuma página e não inventei página.
- Próximo passo menor: abrir o hub com um **comando no controle na página Misc** (ex.: Quadrado) desenhando a árvore do
  host por cima da página (fora do Pane, para o script não ver), com a linha "Recomp settings" como aviso; ou derivar o
  ponteiro de foco para usar a linha com `can_focus`; ou um módulo v5 (fora por enquanto). O hub do Arcade e o
  `slice436-kit` continuam funcionando.

## 32. TASK-213 slice 1: complete hub rows and the game's confirmation popup (2026-11-10)

The Arcade hub now carries the complete row list (38 hidden rows, entry + 5 hub rows + 31 submenu rows). All rows are
display-only in this slice: the only actions are the existing Renderer/Render scale (restart-only, `game-changes.toml`).
Every architecture-bound row carries a visible `(restart)` marker; Fullscreen/Resolution point at the Screen page,
which stays the live path. The submenus open and Circle backs to the hub then to the main menu; headless run exit 0,
`renderer = "coregs20"` and `render_scale = "125"` written.

The game's confirmation popup for the 480p/HiVision change is **script-driven**, not a reusable data widget:
`TryHiVision` is a Screen-page script module string (`option.adc` 0x2549E), and the generic dialog is
`openConfirmDialog` from `share/dialog.ad` (strings at `option.adc` 0x4C/0x5B9/0xB38D). The only Yes/No tree in
`option.mproject` is `LANPopup` (a RootWindow page, children [6]); there is no standalone MessageWindow/Popup template.
Reusing the game popup for a host row would need either calling the script dialog with callbacks (script VM) or cloning
the LANPopup Yes/No tree and host-managing it (game widgets, not a floating panel). Neither lands in slice 1; the rows
that need confirmation stay restart/display-only with the marker, and the live setters + confirmation are the next
slice.

Evidence: `private/slice443-kit` (exe `a4ace123...`, patch `371865ab...`, recipe/report, run.log, state, game-changes,
six frames, README/run-hub.cmd). No existing page/function changed.

## 33. Resumo para o dono (pt-BR) - hub completo e confirmação

- O hub do Arcade agora lista **todas as linhas** (38 no total): Graphics com renderer/escala e as linhas de efeitos,
  blending/readbacks, sharpening, frame rate e HUD; Display com fullscreen/resolução/monitor/vsync; Controls; Audio (só
  o áudio do jogo); System. As linhas de arquitetura mostram **(restart)**.
- Nesta fatia **nada novo é aplicado ao vivo**: só Renderer e Render scale continuam gravando `game-changes.toml`
  (restart). As demais linhas são informativas por enquanto; Fullscreen/Resolution continuam na página Screen.
- O popup de confirmação do 480p é **script** (`TryHiVision` e `openConfirmDialog`), não um widget de dados reutilizável;
  o único Yes/No do mproject está dentro do `LANPopup` (página inteira). Para reaproveitar de verdade seria preciso
  chamar o diálogo do script ou clonar a árvore Yes/No do LANPopup com o host — fica para a fatia dos setters live.
- Kit `private/slice443-kit` com README, comando e hashes; nenhuma página/função existente mudou.

## 34. Slice 2: the Graphics list filters by the active renderer (2026-11-10)

The Graphics list is no longer static: it follows the renderer the process **started with** (`GT4_RENDERER` at prepare),
not the pending selection. The Renderer row remains restart-only; toggling it writes the feedback and keeps the active
row set until the next start (the log says "active rows stay ... until restart").

- **Shared rows:** Renderer, Render scale, Flares, Frame rate, Performance HUD, plus the other submenus
  (Fullscreen/Resolution on the Screen page, Monitor, VSync, Controls, Audio, System).
- **GSdx active:** GSdx blending and GSdx readbacks (gt4-gs INI rows). No road sheen/targets/sharpening.
- **CoreGS 2.0 active:** Road sheen, Effect targets, Sharpening (FSR1). No GSdx blending/readbacks.
- **Proof switch:** `GT4_RECOMP_ACTIVE_RENDERER=hw` forces the visible row set for headless proof only; it never changes
  the renderer. It exists because the in-process CoreGS backend does not load save states, so the CoreGS list could not
  otherwise be exercised from the arcade-menu state.
- **Evidence:** `private/slice444-kit` (exe `343cabf1...`, patch `371865ab...`); `run-gsdx.log` shows
  "active GSdx ... GSdx rows", `run-coregs-set.log` shows "active CoreGS 2.0 ... CoreGS rows"; frames
  `graphics-gsdx-13100.png` and `graphics-coregs-13100.png`. Renderer/scale feedback still written
  (`coregs20`, `125`). No existing page/function changed.
- **Still deferred:** live setters and the confirmation popup (script `TryHiVision`/`openConfirmDialog`; only the
  LANPopup Yes/No data tree exists). Rows are display-only with `(restart)` markers except Renderer/Render scale.

## 35. Resumo para o dono (pt-BR) - linhas por renderer

- A lista **Graphics** agora muda conforme o renderer **ativo no início do jogo**: com GSdx aparecem **GSdx blending** e
  **GSdx readbacks**; com CoreGS 2.0 aparecem **Road sheen**, **Effect targets** e **Sharpening (FSR1)**. Flares,
  Render scale, frame rate, HUD e as demais linhas são compartilhadas.
- Trocar o Renderer grava o `game-changes.toml` e mostra o aviso de restart; a lista **só muda depois de reiniciar**,
  como você pediu. Nada de fingir que a linha do outro renderer está ativa.
- Prova headless: kit `private/slice444-kit` com os logs/frames dos dois conjuntos. O conjunto CoreGS foi provado com o
  switch de desenvolvedor `GT4_RECOMP_ACTIVE_RENDERER=hw`, porque o backend CoreGS em processo não carrega save states
  (o switch só muda a lista, não o renderer).
- Confirmação e setters live continuam para as próximas fatias; nenhuma página/função existente mudou.
