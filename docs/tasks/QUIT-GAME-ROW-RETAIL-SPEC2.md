# The Quit Game row in Retail v2.00 and Spec II

TASK-212 (Retail v2.00) and TASK-212b (Spec II), 2026-11-09. This is the recipe a future agent repeats: one data patch,
one host watcher, one exit path. Read `GT4-MENU-QUIT-SETTINGS-STUDY.md` first for how the menu and the quit path were
found.

Labels: **C** = Confirmed (headless run, test or code), **H** = High confidence, **Hy** = Hypothesis, **U** = Unknown.

## 1. What is shared

| Piece | File | Shared? |
|---|---|---|
| Patch builder | `scripts/gt4_data_patch.py` | yes, with `--target auto` |
| GT4.VOL reader | `scripts/gt4_volume.py` (`opened_volume`) | yes: v2.00 pin, Spec II TOC `0x115B800` and odd page parity |
| Runtime overlay | `native/data_patch.*`, `scripts/data_patch_overlay.inc`, `slice42x-*-runtime` capsule | yes (`disc.cpp` + `system.cpp`) |
| Row watcher | `native/menu_quit.*` | one algorithm, one profile per target |
| Exit path | `native/window.cpp` (`WM_CLOSE`, card/audio/F8 flush) | yes |
| Launcher classification | `launcher/crates/gt4l-core/src/exit.rs` | yes |
| Test harness | `native/menu_quit_fixture.cpp`, `native/menu_quit_spec2_fixture.cpp`, `tests/python/test_gt4_data_patch.py` | yes |
| Build capsule | `scripts/prepare_perf_runtime.py --mtvu --gs-stream --frame-cost --data-patch` (+ `--spec2-iop` for Spec II) | yes |
| Widget text | `hidden::Quit`, `TextFace` `hb_24`, skew `{-16 0}`, scale `0.9`, shadow `2` | same text/style, different anchors |

### 1.1 The data patch

`scripts/gt4_data_patch.py` decodes the target file from the **user's own disc** with the reviewed RoFS reader, applies
text replacements (exactly one anchor each), minifies whitespace outside string literals, re-encodes the payload to the
**original stored extent** and refuses growth. It never writes a new TOC: a same-length replacement leaves every file
size untouched. The output is one `GT4DPAT1` container plus a JSON report of hashes and offsets; neither contains game
data.

`native/data_patch.cpp` (installed by the capsule after the disc opens) loads the container named by `GT4_DATA_PATCH`,
verifies every original range's SHA-256 against the image and rewrites the overlapping bytes in `disc::read`. A patch
built from another disc is refused before a byte is replaced. The container is private, derived game data and never
ships.

```
private/tooling-venv/Scripts/python.exe scripts/gt4_data_patch.py ^
  --iso "<the user's disc>" --recipe <private recipe>.json --output <fresh private dir>
```

The builder auto-detects the target from `SYSTEM.CNF`: `SCUS_973.28` -> v2.00, `SCUS_974.36` -> Spec II. v2.00 keeps
the whole-disc pin; Spec II checks the boot ELF hash (`docs/inputs/spec2-elf.json`) and the volume geometry, because the
base and PT-BR Spec II volumes differ in size. In both cases the runtime's per-range SHA-256 is the final gate.

### 1.2 The widget

The hidden composite gains one child. The label is a literal `TextFace`, not a `key` (a key needs a text table entry
and a script module; the row has no script):

```
Composite {
    name string {"Quit"}
    geometry rectangle {0 0 320 24}
    children [1] {
        TextFace {
            name string {"label"}
            skew vector {-16 0}
            font string {"hb_24"}
            scale_x digit {0.9}
            scale_y digit {0.9}
            shadow digit {2}
            text string {"Quit Game"}
        }
    }
}
```

`has_script` is omitted on purpose. The menu script's `button_list` is a fixed list of widget references in the script
heap, not an iteration over `hidden.children`, so the widget alone creates no row; the host appends it (section 1.3).
**C** on both targets: the hidden composite parses with one extra child, the new row renders in the game's font, and the
script never dispatches a missing module because the host hides Cross/Circle while the row is focused.

### 1.3 The append and the Cross capture (`native/menu_quit.*`)

The watcher runs once per VBlank after the pad is applied (`window.cpp: service_menu_quit`). It scans guest RAM for the
main menu's SelectBox, then for the hidden composite with the patched template, injects once, and tracks the focused
row. When the last row is focused it hides Cross and Circle from the guest; a Cross rising edge posts `WM_CLOSE`
(windowed) or `ps2::halt("quit game")` (headless). A cached tree is reused; when the menu reloads, the vtable check
fails and the scan repeats.

