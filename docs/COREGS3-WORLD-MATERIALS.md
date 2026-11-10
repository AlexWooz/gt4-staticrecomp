# CoreGS 3.0 world materials: the night-course brightness decomposition (TASK-090)

Branch `task/090-world-materials`, based on `task/090-coregs3` at `248ec453`. Read-only measurement study: no
`native/` source change. Inputs: the M1 capture `night-m1.gt4scene`/`.gt4tap`/`.gt4vram` (VBlank 7398) and the CoreGS
2.0 reference frame `frame_07400.ppm` (3x 1920x1344). Every number below was reproduced in this worktree with the
branch's tools (`gt4_coregs3_render`, `gt4_coregs3_tap`, `gt4_coregs3_texture`).

## Question

The offline world render (sky + far underlay + main-view course) was measured on the M1 frame as PSNR 20.36 dB,
SSIM 0.6137, pct_bad 77.19%, mean |diff| 20.29, luma 26.69 (reference) -> 31.19 (ours). Reproduced here exactly. The
picture is brighter and chromatically wrong. The study separates (a) per-item material, (b) fog and (c) the global
post chain, pins each with numbers, and orders the smallest fixes.

## Answer in one line

The gap is **not** fog and **not** a missing global post transform: it is the **texture page and palette content**.
The capture carries one GS-local-memory snapshot per frame, taken at the frame-end Flush; the game streams different
textures through the same VRAM page during the frame (page `TBP0 0x36D0` is bound by course draws 35/37/42/44), so the
snapshot holds whatever was uploaded last, not the world-pass content. The dominant visible page (`0x36D0`, 53.7% of
the compared mask) decodes to an advert/2D-style atlas and correlates with the reference at **-0.009**; it contributes
**+3.90 of the +4.50 luma gap (86.7%)**. The renderer's fragment arithmetic (MODULATE, alpha test, blend, fog) is
CoreGS 2.0's own integer form, so the arithmetic is not the error; the inputs are.

## Measurements

### 0. Baseline and controls (C)

- Baseline reproduced byte-for-byte metric-wise: PSNR 20.36 dB, SSIM 0.6137, pct_bad 77.19%, mean |diff| 20.29,
  luma 26.69 -> 31.19, coverage 100% of the scene area, car spill 1.61%.
- Reference/render alignment: the best luma match over a +/-20 px search is **(0, 0)** (MSE 1314.6 at (0,0), identical
  to the zero-shift value). The scenes are the same moment; misalignment is not a cause.
- The compared mask is the M1 definition: the renderer's coverage (the sky fills it), minus the car mask dilated 2 px,
  minus the measured HUD rectangles: **1,780,790 px** at 3x.
- Tool counters on this frame: 127 selected world draws (sky 1/16 items/940 vertices, far underlay 3/9/378, course
  123/1,192/61,173), 1,215 textured selected items, 1,180 bound to the tapped state and 35 to the record fallback,
  2 untextured.

### 1. (a) Per-item material

#### 1.1 Vertex colour: decoded correctly (C)

