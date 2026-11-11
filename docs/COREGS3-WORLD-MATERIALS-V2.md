# CoreGS 3.0 world materials v2: the remaining brightness decomposition (TASK-090)

Branch `task/090-world-materials-v2`, based on `task/090-coregs3` at `008a71c8` (the S2 merge). Read-only measurement
study: no `native/` source change. Every number below was reproduced in this worktree with the branch's own tools
(`gt4_coregs3_render`, `gt4_coregs3_gs`, `gt4_coregs3_tap`) built from `scripts/coregs2_bench/game/configure.cmd`.

## Status (2026-11-10, branch `task/090-post-chain`): fixes 1 and 2 applied; the post chain explains the level

The follow-up round applied the study's fixes 1 and 2 (worktree `O:/Antigavity/gt4-wt-090-post-chain`, based on this
branch at `0a9a7e38`; method in [COREGS3-HOOKS](COREGS3-HOOKS.md) §29):

- **Fix 1 done (C for the conversion, H for the bias).** The reference is CT16 (1.0000 of the clean-mask values are
  divisible by 8, max 248 - reproduced). The present blit's captured **DTHE = 1, DIMX = 0x0001044501100554,
  COLCLAMP = 1** are now decoded from the stream, and the offline present models the CT16 conversion with the captured
  dither at native 3x cells (CoreGS 2.0's own rule). The metrics script reports the **raw** and the **+4-corrected**
  reference in every run, in the text and the JSON; the raw baseline is unchanged.
- **Fix 2 done (C).** The whole PATH2 full-screen chain is classified and applied in stream order: 11 full-screen
  batches per frame, **11/11 accounted for, 0 unrecognised** (4 pre-world clears, the frame sample/depth clear, the
  pre-reflection alpha clear, the composite, FIX 127, FIX 255, the CT16 present, the display alpha clear). The frame
  sample is **not** a colour transform: FBMSK 0xFFFFFFFF masks every colour lane and ZBUF.ZMSK 0 with ZTST GEQUAL and
  z=1 writes the depth buffer, i.e. it is the frame's depth clear (C).
- **The chain explains the level (C):** world luma 18.95 -> 34.60 pre-present -> 29.85 after the CT16 present, against
  the raw reference 26.69 and the +4-corrected 30.69. Ratio 1.409 raw (1.620 corrected) becomes **0.894 raw** (1.028
  corrected); per-page median ratio **0.943** raw (0x2FC0 0.988, 0x3EE1 0.974, 0x37D0 0.968, 0x3E26 0.966,
  0x36D0 0.943).
- **The residual is structural, not a level (C for the numbers, H for the stage):** the pooled fit is now
  `ref = 0.3727*present + 15.566` (corr 0.492); the correlated pages' slopes are 0.60-0.90, the road page's
  correlation stays 0.099 with a nearly flat reference (`0.123*w + 20.87`), and the raw reference's darkest decile
  (mean 4.09) maps to 16.56 (the pre-chain world was already 11.98). This points at fixes 3 and 4 (the material
  palette/texel path and the mip/LOD path), **not** at the post chain: the two multiplies are real, captured, and
  account for the level; the material path stays 10-40% too contrasty and lifts the black floor.

The original study text follows unchanged; the "Fix list" entries 1 and 2 are marked done below.

## Question

The offline world render is darker than the CoreGS 2.0 reference frame. Frame 0 (VBlank 7398, 3x, reference
`frame_07400`): PSNR 22.350, SSIM 0.6506, pct_bad 64.77%, mean |diff| 14.84, luma 26.69 -> 18.955, whole-mask linear
fit **ref = 0.6341*world + 14.668**; page `0x36D0` fit 0.2831*world + 19.307. The pass-textures, world-families, morph
v6 and S2 rounds are all in. This study tests the fog-blend shape, the per-page material/texture, the coverage/spill and
the S2 frame-0 caveat, and pins what remains.

## Answer in one line

The whole-mask affine is a **pooling artifact**, not a fog blend: the compared mask is the course pass (99.996%), the
tapped fog is almost neutral everywhere (mean F 247.8) and the per-pixel FOGCOL-blend factor solved from the reference is
inconsistent per channel (R 0.57 / G 0.66 / B 0.73) and anti-correlates with the tapped F. The reference is a **CT16
5-bit scanout** (every value is a multiple of 8), so the dump is ~4 levels darker per channel than the framebuffer it
came from; corrected, the reference is **1.62x brighter** than our world (per-channel R 1.64 / G 1.61 / B 1.59, a neutral
gain) and the residual is **per-material** (per-page ratios 1.50-1.93 for nearly every course page, with page 0x3BF6
the one mean outlier at 0.94). The input path is verified exact: the road page's texel indices are identical to
CoreGS 2.0's own dump (agreement 1.000), the road vertices' colours are identical to the tap (48/48, 38/38 and 64/64),
and the blend uses the GS's alpha/128 convention. The sphere-mapper source is fully populated at every captured vblank,
so the S2 frame-0 caveat is refuted: the composite is a 62-78 px horizon band on **every** frame because it is gated by
the reflection pass's destination alpha, not by a black source. The remaining cause is therefore a **uniform ~1.6x
material-colour gain** whose source is inside the uncaptured PATH2 full-screen chain or the reference's display path,
not in the captured inputs; the palette/CLUT and the forced-LOD difference are the two named candidates still open.

## Measurements

### 0. Baseline reproduction and the comparison mask (C)

- Reproduced with the current tools on the morph-v6 capture (`night-morph.gt4scene`/`.gt4vram`/`.gt4tap`, the shipped
  v6 path) and the S1 `.gt4gs`: **PSNR 22.3521 dB, SSIM 0.650793, pct_bad 64.7747%, mean |diff| 14.8375, luma
  26.6882 -> 18.9466, ratio 1.4086, fit ref = 0.6354*world + 14.6487**; page `0x36D0`: PSNR 25.173, luma 16.611,
  fit 0.2857*world + 19.264. The pass-textures/S2 baseline (22.3498 / 0.65055 / 64.776% / 14.840 / 18.955, fit
  0.6341*w + 14.668) differs only by the morph v6 state (+0.00014 dB), so the study is on the same picture.
- Mask: the renderer's coverage (2,580,480 px, the whole 3x frame) minus the 2 px dilated car mask minus the six HUD
  rectangles = **1,780,790 px**. The HUD rectangles are 759,132 px (29.4% of the frame); the car mask is 38,439 px
  (1.49% of the coverage).
- Alignment is not a cause: the earlier round's best luma match over +/-20 px was (0, 0), and the frame identity gates
  (clock, digest, state, 13/13 frames) still pass on this branch.

### 1. (a) Fog blend shape: refuted as the mechanism (C)

The hypothesis is that the reference is our world blended toward `FOGCOL 0x27272A` (luma 39.34) at a per-pixel factor,
numerically ref = a*world + (1-a)*FOGCOL with a ~ 0.63. Measured on the clean mask with the world-materials 1x page/fog
map (v1 `page-fog-map.npz`, 99.9% of the mask has a tapped fog):

| test | result |
|---|---|
| tapped F | mean **247.8**, 38.4% < 255, per-page means 215.7-255; the fog contributes at most (255-196)/255*39 = **9.0 luma** on the strongest-fogged pixel and < 1 luma on the mean |
| per-pixel f = (ref-FOGCOL)/(world-FOGCOL) | solvable on 90.9% of the mask; means **f_R 0.572 / f_G 0.658 / f_B 0.733**, medians 0.652 / 0.682 / 0.839 |
| per-channel consistency | a GS fog blend has one F per pixel; the median per-channel spread is **0.275** (p90 0.679), so the model is inconsistent with a single blend factor |
| relation to the tap | corr(f, F) = +0.179; mean f where F=255 is **0.752**, where F<230 **0.383** - the reference is *closer* to our world where our fog is stronger, the opposite of "the reference applies more fog" |
| model RMSE (0-255 RGB) | identity 19.45, luma affine 0.6354*w+14.649 17.52, per-pixel FOGCOL blend 10.05, best constant f=0.645 17.26 |
| depth quintiles (tapped fog map) | ref/world ratio 1.337, 1.292, 1.473, 1.577, 1.394; mean F 254.7, 254.8, 254.8, 248.8, 226.0; f median 0.720, 0.790, 0.714, 0.682, 0.583. The gap rises with depth but the fog is not the shaper (the strongest-fog quintile has the *smallest* gap) |

The renderer applies the **tapped** per-vertex F (the VU1's own F(Q) formula is not re-derived offline); what is
checked here is therefore the applied F values, the FOGCOL (`0x27272A` from the tapped state/pass journal, 93.5% of
the mask) and the mix `(F*C + (255-F)*FOGCOL) >> 8`, which is the GS's and CoreGS 2.0's own. The per-pixel blend fits
better than the global affine (RMSE 10.05 vs 17.52) because it is a two-parameter per-pixel family; it is not the GS
fog: the GS applies one F per pixel and ours already does, and the solved factor disagrees per channel and runs the
wrong way against F. **Numerically the whole-mask fit happens to look like a blend toward
0x27272A; mechanistically it is a mixture.**

### 2. The whole-mask fit is a pooling artifact (C)

Fitting within the classes the mask mixes gives incompatible relations (luma OLS, ref = a*world + b):

| class | px (share) | a | b | ref/world ratio | corr |
|---|---|---|---|---|---|
| all clean | 1,780,790 (100%) | 0.6354 | 14.649 | 1.409 | +0.465 |
| PRIM.ABE on (blended) | 303,260 (17.0%) | 1.5703 | -0.254 | 1.558 | +0.625 |
| PRIM.ABE off (opaque) | 1,477,530 (83.0%) | 0.5316 | 15.673 | 1.375 | +0.452 |
| tapped F 245..254 | 229,173 (12.9%) | 1.5405 | -0.046 | 1.538 | +0.608 |
| tapped F 255 | 1,096,480 (61.6%) | 1.1730 | 3.736 | 1.381 | +0.596 |
| tapped F 200..229 | 236,903 (13.3%) | 0.3092 | 26.159 | 1.278 | +0.385 |

A single global transform cannot produce slopes 0.31, 1.17 and 1.57 simultaneously; the fogged and blended classes are
close to proportional (a ~ 1.5, b ~ 0), i.e. **a material gain**, while the pooled fit's slope 0.63 comes from mixing
classes with different gains and from the pages whose content differs (section 4). Per-page affine luma fits reduce the
RGB RMSE from 18.83 (identity) to **13.82** (PSNR 25.32 vs 22.63), i.e. **46% of the squared error is page-conditional**
mean/gain structure (56% against the CT16-corrected reference: 25.32 dB vs 21.77 dB identity).

### 3. The reference dump is a CT16 5-bit scanout: the comparison understates the reference by ~4 levels (C/H)

The reference frame is the CoreGS 2.0 HWR "composed scanout image" (run log: `GT4_RENDERER=hw`, `GT4_HW_SCALE=3`,
`GT4_HW_MIPMAP=full`, `GT4_HW_FILTER=ps2`, `GT4_HW_BLEND=high`, `GT4_HW_DITHER=native`, `GT4_HW_ANTIBLUR=1`,
`GT4_HW_SHADEBOOST=50,50,50,50` neutral, `GT4_HW_POSTFX=` empty). The game presents through a **CT16** framebuffer
(the present blit `fbp 0 psm 2`, `FBW 10`).

- **Every** reference channel value on the clean mask is divisible by 8 (**1.0000**; our world: 0.149), and the
  reference maximum is **248 = 31 << 3**. The dump is the 5-bit framebuffer expanded by `<<3`.
- The CT32 -> CT16 present truncates (plus the GS dither when DTHE is set; its register is not in the captured
  census), so the scanout is **3.5-4.5 levels below** the 8-bit GS framebuffer per channel. A 4x4 phase analysis of
  the ref-world residual shows the ordered-dither structure (cell means 5.5..10.6), so the present dithers; the exact
  bias is between +3.5 (truncation) and +4.5 (dithered truncation), and **+4.0 is used below**.
- Corrected (`ref + 4`): luma **30.688 -> 18.947, gap 11.742, ratio 1.6197**; per-channel mean ratios **R 1.640,
  G 1.614, B 1.588** - a *neutral* gain, not a colour cast. Whole-mask fit becomes 0.6354*world + 18.649; identity
  PSNR 21.77 dB, per-page affine 25.32 dB.
- Consequence for the gates: **the headline metrics are computed against a reference that is quantised dark**. The
  study reports both; future fidelity gates should state which reference (raw dump or bias-corrected) they use.

### 4. Coverage: the mask is the course pass; sky, underlay, car, HUD and the composite band are not it (C)

Pass-only renders (`--no-sky/--no-underlay/--no-course`, `--no-wet-reflection`) split the ownership by the draw order
and depth rule (course > underlay > sky):

| owner | clean-mask px | share | ref/world luma | ratio | gap |
|---|---|---|---|---|---|
| course | 1,783,523 | 99.996% | 26.69 -> 18.94 | 1.409 | 7.74 |
| sky | 70 | 0.004% | 28.96 -> 41.34 | 0.700 | -12.39 |
| underlay | 0 | 0% | occluded by the course | - | - |

The sky pass covers the whole viewport as a background, but the course overwrites all of it except **70 px** at the
horizon - the same band the S2 composite touches. Where a pass owns a pixel, the baseline image equals that pass's own
render exactly (max |d| 0.000 on the course). **The brightness residual is a course-pass property**; the sky, the far
underlay and any "missing backdrop" hypothesis are excluded.

- Car spill: the car is excluded with a 2 px dilation (38,439 px, 1.49% of the coverage). Just outside it (the 2-6 px
  ring, 6,012 px) the gap is 8.73 luma (ref 25.79 vs world 17.06), slightly above the mask's 8.00; inside the excluded
  car mask the two images differ by only 0.40 luma. The spill cannot move the mean (0.23% of the mask).
- HUD: 759,132 px (29.4% of the frame) are excluded by the six measured rectangles; the reference HUD is bright and
  our world has none, so the exclusion is necessary and stays.
- The S2 composite band (70-78 px, section 6) lies outside the compared mask.

### 5. The input path is verified exact: indices, vertex colours, blend arithmetic (C)

- **Texture page content.** CoreGS 2.0's own raw dump of page `0x36D0` at the race frame
  (`evidence-pass-textures/core2tex-race36D0/texture.ppm`, R = the PSMT4 index 0..15) has the **same index histogram**
  as our per-job decoded page (`bound_021_tex0_0006D1E5DD40B6D0`, bound at the road job): exact agreement **1.000**,
  histogram `[454,1475,2769,2361,1603,2194,1776,1696,639,535,377,213,146,73,45,28]` on both. The per-job snapshot is
  the right page content; the pass-textures fix is confirmed at the texel level.
- **Palette addressing.** The road's TEX0/TEX2 are `...DD40B6D0`: CBP **0x368F**, CPSM CT32, CSM1, CSA 0, TEX0.CLD 0
  with TEX2.CLD 1 (load). The per-job snapshot copies exactly the four blocks at 0x368F, the decoder checks
  TEX2.CBP == TEX0.CBP and refuses otherwise (0 refused), and the 16-entry group 0 is a grey asphalt ramp (luma
  18.7..67.2, alpha 128). The other 15 groups of the same 256-entry bank are much brighter (group means 34.6..159.8),
  but the registers select group 0.
- **Vertex colour.** The road items' captured V4-8 at TOPS 0x80 equals the tap's emitted RGBAQ byte for byte: draw 35
  item 4 **48/48** exact (86,78,74,128), item 7 **38/38**, draw 42 item 7 **64/64** (98.5,88.4,82.5,128).
- **Blend arithmetic.** `blend_fragment` uses `(((a-b)*factor) >> 7) + d`, i.e. alpha 0x80 = 1.0 (the GS convention),
  and `scaled_product` is `(T*C) >> 7`; the sampled page's MODULATE is the GS's. No arithmetic difference was found.
- What this excludes: the 1.5-1.9x per-page gain cannot come from wrong indices, wrong vertex colours or the blend/
  modulate arithmetic. Those are measured and right.

### 6. The S2 frame-0 caveat is refuted: the composite is a ~70 px band on every frame (C)

The caveat was that page 394 (the sphere-mapper source, job 460559) might be stale/black at race start, which would
explain why the composite changed only 70 px. Measured in the `.gt4vram` v2 sidecar:

- The sphere-mapper job record at every captured vblank (labels 460223/460391/460559/460731/460903/461075/461247/461419,
  vblanks 7397-7404) has page 394 **fully populated and identical**: mean byte 109.13, max 255, 16,261/16,384 non-zero
  RGB pixels, alpha mean 110.6. The source is not stale or black.
- The composite drawn on scene frames 0-4 (streams 7399-7403) changes **70, 77, 62, 78 and 71 pixels** and adds a luma
  sum of 2654, 2772, 2322, 3015 and 2830 over 2.57M covered pixels (**~0.001 luma mean**). The A/B (`--no-wet-composite`)
  differs only there. The composite covers the screen but is gated by the **destination alpha** the reflection pass
  writes (`ALPHA 0x58 = (Cs-0)*Ad+Cd`): the reflection pass writes 188 alpha fragments, 70 with non-zero alpha, on every
  frame. The small effect is the measured alpha mask, not a missing source.
- Therefore the wet composite cannot explain the frame-07400 gap, on frame 0 or on any later frame of the capture.
- Phase and reference limits: scene frame V uses stream V+1 (S2's measured relation), so frames 0-4 use streams
  7399-7403 (the `.gt4scene` holds frames 7398-7402; the `.gt4gs` holds streams 7398-7405). The reference frame set is
  sparse - `frame_06600.ppm` .. `frame_09000.ppm` every 200 VBlanks - so there is **no reference frame for 7399-7404**;
  on those frames only the composite's own contribution (comp vs no-comp) can be measured, not a PSNR.

### 7. The captured post multiplies still overshoot (C, S2 re-checked)

Applying the S2's captured chain (`FIX 127` then `FIX 255`, both `Cd*FIX/128`, full screen) to our world gives a mean
luma of 34.62 with PSNR 20.40 and the affine fit 0.3733*world + 13.763 (the S2 measurement; re-checked here: the chain's
two factors are 0.992 and 1.992, so both = 1.976x before clamping). The corrected reference is 30.69; the multiply
result is 34.62 (12.8% over) and its low slope (0.37) does not match the reference's 0.64. The post multiply remains a
candidate for part of the gain but is not the whole answer; a *single* multiply or a partial application is not
supported by the local ratio map (the local ref/world ratio is a broad unimodal 1.5-1.6 on the road, not a 1x/2x
spatial split).

### 8. What the numbers say remains: a per-material ~1.6x gain, and the named open candidates

Per-page rows over 10k px (ratio = mean ref luma / mean world luma; +4 = CT16-corrected reference):

| page | px | world | ref | ratio | ref+4 | ratio+4 | corr |
|---|---|---|---|---|---|---|---|
| 0x36D0 (road) | 955,413 | 16.61 | 24.01 | 1.445 | 28.01 | **1.686** | +0.111 |
| 0x2FC0 (verge) | 121,940 | 14.66 | 21.32 | 1.454 | 25.32 | 1.727 | +0.468 |
| 0x3EA0 (billboards) | 96,705 | 22.94 | 34.92 | 1.522 | 38.92 | 1.696 | +0.529 |
| 0x37D0 (left wall) | 87,138 | 23.96 | 38.82 | 1.620 | 42.82 | 1.787 | +0.161 |
| 0x3EE1 (paint/kerb) | 75,283 | 28.05 | 47.06 | 1.678 | 51.06 | 1.820 | +0.543 |
| 0x3D36 (right barrier) | 55,350 | 13.69 | 18.58 | 1.357 | 22.58 | 1.649 | +0.908 |
| 0x3E26 | 38,952 | 33.76 | 57.78 | 1.711 | 61.78 | 1.830 | +0.740 |
| 0x3CAE | 38,763 | 7.73 | 7.61 | 0.985 | 11.61 | 1.502 | +0.860 |
| 0x3DCE | 36,882 | 18.45 | 31.60 | 1.713 | 35.60 | 1.930 | +0.630 |
| 0x3D6A | 34,299 | 10.89 | 14.94 | 1.372 | 18.94 | 1.740 | +0.162 |
| 0x3721 | 33,660 | 15.86 | 21.75 | 1.371 | 25.75 | 1.623 | +0.538 |
| 0x3690 | 32,517 | 11.64 | 14.52 | 1.247 | 18.52 | 1.591 | +0.661 |
| 0x3CBE | 28,620 | 16.06 | 20.11 | 1.252 | 24.11 | 1.501 | +0.391 |
| 0x3E47 | 22,563 | 18.00 | 19.39 | 1.077 | 23.39 | 1.299 | +0.318 |
| 0x37B5 | 21,420 | 20.40 | 26.22 | 1.285 | 30.22 | 1.481 | +0.918 |
| 0x3BF6 | 17,406 | 34.68 | 28.69 | 0.827 | 32.69 | **0.942** | -0.113 |
| 0x38FF | 10,683 | 18.92 | 34.61 | 1.830 | 38.61 | 2.041 | +0.703 |

With the CT16 correction the per-page ratio is **1.69 median, sigma 0.25**; almost every course material is
1.5-1.9x too dark in our render, at a neutral per-channel gain. Page 0x3BF6 is the one page whose mean ratio falls
below 1 (0.94 corrected) and whose content is uncorrelated (corr -0.113): its 64x30 px block at 1x is a mixture (a
small bright reference area over a dark world elsewhere), so it is a content item, not a level one. Pages with a high
correlation but a gain (0x3D36 0.908 / 1.65, 0x37B5 0.918 / 1.48) prove the *pattern* is right and the *level* is not.

The two candidates left, with their measurements:

- **Palette/CLUT data (Hypothesis).** With the indices and vertex colours proven, the palette is the only per-material
  colour input left. A 3x per-texel inversion was attempted: rasterising the road items on the renderer's 1920x1344
  grid, sampling the decoded page through the same 10.4 bilinear weights. It does **not** register against our own
  render (corr 0.30 on the road page), so the Python UV/strip reconstruction is not pixel-exact and the per-texel
  palette could not be recovered; what is robust is the **mean**: the reference's implied texel (ref / (V4-8/128), fog
  undone) is **1.43x** the decoded page's mean (39.08 vs 26.83) with the same indices. The palette data is present at
  the right address; whether the reference's CLUT contents differ is **not proven**.
- **Texture LOD (Hypothesis).** 20 road items bind `TEX1 0x00000F5B00100124`: LCM 0, **MXL 1**, MMAG linear,
  **MMIN 4 (linear mip nearest)**. The offline sampler refuses/falls back to **level 0** for mip bindings and counts
  "level-0 mip fallbacks 2"; CoreGS 2.0's `GT4_HW_MIPMAP=full` implements the full LOD path for these draws. A box mip
  does not change the mean, so this cannot explain the 1.6x on its own, but it changes the pattern (and the reference's
  road does look smoother in the distance) and must be aligned before the palette can be compared.

### 9. Uncaptured PATH2 full-screen ops (context, H)

**Update (branch `task/090-post-chain`): the chain is captured, decoded and applied; the list below is the study's
original reading. The follow-up round measured the frame sample as the frame's depth clear (FBMSK 0xFFFFFFFF masks
every colour lane; ZBUF.ZMSK 0 with z=1 writes the depth buffer), and applied the two multiplies and the CT16 present
over the world image: the level is explained (ratio 0.894 raw / 1.028 corrected) and the residual is structural
(fit 0.3727*present + 15.566, per-page slopes 0.60-0.90, the road correlation 0.099). See
[COREGS3-HOOKS](COREGS3-HOOKS.md) §29.**

The S2 census lists **11 full-screen batches per frame**; the S2 applied only two of them (the wet composite and the
two multiplies, the latter diagnostically). The other full-screen ops in the stream (vblank 7399 = scene 7398) are:

- seq 672/675: full-screen black fill, RGB (FBMSK 0) and alpha-only (FBMSK 0x00FFFFFF) - the frame/alpha clear;
- seq 678/681: full-screen fills with TEST `0x32001`;
- seq 684: a full-screen **frame sample** (PRIM 0x15E, TME+ABE, TBP0 0x1A40 = page 210 itself, TEST `0x3800F`) whose
  vertex colour is **(128, 77, 0, 128)**: the sampled framebuffer is modulated by (1.0, 0.60, 0.0) before the normal
  blend. It runs at the start of the stream, before the world draws of the next interval;
- seq 920/934/936/964: the alpha clear, wet composite and the two multiplies (S2);
- seq 1094: the present blit to the CT16 framebuffer (vertex colour (128,128,128,128), neutral);
- seq 1207: alpha-only clear on the presented page.

These are the only remaining *captured* operations that can change the world's colour globally, and the frame sample's
(128,77,0) modulation is the only one with a colour/brightness transform in its vertex data. Their effect depends on the
PATH1/PATH2 interleaving, which the two sidecars do not carry; this is the top open measurement.

## Decomposition table

| # | cause | measured effect | confidence |
|---|---|---|---|
| 1 | Fog blend toward FOGCOL at a per-pixel f | refuted as the mechanism: tapped F 247.8 mean (38.4% <255), per-channel f 0.57/0.66/0.73 with median spread 0.275, f anti-correlates with F (0.75 at F=255 vs 0.38 at F<230), >1 slopes within high-correlation pages; the whole-mask fit is a mixture of classes with slopes 0.31..1.57 | **C** (model refuted); the fit's numerical coincidence with a 0.63 blend is H |
| 2 | Reference CT16 5-bit scanout | every reference value divisible by 8 (1.0000), max 248; dump ~4 levels dark; corrected reference luma 30.69, ratio **1.62** (R 1.64 / G 1.61 / B 1.59); the present's DTHE = 1 / DIMX = 0x0001044501100554 / COLCLAMP = 1 are captured and modelled (`task/090-post-chain`) | **C** for the quantisation and the direction; H for the exact +3.5..4.5 bias; C for the captured dither |
| 3 | Global material gain (~1.6x) | per-page corrected ratios 1.69 median, sigma 0.25; per-page affine RMSE 13.82 vs identity 18.83/20.80; the fogged and blended classes fit ref ~ 1.5*world; the input path is exact (indices 1.000, vertex colours 48/48, blend /128) | **C** that the residual is a per-material level difference in the material path; the *stage* is **H** |
| 4 | Per-page content/colour (low correlation pages) | 0x36D0 corr 0.111, 0x37D0 0.161, 0x3D6A 0.162, 0x3BF6 -0.113 (page mean 0.94, content uncorrelated); high-correlation pages 0x3D36 0.908, 0x37B5 0.918, 0x3CAE 0.860 prove the pattern can match | C for the correlation spread; H for the cause (palette/CLUT vs LOD vs sampling) |
| 5 | Texture index content (per-job binding) | identical to CoreGS 2.0's own dump, agreement 1.000 | **C** (not a cause) |
| 6 | Vertex colour / MODULATE | road items exact vs the tap 48/48, 38/38, 64/64; MODULATE is (T*C)>>7 | **C** (not a cause) |
| 7 | Blend arithmetic | `((a-b)*factor)>>7 + d`, alpha 0x80 = 1.0; the ABE class still shows a 1.56x gain | **C** for the arithmetic; the gain inside ABE is part of cause 3 |
| 8 | Coverage: sky, underlay, car spill, HUD | course owns 99.996% of the mask (sky 70 px, underlay 0); car ring 2-6 px is 6,012 px with an 8.73 gap; HUD 29.4% of the frame is excluded | **C** (not a cause) |
| 9 | S2 wet composite and frame-0 caveat | sphere source fully populated and identical at vblanks 7397-7404 (mean 109.13, 16,261/16,384 non-zero); composite changes 70/77/62/78/71 px on frames 0-4 (~0.001 luma), gated by destination alpha | **C** (not a cause) |
| 10 | Captured post multiplies | both apply 1.976x; the diagnostic gives luma 34.62 (12.8% over the corrected reference) and fit 0.3733*w+13.763, worse than the baseline | **C** (not the whole cause); a partial/single application is H |
| 11 | Uncapped PATH2 full-screen chain | 11 full-screen batches/frame, all classified and applied (`task/090-post-chain`): 4 pre-world clears, the frame sample = the depth clear (FBMSK 0xFFFFFFFF, ZBUF.ZMSK 0, no colour), the reflection alpha clear, the composite, FIX 127, FIX 255, the CT16 present (DTHE 1, DIMX 0x0001044501100554), the display alpha clear; the level ratio 1.409/1.620 becomes 0.894 raw / 1.028 corrected; the residual is structural (fit 0.3727w + 15.566, road corr 0.099) | **C** |
| 12 | Palette/CLUT contents and LOD | palette at CBP 0x368F group 0 (CSA 0, CLD 0/TEX2 load), data present; reference implied texel mean 1.43x ours; 20 road items bind MXL 1/MMIN 4 and are sampled level 0 | **H** (named, not proven) |

## Fix list (ordered, with gates)

1. **Correct the comparison for the reference's CT16 scanout before anything else.** Apply the documented +3.5..+4.5
   bias (or, better, read the GS DIMX/DTHE state and model the dither exactly) in the metrics script and restate the
   gates against the corrected reference. Gate: the metrics JSON records both raw and corrected numbers; the
   quantisation is proven (all values multiples of 8) and the bias range is stated. This is a *measurement* fix, not a
   renderer change, and it changes every future fidelity number (identity PSNR 22.63 -> 21.77 raw->corrected; per-page
   affine 25.32).
   **DONE (`task/090-post-chain`, C/H):** the present's captured DTHE = 1 and DIMX = 0x0001044501100554 are decoded and
   the offline present models the CT16 conversion with the captured dither; the metrics script reports `raw dump` and
   `dump + 4 (CT16)` in the text and the JSON; the raw baseline is unchanged. The final stage is compared raw (both
   sides CT16).
2. **Model the whole PATH2 full-screen chain, not only the composite and the multiplies.** Apply the fills, the
   alpha clears, the frame sample with its (128,77,0,128) modulation and the present in the measured stream order, and
   measure each op's coverage and luma. Gate: every full-screen batch is applied or excluded with a reason; the A/B
   shows which op moves the mean; no op is left in "recognised but not applied".
   **DONE (`task/090-post-chain`, C):** 11/11 full-screen batches classified and accounted for, 0 unrecognised; the
   frame sample is the depth clear (writes no colour); the composite + FIX 127 + FIX 255 + the CT16 present are applied
   in stream order. The level goes from ratio 1.409 raw (1.620 corrected) to 0.894 raw (1.028 corrected); per-page
   median 0.943 raw. The residual is structural (fit 0.3727*present + 15.566, correlated-page slopes 0.60-0.90, road
   corr 0.099, the raw reference's darkest decile 4.09 vs our 16.56), so fixes 3 and 4 are the remaining work.
3. **Resolve the ~1.6x material gain at the fragment level.** Register the UV/strip reconstruction with the renderer
   (fix the Python triangle/UV reconstruction or add a per-pixel sample dump to the tool), then recover the reference's
   palette per texel index. Gate: the road's effective texel ratio is 1.0 +/- 0.1, the per-page corrected ratio spread
   is <= +/-0.1, and the palette used by the renderer is shown equal to the reference's or the difference is named.
4. **Implement or measure the mip path.** The 20 road items with MXL 1/MMIN 4 are sampled level 0. Gate: the same
   items' high-pass correlation with the reference rises and the page ratio does not move outside +/-0.05.
5. **Keep the verified input path as the control.** The page index identity (1.000), the vertex-colour identity and
   the blend arithmetic are already covered by the pass-textures/morph fixtures; add the road page's index-identity
   check as a regression.
6. **Do not add a global post transform or a fog change yet.** The post multiplies overshoot and the fog is refuted;
   adding either would hide the material gain. Only after 2 and 3 may a post op be re-considered, gated on the matched
   pages staying within 5%.

## Evidence (this worktree, gitignored under `build/`)

- `build/evidence-v2/world-base.png`, `world-base-mask.pgm`, `world-base-metrics.json`, `world-base.txt`,
  `world-base.log` - the reproduced baseline (morph v6 capture).
- `build/evidence-v2/pass-{sky,underlay,course}.{png,pgm,txt}` - the per-pass split and ownership.
- `build/evidence-v2/page-state-map.npz`, `per-page-corrected.txt`, `state-split.txt`, `model-fits.txt`,
  `corrected-models.txt`, `tone-curve.txt` - page/fog/depth/PRIM map and the model fits.
- `build/evidence-v2/tex-dump/` (430 bound textures), `palette-invert.txt`, `palette-lsq.txt`, `palette-3x.txt`,
  `mip-test.txt`, `road-3x-map.npz` - the texture/palette/LOD measurements.
- `build/evidence-v2/abe-layers.txt` - the 109 blended items and their coverage.
- `build/evidence-v2/s2-frame{1..4}-{comp,nocomp}.{png,txt,log}`, `s2-frame*-changed.pgm` - the composite on frames
  1-4.
- `build/evidence-v2/view-*.png`, `crop-*.png` - the visual sheets (side by side, signed diff, local ratio, pages).
- Scripts (developed here, rerunnable): `build/analysis/{make_views,make_crops,pass_split,model_fits,tone_curve,
  map_pages2,state_split,palette_invert,palette_lsq,palette_3x,mip_test,corrected_models,abe_layers,page_view}.py`
  plus the world-materials v1 `gt4scene.py` reader copied in.

## Reproduction

```
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
set T134_ARGS=-DGT4_EE_PROFILE=flare -DGT4_EE_LIBRARY=O:/Antigavity/gt4-wt-090-coregs3/build/090-capture-v5/ee-v5b/ps2game.lib ^
  -DGT4_EE_LIBRARY_SHA256=7f3c833fc8b6f8e8657e31fc635541f8158a5e7ff6c2b8ec454a669568685cac -DGT4_COREGS3=ON
call scripts\coregs2_bench\game\configure.cmd materials-v2 v200
cmake --build build\materials-v2 --target gt4_coregs3_render gt4_coregs3_gs gt4_coregs3_tap

build\materials-v2\gt4_coregs3_render.exe <night-morph.gt4scene> --frame 0 --out world-base.png \
  --width 1920 --height 1344 --vram <night-morph.gt4vram> --tap <night-morph.gt4tap> \
  --gs <capture.gt4gs> --world --report world-base.txt --mask world-base-mask.pgm
python build/analysis/pass_split.py          # per-pass ownership
python build/analysis/model_fits.py          # the fog/affine/per-page fits
python build/analysis/corrected_models.py    # the CT16 correction and the model comparison
python build/analysis/state_split.py         # ABE/fog-colour/fog-band classes
python build/analysis/abe_layers.py          # the 109 blended items
python build/analysis/palette_lsq.py         # the palette inversion attempt
python build/analysis/mip_test.py            # the mip-level test
```

## Limits and unknowns

- The CT16 bias is not read from the register: the present's DTHE/DIMX state is not in the `.gt4gs` census, so the
  correction is a **3.5-4.5 range** (+4 used). Everything in section 3 is directional and per-channel; the exact
  framebuffer value is Unknown.
- The 1x page/fog map is a frontmost-depth rasterisation of the course items only; layers that write no depth (the
  paint/kerb page 0x3EE1, the ABE decals) are not in the page assignment, so the per-page classes mix materials. The
  pass split and the whole-mask numbers do not depend on it.
- The per-texel palette inversion does not register (our own render correlates only 0.30 with the reconstruction), so
  the palette contents could not be proven equal or different; only the mean ratio (1.43) is measured.
- The PATH1/PATH2 interleaving is not in the capture, so the frame sample's and the multiplies' effect on the
  displayed world is Unknown until the merged stream order is measured.
- The reference is CoreGS 2.0's HWR backend with the options in section 3; the study treats it as the fidelity target,
  as TASK-090 defines.