The two targets differ in the object model (table in section 2). Every difference is a field of `Profile` in
`menu_quit.cpp`; the algorithm, the Cross edge, the mask and the exit are shared. The scan is throttled on Spec II
(15 VBlanks) because it has no cheap menu-state word; v2.00 keeps the `arca` gate and scans every VBlank while the
arcade project is current.

### 1.4 Exit path and launcher

`WM_CLOSE` is the same path as Alt+F4/F9: input recorder finish, `ps2::halt("window closed")`, card
`flush_cards_now()`, audio close, F8 probe finish, overlay/achievements shutdown. The row exists only on the arcade top
menu, so no save can be in progress. Headless it halts with `quit game`; `exit.rs` classifies `window closed`,
`quit game`, `Exit` and `program asked to run another executable` as normal closes (exit 0 only). Do not
`std::exit`/`TerminateProcess`.

**Launcher integration (planned, not in this test build):** during "Preparing game files", build the patch from the
user's own disc (the same builder, target from the disc), write it under the game folder's private data, pass its path
as `GT4_DATA_PATCH` when starting `gt4.exe`/`gt4-spec2.exe`, and rebuild it whenever the disc identity changes. The
patch is never downloaded, uploaded or committed; a missing patch simply leaves the menu without the row.

### 1.5 Limits

- **Same length only.** The builder refuses a replacement that does not fit the original stored extent. Minification
  makes room for the extra widget; a larger feature needs a new TOC (not built) or a script-side change.
- **Menu reach.** The row appears whenever the game shows the arcade top menu. A save that boots into GT mode or
  straight into the arcade game menu does not show it. The headless tests load a runtime state saved at the top menu;
  the card is never modified.
- **Attract timer.** The arcade TopRoot starts its demo after ~20 s without input (retail observation). Any pad press
  resets it. The watcher does not hold it off while the row is focused.
- **Owner test.** The windowed controller test (scroll to Quit Game, Cross, normal close) remains the owner's gate.
- **No game logic.** The row changes no physics, timing, AI, save or progression. The patch is data; the host only
  appends a widget and closes the window.

## 2. The two targets

### 2.1 Retail v2.00 (SCUS-97328, Adhoc v5)

| Thing | Value | Conf. |
|---|---|---|
| Project | `projects/arcade/arcade.mproject`, `TopRoot` -> `StartScroll`/`SelectBox` | C |
| SelectBox vtable | `0x00671898` | C |
| Row (MComposite) vtable | `0x00662EF8` | C |
| SelectBox geometry | `+0x34` = `{-64,16,460,36}` | C |
| Rows / first / last | `+0xA0` / `+0xA4` / `+0xA8` | C |
| Index / visible end | `+0xBC` / `+0xC4` | C |
| Row next / parent | `+0x5C` / `+0x14` | C |
| Row geometry | `+0x34` = `{64, 6+24*i, 320, 24}` | C |
| Hidden composite | 8 children (7 + Quit), count `+0xA0` | C |
| Menu state | `[[0x00622F4C] + 0x3A3BC] == "arca"` | C |
| `arcade.mproject` record | PS2ZIP kind 2, 13,646 stored / 205,792 decoded | C |
| Volume offset / absolute | `45,195,264` / `262,035,456` on the v2.00 disc | C |
| Patch container | 13,750 bytes, SHA-256 `809ba67b18f2b2ca84739e5f9f450e756b27153802ecda370844108b8aafee45` | C |
| Capsule | `private/slice422-data-runtime` (`disc.cpp`, `system.cpp`) | C |
| Kit | `private/slice421-kit` | C |

Recipe anchors (private): hidden `children [7]` -> `[8]`, and the `Credit` template block plus the `Quit` TextFace.
The headless state trick: a save whose `major_menu_project` is `gtmode` boots into GT mode; the test state's RAM copy
of that six-byte string was patched to `arcade` before the boot read it (the card itself untouched).

Run (headless, no window):

```
set GT4_DATA_PATCH=<kit>\gt4-data.patch
gt4vulkan.exe SCUS_973.28 --ntsc --disc "<v2.00 ISO>" --frames 13200 --memcard <copy>.ps2 ^
  --load-state state-arcade.bin --press 12350:down:2 ... --press 12950:cross:2
```

Expected: `[data-patch] ... verified against the image`, `[menu-quit] Quit Game row appended: 7 rows, last index 6`,
`[menu-quit] Quit Game selected`, `[ee] halted: quit game`, exit 0.

### 2.2 Spec II (SCUS-97436, Adhoc v7, public-beta lineage)