Method: pair every world item with the tapped GIF stream whose emitted screen positions match the item's projected
vertices (the strongest pairing key: wrapped 12.4 coordinates, a per-draw constant drawing offset absorbed with the
median; 1,069 items position-verified at <= 1 px^2 residual). Then compare the capture's decoded V4-8 colour with the
RGBA the VU1 actually emitted (the tap's RGBAQ).

- Over the renderer's UV pairing: 1,203 items / 61,060 vertices, **57,480 exact (94.1%)**, 57,779 within a three-level
  RGB sum (94.6%), alpha exact on every vertex, mean |d| R 1.40 / G 1.31 / B 1.17. The 5.9% tail is dominated by wrong
  UV pairs on repeated quad UVs, not by the decode.
- Over the **position-verified** pairs (mean squared screen residual <= 1 px^2): **928 items / 45,651 vertices, 120
  mismatching vertices (0.263%), only 3 items with any mismatch**, mean |d| 0.18 / 0.19 / 0.18 per lane, alpha exact.
  Those 3 items are where the tapped stream carries a default (128,128,128) or a one-vertex state; the capture colour
  is not the VU1's there. The decode itself is exact on every other vertex.
- Colour channel source: **every paired world item reads V4-8 at TOPS 0x80**; the 15 paired items without a colour
  at 0x80 are exactly the 15 excluded 0xB08 items. The 0xA0 family stores its V4-8 at 0xA0 and is excluded by
  dispatch, so the `0xA0` colour path does not touch the selected picture at all.
- Conclusion: the vertex-colour path is exact (C); it is not a cause. The only differences are stream-pairing
  artifacts (fix 2).

#### 1.2 PRIM, TFX, TCC, alpha (C)

Tapped state census over the 1,218 paired world items:

- **PRIM**: `0x3C` 1,093 items (TME + FGE, ABE off), `0x7C` 125 items (ABE on; the sky and the blended course items).
- **ALPHA**: normal `(Cs-Cd)*As+Cd` 1,215 items; `(A=Cs, B=0, C=As, D=Cd)` 3 items. The blend selector model evaluates
  both.
- **TEST**: course items sample the pass journal `0x5800E` (ATE off) by the M1 rule; sky `0x5820C`; far underlay
  `0x5820D` (ATE GREATER 32). Counted in the report.
- **TEX0.TFX = 0 (MODULATE) on all 1,218 items; TEX0.TCC = 1 on all.** No DECAL or other function is in the selected
  set; the sampler's MODULATE, alpha test, blend and fog arithmetic is the same integer code as CoreGS 2.0
  (`native/hw_gs/pixel/hw_gs_pixel.cpp`), so these cannot explain a systematic brightness difference.
- The alpha lane is exact (100%); the MODULATE alpha uses it as the GS does. AFAIL modes are counted and implemented.

#### 1.3 Texture page content and palettes: the dominant cause (C for the content mismatch, H for the mechanism)

Method: a 1x frontmost-item rasterisation of the capture's course items (the same projection and matrix-source rule
as the renderer, w-buffer, no alpha test; 11 s; covers 286,047 of 286,720 px = 99.8% of the viewport) gives a per-pixel
texture page `TBP0`, draw/item and interpolated tapped fog. Joined with the 3x reference and render.

| class | px (3x clean) | share | luma ref -> world | ratio | mean abs | PSNR |
|---|---|---|---|---|---|---|
| all | 1,780,790 | 100% | 26.69 -> 31.19 | 1.169 | 20.29 | 20.36 |
| page `0x36D0` | 955,413 | **53.7%** | 24.01 -> 31.29 | **1.303** | 18.01 | 21.72 |
| other pages | 825,377 | 46.3% | 29.79 -> 31.08 | 1.043 | 22.93 | 19.18 |

- The luma gap is +4.502. Page `0x36D0` contributes **+3.904 (86.7%)**; all other pages **+0.598 (13.3%)**.
- Page `0x36D0`'s decoded content is a T4 128x128 atlas of adverts/2D-style patches (`tex-dump/...tex21...png`), while
  the reference at those pixels is road, kerb and pit-lane surface. The per-pixel luma **correlation is -0.009**
  (least-squares slope -0.011): the content is unrelated, not merely brighter.
- The page is a real course material: draws 35, 37, 42 and 44 bind it (23 visible items), and it covers the road/pit
  apron. The road items' UVs span -0.49..1.49 (REPEAT, full-page CLAMP), so the whole page tiles; the reference does not show it.
- **Why the sidecar is wrong:** the `.gt4vram` record is written by the GS worker at the frame-end Flush, and the
  renderer selects the record at `frame.vblank + 1` (7399 for frame 7398). Six records exist (VBlanks 7398..7403); the
  page `0x36D0` bytes are **identical in all six** (SHA-256 `a191c894...`; frame-end content is stable), i.e. the
  snapshot can only ever show the last upload of the frame. A page streamed by several draws in one frame cannot be represented. The exact uploader
  of the atlas is **Unknown**; that it is not the world-pass content of draws 35/37 is **High confidence** from the
  zero correlation and the reference.
