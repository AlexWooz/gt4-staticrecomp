# CoreGS 3.0: a native renderer for GT4's graphics, in the spirit of GT3 Revisited

Date: 2026-11-05. Planning and research document for the owner's request of 2026-11-05 (TASK-090). Nothing in the
repository was edited, built or run, and the game was not launched. This plan is meant to be copied into the repository
(for example `docs/coregs3/PLAN.md`, plus a `docs/tasks/` brief per phase) when the owner adopts it.

**Agent status: not PCSX2-tainted.** I opened no PCSX2 or GSdx source, no `gt4recomp-gs` fork, no `private/tooling/pcsx2-*`,
no `private/slice189-pcsx2-ref`, no `C:\Antigravity\armsx2`, and no celosia source (only our prose study of it). I read the
clean-room prose `docs/hw-renderer-spec/ORACLE-ARMSX2-MOBILE-GPU.md` as allowed. External links inside Nenkai's pages that
point at PCSX2 sources were not followed.

**Labels.** **C** = Confirmed (source, file:line or a named measurement), **H** = High confidence, **Hyp** = Hypothesis,
**U** = Unknown. Effort figures are always **Hyp**.

**Paths.** `docs/...`, `native/...`, `scripts/...` are in the rc15 worktree `C:\Antigravity\gt4-wt-rc15` (HEAD 26fbdeee).
"Decomp" is `C:\Antigravity\gt4-staticrecomp\private\slice151-research\gt4-decomp` at `e0b70c4d6` (MIT, research only).
"Nenkai" is `...\slice151-research\nenkai\` (PDTools `ffe0bb2`, MIT). "Translated unit" quotes are from the private v2.00
producer `C:\Antigravity\gt4-staticrecomp\private\slice299-ee-flare-producer\generated\unit_008F.cpp`. "Runtime capsule"
quotes are from `...\private\slice260-instrument-runtime\dmac.cpp`.

---

## 0. Summary

1. **Recommendation: interception at the EE level, at GT4's own GL-like draw call (approach A), run as a hybrid with
   CoreGS 2.0 (approach C).** Every 3D mesh in GT4 goes through one PGL function, `pgluCallShape` (0x0049D308), with the
   whole draw described by guest state: the shape (the model's own VIF packets, i.e. the asset data), the composed matrix,
   the material and texture records, the GS state shadow, and the pass the game is in (C: decomp
   `src/network/unit_00494578/func_0049D328.cpp`, `func_0049CEA0.cpp`, `func_0049E548.cpp`). CoreGS 3.0 reads those
   assets and that state directly and draws them with a modern pipeline: real colour/depth attachments, hardware
   blending, hardware clipping, float depth, MSAA. CoreGS 2.0 keeps drawing whatever is not native yet (2D, HUD, menus,
   effects still pending) and, in a shadow mode, keeps answering every read-back the EE consumes exactly. The VU1
   microcode is never ported as a render path; its output is used only as a **verification oracle** (the "VU1 tap").
2. **Why not VU1-input interception (B) as the main path:** it reproduces the VU1 output, which is GS screen
   coordinates, GS targets by address and GS read-back semantics. That is still "imitating the GS" (the owner's request
   rules it out), it has no knowledge of objects, passes or materials (so no native effects, motion vectors or
   interpolation identity), and its mobile GPU cost stays that of GS emulation.
3. **Game logic stays exact by construction, then by measurement.** Phase 1 hooks are observe-only host calls (no guest
   cycle is added or removed: guest time is retired EE cycles, C: `docs/hw-renderer-spec/STUDY-ASYNC-READBACK.md` §1,
   item 2).
   Later "replace" modes skip VU1 programs on the VU1 worker only, which the guest cannot see (MTVU keeps VIF1 state exact
   on the EE). Every phase is gated on the existing per-VBlank simulation digest, race clock and state dump
   (`native/frame_skip.cpp:122-132`) over recorded drives.
4. **Phase 1 (smallest visible milestone): the cars of a race, drawn natively, injected into CoreGS 2.0's frame.**
   Observe-only hooks capture the car pass of the main view; a new MIT module `native/coregs3/` decodes the shapes and
   textures from guest RAM, renders them with Vulkan at CoreGS 2.0's internal scale and merges them into CoreGS 2.0's
   frame target right after the game's car pass (depth-tested), so smoke, flares, HUD and post effects still draw on top.
   It is tested headless on recorded drives with five automatic gates (identity off, identity in shadow mode, per-vertex
   VU1-tap match, car-mask image metrics, performance) and produces milestone screenshot sheets automatically.
5. **Biggest risks:** (a) the VU1 lighting, fog and texgen formulas must be re-derived per program family (the tap
   makes it measurable, but it is the hardest part); (b) mixing native and GS drawing inside one frame (depth, order,
   effects that read the frame); (c) the long tail of effects and modes (flares, env map, wet reflection, mirror,
   replays, photo, split screen); (d) CPU cost: the guest still runs its whole render code on the EE, so weak EE-bound
   PCs gain little until later phases.
6. **Effort (Hyp):** about 55-90 agent-weeks to a fully native race world with modern features, plus optional native
   menus. Phase 1 is 5-7 agent-weeks. Calendar time depends on review throughput, not on agent quota.

---

## 1. GT3 Revisited and other native-renderer reimplementations

### 1.1 GT3 Revisited: what it is

- **Identity (C):** `github.com/matt-pulsipher/GT3-revisited`, MIT (GitHub API: `license MIT`), created 2026-10-17, first
  public commit "GT3 Revisited" on 2026-10-18, release 0.4.0 "GT3's own race physics" on 2026-10-28 (commit list read with
  `gh api`). Description: "Gran Turismo 3 A-Spec engine reimplementation for modern platforms (bring your own disc)".
  Platforms: Windows, macOS, Linux, Android, iOS, Meta Quest (README).
- **"No original code remains" (C in substance):** `docs/FORMATS.md` says of `CORE.GT3` that "The engine **never
  executes this code**"; it reads it only to identify the release and to check its built-in tables against the disc.
  `CONTRIBUTING.md` rule 1: "Never commit the game program ... including listings of its code". The README calls it "an
  independent reimplementation". The press (Android Authority, 2026-10-21) called it a "recompilation" built "with Claude
  AI assistance"; the repository's own wording is reimplementation. Whether private pre-release work started as a
  recompilation: **U**.
- **How the game logic was redone (C, `docs/dev/PHYSICS.md`):** the race step is "written in the PS2's single-precision
  arithmetic (`ee::F`, `ee::D`, `ee_math`) so that a race is step for step the game's: the 'exact core'". Behaviour is
  recorded in prose (`docs/behaviour/`), code comments cite those sections. This is a full re-implementation of game
  logic, which is **out of scope for GT4Recomp** (AGENTS.md: a decompilation-based source port and changes to physics are
  out of scope). Only the rendering lessons transfer.
- **Assets (C, README, `docs/ARCHITECTURE.md`):** read from the user's disc image at run time, "Nothing is extracted to
  disk or redistributed"; `gt3tool` extracts only on the user's request. Disc tests run only when `GT3R_DISC` points at
  the user's own image and skip otherwise (CONTRIBUTING.md "Tests").

### 1.2 GT3 Revisited: how it renders (C, `docs/dev/RENDERING.md`, `docs/formats/car_model.md`)

- **Backend:** SDL_GPU (Vulkan, Metal), GLSL compiled offline to SPIR-V/MSL/GLSL ES, an own OpenGL ES 3.0 back end for
  PCs without Vulkan. Each 3D view renders to an offscreen RGBA8 target that the 2D UI composites.
- **Models:** GT3's `GTM1` models are decoded from their VIF packets directly: the UNPACK layout per VU address is
  documented (positions V3-32 at 0x00, strip table S-8 at 0x40, normals V3-32 at 0x80, colours V4-8 at 0x80, UVs at 0x40,
  morph key frames), plus a byte-coded "node program" per model (LOD switch, matrices, render-state ops that set GS TEST,
  ALPHA, FBMSK, ZMSK, culling). GT4's ModelSet2 is the same lineage with a larger opcode set (§4).
- **Textures:** "PS2 textures only make sense through GS memory": uploads are replayed into a GS-memory model and read
  back through the real swizzle; car paints are CLUT selections (`TEX0.CSA`/`CBP` patch tables).
- **Lighting:** the VU formula re-implemented per vertex in `model.vert` (emissive + ambient + diffuse + specular with
  GT3's infinite viewer, clamped to 255/128), colours interpolated linearly in screen space like GS Gouraud shading, and
  the fragment stage doing **GS 8-bit integer arithmetic** (`gs_texel`, `gs_color`, `gs_fog`): with float sums "the race
  frame came out 1 to 3 levels above the original's ... (1.015 of it on average, now 0.998)".
- **Clipping detail:** GT3's VU clips at a 2x guard band, so colours on clipped triangles differ; they reproduce it for
  course models (CPU guard-band clip of the triangles that cross it).
- **Depth:** reversed float depth with every pass tested GREATER_OR_EQUAL, mirroring the PS2's 24-bit Z where the maximum
  is near; second passes over the same vertices pass on equal depth; decals stay stable at telephoto distances.
- **Effects:** car reflections from a sphere map rendered every frame above the focus car (they render a cube and
  quantise to 5 bits to match the PSMCT16 map), destination-alpha passes (DATE) emulated with a frame-alpha channel,
  AFAIL RGB_ONLY foliage drawn in two phases, colour grade in 8-bit table arithmetic, replay depth of field, heat haze,
  sun glare and lamp glows as overlays, tyre smoke drawn late with scene depth. Motion blur and some course effects are
  not reproduced ("Known approximations").
- **Performance practice:** merge consecutive primitives into one draw (draws -33%, scene CPU -25%), warm ~110 pipelines
  on a background thread at start (65-200 ms per pipeline on phones), deduplicate textures (Mali ~185 KB per texture
  object), dynamic resolution and 2x MSAA on phones, a Mali-G52 driver crash avoided by hand-made screen-linear varyings.

### 1.3 Other relevant projects (lessons only; no code taken)

| Project | What it does (source) | Lesson for GT4 |
|---|---|---|
| celosia, SotC PS2 port, GPL-3.0 (prose only: `private/research/celosia-study.md` and `docs/COREGS3-CELOSIA.md`; source never read) | Recompiled EE; renderer fed at `modelDisp` from the game's own model/LOD/material/VIF structures in RAM; content-hash GPU caches; **50 of 50 tracked features native by 2026-11-09**, no GS/VU1/VIF on the drawing path; `SOTC_NATIVE=2` draws natively without showing it; GS raster skipped but DMA chains walked for SIGNAL/FINISH/LABEL; all settings live | Same level as approach A; shadow mode first; the GS quirks and effect-chain traps they had to reproduce are our Phase 1/2/4 checklists (§3-§4 of the note) |
| OpenGOAL, Jak and Daxter (opengoal.dev "porting tfrag") | First built slow renderers matching the PS2 exactly (XGKICK output to a "DirectRenderer" for debugging), then a fast PC renderer from game data structures with offline preprocessing; GPU does transform and clipping; "you can often leave out huge chunks of code" | Keep a faithful oracle (CoreGS 2.0 plus the VU1 tap), then replace; reduce draw calls |
| RT64 / Zelda64Recomp (github.com/rt64/rt64 README) | Replays matched draw calls at a higher rate with interpolated, decomposed transforms | Draw identity across ticks is the key to above-60 presentation (Phase 6) |

### 1.4 What transfers to GT4, and what does not

**Transfers:**
- assets read from the user's own disc (here: from the game's own loaded copies in guest RAM), never stored in the repo,
  disc-dependent tests that skip without a disc;
- decoding textures by replaying the uploads into a GS-memory model (we already own MIT swizzle and CLUT code in
  `native/hw_gs/memory/` and `native/hw_gs/clut/`);
- VU lighting re-derived as a per-vertex formula, with optional GS integer arithmetic in the fragment stage for level
  accuracy;
- reversed float depth with GEQUAL, destination-alpha emulation, two-phase AFAIL draws;
- per-frame env map rendered natively at higher resolution;
- draw merging, background pipeline warm-up, texture deduplication, dynamic resolution and MSAA on phones;
- offscreen 3D target composited under the 2D layer;
- **the GS rules and effect-chain traps celosia had to reproduce** (alpha-test depth, untextured colour, blend
  rounding, `COLCLAMP`, fog, texture LOD, UV row, bloom history): `docs/COREGS3-CELOSIA.md` §3-§4. Add them to the
  Phase 1/2/4 acceptance lists.

**Does not transfer:**
- rewriting the game logic (GT4's logic stays the recompiled original: owner rule);
- GT3 formats as such (GT4 uses compressed 16-bit positions and a larger ModelSet2 opcode set);
- SDL_GPU as a dependency (we already have raw Vulkan with CoreGS 2.0's loader, device profile and glslang pipeline);
- GT3 Revisited's AI-generated code base as a source (MIT would allow reuse with attribution, but GT3 formats differ and
  provenance is uncertain; ideas only).

---

## 2. What the repository already has (the starting point)

| Asset | Where | Status |
|---|---|---|
| Feasibility study of a native renderer at `pgluCallShape` (H1..H23 hook table, render passes, GS tricks, VU1 census) | `private/research/gt4-native-renderer-study.md` (= `docs/native-renderer/STUDY.md` on branch `native-renderer/main`, 495e3ff8) | C, 2026-10-17; folded into TASK-090 by `_rules/project-rules` |
| VU1 study (one GIF shape dominates, 70-80% of race vertices are ADC no-draw, hot families A/B, relaxed "CoreGS 3.0" option) | `private/research/vu1-direct-gpu-study.md` | C, 2026-10-22 |
| Read-back inventory (four call sites of `func_004A5178`; the flare table is consumed only by VU1; photo autofocus is read by the EE) | `docs/GS-READBACK-STUDY.md` §1-2 | C |
| Hooks in translated units at a `jal` or a label, with byte-exact receipts | `scripts/camera360_units.py` (TASK-159/160/161/095) | C, in use |
| Runtime dispatch hooks (indirect calls, e.g. `RaceDisplay::render` 0x0039F498, camera `initMonitor` return 0x00388B38) | `scripts/display_aspect_overlay.inc` | C, in use |
| Shape-call observer (bounded tokens, epochs, exports) at the H1 frontier 0x00455A5C/0x00455A64 | `native/shape_observer.*`, `docs/SCENE-OBSERVATION.md` | C, slices 127-139 |
| Offline readers: GT4.VOL catalogue and PS2ZIP, MDLS/shape/material/texture record bounds, VIF framing | `scripts/gt4_volume.py`, `scripts/gt4_model_records.py`, `scripts/gt4_vif_framing.py`; `docs/VOLUME-READER.md`, `MODEL-RECORDS.md`, `VIF-FRAMING.md` | C, slices 120-126 |
| Recorded drives, headless replays, frame dumps, simulation digests, state dumps, frame diff/compare/sheet tools | `scripts/coregs2_bench/` (README), `native/frame_skip.cpp:122-132`, `private/coregs2-bench/inputs/` | C, in use |
| CoreGS 2.0 (in-process MIT GS renderer: targets at scale, SLM, readbacks, tone hook, flare deadline) | `native/hw_gs/` | C |
| Pinned glslang shader toolchain | `native/hw_gs/CMakeLists.txt:177` | C |

---

## 3. The interception level

### 3.1 The three candidates

**(A) EE-level scene interception.** Hook GT4's own render calls in the translated code and draw the original assets
natively.
- Geometry: the shape passed to `pgluCallShape` (0x0049D308 → 0x0049D328(cmd, 0x70002000)); its items are DMA REF
  ranges pointing at the model's own VIF packets in guest RAM (C: `func_0049D328.cpp:32-34`, `p->a=item->count|0x30000000;
  p->b=item->addr`). These packets are the model file's data as loaded from disc.
- Transform: the matrix block the PGL uploads to VU1 address 0xC1: VU0 `vf24..vf27` (rows 0-2 scaled by the shape's
  `Cmd.value`, row 3 unscaled) plus a second 4-qword block from the PGL context +0xFA0 (C: `func_0049E548.cpp`, the
  `0x6C08C000|reg` UNPACK; role of the second block **U**). Camera projection and modelview are set right before
  (C: translated unit labels `L_0033EBB4` after `applyProjection`, `L_0033EBF4` after `applyModelview` in `func_0033EAB8`).
- Material and texture: the per-item callback `func_0049CEA0(event)` resolves a 9-bit texture index to a 40-byte
  PGLUtexture record (`D_0084AF58 + (a-1)*40`, or the external set) and a 7-bit material index to an 80-byte PGLUmaterial
  (`D_0084AF5C + (b-1)*80`) (C: `func_0049CEA0.cpp:6-30`); materials are written into the PGL context at +0xA60..+0xAA0
  (C: `func_0049E228.cpp`).
- GS state: the PGL shadow of GS registers in scratchpad 0x70002000 (blend +0x198, colour mask +0x174, depth +0x188,
  texture/clamp/scissor +0x1A8.., C per the study's §2.2 table).
- Pass and object context: `RacePS2Base::render_world` and the main pass `func_0033EAB8` call sky, course, reflection,
  cars, mirror and flares in a known order (C: decomp `func_0033EAB8.cpp:80-172`; cars at line 128).

**(B) VU1-input interception.** Take the unpacked VIF data and VU1 constants at each MSCAL and run equivalent shaders on
the GPU (exact or relaxed), feeding a GS-like raster or CoreGS 2.0 (the earlier TASK-090 framing,
`vu1-direct-gpu-study.md` §4 and §7).

**(C) Hybrids.** (A) for meshes (cars, track, sky, billboards, env-map scenes) plus native rebuilds of each effect, with
CoreGS 2.0 drawing everything not yet native in the same frame; (B) only where (A) has no semantics.

### 3.2 Comparison

| Criterion | (A) EE-level, PGL draw call | (B) VU1 input | (C) = A first, CoreGS 2.0 for the rest |
|---|---|---|---|
| **Coverage** | Every ModelSet2 mesh through one hook (car, course, sky, billboards via `BillboardSet::render`, env-map scenes, menu cars): C callers 0x455A5C, 0x4507A4, 0x44E918, 0x44EE1C (study §2.3). Sprites, particles, flares, full-screen filters and 2D need their own hooks, one category at a time | Everything VU1 draws, automatically (strip families A/B ~92% of PATH1 qwords, C). 2D/PATH2/PATH3 not covered | Full game always complete: uncovered categories stay on CoreGS 2.0 |
| **Fidelity** | Geometry and textures identical in content (same data); lighting/fog/texgen re-derived per VU1 family, measured per vertex against the VU1 tap; native raster gives sub-pixel precision at any scale; effects rebuilt from game state | Geometrically closest to VU1 (exact mode possible, 8-14 weeks), but output is GS screen space; effects stay GS tricks (targets by address, readbacks) | As (A) where native, CoreGS 2.0 fidelity elsewhere |
| **Native effects, MV, interpolation** | Yes: object identity (shape, model, instance), per-object transforms for motion vectors, camera for jitter/aspect, pass knowledge for occlusion-query flares and real render targets | No object identity; motion vectors and interpolation need primitive matching; flares need the GS read-back model | Yes |
| **Effort** | High total (each effect and mode), but each step is visible and testable | B-exact 8-14 weeks just for the strip families, then still a GS raster; B-relaxed 6-12 months and still GS-shaped | (A) plus a thin coexistence seam |
| **Risk to game logic** | Zero with observe-only host calls (Phase 1); "replace" skips VU1 work only on the VU1 worker (guest-invisible, H); gated per VBlank | Zero (VU1 output is not guest-visible, H; `vu1-direct-gpu-study.md` §3.2) | Zero, same gates |
| **PC performance** | Removes, for covered draws, the VU1 worker work (3.5-8.5 ms per race frame, C) and the CoreGS 2.0 front end per kick (85k kicks per 480p race frame, ~80% ADC no-draw, C) once "replace" is on; GPU work becomes a forward pass with hardware blending (Hyp: well under CoreGS 2.0's interlocked uber-shader cost at 6x-12x). EE render code still runs (no gain on EE-bound PCs) | Removes VU1 CPU work; GS raster cost remains (interlock path) | Same as (A) in covered scenes |
| **Android (Adreno 740)** | Best fit: standard attachments and fixed-function blending, no per-primitive barriers (CoreGS 2.0 needs ~750 fragment barriers per VBlank there, C: `docs/IOS-MACOS-PORT-NOTES.md`), MSAA in tile memory, dynamic resolution | Still pays the GS-emulation raster on a tiler | As (A) |
| **Apple (MoltenVK)** | Standard pipeline, nothing depends on raster-order or interlock exposure (U on MoltenVK) | Depends on CoreGS 2.0's ordered-blend path for exactness | As (A) |

### 3.3 Recommendation

**(C) with (A) as its core.** Concretely:
- **Geometry, materials, cameras and passes:** captured at the EE level (H1 `pgluCallShape` plus the pass, camera,
  material-callback and object hooks listed in §5.2), decoded from the game's own loaded assets.
- **Effects:** rebuilt from the game's own state (FlareClass entries, scene parameters, the ramp CLUT), never from GS
  read-backs.
- **CoreGS 2.0 remains in the same process:** it draws all categories that are not native yet, 2D/HUD/menus and movies,
  and keeps every EE-visible read-back exact.
- **VU1 is used only as the oracle** that numerically validates the native vertex stage (per-draw GIF output captured
  between stream markers). No VU1 microcode is translated to shaders as a render path, so the "no JIT, AOT only" rule is
  trivially kept.

---

## 4. Asset path

### 4.1 Principle

- **Primary source: the game's own loaded copies in guest RAM, at draw time.** The game loads model, course and texture
  files from the user's disc and uses them in place (C for courses: Nenkai `formats/courses/gt4_coursedata.md`, "This file
  is completely loaded into memory, and used in-place"). Reading them through the draw-time pointers gives exactly what
  the game uses, after relocation, LOD choice, paint/CLUT patches and texture-table switches, with zero extraction.
- **Secondary source: the disc, read by our own bounded reader**, only for warm-up (pipelines, texture pre-decode at race
  load), the offline model viewer, and disc-dependent tests that skip without `GT4_DISC`. Nothing is ever written into
  the repository.
- **Caches:** GPU mesh and texture caches keyed by content hash (shape VIF bytes; texture transfer bytes plus CLUT bytes;
  material record bytes), LRU with a byte budget (mobile). An optional decoded-texture disk cache in the user's cache
  folder is an owner decision (§12).

### 4.2 Formats and what is already documented

| Asset | Format and on-disc place | Documented by | Our status |
|---|---|---|---|
| Volume | `GT4.VOL` RoFS ToC, PS2ZIP files | Nenkai `GT4_RoFS_Volume_TOC.bt`, hub `formats/volume/ps2_gt4_volume.md`; GT4FS (no licence: facts only) | Reader C (`scripts/gt4_volume.py`, 407 pages, 18,352 files, slice 120) |
| Car models | `car/lod/<model_code>` (race LODs), `car/menu/<code>`, `car/open/`; `.pat` colour patch; wheels/tyres/wings separate file sets | hub `ps2/gt4/file_structure.md`; `GT4_CAR4_CarInfo.bt`, `GT4_MDLS_ModelSet.bt`, `GT4_Pat0_ColorPatch.bt`; decomp `CarModel::getModelFileName` 0x004541E0.. | Ford Ka `fdeu0006` resolved via SpecDB (C, slice 121); garage 22 shapes/154 packets, race 113/312 (C, slice 122) |
| ModelSet2 (MDLS) | 0x80 header, model table at 0x38 (0x28-byte records, commands at +0x18), ~90 display-list opcodes (matrix ops, GS state, LODSelect, BBoxRender, callbacks, CallVM, tween, variable colour, texgen, `pgluCallShape`) | Nenkai `ModelSetupOpcode.cs` (MIT) | Executed by the game; we capture its `pgluCallShape` results |
| Shapes (PGLUshape) | 16-byte prefix, VIF descriptor array (offset, QWC, 9-bit texture / 7-bit material), VIF packets of UNPACKs; GT4 compressed positions at VU 0x8000 (16-bit, Nenkai divides by 4096), UVs 16-bit at 0x8040, strip reset bytes at 0xC040, 8-bit 3-component data at 0x8080 in 205/312 race packets (likely compressed normals) | Nenkai `PGLUshape.cs:107-248`; our `CHANNEL-SELECTION-AUDIT.md` | Framing C (CL/WL=1,1 assumption); channel roles partly U |
| Dequantisation | The per-shape float `Cmd.value` scales matrix rows 0-2 before upload | decomp `func_0049E548.cpp` | **H** (it folds the vertex scale into the transform) |
| Materials | PGLUmaterial 0x50: ambient, diffuse, specular, a 4th colour, shininess-like float | Nenkai `PGLUmaterial.cs`; decomp `func_0049E228.cpp` (4 qwords + float, global colour scale) | Bounds C; semantics H |
| Textures | Tex1: PGLUtexture 0x28 (TEX0, TEX1, MIPTBP1/2, CLAMP), GS transfer records 0x0C (often swizzled CT32 uploads of T4/T8 data), CLUT animation table at +0x24 (`pgluMapClutAnimation`) | hub `formats/texture/img_tex1_textureset.md`; GT3R FORMATS.md (same Tex1) | Bounds C; decode via GS-memory replay (to build) |
| Courses | `crs/<label>/` GT4CourseData: 0x100 header with main/sky/reflection/after/road-reflection ModelSet2s, VisionLists, BillboardSets, CourseEnvPtr environment, Runway, replay camera data | hub `formats/courses/gt4_coursedata.md`; `GT4CourseData.bt`, `GT4_BLBd_BillboardSet.bt` | Not decoded offline; captured live through H1 |
| HUD | `race_display/<code>/<locale>/display.gpb` (texture container) | hub file structure | U layout; HUD stays on CoreGS 2.0 until Phase 7 |
| Fonts | `font/*.fnt`, `*.kf` | Nenkai `Formats/PS2/GT4/Fonts` | U |
| Menus | `projects/` (Adhoc + strobe Flash-like runtime), `piece/`, `menu/` | decomp `knowledge/architecture.md` (strobe, mRenderContextPS2) | Stays on CoreGS 2.0 (Phase 7 optional) |
| Showroom/pit | `pit/` ScenePack, Concourse | decomp `Concourse::render` 0x00408490 | Phase 5 |

### 4.3 Texture identity

- **EE-side asset identity (recommended):** the material callback gives the PGLUtexture record address, so the native
  texture key is (record bytes, transfer payload hash, CLUT hash). This survives GT4's per-frame texture streaming
  (PATH3 carries about 2.9 MB per 480p race frame, C: vu1 study §2.4), and gives stable keys for texture replacement
  packs keyed by asset rather than by GS hash.
- **Validation mode:** decode the same TEX0 from CoreGS 2.0's 1x shadow memory (SLM) at the draw's stream position and
  compare the RGBA. This catches CLUT patches and CLUT animations that the record-only path misses.

---

## 5. Architecture of CoreGS 3.0

### 5.1 Layers (all MIT, `native/coregs3/`, untainted agents only)

1. **Hooks (EE thread, host-only, observe-only in Phase 1):** fixed storage, no guest writes, no guest calls, no cycle
   budget change. Inserted with the existing two mechanisms: translated-unit insertions at `jal` sites or labels (the
   `camera360_units.py` pattern, receipts proving removal restores the input byte for byte) and runtime dispatch hooks for
   indirect calls (the `display_aspect_overlay.inc` pattern).
2. **Scene snapshot (pure data):** per guest frame: camera blocks, passes, draw records (shape address, item ranges,
   matrix block, PGL state words, material and texture record copies, VU1 program entry `obj->count`), flare entries,
   particle lists, post-effect parameters. Immutable once published; a ring of 3.
3. **Stream markers:** at DMA time, the REF source addresses of the chain are matched against the frame's draw records,
   and in-order markers (`pass_begin/end`, `draw_begin/end` in capture mode) are posted into the GS stream journal. The
   DMA walker knows every REF tag's source address (C: runtime capsule `dmac.cpp:152-215`, `case 3` REF; `deliver()`
   passes a pointer into `g_ram`). This places native work at the exact stream position of the GS work it
   corresponds to, whatever the game's DMA kick timing.
4. **Decoders and caches (pure C++, CTest fixtures):** VIF shape → mesh, Tex1 → RGBA8 through a GS-memory replay (reusing
   `native/hw_gs/memory/` swizzle and `clut/`), material → uniform block.
5. **Renderer (Vulkan, shares CoreGS 2.0's `VkDevice` and queue):** pipelines from PGL state classes (blend, alpha test,
   destination alpha, depth func/mask, cull, fog), reversed float depth with GEQUAL, an RGBA8 colour target with a real
   alpha channel for destination-alpha tricks, an R32UI "GS-equivalent Z" output for merges into CoreGS 2.0, MSAA later.
6. **Coexistence seam with CoreGS 2.0:** a callback CoreGS 2.0's GS worker calls at a CoreGS 3.0 stream marker, handing
   over its command buffer and a view of the current frame target and Z target at scale.

### 5.2 Hook table (v2.00; addresses Confirmed unless marked)

| Id | What | Where | Kind | Reads |
|---|---|---|---|---|
| K1 | Draw call (H1) in `ModelSet2___render` | pre-call 0x00455A5C (`jal 0x0049D308`), continuation 0x00455A64 (`SCENE-OBSERVATION.md`) | unit insertion before the call | a0 = shape `Cmd*`; PGL context 0x70002000; VU0 vf24-vf31 |
| K2 | Material/texture per item | `func_0049CEA0` (called through `obj->callback`, indirect) | dispatch hook | a0 = event; `D_0084AF58/5C/60/64`, `D_00624C7C/80` |
| K3 | Camera projection set | label `L_0033EBB4` (after camera vt+0x10 in `func_0033EAB8`) | unit insertion at label | PGL projection state, camera object |
| K4 | Camera modelview set | label `L_0033EBF4` (after vt+0x18) | unit insertion at label | modelview, camera |
| K5 | Car pass begin / end (main view) | labels `L_0033EE28` (after vt+0x3D8) and `L_0033EE54` (return of `render_cars` vt+0x380 = 0x00340950) | unit insertions at labels | frame id, `self`, `car` and `target` arguments |
| K6 | DMA REF walk | runtime capsule overlay in `run_chain` (REF case) | runtime overlay | tag source address and QWC |
| K7 (later) | Other passes: sky `func_00396B80` (ret 0x0033EC28), course vt+0x370, reflection vt+0x378, mirror vt+0x3C0, flares `func_003B80E8/8848/88C8`, particles `func_003C2838`, post effects `render_post_effect` 0x0033F890, HUD `RaceDisplay::render` 0x0039F498 | decomp `func_0033EAB8.cpp`, study §2.5 | as above | per category |

Placing an insertion at the *start of a label's block* covers both the lexical return and a dispatched continuation after
a stack unwind (the `kMaxDepth` path, C: `SCENE-OBSERVATION.md` "direct depth spill and scheduler unwind").

### 5.3 Coexistence levels

| Level | Name | What CoreGS 3.0 does | What CoreGS 2.0 does | Guest impact |
|---|---|---|---|---|
| 0 | `shadow` | Captures and renders its categories offscreen, nothing shown; side-by-side and metric tools | Everything, presents | None (observe-only) |
| 1 | `overlay` | Injects its categories into CoreGS 2.0's frame target at the pass end, depth-tested, on top of the GS-drawn originals | Everything | None |
| 2 | `replace` | As overlay, and the VU1 worker skips the programs (and their XGKICKs) of covered shapes, so CoreGS 2.0 never draws them; native GS-equivalent Z is merged so depth consumers still see the geometry | Everything else | None to EE state (VU1 work is guest-invisible, H); visual-only read-backs (flare table) may change; gated |
| 3 | `own3d` | Owns the whole 3D frame (render_world) at display resolution and aspect; 2D/HUD comes from CoreGS 2.0 as an overlay layer split at `render_panel` | 2D, HUD, menus, movies; optionally a 1x shadow of the 3D for exact EE read-backs (photo) | None (same gates) |
| 4 | `native` | Also 2D/HUD (and optionally menus) | Fallback per scene only | None |

Per-scene fallback: an allowlist of scene kinds (race single view, then replays, photo, split, showroom...) taken from the
existing scene detectors (race object present, `RaceDisplay::render` running, photo/pause hooks). Outside the allowlist,
or after any capacity/decode failure in a frame, that frame falls back to CoreGS 2.0 entirely, and an F8 counter says so.

### 5.4 Threads

- **EE thread:** K1-K5 fill the frame's snapshot; K6 posts markers into the journal in stream order.
- **VU1 worker:** unchanged in levels 0-1; in level 2 it skips flagged programs; in capture mode it copies the GIF output
  of flagged jobs to the VU1 tap.
- **GS worker (CoreGS 2.0):** at a CoreGS 3.0 marker, flushes its pending batches for the current target, then calls
  `coregs3::record_layer(cmd, TargetView)`, then marks the touched rectangle drawn (so its coherence and read-back rules
  stay valid).
- **CoreGS 3.0 upload thread:** decodes and uploads meshes and textures ahead of use from the published snapshots.
  Pipeline warm-up runs at race load.

### 5.5 Presentation rate: variable FPS is part of the design (owner, 2026-11-05)

**Requirement.** CoreGS 3.0 must support a variable presentation rate from the start, like a modern game: 60, 120,
144 Hz or VRR. The game itself does not change. Physics, logic, timing, AI, saves and every EE-visible read-back stay at
the guest's fixed 60 Hz and stay bit-exact; the per-VBlank simulation digest gate applies unchanged.

**Owner clarification (2026-11-10).** The presentation rate is arbitrary and its cadence must be constant at the chosen
rate: 20, 30, 45, 60, 120, 144 or VRR, independent of the guest VBlank. CoreGS 2.0's frame skipping is not the model: it
repeats or skips guest frames and leaves the frame time uneven. The renderer samples the timeline at the presentation
instant and interpolates the two latest 60 Hz guest states; it never asks the guest physics for an arbitrary instant,
because the game only computes at 60 Hz. Prediction/extrapolation is an optional later switch; interpolation's
one-frame latency is the default.

**Measured prediction (2026-11-10).** A three-window study (32 captured frames each; B-Spec night race, static and moving
cameras) measured the tick correction a viewer would see if the prediction were replaced by the captured frame.
Entrywise matrix extrapolation (the direct alpha>1 extension of `lerp_matrix`) is exact for car bodies and the camera
(<=0.05 px) but distorts spinning wheels by 12-22.6 px at 1x (36-68 px at 3x) in every window, because a wheel spins
0.8-1.3 rad per tick. A rigid predictor (linear world translation, quaternion constant angular velocity, linear scale)
with an entrywise fallback for rank-deficient flat shell/reflection draws gives a tick correction of <=0.37 px for bodies
and <=0.9 px for the worst followed-car wheel at 1x (2.7 px at 3x), p95 <=0.2 px. The camera must be predicted from its
pose (position + orientation) with a constant angular acceleration term: entrywise V extrapolation shifts the image by
0.56-0.75 px during a fast pan, the pose model by 0.04-0.08 px. Interpolation needs the same rigid treatment for wheels
(entrywise lerp wobbles them by 0.9-6.1 px at 120 Hz; rigid slerp <=0.42 px). A player-car-only hybrid is not justified:
the split is part-class and camera-driven, not car-class. Discontinuities (collisions, resets, camera cuts) must drop the
prediction for that tick. The study covers straight near-constant-speed B-Spec running only; braking/cornering, human
input, collisions, camera cuts and course/scenery objects are still unmeasured.

**Contract.**

1. **The guest stays the clock.** A guest frame is captured exactly as today (K1-K5, one snapshot per VBlank) and is
   stamped with its guest time. The timestamp pair is `(vblank, ee ticks)`: the VBlank counter is the unit every gate and
   bench speaks, and the EE cycle counter at that VBlank is the finer clock, so a presentation instant can be placed
   between two frames without assuming that the guest is exactly 1/60 s per frame.
2. **The renderer runs at its own rate.** For each presented image it takes the last **two** captured guest frames, pairs
   their objects by stable identity, and interpolates each object's matrix (and the camera's) by
   `alpha = (present_ticks - older.ticks) / (newer.ticks - older.ticks)`. Linear interpolation of the 4x4 entries is
   enough for one 60 Hz step; quaternion/dual-quaternion interpolation arrives with motion vectors (phase 6).
3. **One frame of latency, on purpose.** Interpolation needs the *next* frame, so the presented image trails the guest by
   about one guest frame (16.7 ms). Extrapolation (which removes the latency but overshoots on impacts) is a later option
   behind its own switch, never the default.
4. **Stable object identity is the pairing key.** An object's key is a hash of the model's own data as the game keeps it:
   the shape's `Cmd` address, every item packet address and the material record address
   (`coregs3::object_key`, `native/coregs3/coregs3_timeline.hpp`). The matrix is what moves; the data does not, which is
   what makes the key stable across frames (measured: the car shapes keep their addresses frame to frame). A model drawn
   twice in one frame gets an `instance` number so two cars never collapse into one.
5. **Fallback is explicit.** An object present in only one of the two frames is drawn with that frame's transform (no
   interpolation), and the count is reported. A pair whose timestamps are not ordered, or whose guest frame was skipped
   by the frame skipper, is refused as a whole and the newer frame is presented as is. Nothing is extrapolated on a
   failed pair.
6. **Ring, not a single frame.** The capture publishes into a ring of `N` frames (`coregs3::FrameRing`, default 8). The
   renderer reads the ring, never the guest's live state: the EE thread publishes and never waits for the renderer, and
   the renderer never blocks the guest. The ring also gives the deferred features their history (motion vectors,
   reprojection, the tone/motion-blur feedback of phase 4).

**Interaction with the CoreGS 2.0 hybrid.** CoreGS 2.0 draws at the guest rate, by construction: its output exists only
when a guest frame was rendered, and its read-backs must stay exact. Therefore:

- In levels 0-1 (`shadow`, `overlay`), the frame CoreGS 2.0 composites into is the **newest** guest frame. CoreGS 3.0
  draws its own categories interpolated at the presentation rate on top of that frame; the 2.0-drawn parts (course, sky,
  HUD, effects not yet native) therefore update at 60 Hz while the native parts move at the display rate. That is a
  visible difference and is reported as such until phase 4, when CoreGS 3.0 owns the 3D frame and 2.0 reduces to the 2D
  layer.
- In level 2 (`replace`) the skipped VU1 work means the 2.0 frame no longer contains the covered categories; the merged
  frame is then the *newer* guest frame with the native layer drawn interpolated over it, and the GS-equivalent Z used for
  the merge comes from the same newer frame.
- In level 3 (`own3d`) the 3D frame is CoreGS 3.0's and interpolates freely; CoreGS 2.0's 2D layer is composited from the
  newest guest frame, at guest rate.

**UI and HUD.** The HUD is 2D and answers the guest's own state (speed, gear, lap time). It is drawn **at the guest rate**
from the newest frame, not interpolated: interpolating a digital readout invents values the game never had. If a scene
needs a smoother-looking HUD, it can be drawn twice with the same numbers, which is exactly what the guest-rate layer
already does when the display refreshes faster.

**Owner direction for VRR (2026-11-11).** The target presentation model is VRR, like a modern game: the renderer
presents on the display's real scanout instead of a fixed grid, and the **HUD should also be brought to the variable
rate later** (drawn at every presentation instant with the newest guest values, never interpolated). The guest-rate HUD
above is the interim behavior until the VRR host clock and the HUD redraw exist.

**Owner direction for native resolution and aspect (2026-11-11).** The renderer must support any resolution and aspect
ratio from birth. It is a native pipeline, not an upscale of the PS2 framebuffer: the output resolution is chosen
natively (start at 1080p 16:9; then 21:9 at 3440x1440), the game's own projection values (Width and Aspect, TASK-080)
drive the 3D framing, and the HUD elements are anchored and repositioned per aspect and validated. The integer 3x
captures (1920x1344) are a validation scale only, not the shipping model.

**What is implemented now (2026-11-10).** The data structures and the identity: `native/coregs3/coregs3_timeline.{hpp,cpp}`
(`GuestTime`, `ObjectKey`, `ObjectTransform`, `TimelineFrame`, `FrameRing` with pairing and unmatched counts,
`lerp_matrix`, `interpolation_alpha`) and its fixture `native_coregs3_timeline`. The capture already carries the guest
timestamp (`vblank`) and everything the key hashes, so no capture-format change was needed.

The live integration lives in `native/coregs3/coregs3_presentation.{hpp,cpp}` and the debug-only
`GT4_COREGS3=present` layer. One presentation tick is 1/3600 s and a guest VBlank is 60 ticks, so every supported rate
(20/30/45/60/120/144 Hz) divides the grid exactly and alpha is exact. The clock snaps its origin to the first captured
frame, marks every grid instant up to the newest guest frame due, and keeps at most eight on a stall (the loss is
counted). Each instant pairs the two latest frames by the timeline's stable object key and interpolates the measured
rigid model: world translation lerp, quaternion slerp, uniform-scale lerp, composed with the camera pose (projection
lerp, view from a position lerp and an orientation slerp). A rank-deficient world matrix (the flat shell/reflection
draws) falls back to the entrywise lerp of the captured matrices; a camera cut, a teleport or a pair spanning more than
two guest frames drops to the newest captured frame instead of blending across the discontinuity. Alpha 0/1 returns the
captured matrices bit for bit, so a 60 Hz presentation (every instant on a frame timestamp) draws exactly what the old
layer drew. Prediction is not implemented (its counter stays 0).

The layer presents the due ticks at `GT4_COREGS3_PRESENT_HZ` (default 60) with
`GT4_COREGS3_PRESENT_MODE=interpolate|nearest`; above 60 Hz it saves the composed CoreGS 2.0 image in a private
R32_UINT scratch image before the first tick and restores it before every later tick, so a tick's interpolated cars
never leave ghosts of the previous tick. The diagnostic `GT4_COREGS3_PRESENT_TICK` draws one chosen tick of each guest
frame for image evidence. The F8 report prints the clock (rate, instants, skipped, drawn ticks, interval
mean/min/max and the 8-tick histogram) and the pairing/interpolation counters (pairs, pairs_refused, paired,
only_newer, only_older, interpolated, entrywise, nearest, predicted, dropped_discontinuity). The guest path and the HUD
stay at 60 Hz.

The windowed host presents at the display rate (TASK-090 host-clock round, 2026-11-10).
`native/coregs3/coregs3_host_clock.{hpp,cpp}` places the due instants of each VBlank on the display grid: the first takes
the next slot at or after the batch's arrival, the following ones one display period later; a slot that passes with no
new tick stays empty and is counted late, an instant more than one guest frame behind its batch's newest tick is stale
and never drawn, and no instant is scheduled twice or backwards. `present::note_guest_frame` computes that plan every
VBlank and the F8 report prints it (plans, ticks, late, stale, skipped, duplicates). Above 60 Hz the backend asks the
layer for one picture per tick (`render_tick`; the composed base is saved before the first drawn tick and restored before
every later one), copies each picture into a picture slot on the rendering queue and hands it to the presentation
thread with its show time (`hw_gs_backend.cpp`); the normal once-per-VBlank slot is skipped when a picture was handed.
The guest and the HUD stay at 60 Hz; `GT4_COREGS3_HOST_CLOCK=0` keeps the old behavior (A/B). It needs the picture
presentation path (own or shared queue); a device on the single rendering-queue path logs the refusal and presents once
per VBlank. Validated: headless 60 vs 120 Hz runs are identical in guest clock, digest and the 64 KiB state (6,997
VBlanks, 0 differences) and the presentation clock's intervals are exactly 60 and 30 ticks; a 120 Hz windowed run drew
375 ticks / 16,538 car draws and handed 305 pictures through the presentation thread (the 3x scene is guest-bound on
the test machine, so the host clock is bounded by the available images there). The owner's windowed test at 1x is the
remaining acceptance for smooth display-rate motion. Measurements and gates: `docs/journal/2026-11-10.md`,
`build/evidence-host-clock/`.

**The live world (2026-11-11).** The `GT4_COREGS3=present` layer draws the captured world as well as the cars: the
main-view course (and, with `GT4_COREGS3_WORLD_BACKGROUND=1`, the sky and far underlay) is selected with the offline
world rule, its pixels come from an in-memory per-VU1-job store (`coregs3_live_world`, the `.gt4vram` record taken
before the job executes), its GS state from the K7 pass journal (TEST/ALPHA/FRAME/ZBUF/XYOFFSET; measured default
PRIM), and it is drawn in clip space through the composed image's viewport, so the live path has no 640x448 or
integer-scale assumption and 1080p 16:9 / 3440x1440 use the same code. The world is drawn before the cars under one
depth buffer at the presentation instant's interpolated matrices; the guest stays at 60 Hz and the identity gate
reports 0 clock/digest/state differences. First measured picture (night race 6600-7400): 3,853 items / 108,677
triangles / 338 textures, 0 refusals, every texture one guest frame stale (the worker executes a frame's jobs after the
VBlank that claims it), fog/blending/filtering/reflection/post chain deferred, and the HUD covered where the world
draws — the 2D overlay split is the next step. The frame-locked A/B is in docs/COREGS3-HOOKS.md §31.


---

## 6. Effects inventory and native restoration

| Effect | How GT4 does it (evidence) | Native restoration | Phase |
|---|---|---|---|
| **Glare / bloom** | `func_0033F1C8` → `func_003BADF8` when course flag bit 0; reduced-size pass, strength scaled by screen diagonal / 781.2 (study §2.1 step 6; role Hyp); parameters at scene +0x1750..0x1764 | Bright-pass, downsample chain and blur at a resolution-independent radius (keep the diagonal normalisation), parameters read from the scene | 4 |
| **Tone sequence** (colour grading) | Every VBlank a 256-entry ramp CLUT (16x16 CT24) is uploaded, then ~1,540 sprite batches apply R'=ramp[R].r, G'=ramp[G].g, B'=ramp[B].b to the frame (C: `STUDY-TONE-STRIPS-ONE-PASS.md` §1; present in v2.00 day races, none in the v2.00 night race) | Capture the ramp from its EE upload source; one full-screen 3x256 LUT pass | 4 |
| Brightness multiply | `func_00342580(buf, scene+0x3C, 1.0)`: colour (0,0,0,y) with ALPHA `Cd*FIX` (C: `func_0033EAB8.cpp:101`, study §2.6) | Multiply in the post chain | 4 |
| Colour filter / gradient | `func_0033F2E8` (`func_004A7D90`, `func_003BBBC0`), scene +0x950..0x994 | Post pass | 4 |
| Motion blur / crossfade | `render_post_effect` 0x0033F890 blends the other display buffer with FIX ≤ 0.8 (`func_004A3998(6, ...)`); crossfade `func_003429E0` copies three 640x224 blocks (study §2.6) | Keep the previous native frame as a texture; above 60 Hz scale the decay per presented frame (k^(60/Hz)) | 4 (6 for high FPS) |
| Depth of field | `func_0033E6E0` if vt+0x460 (focus `cam+0x68 * 1000`, role Hyp) | Native DOF from float depth | 4 |
| **Flares** (sun, course lights, headlights) | Per light: a depth-tested square into the frame's alpha, box-filtered to one texel of a 64x8 table; table read back and REF'd into VU1, which scales the flare sprite; split screen uses the constant 0xFF except for the sun (C: `GS-READBACK-STUDY.md` §2) | Read FlareClass light entries (obj = RacePS2Base+0xDA0) at K7; per light, an occlusion query over the same projected square or a compute pass over native depth giving the visible fraction × strength; flare sprites modulated on the GPU, same frame, no read-back | 4 |
| **Car env map** (paint reflection) | `func_0044E660`: three views from the car, yaw −120° steps, pitch 79.97°, asymmetric frustum, then a "sphere mapper" shape into an N×N buffer, rebuilt every frame; sampled by the car's reflection passes with texgen opcodes 86/87 and DATE (study §2.4) | Phase 2: borrow CoreGS 2.0's env-map target by GS address (same process) and compute reflection UVs natively. Phase 3/4: render the env map natively from the captured env-map draws (callers 0x44E918, 0x44EE1C) or as a cube map, at higher resolution | 2 → 4 |
| **Wet-look road reflection** | `render_reflection` 0x003411D0, `render_reflection_cars` 0x00341710, `_car_lights` 0x00341A30: mirrored course and cars into a buffer (size scene +0xC24/0xC28), N blur passes (+0xC2C), composited through a destination-alpha mask (`func_00342370`) | Planar reflection render target, blur, masked composite with a real alpha/stencil channel | 4 |
| Back mirror | Rect 260x58 of 640x448 at scale 1.3, mirrored projection (`pglScale(1,1,-1)`, `pglFrontFace(1)`) (C: `func_0033EAB8.cpp:146-158`) | Second view into its own target, composited | 4 |
| Shadows | Car shadow shapes (PGLUshape type 5, alpha coverage), projection shadow if scene flag 0x800000 (`isUseProjectionShadow` 0x003411A8); details inside `render_cars` U | Draw the game's own shadow geometry natively (faithful); optional shadow maps as an opt-in enhancement | 2 |
| Particles, sparks | `ParticleManager` `func_003C2838`, `SparkParticle::Render` 0x003C1900, `ThickLine2` 0x003C1788 (additive) | Instanced sprites/lines from the particle list | 4 |
| Smoke / dust | `RaceSmoke` `func_003E7CC8`, downsampled framebuffer copy into tiles (Hyp) | Soft particles with scene depth | 4 |
| Tyre marks | **U** (no symbol found, study H15) | Locate first (capture scan) | 4 |
| Weather / rain | GT4 has no dynamic rain (H); wet look = road reflection; rally = smoke/dust | — | — |
| Night lights | Brake lights by `CarModel::Callback`; headlight and course lights as flares; possible headlamp pool geometry (Hyp, GT3 had one) | Native emissive materials; flares as above | 2 / 4 |
| Destination alpha | DATE/DATM opcodes 29-31; reflection masks with alpha-only writes | Real alpha channel in native targets with the same write and test rules (GT3R did this) | 2 |
| Fog | Per-vertex fog from VU1 with `pglFog` (`func_004A40B0`, called by `func_00397DA0`), GS FOGCOL | Vertex fog, optional GS integer arithmetic | 1-2 |
| Dither / fields | DIMX toggled per frame; 640x224 field tricks | Progressive native output; no port | — |
| **Replays** | Replay camera lerps two keys by `t`, aspect in key field 10 (study §2.4) | Same passes; camera from K3/K4; replay-only effects (DOF, crossfade) | 5 |
| **Photo mode** | Tiled high-resolution capture and an 8x4 depth read for autofocus that **the EE reads** (C: `GS-READBACK-STUDY.md` §1.1) | Keep CoreGS 2.0 as a 1x shadow there so the autofocus and saved photo bytes stay exact; native high-resolution photo as an opt-in later | 5 |
| **Split screen** | `render_world_split` 0x0033FD88; flares use the constant table except the sun | Two native views | 5 |
| Showroom / pit / menu cars | `Concourse::render` 0x00408490 with a fixed sphere map (`registerSphereMap` 0x00408470), `mCarModelPS2`, `mSceneViewFace` | Same mesh path, different lights | 5 |
| Car icons, loading picture, thumbnail | Render-to-target plus one-shot read-backs the EE keeps (C: `GS-READBACK-STUDY.md` §1.1) | Stay on CoreGS 2.0 (exact bytes); native substitution of the displayed image is optional | 5 |
| **HUD / 2D** | `RaceDisplay::render` 0x0039F498 and 32 element `render_main`s, 640x448 ortho, `wideOffset` anchoring (study §5.3); textures in `display.gpb` | Level 3: CoreGS 2.0 draws 2D into a transparent overlay layer split at `render_panel`. Phase 7: native anchored HUD from the same textures | 4 / 7 |
| Menus | strobe (Flash-like) runtime and `mRenderContextPS2` | Stay on CoreGS 2.0 (native optional, Phase 7+) | 7+ |

---

## 7. Coexistence and per-scene fidelity

- **Switch:** `GT4_COREGS3=off|shadow|overlay|replace|own3d` (off by default; any other value refuses the start, like
  `GT4_FORCE_480P`). Requires `GT4_RENDERER=hw` with CoreGS 2.0 core 2. Not in the launcher until the owner approves per
  scene.
- **Category switch inside a level:** `GT4_COREGS3_CATEGORIES=cars,course,sky,...` so a regression can be isolated
  without rebuilding.
- **Fallback:** per frame (any decode/capacity failure, or a scene not on the allowlist) and per draw (an unknown VU1
  program entry, an unknown shape layout or an unsupported state class stays a GS draw; in `replace` it is simply not
  skipped). Each fallback is counted per cause in F8.
- **Per-scene fidelity measure (owner signs off the thresholds):**
  - reference = CoreGS 2.0 frames on the same recorded drive, same VBlank, same scale (deterministic replays, C:
    `scripts/coregs2_bench/README.md`: "two runs of one build are identical at 3x and 12x");
  - masks from the native renderer's own ID/coverage output, eroded by 1 px at 1x-equivalent to remove sub-pixel edge
    differences;
  - metrics per frame and per mask: PSNR, SSIM (luminance), pct_bad (the `docs/hw-renderer-spec/14` definition), mean
    luminance ratio (GT3R's level check), and the FLIP error image as an optional test-only tool (BSD licence, never in
    the game executable);
  - initial thresholds, taken from decision 0071's plan (**Hyp**, to tune): masked PSNR ≥ 30 dB (cars ≥ 32 dB),
    pct_bad ≤ 8%, mean luminance ratio within 0.98-1.02;
  - scene list: day race (licence B-1 and a time trial), night race (`night-run.sh`), Arcade race with AI
    (`arcade-run.sh`), then replays, photo, split screen, showroom.
- **Black-box second reference:** gt4-gs.exe final frames through `scripts/coregs2_bench/replay-gsdx.sh` (allowed for
  untainted agents; read only the game's own log lines).

---

## 8. Autonomous verification

All runs are **headless** (AGENTS/project rules: agents never launch the game windowed without the owner). Performance
A/B runs serially, never while builds run, on a quiet machine (the Avell over SSH when idle, per owner practice).

### 8.1 Tools that exist

- Recorded drives: `private/coregs2-bench/inputs/*.txt` with `.setup` (game, card, card hash). v2.00 recordings today:
  `licenca-b1` (3,514 VBlanks, driving from about 1,990), `photo-mode` (1,539), `usedcar-showroom` (1,684),
  `selftest-gt4card-20261104`. Scripted v2.00 scenes: `night-run.sh` (GT Mode B-Spec night race, Hong Kong Reverse, VW
  Lupo, 5 AI cars) and `arcade-run.sh` (menus, showroom, pre-race, Route 5 race start). Synthetic recordings can live in
  a worktree via `GT4_BENCH_INPUTS` (the format is plain text: `v <vblank> <buttons> <sticks> <pressures>`).
- `replay-run.sh` (`GT4_INPUT_REPLAY`, frame dumps `GT4_HW_DUMP_DIR/EVERY/FROM`, scale), `frame_diff.py` (identity),
  `frame_compare.py`, `frame_sheet.py`, `keep_frames.py`.
- Game state: `GT4_FRAME_SKIP_LOG` (race clock and an FNV-1a digest of the simulation state per VBlank),
  `GT4_FRAME_SKIP_DUMP_AT` + `GT4_FRAME_SKIP_DUMP_FILE` (simulation state plus 64 KiB of the race object at one VBlank)
  (C: `native/frame_skip.cpp:122-132`), and the three-run pattern of `camera360-check.sh` (ref / candidate off /
  candidate on).

### 8.2 Tools to add (Phase 1 builds the first four)

1. `scripts/coregs3_bench/coregs3-check.sh <recording> <ref exe> <cand exe> <dump VBlank>`: four headless replays (ref;
   cand off; cand shadow; cand overlay), then: clock and digest per VBlank, `state.bin`, frame identity for off and shadow,
   metric report for overlay. Exit code 1 on any identity failure.
2. **VU1 tap** (capture mode only, `GT4_COREGS3_CAPTURE=<file>` with a VBlank range): per draw, the GIF bytes VU1 emitted
   between `draw_begin(seq)` and `draw_end(seq)` markers, next to the `.gt4scene` draw records. `coregs3_tap_compare` then
   compares, per vertex, the native CPU reference vertex stage with the VU1 output (screen XY, Z, RGBA, STQ, fog; ADC
   vertices and clipper output reported separately).
3. `coregs3_metrics.py`: masked PSNR/SSIM/pct_bad/luminance per frame, JSON plus CSV, per-scene thresholds file.
4. `coregs3_sheet.py`: milestone sheets (CoreGS 2.0 | native layer alone | composite | diff heat map), PNG, with the
   build id, scene, VBlank and metrics burned into the caption.
5. Read-back log (from `GS-READBACK-STUDY.md` P0): every READ_FIFO with the EE return address and a reply hash. Gate:
   every site other than the flare must be byte-identical: 0x00105A50 (`func_001059F8`, used by the car icons and the
   photo tiles; the loading picture and thumbnail paths too, H), 0x003C7B30 and 0x00330954 (photo autofocus). The flare
   site (0x00497F30) is logged and may differ only in visual levels.
6. Performance counters: EE hook cost (new F8 category `coregs3_capture`), decode and upload times, native GPU pass time
   (timestamp queries), draws native / fallback per cause, VU1 jobs skipped (level 2), CoreGS 2.0 front-end time.

### 8.3 Gates every phase uses

| Gate | Pass condition |
|---|---|
| G-OFF (identity, hooks compiled in, switch off) | Frames byte-identical to the reference build; race clock and digest identical at every VBlank; `state.bin` identical |
| G-SHADOW (observe-only) | Same as G-OFF with capture and native rendering on but nothing composited |
| G-LOGIC (overlay, replace, own3d) | Race clock and digest identical at every VBlank; `state.bin` identical; EE-consumed read-back hashes identical |
| G-TAP | Per-vertex tolerance (Phase 1 values below) on all captured draws of the covered categories |
| G-IMAGE | Per-scene masked metrics within the signed-off thresholds; coverage of the GS category mask ≥ 98% |
| G-PERF | Phase-specific budget (below), no new frame over 16.7 ms in headless frame-time logs |
| G-ROBUST | 0 unmatched markers, 0 orphan draws, clean disable on capacity overflow, no Vulkan validation errors in a validation run |

### 8.4 Milestone screenshots

Each acceptance run writes `build/evidence/coregs3/<phase>/` (git-ignored): the sheets, the metric CSV and a short
`report.md` (scene, recording, VBlanks, metrics, counters). The orchestrator reviews the sheets (the implementing model
may not have vision) and sends them to the owner through the owner's usual private channel. Screenshots of game content
are never committed.

---

## 9. Phased plan, smallest visible milestone first

### Phase 1: the cars of a race, native, inside CoreGS 2.0's frame (specified for an implementation agent)

**Goal.** In a v2.00 race, the cars drawn by `render_cars` in the main view are captured at the EE level, rendered by
CoreGS 3.0 at CoreGS 2.0's internal scale, and merged into CoreGS 2.0's frame target right after the game's car pass,
depth-tested, on top of the original cars (level 1, `overlay`). Nothing in the guest changes.

**Preconditions.** Build recipe of the current rc15 line (`GT4_RENDERER=hw`, core 2). A fresh worktree from `main`
without a `private/` junction (AGENTS.md). Untainted agent.

**Steps and deliverables.**

1a. **Hooks and snapshot (observe-only).**
- Extend the unit-insertion tool (new `scripts/coregs3_units.py`, same receipts and refusal rules as
  `camera360_units.py`) with:
  - K1: a call before the `jal 0x0049D308` at 0x00455A5C;
  - K3 and K4 at the block starts of labels `L_0033EBB4` and `L_0033EBF4`;
  - K5 at the block starts of `L_0033EE28` and `L_0033EE54`.
- Add a dispatch overlay hook K2 for `pc == 0x0049CEA0`.
- New `native/coregs3/coregs3_hooks.cpp` and `coregs3_scene.hpp`:
  - per frame, a bounded `FrameSnapshot`: up to 4,096 draw records, 8 passes, 4 cameras;
  - each `DrawRecord` holds: seq, pass id, shape guest address and a copy of `Cmd` with up to 64 items (addr, count,
    event), PGL context words (a fixed list of offsets: the GS shadows at +0x174, +0x188, +0x198, +0x1A8..+0x1C0, `count`,
    `x`, `y`, `value`, `bits`), VU0 vf24-vf31, the 4 qwords at +0xFA0, the material and texture record addresses and
    their bytes (80 and 40 bytes).
- Only draws between K5 begin and end are kept in Phase 1 (category `cars`); everything else is counted, not stored.
- Guest reads go through bounds-checked readers; no guest pointer outlives the hook.

1b. **Markers.**
- A runtime capsule overlay in the DMA chain walker's REF case matches the source range of each REF tag against the
  frame's pending car draws (FIFO order).
- It posts `pass_begin(cars, frame)` before the first match and `pass_end(cars, frame)` after the last, into the GS
  stream journal, flushing the pending VIF batch first so that ordering is exact.
- In capture mode it also posts `draw_begin/end(seq)` around every matched draw.
- Unmatched or out-of-order matches are counted and the frame falls back.

1c. **Decoders and the first offline screenshot.**
- `coregs3_shape_decode`: UNPACK framing with explicit cycle state; GT4 channels (position 16-bit at 0x8000, UV 16-bit at
  0x8040, reset bytes at 0xC040, colours V4-8, the 8-bit 3-component data at 0x8080 as candidate normals); strip
  topology with resets.
- `coregs3_texture_decode`: Tex1 transfers replayed into a GS-memory model built on `native/hw_gs/memory/` and `clut/`;
  T4/T8 with CSA/CBP.
- A capture writer and reader (`.gt4scene`, versioned, assets deduplicated by hash, private outputs only).
- An offline tool `gt4_coregs3_render <capture> <vblank> <out.png> [--background <coregs2 dump>]` that renders the
  captured car pass with the captured matrices.
- **First visible milestone:** native cars over a CoreGS 2.0 dump of the same frame (dump index offset +2, C:
  `scripts/coregs2_bench/README.md` on the double buffer), plus four orbit views of one car.

1d. **VU1 tap and the native vertex stage.**
- The tap: GIF output between `draw_begin/end` markers, captured on the GS-worker side of the stream.
- `coregs3_vertex_ref` (CPU) is the specification of the shader vertex stage:
  - clip = M × p, with M = the 0xC1 block;
  - **update 2026-11-09 (H, measured on the night-race tap):** the 0xC1 block is four **columns** `vf24..vf27`, used
    **unscaled** (the uploaded packet rows are `vf * cmd.value`; the vertex positions are divided by 4096 by the model
    decoder, which cancels `value`), and the screen map is
    `((0.5 + 0.5*cx/cw)*width, (0.5 - 0.5*cy/cw)*height)`. The VU1 then adds `XYOFFSET` (measured about
    `(-2368, +1824) px` on that frame); the GS subtracts it. Numerals and residual table:
    [COREGS3-HOOKS](COREGS3-HOOKS.md) §10.2. `coregs3_render.cpp`'s `draw_matrix()` still reads rows and scales by
    `value`, so its screen mode is known-wrong until changed;
  - the GS Z mapping: fit from tap data, then located in PGL state;
  - fog from depth;
  - UV and Q;
  - lighting: start with an approximation, then derive the formula from the AOT C++ of the hot strip entries
    (families A `1BF8..2698` and B `1C48..2CA8`, `vu1-direct-gpu-study.md` §2.1), material at +0xA60 and the light
    block (**U**: find the light upload, candidates `func_00397CE0` → `func_00397F18`/`func_003986E8` and
    `func_0049E5F8(obj, 0xD9)`).
- Gate G-TAP thresholds for Phase 1 (**Hyp**, tuned once and then frozen):
  - drawn vertices (ADC = 0): screen XY within 0.25 px at 1x for 99.9% of them;
  - Z within 1 part in 2^16 of the Z range;
  - STQ relative error ≤ 1e-4;
  - RGBA within ±2 levels for 99% of vertices (±8 allowed in step 1d-i before the lighting derivation lands);
  - fog within ±2.

1e. **Live overlay.**
- `coregs3_renderer` in the same `VkDevice` as CoreGS 2.0:
  - attachments: RGBA8 colour, D32 reversed depth, R32UI GS-equivalent Z, at the frame target's scale and size;
  - pipelines from the captured PGL state classes;
  - multi-pass car materials: base, reflection pass, glass.
- Seam in CoreGS 2.0:
  - a new in-process stream record for CoreGS 3.0 markers;
  - at `pass_end(cars)`, the core 2 renderer flushes, calls `coregs3::record_layer(cmd, TargetView)`, and marks the rect
    drawn.
- `coregs3_inject.comp`: per pixel at scale, where the native layer has coverage and `nativeZ >= targetZ` (GEQUAL),
  write the packed colour (keep alpha per FBMSK) and the Z word.
- The car's reflection pass samples CoreGS 2.0's env-map target by GS address (borrowed; native env map is Phase 4). If
  the target is not found, the pass is skipped and counted.
- A debug-only `GT4_COREGS3=present` draws the layer over the final image without depth. It gives an early live picture
  before the seam lands and is never used for acceptance. The prepared insertion points (not implemented): after
  `ComposeFrameGpu` in `Backend::end_frame` and `Backend::scanout`, submitting on `Impl::queue`; details in
  [COREGS3-HOOKS](COREGS3-HOOKS.md) §10.3.

1f. **Tools and gates.** `coregs3-check.sh`, `coregs3_metrics.py`, `coregs3_sheet.py`, CTest fixtures, the
`coregs3_capture` F8 category.

**Scenes.**
- P1-A `licenca-b1` (v2.00, existing recording; dumps at VBlanks 2050-2430 every 20).
  **Update 2026-11-05 (measured): P1-A is not a car scene.** Its timed run uses a bumper camera: the K5 bracket runs every
  frame and captures 0 car draws, and the reference frame at VBlank 2060 shows the road and the HUD with no car body. The
  car only appears at the result screen (~VBlank 3000) through a different pass. P1-A therefore stays as an **identity**
  scene (G-OFF/G-SHADOW, clock/digest/state) and **P1-B, the night race, is the phase-1 car scene**. See
  [COREGS3-HOOKS](COREGS3-HOOKS.md) §5 and decision 0077.
- P1-B `night-run.sh` (six cars, night; dumps 7400-11000 every 600).
- P1-C a new synthetic v2.00 Arcade time trial written as a press script by the agent (car and track available on the
  Arcade card; the owner's Supra/Nordschleife only if a card with it exists among the bench cards).

**Acceptance (all automatic).**
1. G-OFF and G-SHADOW on P1-A and P1-B (and `arcade-run.sh` with `EVERY=700 FROM=700`).
2. G-TAP on at least 2,000 captured car draws from P1-A and P1-B.
3. G-IMAGE at 3x on P1-A/B/C:
   - car-mask PSNR ≥ 32 dB, SSIM ≥ 0.90, pct_bad ≤ 8%;
   - coverage ≥ 98% of the GS car mask, where the GS car mask is rasterised offline from the tap's drawn triangles, and
     spill ≤ 2%.
4. G-LOGIC in overlay on P1-A/B/C.
5. G-PERF (headless, quiet machine): EE capture overhead ≤ 0.5 ms per race frame at the 95th percentile; native car
   pass plus merge ≤ 1.5 ms GPU at 3x on the RX 9060 XT; no rise in CoreGS 2.0's GS-worker time beyond 0.3 ms.
6. G-ROBUST over the three scenes, including the end of the licence test and the return to menus.
7. Unit tests:
   - synthetic VIF packets authored by hand (no game data);
   - capture round trip;
   - PGL-state decoding;
   - an injection compute GPU fixture on synthetic targets (device-derived limits, AMD `maxComputeWorkGroupCount` note
     in AGENTS);
   - hook receipts (each insertion exactly once; removal restores the units byte for byte).

**Screenshots.** Offline milestone (1c), then the live overlay sheets on the three scenes (1e/1f).

**Effort and split (Hyp).**

| Step | Effort | Who |
|---|---|---|
| 1a | 1 week | **review** (hook sites, guest identity) |
| 1b | 0.5-1 week | **review** (journal, threading) |
| 1c | 1-1.5 weeks | **DeepSeek alone**, with fixtures |
| 1d | 1-1.5 weeks | tap plumbing **review**; vertex reference and tuning **DeepSeek alone** against the tap; lighting derivation from AOT C++: a stronger model or design review if DeepSeek stalls |
| 1e | 1-1.5 weeks | renderer **DeepSeek alone** from the spec; CoreGS 2.0 seam and inject semantics **review** (touches `native/hw_gs/`) |
| 1f | 0.5 week | **DeepSeek alone** |

Total 5-7 agent-weeks.

**Risks.**
- The DMA-order match fails for some draws (mitigation: per-draw counters; fall back for the frame).
- The lighting formula takes longer (overlay is still useful with the ±8 tolerance; the image gate will show it).
- The CoreGS 2.0 seam disturbs its coherence (mitigation: overlay writes only inside the pass rect, marked drawn;
  identity gates in off and shadow modes catch regressions).

**Stop conditions.** Stop and report at any G-OFF or G-SHADOW difference, any G-LOGIC difference, or a tap mismatch that
cannot be explained after one derivation round.

### Phase 2: the cars, replacing the originals

- **Scope:**
  - level 2 for `cars`: the VU1 worker skips the flagged programs, after an MSCAL-level match of the REF ranges;
  - the native car carries the full look: lighting exact to the tap, texgen reflection with the borrowed env map, glass
    and decals with destination alpha, brake-light callback states, the car shadow shapes, the night headlamp pools if
    they exist;
  - all cars on the grid; wheels and tyres; the pit-crew texture pass `func_0033DC88`.
- **Extra gates:**
  - non-skipped PATH1 records byte-identical to the reference (proves skipping has no side effect on other programs'
    VU1 state);
  - G-LOGIC including the read-back log;
  - VU1-worker time drops by the covered share.
- **Effort:** 6-9 agent-weeks. **Risk:** medium (skipping semantics on VU1 memory state; multi-pass DATE).
- **Who:** skip mechanism and gates are review work; materials and passes DeepSeek with tap and image gates.

### Phase 3: the track, sky and billboards

- **Scope:**
  - `render_course` (Voronoi visibility lists are the game's own; we draw what it submits), the sky with its own depth
    range and the depth reset (`func_00396B80`, `pglClear(2)`), BillboardSet (trees, crowds);
  - course VM objects (`render_course_for_renderer` 0x00396AD0), CLUT animations (`pgluMapClutAnimation`; follow the
    CLUT bytes), `render_after`;
  - fog per scene; the reflection-pass and mirror-pass draws in shadow mode only.
- **Gates:** G-TAP for course families; G-IMAGE on the world mask; ≥ 99% of H1 draws in the reference race native or
  explained.
- **Effort:** 6-10 agent-weeks. **Risk:** medium (fog and texgen variants, foliage AFAIL, depth precision on long
  courses).
- **Who:** mostly DeepSeek once Phase 1/2 patterns exist.

### Phase 3 status (2026-11-11)

The first live step is in: the captured main-view course is drawn natively in the game window over the CoreGS 2.0
image (hooks §30). The sky and far underlay are kept from the base by default (they are background layers that would
hide it); `GT4_COREGS3_WORLD_BACKGROUND=1` draws them. The 2026-11-11 picture round (hooks §32) wires the live
in-memory tap: the emitted per-item TEX0/TEX1/CLAMP state and UVs bind the items the record path refused, the emitted
per-vertex fog is applied with the GS mix, and linear items sample with the GS bilinear; the remaining untextured
items are skipped by name. CLUT animation, the wet reflection and the post chain remain the Phase-3 work.


### Phase 4: "it rendered a race": the 3D frame is CoreGS 3.0's

- **Scope:**
  - level 3: CoreGS 3.0 owns render_world at display resolution and aspect;
  - CoreGS 2.0 splits 2D at `render_panel` into a transparent overlay layer (stream marker at `render_panel` begin; this
    is also CoreGS 2.0 roadmap item "UI layer") and optionally keeps a 1x shadow of the 3D;
  - native env map, wet reflection, back mirror, flares by occlusion, particles, smoke, glare/bloom, tone LUT,
    brightness, colour filter, motion-blur feedback, DOF.
- **Gates:**
  - G-LOGIC;
  - per-effect image gates per scene (night, day with tone, Arcade with AI);
  - 60 FPS headless frame-time at 4x on the desktop;
  - flare levels within the documented tolerance of the GS flare.
- **Effort:** 12-20 agent-weeks. **Risk:** high (many effects, 2D composition order, the frame-reading passes).
- **Who:** each effect is a closed brief with its own gate (DeepSeek); the layer split and the shadow-GS policy are
  review work.

### Phase 4 note (2026-11-11)

Level 3 (CoreGS 3.0 owns the 3D frame) additionally needs the 2D overlay split: the base's HUD is composed before the
world, so the live world covers it where it draws. The live world foundation (native output resolution and aspect
included) is in place for that split. Design and the blocking CoreGS 2.0 work: [COREGS3-2D-SPLIT](COREGS3-2D-SPLIT.md).

### Phase 5: other modes

- **Scope:** replays (replay camera), photo mode (EE-read autofocus stays on the CoreGS 2.0 shadow; native
  high-resolution photo optional), split screen, showroom/concourse and menu cars, licence demos, pit stops (pit crew
  skinning appears EE-side, Hyp).
- **Gates:** per mode, as Phase 4, with the `photo-mode` and `usedcar-showroom` recordings plus new synthetic ones.
- **Effort:** 8-14 agent-weeks. **Risk:** medium.

### Phase 6: modern features (what the native pipeline unlocks)

- **Scope:**
  - present above 60 Hz with per-draw interpolation between ticks (TASK-045): draw identity = (shape, model, instance,
    ordinal); match rate ≥ 95% in races; decomposed transforms (RT64 lesson);
  - object motion vectors and jitter for FSR 3 / TAA;
  - native aspect ratios with the game's own projection widened (TASK-080 hooks) and HUD anchoring;
  - any resolution, MSAA, anisotropic filtering, texture replacement keyed by asset identity, dynamic resolution.
- **Gates:** G-LOGIC (no physics change: headless digests per VBlank equal at 60/120/144 present caps); match-rate
  counters; image gates at 16:9 unchanged.
- **Effort:** 8-12 agent-weeks. **Risk:** medium-high (interpolation identity).

### Phase 7: native 2D and HUD (menus optional)

- **Scope:** `RaceDisplay` elements drawn natively from `display.gpb` textures with anchored layout; fonts; menus only
  if the owner wants them (strobe runtime: large, 12+ weeks).
- **Effort:** HUD 4-6 agent-weeks. **Risk:** medium.

### Phase 8: mobile, Apple and the performance levels

- **Scope:**
  - Android/Apple tuning: pipeline warm-up, texture dedupe, MSAA 2x and dynamic resolution on phones, tile-friendly
    passes;
  - remove the CoreGS 2.0 1x shadow where no EE-read read-back exists in the scene;
  - measure on the S23 and the laptop.
- **Effort:** 6-10 agent-weeks. **Risk:** medium.

### Totals and order

| Phase | Visible result | Effort (Hyp) | Risk |
|---|---|---|---|
| 1 | Native cars in a race (overlay) | 5-7 w | medium |
| 2 | Native cars replace the originals | 6-9 w | medium |
| 3 | Native track, sky, trees | 6-10 w | medium |
| 4 | Native 3D race with effects | 12-20 w | high |
| 5 | Replays, photo, split, showroom | 8-14 w | medium |
| 6 | High FPS, ultrawide, FSR/TAA | 8-12 w | medium-high |
| 7 | Native HUD (menus optional) | 4-6 w (+12 w) | medium |
| 8 | Mobile/Apple, performance | 6-10 w | medium |

Total about 55-90 agent-weeks before optional menus. Phases 3 and the effects of Phase 4 can overlap once Phase 2's
patterns exist.

### What DeepSeek does alone, and what needs review

- **Alone (closed briefs with numeric gates):**
  - decoders with synthetic fixtures;
  - the CPU vertex reference tuned against the tap;
  - shaders and pipelines from a written state table;
  - offline tools, metrics, sheets, CMake wiring;
  - each effect once its inputs and gate are written down;
  - recordings authored as press scripts.
- **Needs design review (orchestrator, or a stronger model):**
  - every new hook site (guest identity, receipts);
  - the DMA marker and journal ordering;
  - the CoreGS 2.0 seam and anything in `native/hw_gs/`;
  - VU1 skipping (Phase 2);
  - the 2D layer split (Phase 4);
  - deriving formulas from VU1 AOT C++ when the tap does not converge;
  - interpolation identity;
  - pins, manifests, launcher and release steps;
  - every licensing question.
- **Brief discipline** (owner rule `_rules/project-rules`): explicit start, ordered middle, deliverables, acceptance
  evidence, stop. The orchestrator reruns every gate before accepting a claim.

---

## 10. Legal and scope guardrails

- **No game assets or code in the repository:**
  - no captures (`.gt4scene`, tap files), decoded meshes or textures, game screenshots as fixtures, or translated or
    decompiled code;
  - public fixtures are hand-authored synthetic data;
  - disc-dependent tests skip without `GT4_DISC`;
  - outputs live under `private/` or `build/evidence/` (ignored).
- **Assets come from the user's own disc at run time** (through the game's own loaded copies); never distributed.
  Milestone screenshots stay private unless the owner decides otherwise.
- **MIT code only, no GPL in the game executable:**
  - CoreGS 3.0 dependencies limited to Vulkan headers and MIT/BSD/zlib libraries;
  - FLIP (BSD) only in test tools;
  - FSR (MIT) later.
- **Clean room (decision 0070):**
  - CoreGS 3.0 agents are untainted;
  - they never open PCSX2/GSdx, the `gt4recomp-gs` fork, ARMSX2, `private/tooling/pcsx2-*` or
    `private/slice189-pcsx2-ref`;
  - gt4-gs.exe final frames only, as a black box;
  - celosia: prose study only.
- **Research references:**
  - the decomp (MIT) and Nenkai's MIT tools and templates give names, layouts and format facts; no implementation is
    copied into the executable;
  - GT4FS and the modding hub (no licence) are facts only;
  - GT3 Revisited (MIT) is ideas only.
- **Scope (AGENTS.md):**
  - game logic, physics, timing, AI and saves never change;
  - no decompilation-based source port;
  - no remaster features inside this project;
  - rendering enhancements stay opt-in.
- **Faithful default:** CoreGS 2.0 stays the default and the reference; CoreGS 3.0 is off by default and enters the
  launcher only with the owner's per-scene approval.
- **No JIT:** shaders compiled ahead of time with the pinned glslang; the driver compiles SPIR-V (allowed); no VU1
  microcode is compiled at run time.
- **Never claim** zero emulation or full PS2 fidelity: the guest still runs its EE render code and the IOP stays
  interpreted.

---

## 11. Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| VU1 lighting/fog/texgen formulas per family are hard to re-derive | high | quality | VU1 tap gate; derive from AOT C++ of ~30 hot entries; allow approximations behind the image gate |
| Mixed GS/native frames show order or depth seams | medium | quality | Inject at exact stream positions; GEQUAL with GS-equivalent Z; own3d as soon as a scene is covered |
| GS state quirks not reproduced (alpha-test depth, untextured colour, blend rounding, `COLCLAMP`, texture LOD, UV row) | medium | quality | The checklist in `docs/COREGS3-CELOSIA.md` §3 applied to the PGL state table and the Phase 1/2 image gates |
| Post effects approximated generically (bloom/glare shape, history feedback) | high | quality | Start Phase 4 from the game's own pass chain and parameters; a generic bloom is a rejected candidate (`docs/COREGS3-CELOSIA.md` §4) |
| Effects that read the frame (tone, brightness, smoke grab, feedback blur) misbehave in overlay/replace | medium | quality | They run after the inject on the merged frame; native versions in Phase 4 |
| Guest-visible side effects of skipping VU1 programs | low | logic | PATH1 identity of non-skipped programs; G-LOGIC per VBlank |
| EE-consumed read-backs (photo, icons, loading picture) | known | logic | Always answered by CoreGS 2.0 (shadow at 1x if needed) |
| CPU: EE render code still runs | known | performance on EE-bound PCs | Honest limit; later optional "skip guest render work" only with strict gates (not planned) |
| GPU memory on phones | medium | stability | Budgets, dedupe, LRU, dynamic resolution |
| Two renderers to maintain | known | cost | Shared device, shared memory/CLUT code, one seam, the same bench |
| Agent-invented results | medium | trust | Closed briefs, reruns by the orchestrator, evidence folders |

---

## 12. Open questions for the owner

1. **Where the code lives:** on `main` behind the off-by-default switch (recommended: hooks are proven identical when
   off, CoreGS 2.0 and 3.0 stay in sync), or on the old long-lived branch `native-renderer/main` (decision 0071)?
2. **Approval without play tests:** may a CoreGS 3.0 phase be merged (still off by default) on the strength of the
   automatic gates and the milestone screenshots, with your play test only when it is offered in the launcher?
3. **Reference scene for Phase 1:** is a licence test plus the night race plus an Arcade time trial enough, or should we
   build a bench card with your Supra/Nordschleife time trial from a copy of your save library (your card is never
   touched)?
4. **Disk cache:** may decoded textures and meshes be cached on disk in the user's local cache folder (derived from the
   user's disc, never shared), or memory only?

---

## Appendix: external sources (accessed 2026-11-05)

- GT3 Revisited repository and its files `README.md`, `CONTRIBUTING.md`, `docs/ARCHITECTURE.md`, `docs/FORMATS.md`,
  `docs/dev/RENDERING.md`, `docs/dev/RACE.md`, `docs/dev/PHYSICS.md`, `docs/formats/car_model.md`:
  https://github.com/matt-pulsipher/GT3-revisited (read through `gh api`, default branch `master`).
- Android Authority, 2026-10-21, on GT3 Revisited:
  https://www.androidauthority.com/gran-turismo-3-android-port-recompilation-3726051/
- OpenGOAL porting notes for tfrag: https://opengoal.dev/docs/porting-info/drawable_and_tfrag/porting_tfrag
- RT64 repository README (draw-call replay and transform interpolation): https://github.com/rt64/rt64
- Nenkai PDTools (MIT, local clone `ffe0bb2`), GT-File-Specifications-Documentation (MIT), gt-modding-hub (no licence,
  facts only): local clones under `private/slice151-research/nenkai/`.
- gt4-decomp (MIT, local clone `e0b70c4d6`; README progress paragraph: 29,551 of 30,962 functions matched, 95.4%):
  `private/slice151-research/gt4-decomp/README.md`.
- celosia (GPL-3.0, prose only): https://github.com/HenryKun55/celosia, `README.md` and `docs/STATUS.md`,
  `docs/native_port_plan.md`, `docs/native_port_log.md`, `docs/native_renderer_map.md`, `docs/known_issues.md`
  (accessed 2026-11-09; no source read; see `docs/COREGS3-CELOSIA.md`).