The Spec II executable is the beta lineage plus a plugin segment; the menus are **data/scripts inside `GT4.VOL`**, not
code in the ELF. The arcade project is Adhoc **v7**; `arcade.adc` is 401,983 bytes and `arcade.mproject` is 216,146
bytes (raw, kind 1). The game is one plain ELF (no `CORE.GT4`); the derived image is `private/slice272-spec2-elf/SCUS_974.36`
(6,520,944 bytes, SHA-256 `36f60b1a...`). The base and PT-BR v1.10 volumes differ in size; the boot ELF is identical.

| Thing | Value | Conf. |
|---|---|---|
| Project | same `projects/arcade/arcade.mproject`, `TopRoot` -> `StartScroll`/`SelectBox` | C |
| SelectBox vtable | `0x006D7118` | C (state) |
| Row (MComposite) vtable | `0x006C3EA0` | C (state) |
| List-node vtable | `0x006C3AD8` | C (state) |
| SelectBox geometry | `+0x24` = `{-64,16,460,36}` | C |
| Rows / first / last | `+0x90` / `+0x94` / `+0x98` (node bases) | C |
| Index / visible end | `+0xB8` / `+0xC0` | C (index moved when Down was pressed) |
| SelectBox node | widget - `0x28` | C |
| Row node -> widget | node + `0x28` | C |
| Row prev / next / parent (widget) | `+0x18` / `+0x1C` / `+0x14` | C |
| Row geometry | `+0x24` = `{64, 6+24*i, 320, 24}` (scroll moves the first y) | C |
| Hidden composite | node `0xEA83F0`, widget `0xEA8418`, geometry `+0x24`, count node `+0x20` = 11 (extra `+0x28` = 12) | C (state) |
| Hidden templates | 11 row nodes; Quit node `0xEA9DF0`, widget `0xEA9E18` | C (state) |
| StartScroll parent | node `+0x28` -> widget; geometry `+0x24` = `{160,332,320,68}` | C |
| Menu state | no reliable `arca` word found; structural gate (SelectBox + StartScroll + row layout) | H |
| `arcade.mproject` record | raw kind 1, 216,146 stored = decoded | C |
| Volume TOC | `0x115B800`, odd page-count parity | C |
| Volume offset / absolute | `226,906,112` / `3,217,780,736` on the PT-BR ISO | C |
| Patch container | 216,250 bytes, SHA-256 `6b20fde24ffde8c46936968f42cb26a20373cacb708d8eb88d43309def8ac857` | C |
| Capsule | `private/slice423-spec2-data-runtime` (`disc.cpp`, `system.cpp`, `sif.cpp`), pin `docs/inputs/slice423-spec2-data-runtime.json` | C |
| Kit | `Y:\gt4recomp-scratch\slice423-spec2-kit` (C: was full; build tree on Y:) | C |

**What could not be shared:** the vtables, every SelectBox/row/hidden offset, the node wrapper, the index word
(`+0xB8` vs `+0xBC`), the hidden count (11 vs 8), the scroll-tolerant row check, and the menu gate. The data-patch
builder, the runtime overlay, the widget text/style, the exit path and the launcher classification are shared.

**Adhoc v7 / beta difference:** Spec II's menus are the publisher's v7 scripts and widget trees on the beta lineage;
retail v2.00 is v5 with its own data. The approach is the reference, never the files. The plugin supplies behaviour for
the new options, not the menu rows.

**Spec II recipe anchors (private):** hidden `children [10]` -> `[11]`, and the `Achievements` template block plus the
`Quit` TextFace. The record is raw, so the replacement is padded to exactly 216,146 bytes with newlines after
minification (no PS2ZIP re-encode).

**Runtime evidence (headless, from the saved top-menu state):**

```
[data-patch] ...: 1 range(s), 216146 payload bytes, verified against the image
[menu-quit] Spec II watcher active
[menu-quit] Spec II: SelectBox 0x00A95E68, hidden 0x00EA83F0, template 0x00EA9DF0
[menu-quit] Quit Game row appended: 8 rows, last index 7
[menu-quit] Quit Game selected; closing the game
[ee] halted: quit game
```

Exit 0, frame 9,250 shows "Créditos da Mod" then **"Quit Game"** as the last row
(`quit-row.png` in the kit). The full boot (new card) finds a transient arcade TopRoot at `0xC66DF8` first, then the
current one at `0xA95E68`; both are the arcade menu at different project loads, and the quit comes from the current one.

**Build (plain, no PGO/ThinLTO; C: was full, so the tree is on Y:):**