- Other pages show the same capture-side problem in both directions: pages whose content is unrelated (correlation
  < 0.2) at 1x include `0x3832` (15,327 px), `0x37D0` (9,854), `0x3D6A` (4,132), `0x3DCE` (4,098), `0x3C40` (3,975).
  Pages whose content tracks the reference but with a per-page gain (correlation > 0.9) include `0x3D36`
  (corr 0.960, slope 1.848, mean ref/world 1.37, 6,150 px) and `0x3CAE` (corr 0.931, slope 1.423, mean ratio **1.006**,
  4,307 px). The per-page means at 3x range 0.32..2.09; the matched pages do not share one gain.
- Per-channel means (1x, reference/world): all R 0.859 / G 0.864 / B 0.764; page `0x36D0` R 0.750 / G 0.791 / B 0.656;
  other pages R 0.992 / G 0.950 / B 0.905. The blue-violet excess is mostly the atlas page; the remaining pages are
  near 1.0 in red and green and about 10% high in blue.

### 2. (b) Fog: applied from the tap, small effect, not the cause (H)

- The M1 renderer applies the **tapped** per-vertex fog (the XYZF2 lane the VU1 emitted) with CoreGS 2.0's integer mix
  formula `(F*C + (255-F)*FOGCOL) >> 8` toward `FOGCOL 0x27272A` (luma 39.4). The formula is the reference renderer's
  own, so the fog response cannot differ from the reference; only the values and their pairing can.
- Measured in the page map: fog range 196..255 (255 = no fog); on the 3x clean mask 38.3% of pixels have fog < 255, mean 236.3.
  Inverting the mix to recover the unfogged fragment gives a fog luma delta of **mean -0.167 levels on fogged pixels,
  mean |delta| 2.17, maximum +8.26**, i.e. **-0.51% of the reference luma** over the clean mask (about -0.14 levels of
  26.69). Fog cannot produce a +4.5-level gap.
- Depth dependence: on the asphalt page `0x3EE1` (ref/world luma ratio, depth quintiles) the ratio is 1.245, 1.319,
  1.235, 1.385, 1.242 with fog 255, 255, 255, 250.6, 233.8. There is no depth trend; fog does not shape the residual.
- Pairing: the renderer pairs each item with the tapped stream by UV equality. The position-verified pairing differs
  for **252 of 1,180 selected items (15.5% of the selected area)**. The two streams' fog differs there by an
  area-weighted **10.76 levels**, a rough luma effect of **0.09 levels over the whole picture** (0.3% of the gap);
  their TEX0 differs on only 1,013 px (0.4% of the area), so the pairing is not the texture-page cause. It is a real
  but small fog/PRIM accuracy item (see fix 2).

### 3. (c) The global post chain: no measurable uniform transform (M-H, post itself Unknown)

The effects study (journal 2026-11-10) says the post chain (brightness/tone/glare/crossfade) is EE immediate PATH3,
is not captured, and reports **no tone stage at night**. The frame data agree that no global transform is needed:

- A global gain would multiply every texture page's fit by one factor. Over the pages above 10k px the fits span slope
  **-0.03..2.09** and intercept **-9..+55**; the high-correlation pages disagree (mean ratio 1.006 vs 1.37). There is
  no single gain, offset or per-channel scale.
- The best-matching page (`0x3CAE`, kerb stripes, 38,763 px at 3x) has luma 7.61 vs 7.66 (**ratio 1.006**), PSNR
  35.06 dB, mean |diff| 5.02. A global 17% brightening of the reference (the size the headline ratio implies) would
  move this page by about +1.3 levels and is excluded.
- The tone-curve test (reference luma at the same world luma, per page) shows some pages agreeing within 1.5-2.2
  levels and others differing by 5-13 levels at the same input. A shared curve would agree everywhere; the spread is
  per-material.
- Window of doubt: with the 2D/post/backdrop passes uncaptured and the HUD drawn after the post, a global transform
  smaller than the material noise (roughly 2-3% luma) cannot be excluded. It is **not needed** to explain the gap.

