# CoreGS 3.0 car pass study: why the live present loses the car's paint, textures and reflection

Read-only study, 2026-11-11. Worktree `O:/Antigavity/gt4-wt-090-car-study`, branch `task/090-car-study` from
`task/090-coregs3` at `e7e67e2d`. No native source was changed; the analysis tools and evidence live under `build/`
(gitignored).

## 1. Question and answer

**Question.** In `GT4_COREGS3=present` the cars are neutral grey/dark silhouettes, while the offline car renderer
(`gt4_coregs3_render`, the textured car milestone plus the round-20 reflection) draws the car with its paint, textures,
lamps and paint reflection. What state does the live car pass bind that the offline path binds differently, and what is
the fix?

**Answer (Confirmed).** The live car pass draws the captured car draws as **lit vertex colour only**. It has no UV
attribute, no per-item texture binding, no per-item GS pixel state and no reflection pass:

- `Impl::build_frame` (`native/hw_gs/coregs3/hw_coregs3_present.cpp:1016`) emits `PresentVertex` records of position +
  RGBA8 colour only (`:132`); item colours are the `shade_mesh_items` output (so the VU1 lighting is applied, see §3),
  and alpha is forced to 255 (`:1137`).
- `coregs3_present.vert` (`native/hw_gs/shaders/coregs3_present.vert:32`) passes the colour through;
  `coregs3_present.frag` (`native/hw_gs/shaders/coregs3_present.frag:30`) stores it with `alpha = 255`. There is no
  texture fetch, no alpha test, no blend, no destination-alpha test, no FBMSK and no fog.
- The car pass never calls `build_frame_texture_table`; only the live *world* build does
  (`hw_coregs3_present.cpp:1240`). The car items' T4/T8 textures and their CT32 CLUTs are never decoded or sampled.
