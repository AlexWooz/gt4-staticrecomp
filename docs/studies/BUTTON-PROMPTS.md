> **Study by Luan Antony**, from pull request #166 (issue #161), imported on 2026-11-04 at the owner's word.
> Task numbers inside are the author's own numbering (TASK-145); in our TASKS.md this study belongs to TASK-165.
> It is a study, not a decision: anything it proposes is checked again before it is built.

# In-Game Xbox & Custom Controller Button Prompts Architecture (TASK-145 / Issue #161)

## 1. Executive Summary & Problem Scope

Gran Turismo 4 was developed natively for the PlayStation 2 and was designed exclusively around the DualShock 2 controller. All user interface menus, navigation bars, driving mission instructions, license test prompts, and in-race HUD indicators render hard-coded PlayStation button glyphs:
$$\text{Cross } (\times), \text{ Circle } (\bigcirc), \text{ Square } (\square), \text{ Triangle } (\triangle), \text{ L1/R1, L2/R2, Start, Select}$$

With GT4Recomp enabling native PC gameplay on Windows, Linux, and SteamOS (Steam Deck), a significant portion of players use Xbox controllers, Steam Deck built-in controls, or third-party PC gamepads. In [Issue #161](https://github.com/moesuito/gt4-staticrecomp/issues/161), community contributor *ryuzearaki* requested an option in the launcher to toggle in-game button prompt UI between PlayStation and Xbox layouts, referencing similar functionality in the *Crash Team Racing Nitro-Fueled* PC recompilation (*OCTR-NitroFueled-PC*).

This document establishes the technical feasibility study, asset pipeline, and launcher integration architecture for **TASK-145** ("In-game Xbox / PC button prompt option").

---

## 2. Gran Turismo 4 Button Prompt Rendering Mechanics

### 2.1 Where Button Prompts Appear
Button glyphs are ubiquitous throughout the game's presentation layer:
1. **Bottom Menu Navigation Bar:**
   * Found in almost every menu screen (GT Mode, Arcade, Garage, Dealerships, Settings).
   * Displays primary action confirmations: `"Enter: [Cross]"`, `"Cancel: [Circle]"`, `"Help/View: [Triangle]"`, `"Options: [Start]"`.
2. **License Tests & Driving Missions:**
   * Action prompt guides instructing the player when to accelerate (`[Cross]`), brake (`[Square]`), shift gears (`[R2]/[L2]`), or use the handbrake (`[Circle]`).
3. **In-Race HUD & Telemetry:**
   * Real-time indicators for reverse gear engagement, rearview mirror toggle (`[R1]`), ghost car toggle, and split-time cycling.
4. **Replay Viewer & Photo Mode:**
   * Transport controls (Play/Pause, Fast Forward, Camera Cycle, Shutter).

### 2.2 Texture Representation and Storage
In the original game disc:
* Button icons are compiled within **Graphic Picture Book (`.gpb`)** UI trees located in `GT4.VOL` (e.g. `projects/<menu>/<menu>.gpb` and `projects/common/`).
* Individual glyphs are stored as compact 8-bit paletted textures (`PSMT8` with CLUT) or 32-bit RGBA textures (`PSMCT32`).
* When a menu or HUD element is drawn, the Emotion Engine submits textured primitives (Sprites and TriangleFans) across PATH3 to the Graphic Synthesizer (GS).
* In GSdx (`gt4-gs.exe`), the texture cache hashes each uploaded VRAM texture surface:
  $$\text{Texture Hash} = \text{CRC32} / \text{Murmur3}(\text{width}, \text{height}, \text{PSM}, \text{pixels}, \text{CLUT})$$
  Because the button glyphs in GT4 are static bitmaps with invariant pixel data, their texture hashes are **100% deterministic and constant** across all sessions and hardware.

---

## 3. Architectural Options & Comparative Evaluation

| Option | Description | Engine Impact | Launcher Impact | Verdict |
|---|---|---|---|---|
| **A. Texture Replacement Seam (GSdx `[Folders] Textures`)** | Substitute glyph textures via GSdx texture replacement hashes | **Zero code changes** to game executable | Add dropdown in Settings > Controls; dynamic profile linking | **Recommended (Phase 1, Release 1.1.0)** |
| **B. `GT4.VOL` Virtual Sector Overlay (TASK-057)** | Host-side sector interception in `disc::read` replacing `.gpb` files | Zero ISO mutation; requires virtual TOC | Launcher manages overlay directory | **Recommended for CoreGS 2.0 (Phase 2)** |
| **C. Runtime Guest RAM/VRAM Poking** | Overwriting texture memory dynamically at runtime | High risk; fragile; induces pipeline stalls | Unbounded | **Rejected** (Violates AOT invariants) |

### 3.1 Why Option A Is the Superior Architecture for Release 1.x
1. **Zero Game Modification:** GT4Recomp's game executable (`gt4vulkan.exe`) remains completely pristine. No AOT units (`unit_XXXX.cpp`) are touched.
2. **Built-in Renderer Support:** GSdx already has native support for texture replacement (`LoadTextureReplacements = true`, [`test_gs_live.py`](file:///c:/Users/Antony/Desktop/gt4-staticrecomp/tests/python/test_gs_live.py)).
3. **Clean-Room & Decision 0070 Compliant:** Operates strictly on image assets placed in the textures directory, preserving the clean boundary between the MIT launcher/game and the GPL renderer.
4. **Instant Reversibility:** Switching between PlayStation, Xbox, and Nintendo Switch layouts requires only changing which replacement subfolder is linked, taking effect immediately on the next game boot.

---

## 4. Detailed Implementation Architecture

```
+-------------------------------------------------------------------------------+
|                       LAUNCHER SETTINGS (gt4l-core)                           |
|   Settings > Controls: Button Prompts = "xbox"                                |
+---------------------------------------+---------------------------------------+
                                        |
                                        v
+-------------------------------------------------------------------------------+
|                     LAUNCH PLAN COMPILATION (launch.rs)                       |
|   Inspects button_prompts setting:                                            |
|   Links <data_dir>/prompts/xbox/* into <textures_dir>/replacements/prompts/   |
+---------------------------------------+---------------------------------------+
                                        |
                                        v
+-------------------------------------------------------------------------------+
|                      RENDERER PROCESS (gt4-gs.exe)                            |
|   Loads texture replacements from [Folders] Textures                          |
|   Replaces PlayStation button hashes with clean Xbox (A/B/X/Y) icons          |
+-------------------------------------------------------------------------------+
```

### 4.1 Button Hash Crosswalk (PlayStation to Xbox Mapping)

| PS2 Digital Function | Original PS2 Glyph | Standard Xbox Glyph | Nintendo Switch Glyph |
|---|---|---|---|
| **Cross (Confirm / Gas)** | Blue Cross $(\times)$ | Green **(A)** | Red **(B)** |
| **Circle (Cancel / Brake)** | Red Circle $(\bigcirc)$ | Red **(B)** | Yellow **(A)** |
| **Square (Secondary / Handbrake)** | Pink Square $(\square)$ | Blue **(X)** | Green **(Y)** |
| **Triangle (View / Reverse)** | Green Triangle $(\triangle)$ | Yellow **(Y)** | Blue **(X)** |
| **L1 / R1 (Shoulders)** | `[L1]` / `[R1]` | `[LB]` / `[RB]` | `[L]` / `[R]` |
| **L2 / R2 (Triggers)** | `[L2]` / `[R2]` | `[LT]` / `[RT]` | `[ZL]` / `[ZR]` |
| **Start / Select** | `[Start]` / `[Select]` | `[Menu]` / `[View]` | `[+]` / `[-]` |
| **D-Pad / Left Stick** | D-Pad Arrows / L3 | D-Pad / Left Stick (LS) | D-Pad / Left Stick |

### 4.2 Launcher Core Extension (`gt4l-core`)
1. **Settings Schema (`launcher/crates/gt4l-core/src/settings.rs`):**
   * Add key: `"button_prompts"`, default value: `"playstation"`.
   * Allowed values: `["playstation", "xbox", "switch"]`.
2. **Session Launch Hook (`launcher/crates/gt4l-core/src/launch.rs`):**
   * When compiling `Pcsx2Paths`:
     * If `button_prompts == "xbox"`, ensure the prompt textures directory (`<data_dir>/prompts/xbox`) is linked or present in the textures search tree.
     * If `button_prompts == "playstation"`, ensure custom prompt links are removed or deactivated so the game renders default original graphics.
3. **UI Integration (`gt4l-tauri`):**
   * In **Settings > Controls**, add a dedicated control row:
     * **Label (en-US):** `Button Prompts`
     * **Label (pt-BR):** `Ícones dos Botões`
     * **Description (en-US):** `Chooses in-game controller button prompts (PlayStation, Xbox, or Nintendo Switch).`
     * **Description (pt-BR):** `Escolhe os ícones de botões nos menus e na corrida (PlayStation, Xbox ou Nintendo Switch).`
     * **Dropdown Options:**
       * `PlayStation (Default)` / `PlayStation (Padrão)`
       * `Xbox (A / B / X / Y)`
       * `Nintendo Switch (B / A / Y / X)`

---

## 5. Asset Provenance & Legal Safety

1. **Permissive Licensing (CC0 / MIT):**
   * The button prompt graphic assets will be sourced from permissively licensed open icon libraries (such as *Xelu's Free Controller Prompts*, CC0 Public Domain) or custom-vectorized by contributors.
2. **Zero Trademark Infringement:**
   * Avoid proprietary console hardware logos or trademarked font lettering.
   * Render clean, high-contrast, modern circular glyphs with standard color schemes (Green A, Red B, Blue X, Yellow Y).
3. **No Game Asset Redistribution:**
   * The replacement pack contains only modern host replacement textures. No original Sony or Polyphony Digital proprietary assets are extracted or distributed.

---

## 6. Acceptance Criteria & Quality Gates

| Requirement | Metric / Target | Validation Method |
|---|---|---|
| **Visual Replacement Fidelity** | 100% of menu bar & license prompts display selected prompt style | Manual and automated visual audit in Arcade and GT Mode menus. |
| **Alpha Transparency & Blending** | Zero dark halo or edge fringing around circular button icons | Inspection at 1x, 3x, and 6x internal render scales. |
| **Instant Reversibility** | Switching to "PlayStation" immediately restores original PS2 glyphs | A/B verification across consecutive launcher boots. |
| **Cross-Platform Parity** | Works identically on native Windows and SteamOS / Linux | Verified on Windows x86-64 and SteamOS (Proton GE). |
| **Performance Invariant** | 0.0 ms impact on frame rate or frametime consistency | F8 performance probe; texture cache size overhead $< 2\text{ MB}$. |

---

## 7. Phased Implementation Roadmap

* **Phase 1: Texture Hash Harvesting & Asset Pack Creation:**
  * Run headless menus and race captures with `DumpReplaceableTextures = true`.
  * Catalog the exact texture hashes corresponding to GT4's button glyphs.
  * Package CC0-authored Xbox and Switch replacement PNGs into `assets/prompts/xbox/` and `assets/prompts/switch/`.
* **Phase 2: Launcher Core Settings & Session Wiring (`gt4l-core`):**
  * Add `button_prompts` setting to schema and migration chain.
  * Wire dynamic linking / folder activation in `launch.rs`.
  * Add unit tests in `launcher/crates/gt4l-core/tests/launch_contract.rs`.
* **Phase 3: Launcher UI Controls Page (`gt4l-tauri`):**
  * Expose dropdown in Settings > Controls with localized labels in `en-US.json` and `pt-BR.json`.
  * Verify spatial navigation focus behavior.