### 4. Excluded and missing content (context, not the cause)

- The world selection excludes 72 course items (0xA0 46, 0xB08 15, 0x1278 11) and 14 mirror items. They own at most
  ~5.7% of the page map's covered pixels and cannot move a 4.5-level mean.
- The captured wet-road reflection pass (pass 7, 4 draws / 7 items) and the 2D/post passes are not drawn by the world
  renderer. The reference is brighter than our render on the asphalt page (`0x3EE1`, ref/world 1.24-1.39 across depth)
  where an additive reflection layer applies; that is a missing-layer candidate for fix 3, not a global transform.

## Decomposition table

| # | cause | measured effect | confidence |
|---|---|---|---|
| 1 | Texture page content from the frame-end SLM sidecar (dominant page `0x36D0`) | +3.90 of the +4.50 mean luma gap (86.7%); 53.7% of the mask; page/reference luma correlation -0.009 | C that the content is not the reference's; H for the frame-end-streaming mechanism |
| 2 | Other pages' content / palette (same sidecar) | +0.60 luma (13.3%); per-page ratios 0.32..2.09; matched pages show gains 1.0..1.85; "other pages" PSNR 19.18 dB | H (content correlation) / M (palette) |
| 3 | Vertex colour decode (V4-8 0x80) | position-verified: 0.263% of 45,651 vertices differ, 3 items, alpha exact; not a cause | C |
| 4 | TFX / MODULATE / TCC / CLUT arithmetic | all selected items MODULATE with TCC=1; arithmetic is CoreGS 2.0's | C |
| 5 | Fog (tapped F, FOGCOL mix) | -0.51% of the reference luma overall; no depth trend | H |
| 6 | Fog/state stream pairing (UV vs position) | 15.5% of the selected area; ~0.09 luma levels overall | H |
| 7 | Global post chain (brightness/tone/filter) | no uniform transform measured; a page with verified content matches at 1.006 | M-H; post passes themselves Unknown |
| 8 | Excluded 0xA0/0xB08/0x1278, mirror, reflection, effects | <=~5.7% of covered pixels for the programs; reflection may explain a local road deficit | H for area; M for the reflection attribution |

## Fix list (smallest first, priority order)

1. **Capture texture pixels and palettes at the world pass, not only at frame end (root cause).**
   Cheapest form: one extra `.gt4vram` record at each K7 pass-begin marker (the pass journal already fires them), so
   the course pass has its own SLM snapshot; the complete form: copy the page/palette on every TEX0/TEX2 upload or when
   a draw first binds a page. Expected: the `0x36D0` and low-correlation pages get their real asphalt/kerb/concrete
   content and the gain pages their real palettes; the +3.90 luma contribution and most of the PSNR gap go away.
   Gate: page `0x36D0`'s luma correlation with the reference > 0.5 and its ref/world ratio 1.0 +/- 0.15; every page
   over 10k px the same; world PSNR >= 24 dB, SSIM >= 0.75, pct_bad <= 10% on the M1 3x frame (the M1 Hyp thresholds);
   the car render stays byte-identical.
2. **Pair per-item state by position, not UV only.** Use the position residual (wrapped 12.4, per-draw drawing offset)
   as the pairing key, or reject a UV match whose position residual is above 1 px. Expected: the 252 items (15.5% of
   the area) get their own fog/PRIM/TEX0; local fog corrections of ~10 levels where those items are visible (about
   0.09 levels on the mean); no change to the texture-page class split (only 1,013 px of TEX0 differences).
   Gate: the census shows 0 selected items whose drawn stream is not the position-verified one; the per-vertex RGB
   match to the tap stays 100% within 1 level.
3. **Draw the captured wet-road reflection pass before attributing the remaining road darkness to palettes.** It is in
   the capture (pass 7, 4 draws/7 items, additive `(Cs-0)*Ad+Cd`, DATE, ZBUF no-write, FOGCOL black). Expected: the
   asphalt page's ref/world ratio moves from 1.24-1.39 toward 1.0. Gate: page `0x3EE1` ratio 1.0 +/- 0.1 and the
   reflection pass's own fragments match the reference in its mask.