- The body/paint items (`program_dispatch 0x1058`, the near car's 17 body items in draw 271) are drawn **opaque** with
  depth write instead of as the measured environment-map reflection pass: CT32 256x256 page **0x2BC0**, texgen S,T,
  MODE `(Cs-Cd)*As+Ad` (ALPHA 0x58), destination alpha test (TEST 0x5400C, DATE), **no depth write** (ZBUF.ZMSK=1),
  PRIM 0x7C, fog toward black. The opaque lit body layer is what the owner sees as the grey/dark shell.

**Measured.** After the 2-VBlank alignment (offline capture frame at VBlank 7398, live present at VBlank 7400; shift
`(-12,-14)` at 3x), the live near car is the offline renderer's `--no-textured` picture almost exactly: **PSNR 36.08 dB,
mean |d| 1.01 level, 2.9% pixels over 8** on the eroded near-car mask (10,208 px at 1920x1344). Against the offline
textured + reflection picture the same mask gives **20.12 dB, mean |d| 18.23, 66.2% over 8**. The live car is therefore
the pre-texture base path, not a lighting or geometry failure.

Confidence: root cause **Confirmed** (code + pixel equivalence); exact pages/state **High confidence** (captured and
tapped values, §5-§6); surface names (glass/lights/wheels) **Medium** (visual attribution of the exact draw).

## 2. Method and tools

All tools were built from this branch (`GT4_HW_GS=ON`, `GT4_COREGS3=ON`, VS 18 `VsDevCmd`, Vulkan SDK 1.4.341.1;
the offline tool does not link the EE library). The v6 ALL_DRAWS capture
`O:/Antigavity/gt4-wt-090-mip-recapture/build/evidence-mip-recapture/capture/night-mip.gt4scene` (VBlanks 7398-7402,
285 draws/frame: 116 car draws, 169 world draws) was filtered to its car pass with a small analysis tool
(`build/analysis-car/filter_cars.exe`, linked against the branch's `gt4_coregs3.lib`); the filtered capture keeps the
draw sequences (169-284) and packet offsets so the tap/vram pairing is unchanged. The v6 tap and vram sidecars are the
same capture round's (`night-mip.gt4tap`, `night-mip.gt4vram`).

- `gt4_coregs3_render` (branch build) rendered frame 0 (VBlank 7398) at 1920x1344: `--no-textured` (base),
  `--vram/--tap` (textured), `--vram/--tap --no-reflection`, and per-draw (`--draw N`) for all 116 draws.
- `build/analysis-car/dump_car_bindings.exe` prints the per-item binding and GS state from the same
  `build_frame_texture_table` call the offline renderer uses (TEX0/TEX1/CLAMP decode, sampler size/filter, TEST/ALPHA/
  FRAME/ZBUF/PRIM/FOGCOL, reflection flag and per-vertex fog).
- The live pictures are the existing present dumps `night-liveworld-off/frames/frame_07400.ppm` (car only) and
  `night-c3n-ref/frames/frame_07400.ppm` (the faithful CoreGS 2.0 reference), both 1920x1344, from
  `O:/Antigavity/gt4-wt-090-live-world/build/evidence-live-world/`.
- The live present's own F8 report (`night-merged-on/log.txt`) shows the same draw list and lit colours:
  `draws 699 refused_draws 0 unlit_draws 0`, `family body_paint items 483 lit 483 fallback 0` over 6 passes
  (~80 body/paint items per frame, the offline count), `light_sources uploaded 1067 carried 918 uncertain 30 none 0`.
  The VU1 light state is captured and applied in the live path; **lighting is not the missing piece**.

Reproduction commands are in §11.

## 3. What the live car pass does today (exact)

| Stage | Live present |
|---|---|
| Vertex record | `PresentVertex { float x,y,z; uint8 r,g,b,a; }` (16 B), `hw_coregs3_present.cpp:132` |
| Vertex stage | metre transform columns in the push constant; outputs `colour` only (`coregs3_present.vert:32-36`) |
| Per-item shading | `shade_mesh_items` (VU1 light block + ITOP program), the same module the offline renderer uses |
| Per-item texture | **none** (no `build_frame_texture_table` for cars, no TEX0/CLAMP/atlas rect, no UV attribute) |
| Per-item GS state | **none** (TEST/ALPHA/FRAME/ZBUF/PRIM not bound) |
| Alpha test | none; output alpha forced 255 (`:1137`, `coregs3_present.frag:36`) |
| Blend | none; the fragment stage writes the packed word directly (no destination read except the optional post chain) |
| FBMSK | none |
| Fog | none |
| Depth | GEQUAL, depth write **on** for every car draw (`:519-527`), including body/paint items whose measured state has ZMSK=1 |
| body_paint items | drawn as opaque lit geometry; no texgen S,T, no environment-map sample, no DATE, no additive blend |

The draw list is correct: with `GT4_COREGS3_LIVE_WORLD=0` the hook snapshot is car-only and `build_frame` draws all 116
car draws in capture order (`draw.pass != kPassCars` filter at `:1029`). The offline and live per-family item totals agree
per frame (335 car items: 80 body_paint, 112 nine_qword_first, 21 pow8, 62 pow16, 8 direct, 1 six_qword, 25 three_qword,
26 passthrough). The failure is entirely in the per-item state/texture/reflection binding.

## 4. What the offline textured car path does (exact)

`build_frame_texture_table` (`native/coregs3/coregs3_texture_frame.cpp`, options at
`coregs3_texture_frame.hpp:149`) with `use_tap = true`, `reflections = true`, `world_channels = false` and the VBlank
vram image binds each item. `gt4_coregs3_render` then rasterises with `coregs3_render.cpp`: the base draws sample the
item's texture (MODULATE, TCC, level 0, linear/nearest, four CLAMP modes), run the alpha test (ATE/ATST/AREF, AFAIL
KEEP), apply the measured ALPHA/FBMSK and, for the body/paint items, draw the round-20 reflection pass instead
(`rasterise_reflection_triangle`, `coregs3_render.cpp:405`): texgen S,T (`coregs3_reflection.hpp:62`), CT32 map sample,
fog toward FOGCOL, alpha test, DATE, GEQUAL depth with no depth write, additive `(0,2,1,1)` blend, FBMSK.

Our branch render of the filtered v6 frame 0 at 3x:

| Quantity | Value |
|---|---|
| Car draws / items | 116 / 335 (same list as the live frame) |
| Lighting | 116/116 draws lit, 12,146/12,146 vertices lit, 0 fallback; light sources 177 uploaded / 153 carried / 5 uncertain |
| Texture items | 335 visited: **219 textured** + **80 reflection-bound** + 36 no-record (the report's `untextured` count), 0 decode refusals |
| Texture decode | 169 decodes, 10 payload cache hits; tap paired 327 / defaulted 8 / ambiguous 116 |
| Texture raster (3x) | 214 items, 60,061 fragments, 5,639 alpha-fail, 9,205 blended, 9,180 FBMSK, 475 depth-masked |
| Reflection | **80 items**, 20,302 fragments written, 9,139 DATE-failed, 9,850 depth-failed, 20,302 blended, 0 FBMSK, 20,302 depth-masked (ZMSK=1) |
| Texture census | 139 distinct TBP0 pages; 214 items PSMT4 (PSM 0x14), 5 PSMT8 (0x13), 80 CT32 (the env map); 69 distinct CT32 CLUT pages |

The same numbers at the older v4 captures (textured round: 216 textured / 119 untextured with texgen 80, no-record 36,
no-uv 3; reflection round: 80 reflected, 2,258 fragments at 1x, DATE-failed 1,003, depth-failed 1,133) confirm the v6
render reproduces the recorded milestones (1x/3x = 9x fragments).

## 5. Per-draw / per-surface comparison (near car, frame VBlank 7398)

The near car (the followed Mégane) is draws 251-282; the far cars are 169-250. The live column is identical for every
draw: no texture, no UV, no CLAMP, no TEST/ALPHA/PRIM/FBMSK, alpha 255, depth write on, body_paint drawn opaque.
"Offline state" is the captured/tapped state the textured table binds.

| Draw | Surface (confidence) | Family / items | Live binding | Offline binding (TEX0 / CLUT) | Offline state |
|---|---|---|---|---|---|
| 169-170 | far car shadow/underbody (Medium) | three_qword_gain, 3+2 items | none | no texture (pass-through) | TEST 0x5820C, ALPHA 0x44, **FBMSK 0xFF000000 (alpha only)**, **ZMSK 1**, PRIM 0x6C (every item) |
| 171 | first item with a texture binding | three_qword_gain, 1 item | none | PSMT4 32x32, TBP0 **0x33A0**, CBP0 0x33A2, CSM 0, TCC 1 | TEST 0x5820C, ALPHA 0x800000048, FBMSK 0xFF000000, ZMSK 1, PRIM 0x7C |
| 172 | first visible textured fragment | nine_qword_first, 1 item | none | PSMT4 32x32, TBP0 **0x3351**, CBP0 0x3348, CSM 12 | TEST 0x5000D, ALPHA 0x44, FBMSK 0, PRIM 0x3C |
| 174 | first body/paint reflection item | body_paint, 1 item | none | CT32 **256x256 TBP0 0x2BC0**, TCC 1, linear/linear | TEST 0x5800C, ALPHA 0x58 (additive), **ZMSK 1**, PRIM 0x7C, FOGCOL 0, fog 46 |
| 251-252 | near car shadow / underbody / wheels (Medium) | three_qword_gain, 3+2 items | none | **no texture** | TEST 0x5820D/0x5820C, ALPHA 0x44, FBMSK 0 / **0xFF000000**, ZMSK 0 / **1**, PRIM 0x3C / **0x6C** |
| 253 | rear lamp bar / glow (Medium) | three_qword_gain, 1 item | none | PSMT4 32x32, TBP0 **0x3740**, CBP0 0x3742, CSM 0 | TEST 0x5820C, ALPHA 0x800000048, FBMSK **0xFF000000**, ZMSK 1, PRIM 0x7C |
| 267 | near car body shell / paint base (High) | nine_qword_first + pow16, 31 items | none | PSMT4: 32x16 **0x3662** CBP0 0x3668/0x3669; **256x128 0x367A CBP0 0x36EB CSM 4**; 64x64 0x36C7/0x36E0; 128x128 0x3709; TCC 1, linear/linear, 10 items nearest | TEST 0x5000D, ALPHA 0x44, PRIM 0x3C; standalone 3,572 textured fragments, 628 alpha-fail |
| 268 | rear window glass / frame (Medium) | nine_qword_pow16, 4 items | none | PSMT4 64x64 **0x36BF CBP0 0x36D4 CSM 8**, 64x64 0x36EE, 128x128 0x3709 | TEST 0x5000C, ALPHA 0x44, **FBMSK 0xFF000000**, PRIM 0x7C |
| 270 | window inner layer (Medium) | passthrough, 4 items | none | same PSMT4 pages, CSM 4 variant | TEST 0x5000C, ALPHA 0x44, FBMSK 0xFF000000, PRIM 0x7C |
| 271 | near car **body paint reflection** (High) | body_paint, 17 items | none | CT32 **256x256 TBP0 0x2BC0**, TCC 1, linear/linear | TEST 0x5400C (**DATE**), ALPHA 0x58, PRIM 0x7C, **ZMSK 1**, FOGCOL 0, fog 56-64 |
| 273-274 | rear lamps / small rear parts (Medium) | nine_qword_first, 3+3 items | none | PSMT4 32x32 **0x3660 CBP0 0x36D2**, 32x16, 32x32 **0x3678**; TCC 1, linear | TEST 0x5000D, ALPHA 0x44, PRIM 0x3C |
| 279, 282 | rear reflectors / trim (Medium) | body_paint, 1-2 items | none | CT32 0x2BC0 | TEST 0x5400C/0x5420C, ALPHA 0x58, PRIM 0x7C, ZMSK 1, fog 18-46 |

The car pass's paint colour therefore lives in the **T4 textures with CT32 CLUTs** (the 256x128 page 0x367A for the
body paint, 32x32/64x64 pages for trim, glass, lamps and decals) and in the **CT32 256x256 environment map at 0x2BC0**
sampled with VU1 texgen S,T by the body/paint items. The live path samples neither.

## 6. First draw / item where the colour diverges

- **First state divergence:** draw **169 item 0** (VBlank 7398, capture order): the measured state writes alpha only
  (FBMSK 0xFF000000), blends (PRIM 0x6C) and does not write depth (ZMSK 1); the live path writes opaque RGB and depth.
- **First texture binding:** draw **171 item 0** (PSMT4 32x32, TBP0 0x33A0, CLUT 0x33A2). Its bound texels fall
  outside the sampled region, so it writes no textured fragment in the standalone render (0 pixels).
- **First visible textured fragment:** draw **172 item 0** (PSMT4 32x32, TBP0 0x3351, CLUT 0x3348): including draw 172
  changes 16 pixels (max channel delta 43) versus the same subset without textures.
- **First body/paint reflection item:** draw **174 item 0** (CT32 0x2BC0, texgen S,T, ALPHA 0x58 additive, ZMSK 1; its TEST 0x5800C has DATE off, the near-car draw 271 has DATE on):
  +4 pixels alone; the full-pass reflection count is 20,302 fragments at 3x.
- **Owner-visible near car:** first body texture draw **267 item 0** (T4 32x16 TBP0 0x3662) and the body paint page
  0x367A (items 4-10); first near car reflection **271 item 0** (CT32 0x2BC0). Including draw 267 changes 1,842 pixels
  in the near-car box (1x) and draw 271 alone adds 785 reflection pixels there once the base alpha exists.

## 7. Measurements (live / offline / reference)

Near-car mask: pixels in the 1920x1344 near-car rectangle that differ between the live present (VBlank 7400) and the
faithful CoreGS 2.0 reference (18,568 px; eroded interior 10,208 px). Offline images rendered from VBlank 7398 and
aligned by the measured 2-VBlank shift (-12,-14).

| Image | vs live (eroded mask) | vs live (full mask) | Car mean RGB (full mask) | red-dominant |
|---|---|---|---|---|
| Live present (world off) | - | - | **27.5, 26.9, 25.8** | 2.8% |
| Offline `--no-textured` | **36.08 dB**, 1.01, 2.9% | 30.07 dB, 2.00, 5.7% | 26.8, 26.2, 25.3 | 2.5% |
| Offline textured + reflection | 20.12 dB, 18.23, 66.2% | 20.17 dB, 17.42, 62.9% | 12.5, 12.2, 10.5 | 4.6% |
| Reference (CoreGS 2.0, same frame) | - | - | 24.2, 20.2, 19.5 | **8.2%** |

(pairs are PSNR, mean |d|, percent of pixels with max-channel delta > 8. The textured path is darker than the reference
in absolute level because the offline reflection model's texgen scale is still open - round 20's measured 17.9 dB
against the reference; what it restores is the *content*: paint texture, lamps, window textures and the additive paint
reflection. The fix's reference is the offline renderer, not the final reference image.)

## 8. Fix proposal (minimal, no fidelity shortcuts)

The fix reuses the code the live world already proved; it is a port of the offline car state to the present pipeline,
plus the reflection pass. Estimated scope: one present-layer change, one shader pair, one texture-store switch change.

### 8.1 Decouple the live texture store from the world switch

The per-job texture store and the VRAM base are currently disabled when `GT4_COREGS3_LIVE_WORLD=0`
(`coregs3_live_world.cpp:124-145` `note_job_source`/`note_job_textures`, `:238` `note_vram_base`; sink installed only
for the world at `coregs3_hooks.cpp:468-477`; base copy gated on `live_world::active()` at `hw_gs_backend.cpp:2134`).
The car needs those pixels too, including the car-only A/B.

- `native/coregs3/coregs3_live_world.hpp/.cpp`: keep `active()` as the **store** switch (on in `present`), and add
  `bool world_enabled()` that reads `GT4_COREGS3_LIVE_WORLD` (default 1) for the world draw decision.
- `native/coregs3/coregs3_hooks.cpp`: in `present`, always `configure_from_environment()` and install the job source
  sink; set `g.world_draws = live_world::world_enabled()` (car draws are stored by `g.in_car_pass` regardless).
- `native/hw_gs/coregs3/hw_coregs3_present.cpp::init` (`:2007`): `live_world_enabled = live_world::world_enabled()`;
  the store's `find()` remains available to the car build.
- `native/hw_gs/hw_gs_backend.cpp::flush`: the VRAM base copy stays gated on the store (`active()`), which is true in
  `present`.

### 8.2 Build the car texture table and per-item ranges

In `Impl`, on the claim/render thread under the existing `world_mutex`, build a car `FrameTextureTable` from the same
frame:

1. Collect the car item packet addresses (`pass == kPassCars`, `packet_bytes != 0`), look each up with
   `live_world::find(address)` into a `std::map<std::uint32_t, VramRecord>` (the `build_world` code at `:1166-1201`
   does exactly this for the world), and synthesize the tap view with
   `live_world::build_tap_snapshot(frame.vblank, car_addresses, tap_data, tap_records)` (`:1233`).
2. Reuse the frame-end VRAM base (`world_base_slm`, filled at `:1207-1225`) as the decode image; a page no job window
   covers decodes from it, never from a zero fill.
3. `TextureFrameOptions car_options; use_tap = live_tap; live_job_textures = &car_live_map; reflections = true;
   world_channels = false;` and call `gt4::coregs3::build_frame_texture_table(frame, world_base_slm, tap_data.data(),
   tap_records, car_options, car_table, car_counters, error)`.
4. Keep the car table per frame (it owns the decoded payloads), like the world table is derived per frame.

The table binds the same 219 textured items and 80 reflection items (§4). Then `build_frame` must emit ranges per
**item**, not only per position encoding: each range carries the item's `SampleState` (TEST/ALPHA/FBMSK/ZBUF/PRIM/
FOGCOL), the sampler's TEX0/CLAMP/atlas rectangle, the model UV per vertex, the per-vertex fog (reflection items) and a
`reflection` flag. The world `WorldPush`/`WorldRange` structures (`:95-122`) are the template; the car range keeps the
car's metre columns (`columns_from_upload_rows`, `:178`).

### 8.3 Car vertex/fragment state to bind

- Vertex: add `vec2 uv` (model UV; `build_mesh` already produces `MeshVertex::uv`) and `float fog` to `PresentVertex`
  (16 -> 28 B, like `WorldVertex`), model position in metres and the lit RGBA8 colour unchanged.
- Fragment (new car stage, based on `coregs3_world.frag` which already samples the atlas with the measured 10.4
  coordinate, nearest/bilinear with the four wraps, MODULATE with TCC, alpha test AFAIL KEEP, per-vertex fog to
  FOGCOL, FBMSK): add the two states the world path counts as deferred and the car needs:
  - **blend**: read the old display word (`imageLoad`, the shader already does this for FBMSK), compute the measured
    ALPHA equation in 8-bit (`(Cs-Cd)*As+Ad`, the car default 0x44; the reflection additive (0,2,1,1) 0x58) exactly as
    `blend_fragment` does in `coregs3_render.cpp`, clamp (COLCLAMP), then merge FBMSK. The alpha lane must be the
    measured one, not 255, because the reflection DATE tests it.
  - **DATE**: TEST bit 14 and DATM bit 15; test the old word's alpha MSB against DATM and discard on mismatch.
- Depth: the base car pipeline stays GEQUAL with depth write (ZMSK=0 items). Add a second pipeline with
  `depthWriteEnable = VK_FALSE` for ZMSK=1 items (reflection and the alpha-only glass/shadow items). Draw the ranges in
  capture order; the reflection draw keeps the same GEQUAL test (its depth equals the base surface), so no write is the
  only difference.

### 8.4 Add the body/paint reflection pass

For every item whose binding is `binding.reflection` (or `is_body_paint_item(item)` + the reflection binding check):

1. Per vertex compute the texgen coordinate with `gt4::coregs3::reflection_st(item, vertex, out)`
   (`coregs3_reflection.hpp:62`; it uses the item's six-qword light block, the raw V3-16 position and the V3-8 normal,
   all present in the live snapshot - the F8 report shows the blocks are captured).
2. Sample the bound CT32 environment map (page 0x2BC0) with that S,T (linear/linear, region clamp), MODULATE with the
   lit vertex colour, mix the tapped per-vertex fog toward FOGCOL (black), run the alpha test.
3. Apply the measured pass state: TEST 0x5400C (DATE against the destination alpha written by the base draws), ALPHA
   0x58 additive, PRIM 0x7C, ZBUF ZMSK=1 (no depth write), FBMSK 0, GEQUAL.
4. The reflection fragments must be counted (`reflection_items`, `reflection_pixels`, `date_failed`, `depth_failed`,
   `blend_pixels`) and reported in F8; the offline numbers are the control (§4).

### 8.5 Atlas and counters

- Extend the world atlas (or add a second 2048x2048 atlas bound to the present descriptor set) with the car table's
  decoded payloads; when both the world and the car are drawn in one tick, pack both tables' `SampledTexture` records in
  the same atlas (the table records are stable pointers, the `build_world` atlas map is already the pattern).
- Extend `present::WorldFrameCounters` (or add `CarFrameCounters`) with `car_texture_items`, `car_textured`,
  `car_reflected`, `car_no_record`, `car_refused`, `car_texture_pixels`, `car_reflection_pixels`,
  `car_shadow_blended`, `car_date_failed`; print them in the F8 report next to the present family lines.
- Add an A/B switch `GT4_COREGS3_CAR_TEXTURES` (default 1, `0` keeps the current path byte for byte) for the
  implementation round's evidence.

## 9. Acceptance checks for the implementation round

1. **Car matches the offline renderer.** Present with `GT4_COREGS3_LIVE_WORLD=0` at the same VBlank as the offline
   capture and compare after the measured 2-VBlank alignment: the eroded near-car mask must reach **PSNR >= 28 dB,
   mean |d| <= 3 levels, pct_bad <= 10%** against the offline textured+reflection 3x render (today 20.12 dB / 18.23 /
   66.2%; the live base is 36.08 dB / 1.01 / 2.9%). The full mask gate: **PSNR >= 25 dB, pct_bad <= 20%**.
2. **The fix must change the car, not be a no-op.** Live-fixed vs live-base on the near-car interior must differ by
   mean |d| >= 5 levels, and the red-dominant fraction of the body mask must rise from 2.8% toward the offline path's
   4.6% (the remaining gap to the 8.2% reference is the open texgen scale, not this fix).
3. **State counters.** Per frame the car table reports **219 textured, 80 reflected, 36 no-record, 0 refused** (the
   offline frame-0 counts); the reflection fragment count is within +/-5% of the offline 20,302 at 3x (or the exact 1x
   scaling), DATE-failed within +/-5% of 9,139, and the blend/FBMSK counts are non-zero for the glass/lamp draws.
4. **A/B.** `GT4_COREGS3_CAR_TEXTURES=0` reproduces the current present image byte for byte (frame dump SHA-256 at 1x/3x).
5. **No regressions.** CTest coregs3 (all), Python suite, and the 3x off/shadow identity gate **COREGS3-NIGHT-CHECK-OK**
   (8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences, 13/13 frames) unchanged; the present is not part of
   the gate, the gate proves the store decoupling did not touch the guest.
6. **Owner-visible.** In the live present the Mégane shows its paint colour, window/glass texture and red taillights;
   the near-car body no longer reads as a neutral grey silhouette. Per-scene visual check (night race first; then rain,
   rally, replays, split screen) before the default is claimed.

## 10. Confidence and open questions

- **Confirmed:** the live car pass binds only lit vertex colour; the measured live image is the offline no-texture path
  (36.08 dB after alignment); the offline textured path binds 219 textures + 80 reflection items and restores the red
  paint/lamps/reflection. Code and pixel evidence agree.
- **High confidence:** the exact pages/CLUTs/state per draw (captured and tapped values printed by the branch's own
  table builder); the first diverging draw/item list.
- **Medium:** the surface names for glass/lights/wheels/shadow (visual attribution of draws 251/252/253/268/270/273/274);
  the family, state and pixels are exact, only the human label is inferred.
- **Open:** (1) the offline reflection model's absolute colour (round-20 texgen scale; 17.9 dB against the reference),
  so the fix's target is the offline renderer, not the final reference; (2) the base car pass's fog (the offline car
  path deliberately does not fog; the live car would inherit the same limitation); (3) whether the live job record for
  the reflection job carries page 0x2BC0 or the frame-end base must supply it (the offline car path uses the vblank
  image; the live store's per-job windows should cover it, to be verified in the round); (4) the car pass's texture
  mip level (the car items measured MXL 0 in round 15; level 0 only here).

## 11. Evidence and reproduction

Evidence root: `O:/Antigavity/gt4-wt-090-car-study/build/analysis-car/` (gitignored).

- Tools: `tool-build/gt4_coregs3_render.exe`, `tool-build/gt4_coregs3_tap.exe`; analysis helpers
  `filter_cars.exe`, `dump_car_bindings.exe`; capture `cars-7398.gt4scene` (filtered v6, 116 car draws/frame).
- Reports: `cbase-3x.txt` (no texture), `ctex-3x.txt` (textured + reflection), `ctex-norefl-3x.txt`,
  `car-bindings.txt` (335 lines, per item/draw state), `perdraw/d*-texrep.txt` (per-draw binding reports).
- Images: `cbase-3x.png`, `ctex-3x.png`, `ctex-v7402-3x.png`, `ctex-norefl-3x.png`, `nearcar-draws.png`,
  `ref_car.png` / `present_car.png` (reference vs live near car).
- Inputs: capture/tap/vram from
  `O:/Antigavity/gt4-wt-090-mip-recapture/build/evidence-mip-recapture/capture/`; live present and reference dumps from
  `O:/Antigavity/gt4-wt-090-live-world/build/evidence-live-world/night-liveworld-off/frames/frame_07400.ppm` and
  `night-c3n-ref/frames/frame_07400.ppm`; offline milestone evidence
  `O:/Antigavity/gt4-wt-090-textured-render/build/evidence/coregs3-textured/`,
  `O:/Antigavity/gt4-wt-090-body-reflection/build/evidence/coregs3-refl/`,
  `O:/Antigavity/gt4-wt-090-family-light/build/evidence/coregs3-r18/`.

Core commands (from the worktree root; tools under `build/analysis-car`):

```
cmake -S . -B build/analysis-car/tool-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl \
  -DGT4_ASSETS_ROOT=C:/Antigravity/gt4-staticrecomp \
  -DGT4_PYTHON=C:/Antigravity/gt4-staticrecomp/private/tooling-venv/Scripts/python.exe \
  -DGT4_HW_GS=ON -DGT4_COREGS3=ON -DGT4_HW_VULKAN_SDK=C:/VulkanSDK/1.4.341.1
cmake --build build/analysis-car/tool-build --target gt4_coregs3_render gt4_coregs3_tap

build/analysis-car/filter_cars.exe night-mip.gt4scene cars-7398.gt4scene
build/analysis-car/tool-build/gt4_coregs3_render.exe cars-7398.gt4scene --frame 0 \
  --vram night-mip.gt4vram --tap night-mip.gt4tap --width 1920 --height 1344 \
  --out ctex-3x.png --report ctex-3x.txt --reflection-mask crefl-mask-3x.pgm
build/analysis-car/dump_car_bindings.exe cars-7398.gt4scene night-mip.gt4tap night-mip.gt4vram 0 > car-bindings.txt
```

The metrics script `build/analysis-car/probe_images.py` / the inline scripts in the shell history compute the aligned
PSNR, mean |d| and red-dominance tables of §7; the masks are `cbase-mask-3x.pgm`, `ctex-mask-3x.pgm` and
`crefl-mask-3x.pgm`.