```
cmake -S <worktree> -B Y:\gt4recomp-scratch\slice423-spec2-build\build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" -DGT4_TARGET=spec2 -DGT4_EE_PROFILE=spec2 ^
  -DGT4_ASSETS_ROOT=C:/Antigravity/gt4-staticrecomp -DGT4_PERF_RUNTIME_SOURCE=<assets>/private/slice423-spec2-data-runtime ^
  -DGT4_LICENSE_GUARD_SOURCE=<assets>/private/slice376-frame-render-license-guard-runtime ^
  -DGT4_PORT_DIR=<assets>/private/slice200-port -DGT4_SHADER_DIR=<assets>/private/slice153-pacing-shaders ^
  -DGT4_VU1_GENERATED_DIR=<assets>/private/slice397-vu1-gen ^
  -DGT4_VU0_GENERATED_DIR=<assets>/private/slice394-spec2-vu0-gen ^
  -DGT4_VU0_IMAGE_FILE=<assets>/private/slice390-spec2-vu0-image/vu0_image.bin ^
  -DGT4_VU_CORPUS_DIR=<assets>/private/slice155-vu1-full-corpus -DGT4_RELEASE_DISTRIBUTION=OFF
```

The capsule is derived with `prepare_perf_runtime.py ... --mtvu --gs-stream --frame-cost --spec2-iop --data-patch` and
pinned with `pin_frame_cost_runtime.py`. `native/CMakeLists.txt` accepts a `slice42x-spec2-*-runtime` capsule as both
the frame-cost family and the Spec II IOP carrier (it contains `sif.cpp` and `disc.cpp`).

**Kit (private, test only):** `gt4vulkan-spec2.exe` SHA-256
`a767af6e96e109cf988927270886beaf27d8674d68bacb20174d83e33818e58f`, `gt4-data.patch`, `recipe.json`, `report.json`,
`state-topmenu.bin`, `card.ps2`, `SCUS_974.36`, `shaders/`, `quit-row.png`, `quit-row.log`, `README.md`.

Run exactly as tested (from the kit, headless, create `frames` first):

```
set GT4_DATA_PATCH=%CD%\gt4-data.patch
gt4vulkan-spec2.exe SCUS_974.36 --ntsc --disc "<Spec II v1.10 ISO>" --frames 9500 --memcard card.ps2 ^
  --load-state state-topmenu.bin --frames-dir frames --frame-every 50 ^
  --press 8700:down:2 ... --press 9000:down:2 --press 9250:cross:2
```

The seven Down presses focus the eight row (index 7); the watcher masks Cross/Circle; the first Cross rising edge
halts with `quit game`.

**Tests:** CTest `native_menu_quit` (v2.00 layout, forced with `GT4_TARGET_FIXTURE_V200`) and
`native_menu_quit_spec2` (node layout), plus the full CTest 323/324 (one skipped) and Python 813 OK / 70 skipped
(`GT4_NATIVE_BUILD` = the Spec II build).

## 3. Add another row or setting

The same pipeline adds a different row:

1. **Widget:** add another child to the hidden composite (same 320x24 + `TextFace` style) and bump the hidden count in
   the recipe. Keep `has_script` off unless you also add a script module. For two rows, append both.
2. **Patch:** build the patch from the user's disc; the builder refuses growth. Extend the recipe with one anchor per
   new child.
3. **Host:** `menu_quit` appends the **last** hidden child. For N new rows, either append N templates (walk backwards
   from the hidden last) or add one "Quit Game" and one other row with a small queue in the profile. Keep the last index
   and the Cross mask logic; map the Cross action to the new host action instead of the quit.
4. **Action:** a host setting (fullscreen, renderer, audio) must write host config only, never the save's option
   struct (a real PS2 could not use it). A game-side setting needs a v5/v7 script edit and the script round trip
   (TASK-078 P1/P2), not this host append.
5. **Both games:** the same recipe shape works, but the anchors and the target profile differ. Re-derive the numbers
   from a runtime state; do not copy v2.00 offsets into Spec II.
6. **Tests:** a fixture that builds the target's synthetic tree, the headless frame, the Cross activation and the
   normal halt; then the owner's windowed test.

## 4. Unknowns / next

- **Spec II menu-state word:** not located. The structural gate (SelectBox vtable + geometry + StartScroll + row
  layout) is strong, but a transient duplicate arcade TopRoot is injected first in a full boot. **H** that both are
  the same arcade menu at different project loads; **U** whether a future Spec II build could show a second arcade
  SelectBox that is not the top menu.
- **Attract timer:** the headless runs are short; the owner's windowed test is the timing gate. **U** whether the demo
  timeout can fire while the row is focused during a long idle.
- **Index width:** the Spec II index is a small word at `+0xB8`; rows beyond 255 would need a different type. **Hy**
  not relevant for this menu (10 rows).
- **Launcher patch build:** not implemented here; the plan is section 1.4. **C** design, **U** the actual
  "Preparing game files" integration and disc-change invalidation.