4. **Do not add a global post transform yet.** Capture the PATH3/2D GS stream and measure the post chain first
   (the effects study's capture item). Gate: the captured post passes reproduce the reference's global curve; with the
   transform applied the matched pages stay at ratio 1.0 +/- 0.05 and the best page's PSNR does not fall below 35 dB.
5. **Keep the fragment arithmetic and the tapped fog unchanged (with fix 2's pairing).** No code change; optionally
   pin the fog mix and MODULATE against `hw_gs` with high-F cases in `native_coregs3_*` fixtures.

## Evidence (this worktree, gitignored under `build/`)

- `build/evidence/world-materials/baseline-3x.png`, `baseline-mask.pgm`, `baseline-report.txt`, `baseline-metrics.json`
  - the reproduced baseline.
- `build/evidence/world-materials/course-only-3x.png` / `-mask.pgm`, `underlay-only-*`, `sky-only-*` - the per-pass
  split (the sky covers the whole viewport; the course covers 99.8%).
- `build/evidence/world-materials/page-fog-map.npz` - per-pixel page, draw/item, depth and tapped fog at 1x.
- `build/evidence/world-materials/final-decomposition.json` - the 3x page-class table, luma-gap shares and fog share.
- `build/evidence/world-materials/tex-dump/`, `tex-pages/` - the decoded pages and contact sheets (`tex21` = the
  `0x36D0` atlas; `tex243` = the `0x3EE1` asphalt; `0x3CAE` = the kerb).
- `build/evidence/world-materials/road-zoom.png`, `preview/`, `atlas-vs-hud.png` - visual evidence.
- Scripts (developed here, rerunnable): `build/analysis/gt4scene.py` (scene/tap/GIF/VIF reader), `probe_pairing.py`,
  `probe_colors.py`, `probe_color_pos.py`, `probe_color_detail.py`, `probe_state_census.py`, `probe_selected_pairing.py`,
  `probe_cover.py`, `map_pages.py`, `decompose_pages.py`, `decompose_fog_post.py`, `decompose_fit.py`,
  `final_decomposition.py`, `tone_curves.py`, `preview.py`.

## Reproduction

```
cmake -S . -B build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=".../clang-cl.exe" -DCMAKE_CXX_COMPILER=".../clang-cl.exe" \
  -DGT4_HW_GS=ON -DGT4_ASSETS_ROOT=C:/Antigravity/gt4-staticrecomp ...   # build/configure-tools.cmd
cmake --build build/tools --target gt4_coregs3_render gt4_coregs3_tap gt4_coregs3_texture

build/tools/gt4_coregs3_render.exe night-m1.gt4scene --frame 0 --out baseline-3x.png \
  --width 1920 --height 1344 --vram night-m1.gt4vram --tap night-m1.gt4tap --world \
  --report baseline-report.txt --mask baseline-mask.pgm
python build/analysis/map_pages.py night-m1.gt4scene night-m1.gt4tap page-fog-map.npz
python build/analysis/final_decomposition.py
python build/analysis/decompose_fog_post.py
python build/analysis/probe_color_pos.py night-m1.gt4scene night-m1.gt4tap
```

## Limits and unknowns

- The page map is a 1x frontmost-w rasterisation of the course items only (no sky/underlay, no alpha test/blend,
  nearest upsampled to 3x); it still covers 99.8% of the clean mask. The excluded items own at most ~5.7% of it.
- The exact pass or uploader that overwrites page `0x36D0` is **Unknown**. The frame-end snapshot timing, the stable
  frame-end page bytes across six records and the zero correlation with the reference establish that the sidecar cannot
  be the world-pass content; they do not name the later writer.
- The post chain is not captured; only the *absence of a required global transform* is measured (a residual below
  ~2-3% luma is Unknown).
- The reference renderer is CoreGS 2.0's own frame; the study assumes it is the fidelity target, as TASK-090 defines.
- The 1x page correlation numbers are statistical over 4-138k-px classes; the 3x page-class numbers are the headline
  metrics.
