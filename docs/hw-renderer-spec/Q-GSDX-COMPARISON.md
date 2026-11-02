# Q — Reference renderer comparison (TASK-026 specification team, for TASK-027)

Behavioural comparison of the reference emulator's GS renderer (PCSX2/GSdx, read-only) with CoreGS 2.0 as it stands
on `task/027-coregs2` at IN-384, written for the implementer of PARITY-PLAN sections 3 and 10.

**Status of this document and clean-room rules.**

- Written by the TASK-026 specification team, which is allowed to read the reference sources. The implementation
  agent is not, and must not open them, the `gt4recomp-gs` fork or any `private/tooling/pcsx2-*` tree (decision 0070).
- This file contains **no source code, no pseudocode of their functions and no identifiers from their sources**. It
  describes behaviour, rules, formulas, invariants and edge cases in prose. Every number is a real constant from the
  reference or from our own documents.
- Read it as evidence about *what the reference does*, never as code to port. Where the reference's behaviour comes
  from an option or a per-game database entry, the entry is described, not named.
- Method: read-only study of the pinned reference tree, its shaders, its configuration defaults and its GT4
  per-game entry. Our side was read from `docs/hw-renderer-spec` (06, 13, PARITY-PLAN, IN-371..IN-384) and from
  `native/hw_gs/writeback/hw_gs_writeback_gpu.cpp`, `native/hw_gs/writeback/hw_gs_writeback.cpp` and the shaders named
  in the brief. Nothing was built, run or measured for this document; no reference log or dump was opened.

**Labels.** *Confirmed* = seen in the reference source, its shaders or its configuration, or in our own source.
*High confidence* = follows from two or more confirmed facts with no plausible alternative. *Hypothesis* = a
plausible mechanism; the falsification test is given. *Unknown* = not determined.

---

## 0. The structural difference that explains most of the rest

**Confirmed: the reference never keeps an exact 1x image.** At an internal resolution above 1x it rasterises every
target once, at the scale, into a real colour attachment (8 bits per channel) and a real depth attachment. There is
no second, exact copy of the frame in GS words; the scaled image *is* the frame. A "1x image" only exists when the
internal resolution is 1x, which is a different render of the same scene, not a downsample of the scaled one.

Consequences that matter for every topic below:

| | Reference | CoreGS 2.0 today |
|---|---|---|
| Authority | the scaled attachment | the 1x GS words (`frames`/`depths`), the scaled image is derived |
| Draws per frame | one | two (1x exact + scaled), the scaled one skippable (IN-382) |
| Intermediates (tone, channel shuffle, brightness) | done at scale, in place, on the scaled target | done at 1x for memory exactness, then the scaled image is rebuilt (IN-379 carry) |
| Textures that come from a target | sampled from the scaled target, at scale | sampled at scale only when the source is a resident slot (IN-374); otherwise decoded from the GLM at 1x |
| Feedback fallbacks | none: there is nothing to fall back to | per sub-pixel fallback to the 1x word (IN-377 shadow check, IN-379 carry check) |

The owner's artifacts split exactly along this line: everything that looks like "1x content inside a 6x frame"
(lattice, dashes, checkerboard, ghost) comes from the right-hand column, and the reference cannot produce it because
it has no 1x authority to fall back to. That is not an argument for dropping the 1x authority (it is a project
requirement, and memory exactness is the reason TASK-027 exists); it is an argument for making the *scaled* result
independent of per-pixel validity tests, which is what the reference's architecture gives for free.

---

## 1. Sub-pixel alignment when upscaling

### What the reference does

**Confirmed.** At scale s > 1 the reference has exactly two families of vertex mapping, chosen per game (and, inside
the first family, per target):

1. **Scaled family** (the default for most games): the vertex is placed at `s·P + 0.5` output pixels, where P is the
   GS position in native pixels. The 0.5 means "the centre of the first sub-pixel of native pixel P". A per-game
   multiplier may enlarge that half-pixel term to `0.5·m` with `m = s + 0.2` (s even), `s + 0.3` (s ≡ 1 mod 4, e.g.
   1, 5) or `s + 0.1` (otherwise, e.g. 3, 7).
2. **Native family**: the geometry is expressed in native units and then magnified, which places the vertex at
   `s·P + s/2` — the **centre of block P**. Every edge lands on a native pixel boundary, so a primitive that starts
   exactly on native edge 0 leaves the first `floor(s/2)` sub-pixel rows and columns undrawn.

**Confirmed: GT4 uses the native family.** The GT4 entry of the reference's per-game database selects its
native-alignment half-pixel mode (one of five modes besides "off"), with the comment that it fixes edge shadows and
depth bleed at the edges. That is the same entry our spec 06.3 quotes as the provenance of our `native` default.

**Confirmed, and identical to our spec:** the two compensations that the native family needs are the ones we already
implement.

- Box downsample of a scaled target to 1x: the average of the s x s block, with the read origin clamped to `max(out·s,
  s)` — i.e. the first s sub-pixels are skipped because they were never drawn. This is exactly 06.7.
- Region-clamp margins when sampling a scaled source in this mode: `(1, 1, 0.1, 0.1) + 0.1·s` texels — exactly the
  `a = 1 + 0.1·s`, `b = 0.1` of 06.3.

**Confirmed: sampling uses half a *scaled* texel.** The bilinear offset the reference subtracts from the texture
coordinate is `-0.5` divided by the sampled texture's *scaled* size, i.e. half a sub-texel, the same convention as our
`(U·s) − 8` in `hw_pixel_uber.frag` (`sample_texture_scaled`). There is **no** texture-coordinate realignment for GT4:
the realignment code paths belong to the other half-pixel modes, and the native mode does not use them.

**Confirmed: the reference has no box-downsample identity to satisfy.** It never renders a 1x image at scale, so
"does a box downsample reproduce 1x" is not a property it maintains. When it *does* need a 1x copy of a scaled target
(palette sources, 1x destinations, CPU-side reads) it produces one with the box filter above, which is the defined
approximation, not an equality.

### What CoreGS 2.0 does today

`hw_vertex.vert` maps `gpu = scale·P + 0.5·scale_offset − 1/320`, with `scale_offset = 1` at 1x and `= s` in native
mode; `record_scaled_draws` moves the native scissor to `native·s + floor(s/2)`; `hw_display_compose_scaled.comp`
clamps its read to `undrawn = s/2`; `hw_scale_upsample.comp` refreshes nearest; `sample_texture_scaled` uses half a
scaled texel. So for GT4 we are on the same mapping as the reference, formula for formula.

### The difference

**Confirmed: there is none in the geometry.** Our `native` (default) equals the reference's native family; our `off`
equals its scaled family; our `normal` equals its scaled family with the multiplier. The remaining differences in
this area are not geometric:

- the reference filters the final image at present time (section 6), ours presents the internal image (IN-375 box
  steps plus one linear blit);
- the reference reads a scaled target for the whole display rectangle, ours selects a source per sub-pixel
  (section 3 and 7);
- the reference has no 1x layer to sample (section 0).

### Which artifact it could explain

**None of them, by itself.** This is the useful part of the answer: the half-pixel convention is not a suspect, which
agrees with the third measurement in PARITY-PLAN section 10 (shift = s gives 27.4-28.0 dB box PSNR against our own
1x, shift = 1 gives 21.7-22.0 dB). The 6-pixel lattice is not a vertex-alignment artifact. If someone re-opens this
question, the falsifying observation would be a *whole-image* half-native-pixel shift between our 6x dump and the
reference's 6x screenshot of the same frame — which the native family in both renderers rules out.

---

## 2. Dithering at scale

### What the reference does

**Confirmed.** Dither is a per-fragment add of the game's 4x4 matrix value, in the pixel shader, with three
preconditions: the game's dither enable bit is on, the destination is a 16-bit target, and the matrix is not all
zero.

**Confirmed: the index has two modes and the default is per *output* pixel.**

- one mode divides the fragment coordinate by the render target's scale before indexing, so each matrix cell covers
  `s x s` output pixels (the "per native pixel" pattern, the one our spec calls `native`);
- the other indexes with the raw fragment coordinate, so the pattern is 4x4 *output* pixels whatever the scale (the
  one our spec calls `fine`);
- a third setting forces 32-bit output and disables the dither;
- the shipped default is the **raw fragment coordinate**, i.e. the fine pattern.

**Confirmed: the dither is added after the shader-side blend, with an alpha compensation.** When the blend is done by
the hardware blend unit (which runs after the fragment shader), the dither value is divided by the source alpha so
that, after multiplication by that alpha, the visible dither amount is the one the GS would have added after blending.
So the reference approximates the GS's post-blend dither instead of computing it exactly.

**Confirmed: upscaling does not disable the dither.** The reference keeps it at every scale and only changes the space
the matrix is indexed in (and, in the "force 32-bit" setting, the output precision). There is no scale-dependent
switch that turns it off, and no per-target exception.

### What CoreGS 2.0 does today

`hw_pixel_core.glsl` (`hw_process_pixel`, `hw_dither_entry`) adds the matrix value **after** the exact integer blend,
which is the GS's own order, and indexes it with `floor(gl_FragCoord)/s` — the native pixel — unconditionally
(`hw_pixel_uber.frag`, `f.x = pixel.x / pixel_scale`). `GT4_HW_DITHER=native|fine|off` is parsed in
`hw_gs_options.cpp` but **never reaches the GPU**: a grep of the drawer and the shader shows the only dither input is
the game's enable bit. So `fine` and `off` are documented but not implemented.

### The difference

| | Reference (default) | CoreGS 2.0 |
|---|---|---|
| Cell size at 6x | 4x4 **output** pixels | 4x4 **native** pixels = 24x24 output pixels |
| Appearance on a flat gradient | fine noise | one flat 6x6 tile per native pixel, with a 4-native-pixel modulation |
| Order | after the shader blend, alpha-compensated before a hardware blend | after the exact integer blend (GS order) |
| 32-bit targets | dither off (16-bit only) | dither off (`format == CT16` only) |
| On a 480i field | 4x8 output pixels after the 2x field stretch | 6x**12** output pixels after the 2x field stretch |

### Which artifact it could explain

**High confidence: the 6x12 cell grid on flat backgrounds (owner test 11, the GT Mode map, the "Info" dialog).**
The measured period is one native pixel horizontally and *two* native rows vertically. Two native rows is exactly what
a 480i field stretch does to one native row (224 rows shown as 448), and one native pixel is exactly one dither cell
in our mode. So "6 wide x 12 tall cells" = one 6x6 native dither cell, doubled vertically by the field. The reference,
on the same scene, re-dithers per output pixel and shows a 4x4 (then 4x8) pattern instead — no tile grid.

Falsification: force the shader to index the matrix with the raw fragment coordinate (the reference's default) and
re-dump the same menu frame at 6x. The 6x12 lattice must disappear and the flat area must turn into fine noise. If the
lattice survives, the cause is a magnified 1x *texture* (then look at the source format and filter of that draw, not
at dither), and the second test is to dump the draw's target format: if it is CT32, dither is off in both renderers
and the lattice is not dither at all.

**Also relevant:** the owner compares window screenshots, and the reference's present filter (section 6) further
smooths a 4x8 dither. Our dumps are pre-present. A like-for-like comparison needs both images before the present
filter, or the same filter on both.

---

## 3. Interlaced frames at scale

### What the reference does

**Confirmed.** The order is: merge the display circuits into a full-size image at the scale, then deinterlace that
merged image, then post-filters, then present.

- **Field offset**: when the frame is really interlaced, the game uses half-height fields (`FFMD = 1`), and the merge
  is not a bob, the destination rectangle of each circuit is shifted down by `scale.y · (field XOR parity)` — one
  *native* row, i.e. s output rows. A setting can disable it; it is enabled by default. This is the same rule and the
  same magnitude as our 13.3 step 3 / IN-381.
- **Field stretching**: the read rectangle of a half-height field is half the display height, and the merge stretches
  it to the full display height. So each source row becomes **two** native rows in the merged image (line doubling),
  before any deinterlace. The stretch filter is nearest, because the option that would make it bilinear is off by
  default.
- **Deinterlace on the upscaled image**: every mode works on the merged image, whose size is the display size times
  the scale, and a "row" in the algorithm is **one output row**, not one native row:
  - weave: writes only the output rows whose parity matches the field, keeps the other output rows from the previous
    frame (so at 6x it keeps 1344 of 2688 rows, and the comb period is 2 output rows, i.e. it interleaves the two
    fields *inside* each doubled source row);
  - bob: shifts the whole merged image down by `scale·(1 − field)` output rows (bilinear);
  - blend: weave, then a vertical 1/4, 1/2, 1/4 filter;
  - motion-adaptive (its default for GT4, because `FFMD = 1`): a two-bank history buffer at twice the merged height,
    three-line comparison above/at/below with a per-channel threshold of 0.08 (normalised), weave when static, average
    of the line above and below when moving; field rows and the first/last row pass through.
- **Presentation**: the interlaced image is not filtered by FXAA unless the user asks; the present filter of
  section 6 applies to it as to any other image.

**Confirmed: there is no 1x deinterlace at scale.** The history images, the weave buffer and the adaptive buffer are
all allocated at the merged (upscaled) size.

### What CoreGS 2.0 does today

`hw_display_compose_scaled.comp` reads the field's read rectangle (224 rows) and stretches it over the placement (448
rows) with `ny·srch/h` — a nearest line doubling at *sub-pixel* resolution, i.e. each source row becomes 12 identical
output rows at 6x, and the sub-pixel detail inside the row is kept. `hw_deinterlace_scaled.comp` then applies every
13.4 rule **per native row** (`native_row = gid.y / scale`): the field's rows are the native rows of the field's
parity, the line above/below are s output rows away, and the edges are the first and last native rows. The history is
four fields, tracked in `Renderer::Impl::DeinterlaceGpu`.

### The difference

| | Reference | CoreGS 2.0 |
|---|---|---|
| Where the rules run | on the merged upscaled image | on the merged upscaled image |
| What a "row" is | one **output** row | one **native** row (s output rows) |
| Comb period at 6x | 2 output rows | 2 native rows = 12 output rows |
| Field offset | one native row (s output rows) | one native row (IN-381) — same |
| Line doubling | nearest, before the deinterlace | nearest at sub-pixel resolution, before the deinterlace — same |
| A one-line difference between the two fields | a half-block comb (a slight blur) | a **full native row ghost** (6 output rows at 6x) |
| History | 2 banks x 2 fields, one buffer at twice the height | 4 fields + woven image + last output, separate images |

### Which artifacts it could explain

**High confidence: the ghosted second copy of a dialog's text (480i menus).** If the two fields' text differs by one
line — which is what a half-line XYOFFSET difference between fields produces when the offset and the weave parity do
not cancel exactly — then our per-native-row weave shows the two copies one full native row apart, which at 6x is a
6-pixel ghost of high-contrast text, exactly "a second copy". The reference, weaving at output-row granularity,
splits the same mismatch inside each doubled source row, so it degrades to a half-block comb that reads as a slight
blur instead of a second copy.

Falsification, in order: (a) on the same menu frame, force `GT4_HW_DEINTERLACE=bob`; if the ghost becomes a vertical
smear of the whole text block, the two fields do carry different text and the weave is what duplicates it; (b) dump
the two consecutive merge images of that frame and compare the text rows; (c) if the two fields are identical, the
ghost is not the deinterlacer and the suspect moves to the compose's per-sub-pixel source selection (section 7).

**High confidence: the "jitter" on the 3D sprite (480i).** A one-native-row alternation between fields is a 6-pixel
vertical jump at 6x in our weave; in the reference the same alternation is 2 output rows, i.e. sub-native, and reads as
a shimmer. Same falsification as above.

**Hypothesis: the thin vertical line.** A line one native pixel wide is the width of one *native* column, which is the
width of our per-sub-pixel fallbacks (a column where the shadow check or the carry check fails shows the 1x word, and
the 1x word is one native pixel). The reference cannot produce this because it has no fallback. The falsification is
in section 7.

---

## 4. The fast-path classification (feeds Part B)

### What the reference does

**Confirmed: the reference has exactly one uber pixel shader** with a large permutation space, and one vertex shader.
Texture format (indexed 4/8-bit, 16-bit with TEXA expansion, 24-bit, depth-as-texture, palette), wrap mode (including
region repeat and region clamp), texture function, fog, alpha test method, colour write mask and dither mode are all
*shader permutations*, not different paths. They cost shader variants, never an extra pass.

**Confirmed: what makes the reference leave its single-pass hardware raster is *feedback* — reading the destination
or the depth buffer — and precision it cannot express in the fixed-function pipeline.** Concretely:

| GS state | Reference behaviour | Cost class |
|---|---|---|
| Blend the API can express: `(Cs − Cd)·As + Cd`, `(Cs − 0)·As + Cd`, `(Cs − 0)·FIX + Cd`, opaque, additive | hardware blend unit, factors from the API; `Ad` is replaced by the constant `Af` when the target's alpha range is constant, and by 1.0 for a 24-bit target | fast |
| Accumulation (`Cs·As + Cd`, `Cs·Af + Cd`) | shader multiplies the source by the alpha, hardware blend `ONE/ONE` | fast |
| Blend that needs the destination *colour* per pixel (`Cs·Ad`, `(Cs − Cd)·Ad` with a varying `Ad`, subtract with a varying factor) | software blend: the shader reads the target (texture barrier, framebuffer fetch or ROV); without barriers, a two-pass blend when the primitives do not overlap | slow |
| Alpha test, failure action KEEP | `discard` in the shader | fast |
| Alpha test, failure action FB_ONLY / ZB_ONLY / RGB_ONLY | feedback of the target and/or the depth buffer, or two passes; the reference first simplifies the action away when it cannot matter (no colour write, no depth write, no alpha write) | slow |
| DATE | an 8-bit stencil in the depth attachment, or an alpha test plus a barrier; a primitive-id variant needs a full barrier | slow-ish |
| PABE | the shader decides the blend per pixel from the source alpha, reading the target | slow |
| COLCLAMP = 0 (colour wrap) | a **separate 16-bit-per-channel UNORM target** at the same size, plus convert/resolve passes around the draws that need it (9 bits of headroom for the wrap) | very slow |
| FBMSK all ones or all zeros per byte channel | the API's colour write mask | fast |
| FBMSK partial inside a byte | shader read-modify-write of the target with a barrier | slow |
| Texture format, AEM, palette, depth-as-texture | shader permutation | fast |
| Wrap modes incl. REGION_REPEAT / REGION_CLAMP | shader arithmetic on integer texel coordinates | fast |
| Z16 / Z24 / Z32 | one 32-bit float depth attachment for all three; the shader clamps the incoming Z to the format's maximum, and floors it when the target's precision requires it | fast |
| Depth target whose page offset differs from the colour target's | a temporary depth buffer at the colour target's size and scale, with copies around the draw | slow |
| Texture = the target being drawn (feedback) | sample the same texture when the coordinates match, else a copy of the region | depends |
| GT4's RGB channel shuffle + ramp palette (the tone/brightness sequence) | a per-game database entry **replaces it with one HLE draw at the source's scale**, reading the channel and the palette per output pixel | fast (and it is the same idea as our IN-379/IN-383) |

**Confirmed: the reference also has a CPU rasteriser for tiny sprites**, chosen for speed, not for exactness. It is
not a fallback for unsupported state.

### What CoreGS 2.0 does today

`record_scaled_draws` runs the exact uber shader for every sample of every scaled draw: colour and depth are
read-modify-written in `R32_UINT` storage images under the fragment interlock, textures are fetched with `imageLoad`,
and there are no attachments. The classification of PARITY-PLAN section 3 is not implemented.

### The difference, and what to change in Part B

The reference's split is **"does the fragment need the destination?"**, not "is the state unusual?". Two rows of our
planned classification are therefore stricter than the reference and cost us draws for no fidelity reason:

- **Wrap modes.** `REGION_REPEAT`, `REGION_CLAMP` and the 4-bit fix do not need the destination. A hardware sampler
  cannot express them, but a shader can (the reference does it in the fragment shader with integer texel arithmetic,
  on top of a clamp-to-edge sampler). Our plan sends them to the exact path; they could stay fast with a shader-side
  wrap, at the price of a sampler that must be clamp-to-edge and a coordinate that must be computed, not fetched.
- **CT16 targets.** The reference keeps every colour target in 8 bits per channel and quantises to 5 bits in the
  shader, so a 16-bit target is the same path as a 32-bit one. We cannot do that without giving up the exact packed
  word, which is the authority. This one is a genuine constraint of our architecture, not an oversight: CT16 targets
  must stay exact until (if ever) we give them an attachment representation of their own.

The rows where our plan and the reference agree, and where the exact path is right: DATE, PABE, AFAIL ≠ KEEP,
COLCLAMP = 0, a non-byte-aligned FBMSK, Z16/Z24 depth *writes* (our packed word merges the upper byte; the reference
has no packed word), and anything that reads depth in the shader.

One more thing the reference does that we should consider for Part B: **it renders the alpha test as a discard even
when the action is KEEP and the comparison needs a shader-side clamp of AREF** (it corrects the reference value when
the target format quantises alpha). Our plan already does the discard; the AREF correction for CT16 is the part worth
copying, because it is the difference between "the same pixels pass" and "almost the same pixels pass".

---

## 5. Depth at scale

### What the reference does

**Confirmed.**

- Every depth target is a **real depth attachment** at `ceil(unscaled · s)` per axis, in a 32-bit float format (with
  an 8-bit stencil when DATE needs one). Depth test and depth write are the fixed-function ones, so **early-Z works**,
  and the driver's depth compression applies.
- Z16 and Z24 are *not* separate formats: the attachment is always 32-bit float, and the shader clamps the incoming Z
  to the format's maximum (`0xFFFFFFFF >> (fmt·8)`), floors it when the target's precision requires it (non-flat Z
  with a depth write, or a "greater or equal" read), and lets the hardware compare the resulting float. A 24-bit
  target is therefore stored with float32 precision (24 mantissa bits), not with the GS's exact 24-bit word.
- Depth is turned into a colour-like texture only when the game samples it, and then as `R32_UINT` for 24/32-bit and
  `R16` for 16-bit, with the sampling coordinate multiplied by the target's scale.
- A depth target whose offset relative to the colour target differs from the frame's uses a **temporary depth buffer**
  at the colour target's size and scale, copied in and out around the draw. That is the only depth "conversion" the
  reference has.
- Feedback depth (software blending that needs the depth value, or ROV) uses a depth read in the shader, which is a
  separate feature and off unless the state needs it.

### What CoreGS 2.0 does today

The scaled depth is an `R32_UINT` storage image (`scaledDepths`), read-modify-written by `hw_pixel_uber.frag` /
`hw_pixel_core.glsl` under the interlock, with the exact GS words. There is no depth attachment, so there is no
early-Z and no compression, and the plan's measurement (dropping the interlock saved only 0.5-1 ms of 8 ms) is
consistent with the cost being the per-sample shading, not the ordering.

### The difference

**Confirmed:** the reference has **one** depth representation and it is a hardware attachment; we will have **two**
(the exact words plus a derived `D32_SFLOAT` twin) and must keep them in step. Three consequences for the plan:

1. **The r32ui → D32 conversion is lossy in the same way the reference is lossy.** A `uint32` depth word has 32 bits;
   a `float32` has 24 mantissa bits. Converting on the way in keeps the top 24 bits, which is exactly what the
   reference stores. So the twin is *not* less faithful than the reference; it is less faithful than our own 1x path,
   which is why the plan's rule "never convert D32 back to r32ui" is right: keep the words as the authority and treat
   the twin as a test-only derivative.
2. **The mirror write is our invention and is the risky part.** The reference never writes two depth representations.
   Our fast shader must write the depth word it computed into the storage image *and* let the attachment do the test,
   and the two must agree fragment for fragment. That only holds if the depth test is guaranteed to run **before** the
   fragment shader: with the early-fragment-test declaration the attachment rejects a fragment before the shader runs,
   so a rejected fragment never writes the mirror, and an accepted one writes the same word the attachment stores at
   the end of the fragment. Without that guarantee (the default in Vulkan) the implementation may run the shader
   first, and a fragment the attachment then rejects has already written the mirror — the two representations
   diverge, silently, only on hidden geometry. The plan should state this as a requirement of step 3, not as an
   optimisation, and gate it with a fixture that draws overlapping triangles with a depth write and compares the
   r32ui image against the exact path bit for bit.
3. **Z24 writes on the fast path.** The plan's rule (Z24 keeps the exact path) is stricter than the reference, which
   stores Z24 in float and never merges bytes. We need the merge because the packed word is the authority. Keep the
   rule.

### Which artifact it could explain

**None of the owner's artifacts.** Depth is not presented; a depth error shows up as wrong occlusion, which the owner
has not reported. The one place it could leak into the picture is the tone carry and the display shadow, which use
depth writes on the shown buffer (IN-377), and those are 1x-word mechanisms, not attachment mechanisms.

---

## 6. Upscaling filters

### What the reference does

**Confirmed.** There are three separate filter decisions, and only one of them is "the upscaling filter" in the sense
the owner means.

1. **Texture filtering** (how a texture is sampled inside a draw): four settings — nearest; forced bilinear; **the
   game's own filter bits (the default)**; forced bilinear except on sprites. Plus a trilinear/mipmap setting
   (automatic, off, game, forced) and anisotropic filtering. So by default the reference does **not** force bilinear
   on a texture the game samples nearest. There is an option to do it, and a variant that spares sprites because
   forcing bilinear on sprites is what produces seams at sprite edges when upscaling.
2. **Uploads into an upscaled target**: when the EE writes into a region that a scaled target covers, the reference
   blits the 1x upload into the scaled target with **bilinear** by default (nearest is an option), and always
   bilinear for depth. This is a *forced* filter that has nothing to do with the game's filter bits, and it is on by
   default.
3. **Presentation** (the final image to the window): three settings — off (nearest), bilinear smooth (the shipped
   default), bilinear sharp (the UI default). The sharp mode first pre-scales the image by an integer factor with
   nearest until it covers the window, then samples bilinearly, which keeps text crisp; the smooth mode is a plain
   bilinear sample of the whole image into the destination rectangle. Optional CAS (sharpen only, or sharpen and
   resize) and FXAA sit before it.

**Confirmed: the reference's present filter is the main reason a 6x internal image does not look like blocks.** A
bilinear sample of a 3840x2688 image into a 2560x1440 window averages about 1.5 x 1.9 source pixels per output pixel;
a 6-pixel lattice survives only as a faint modulation. The sharp mode preserves more of it, by design.

### What CoreGS 2.0 does today

- Textures: `sample_texture` / `sample_texture_scaled` honour the game's linear bit; `GT4_HW_SCALED_FILTER=linear`
  forces bilinear on 1x textures in the scaled pass (the owner's A/B, no visible change, as measured). `GT4_HW_FILTER`
  exists in `hw_gs_options.cpp`.
- Uploads into a scaled target: `refresh_scaled` / `hw_scale_upsample.comp` are **nearest**, always.
- Present: IN-375 reduces a frame larger than the presentation rectangle by 2x2 box steps and one linear blit; spec
  13.6 also defines nearest and "sharp bilinear" (`GT4_HW_PRESENT_FILTER`). FXAA (IN-376) is edge-only.

### The difference

| | Reference | CoreGS 2.0 |
|---|---|---|
| Texture filter | game's bits by default; forced-bilinear options | game's bits; forced-linear option |
| 1x upload into a scaled target | **bilinear by default** | **nearest** |
| Present filter | bilinear smooth (default) / sharp / nearest, plus CAS | box steps + one linear blit; nearest and sharp exist |
| Present default at 6x | smooth, i.e. the lattice is filtered | linear blit after box steps (similar), but the dumps are pre-present |

### Which artifact it could explain

- **The 6x12 lattice (A1), partially.** Even with our dither unchanged, the reference's present filter would soften
  it in a window screenshot. This is why the owner's comparison must be made before the present filter, or with the
  same filter on both sides. Note the plan's own finding that `GT4_HW_SCALED_FILTER=linear` changed the picture by a
  mean absolute difference of 0.44 and did **not** remove the grid: that rules out the *texture* filter and leaves
  dither (section 2) and the present filter.
- **The map's white dashed lines and the 6-pixel checkerboard (A6, A7), partly.** If the map layer reaches the frame
  through an EE upload into a scaled target, the reference smooths it with bilinear and we do not. Falsification: find
  the draw that produces that layer in the replay and check whether it is a dirty upload (`UploadIntoSlot`) or a
  GLM decode. If it is a decode, the filter is not the cause and section 7 is.

---

## 7. The two mechanisms that have no reference counterpart

These are the two places where our architecture can produce an artifact the reference cannot, and they are the
remaining unexplained owner artifacts.

**7.1 Per-sub-pixel source selection in the compose.** `hw_display_compose_scaled.comp` chooses, for *every sub-pixel*,
between the slot's scaled image, a display shadow (with a two-word validity check) and the GLM at 1x. A sub-pixel that
fails a check shows the 1x word. The reference has one source per circuit (the scaled target) and no validity test.

- Artifacts: a one-native-pixel vertical line (A2), a one-native-pixel column or row of 1x content, and any artifact
  whose measured width is exactly one native pixel.
- High confidence that this is the *class* of the line; the plan's own bisection already showed the line survives with
  the tone carry off, which points at the shadow check or at a column the slot does not cover.
- Falsification: add the counter the plan describes (per circuit, how many sub-pixels came from each source, with a
  per-column and per-row histogram) and check whether the measured column or row is the one with the fallback. Then
  the fix is a policy choice, not a bug fix: fall back per native pixel, per block row, or per frame. Per native
  pixel turns a 1-pixel line into a 6-pixel block (aligned with the lattice, arguably worse); per frame turns it into
  a whole frame at 1x (the 0.8% of frames already seen, `Counters::gpuComposeSkips`). There is no option that matches
  the reference, because the reference does not have the problem.

**7.2 The tone carry.** `TrackToneRun` / `FinishToneRun` / `CarryTone` rebuild the scaled image of GT4's tone/brightness
source from a per-channel table learned on the GPU, and fall back to the 1x word per sub-pixel where the table cannot
explain the current word. The reference does the same sequence as **one HLE draw at the source's scale** (its GT4
database entry), i.e. it applies the palette per output pixel on the scaled target and never produces a 1x
intermediate.

- Artifacts: the 6-row banding with the anomalous phase (A3), any per-native-row structure in the race, and the
  "linha certa, linha preta" banding.
- Confirmed by the plan's bisection that the banding tracks the carry; the mechanism is that a per-native-row (or
  per-sub-pixel) decision is a 6-row structure at 6x, and the reference has no such decision.
- Falsification / fix direction: the plan already tried a per-native-pixel version of the decision and the phases did
  not move, which means the fallback is not the only mechanism. The next measurement should be the *learned table*
  itself: dump the table (the kernel already exists) and check whether the band corresponds to a row range where the
  table has an unseen value (interpolated) rather than a fallback. If the band is a table interpolation artifact, the
  fix is a better table (more samples, or a per-row table), not a per-pixel decision.

---

## 8. Owner artifacts, ranked

| # | Artifact | Most likely mechanism | Label | Test that settles it |
|---|---|---|---|---|
| A1 | 6-pixel lattice / 6x12 cells on flat backgrounds | our per-native-pixel dither, doubled by the field stretch | High confidence | dither index = raw fragment coordinate; re-dump |
| A1b | same, if dither is off for that draw | 1x texture magnified nearest, or the missing present filter | Hypothesis | check the draw's target format; present with the reference's filter |
| A2 | one-native-pixel vertical line | per-sub-pixel source selection in the compose | High confidence (class) | the per-source, per-column fallback counter |
| A3 | 6-row banding, phase 5 | the tone carry (table or fallback) | Confirmed that it tracks the carry; mechanism Unknown | dump the learned table and the fallback mask |
| A4 | ghosted second copy of dialog text | per-native-row weave against a one-line field difference | High confidence | force bob; compare the two fields' rows |
| A5 | jitter on a 3D sprite | same as A4 | High confidence | same |
| A6 | map's dashed lines | 1x layer (GLM decode) sampled at 1x | Confirmed as a class (IN-374 limitation) | find the draw; if it is a decode, only a scaled decode fixes it |
| A7 | 6-pixel checkerboard on a full-screen region | same as A6 | Confirmed as a class | same |
| A8 | lattice on the car's rear window and hood | a texture taken from a slot but sampled from the wrong origin, or a 1x layer | Hypothesis (IN-384 follow-up fixed one origin bug) | replay the frame and dump the scaled texel image of that draw |
| A9 | 0.8% of frames presented at 1x | `GpuComposeApplies` declining, or the compose failing | Unknown (instrumented) | the `compose_skips` counter on the owner's run |

---

## 9. What to copy, what not to copy

**Copy (behaviour, not code):**

1. Dither per output pixel as the *default at scale*, with per-native-pixel as the faithful option. This is a
   one-line change in `hw_pixel_uber.frag` and it is the highest-value item in this document. Record it as a new IN
   note with the owner approval of 2026-10-23 (presentation-only tolerance, same rule as Part B).
2. Bilinear for 1x uploads into a scaled target, at least as an option, and always for depth. It is what the
   reference does by default and it removes the block edges of EE-uploaded content (A6/A7 class).
3. A present filter choice that matches the reference's three modes, and a comparison protocol that dumps the
   internal image on both sides.
4. The Part B classification rule "feedback decides", with wrap modes and texture formats staying on the fast path.

**Do not copy:**

1. The single-representation architecture (no 1x authority). It is incompatible with TASK-027's memory exactness and
   with every 1x gate.
2. The 16-bit target as 8 bits per channel with a shader-side mask. Our packed word is the authority.
3. The per-game database hacks (the tone HLE is the one that matters for GT4, and we already have our own).
4. The reference's depth precision (float32 for Z24/Z32). Our exact words are stricter; keep them, and treat the D32
   twin as a test-only derivative.

---

## 10. Limits of this document

- No measurement was taken. Every claim about *our* current picture is quoted from PARITY-PLAN section 10 or from the
  code, not re-measured.
- The reference's per-game GT4 entry is one data point in a database that changes upstream; if a future reference
  build changes GT4's half-pixel or skip entries, section 1's "we already match" conclusion must be rechecked.
- The tone-carry mechanism (section 7.2) is the least understood item here and the only one where the reference gives
  no usable rule, because it does not have the problem.

---

## 11. Follow-up: 480i menu layers at scale

Question from the implementer, after the owner reported a diagonal cross-hatch with a cell of roughly 6-8 output
pixels over the whole background of the GT Mode "Info" dialog (480i menu, 6x, window 3440x1369), where the reference
shows flat grey; the same pattern appears as thin doubled and broken white road lines on the GT Mode map. All
measurements quoted here are the implementer's, at 6x on the same frame.

### 11.1 How the reference renders a 2D menu layer above 1x

**Confirmed: there is no 2D path.** A menu background, a full-screen dim and a HUD sprite go through the same uber
shader, the same vertex mapping and the same targets as 3D, at the internal scale. The reference has no "2D at native
resolution" mode, no menu-resolution setting and no per-game flag for it. The GT4 entry of its per-game database
contains four items and nothing about 2D: the native-alignment half-pixel mode, an auto-flush at sprite boundaries, a
skip-draw hook that replaces the RGB-shuffle (tone/brightness) post-processing with one HLE draw, and a recommended
blending accuracy.

**Confirmed: the only 1x content a menu can show at scale is a 1x *source*.** Two cases: a texture uploaded by the EE,
which the reference keeps at its own size and magnifies with the sampler; and a target it deliberately reduces to 1x
because it serves as a palette or index source (a narrow case: the reduction is a box downsample with the first s
sub-pixels skipped, the same rule as 06.7).

### 11.2 What happens to a pattern that lives at 1x in the source

**Confirmed: the magnification is done by the sampler, not by a blit, so the filter decides the result.**

- nearest: each source texel becomes an s x s block, i.e. a magnified checkerboard, which is the picture the owner
  sees in ours;
- bilinear: the four neighbouring texels are weighted, so a plus-or-minus-one-level checkerboard becomes a smooth
  half-level diagonal ramp and reads as flat grey, and a one-pixel road line becomes a soft s-pixel line instead of a
  hard one.

**High confidence: that is the whole of the reference's smoothness for a *textured* 1x layer.** Its sampler is
bilinear whenever the game asks for linear (the default setting honours the game's bits), and it is *forced* bilinear
in two places: uploads of 1x data into a scaled target (always, including depth) and depth sampling. There is no other
smoothing step.

### 11.3 Any reference behaviour that averages a 1x pattern over the s x s block

**Confirmed: three, all narrow, none of them a general rule.**

1. The **box downsample** used when a scaled target has to become 1x (palette and index sources, 1x destinations): the
   average of the s x s block with the read origin clamped to `max(out * s, s)`. It produces a 1x image; it never feeds
   the presented frame at scale.
2. The optional **native-scaling** behaviour (off by default): it reduces a scaled source to 1x before a draw that
   itself downsamples.
3. The **present filter**: a resample of the final image, not a block average. At the owner's window the image
   rectangle is about 2434 x 1369, so the 3840 x 2688 frame is reduced by about 0.63x per axis, which averages roughly
   1.6 x 1.6 source pixels per output pixel: it attenuates a 6-pixel structure but does not remove it. The sharp
   present mode pre-scales by an integer nearest factor and then samples bilinearly, so it *preserves* a 6-pixel
   structure more than the smooth mode does.

**Confirmed: there is no "2D at native resolution" flag, and nothing in the reference re-expands a downsampled 1x
layer.**

### 11.4 What the evidence favours

**Confirmed by the implementer's measurements plus our code:** the pattern is a function of the *native pixel index*.
It survives the box downsample, each 6x6 block is internally uniform, and it is unaffected by the dither mode, by the
1x-atlas filter, by the upload refresh and by the half-pixel geometry.

**High confidence: the reference is smooth because no per-native-pixel pattern ever exists in its image at scale.**
Everything it computes at 6x is per output pixel: the dither (output-resolution by default), the blend, the tone HLE
draw, the sampler. Nothing in its 6x image is indexed by the native pixel, so nothing can be magnified into blocks.
Ours has three mechanisms that *are* indexed by the native pixel, and the implementer's tests removed only the first.

**Confirmed: at 1x our two dither modes are identical by construction** (the index divides by the scale, which is 1),
so a dither A/B can never change the 1x words and can only change draws that are rasterised at scale. That is a useful
invariant, because it makes the dither index memory-safe at 1x, but it also means the A/B cannot exclude a pattern that
was already in the 1x content.

**Hypothesis, in order of likelihood, for the three remaining creators of a per-native-pixel pattern:**

1. **A texture sampled at scale with nearest, through a path the forced-linear switch does not cover.** The switch is
   applied only when the sampled texture is the 1x atlas; a texture taken from a resident slot is sampled at scale and
   ignores it by design, because `sample_texture_scaled` honours the game's bit only. If the menu background reaches
   the frame that way, forcing linear changes nothing and the 6x image keeps one block per source texel.
   Falsification: extend the force to `tex_scale > 1` and re-dump, or find the batch in the replay trace and read
   whether its texture is the atlas or a slot copy.
2. **The tone carry's learned table.** A one-level per-channel error in the table is exactly a per-native-pixel
   pattern, it is invisible at 1x because the 1x words are exact, and no filter or refresh setting can touch it. It
   also fits the map's *broken and doubled* road lines better than anything else: a one-pixel bright line with a
   one-level error breaks into dashes and doubles. Falsification: `GT4_HW_TONE_CARRY=0` on that frame; if the pattern
   disappears it is the carry, and the fix is the table, not a filter.
3. **A 1x layer read through the compose's GLM fallback** (a circuit or a sub-rectangle the scaled slot does not
   cover), which is a nearest magnification of the 1x word by construction. Falsification: the per-source, per-column
   fallback counter already planned in PARITY-PLAN section 10; if the pattern's blocks all come from the GLM path,
   this is it.

**The single measurement that settles it:** dump our 1x `vram` image and our 6x frame of the *same* VBlank and compare
them per native pixel. If the 1x words are flat and the 6x blocks differ, the pattern is created by the scaled pass
and the answer is item 1 or 2 above. If the 1x words already carry it, it is in the 1x content (dither comes back as a
suspect, or a 1x-only pass wrote it), and the reference's difference is then in its 1x-equivalent content, which it
does not have.

### 11.5 Note on the measured period

**Confirmed arithmetic:** at 6x, 6-8 output pixels is one native pixel. What was measured is therefore a *cell*, not
a period; a true 6-pixel period would be sub-native and cannot come from content that is 1x. The period of an ordered
dither matrix would be 4 native pixels (24 output pixels) and would show as a slow modulation on top of the 6-pixel
cell, which is consistent with a cross-hatch whose cell is 6-8 pixels.

### 11.6 Correction to section 2

The implementer has since wired the dither index: `hw_pixel_uber.frag` now selects between the raw fragment
coordinate (the default, matching the reference) and the native index, and `GT4_HW_DITHER=native` selects the faithful
one. Section 2's "documented but not implemented" is superseded; the behaviour table there still stands, and the
invariant above (identical at 1x) is the reason the change is memory-safe.

---

## 12. Follow-up 2: the 4-native-pixel checkerboard

New measurement from the implementer, same scene (GT Mode "Info" dialog, 480i, 6x, headless): the 6x background is a
regular checkerboard of two greys with a square of about 24 output pixels, i.e. **4 native pixels**; the 1x image of
the same background is nearly flat (20 distinct values in a 190 x 100 region, mean adjacent difference 1.83); a box
downsample of the 6x frame differs from the 1x image by 4.47 mean absolute, so the scaled pass creates the structure.
With the deinterlacer off the deviation falls to 2.31 and a 329-value noise remains. The pattern is unchanged by the
dither mode, by forcing linear on 1x and scaled textures, by the refresh filter, and by the tone carry (menus have no
tone run). Display shadows serve about 13,000 of 14,000 circuits on menus.

### 12.1 Is a 4-native-pixel period a signature of anything?

**Confirmed: 4 native pixels is exactly the period of a 4x4 ordered dither matrix indexed by the native pixel**, and
that is the only thing in either renderer whose natural period is 4 native pixels. The reference has the same matrix
but indexes it by the output pixel by default, so the period cannot appear in its image at scale; ours indexed it by
the native pixel until the recent change. The *period* is therefore a genuine structural difference between the two
renderers, and it is the reason the owner's two screenshots differ in character even when both are "correct".

**Confirmed: the implementer's dither A/B is nevertheless valid, and the dither is not this pattern.** Three facts
settle it. The flag is delivered correctly: the push word masks the filter bit and the dither bit separately, so the
shader sees the intended value. The 1x words are identical in both modes by construction (the index divides by a scale
of 1), so the only place the flag can act is the scaled pass, which is exactly the pass that creates the pattern. And
if the scaled draw dithered, the two modes would differ. Byte-identical frames therefore mean the scaled draw adds no
dither: the target is not 16-bit, or the game's dither enable is off, or the matrix is all zero (a case that occurs in
real games and that the reference explicitly tests for).

**Confirmed: the single dump that proves the state** is the per-batch trace of the batch that covers the background
rectangle, read for four words: the frame format, the dither enable, the dither matrix and whether the batch is
textured at all. That is one command with the existing replay tooling and it closes question 1 completely.

### 12.2 Can the display shadow produce it?

**Confirmed: the reference has no comparable behaviour.** One source per circuit, no validity test, no per-pixel
source selection. It cannot produce a per-native-pixel mosaic of two sources.

**Confirmed by arithmetic: the shadow cannot be the cause here.** A native pixel that fails the shadow validation
shows the 1x word, and the 1x word is the smooth value. The shadow path can therefore only pull the 6x image *towards*
the 1x image, never away from it. With the 6x image 4.47 away from the 1x image, the checkerboard must live in the
*passing* pixels, i.e. in the slot's scaled image, which the shadow merely copies.

**High confidence, and cheap to confirm without the planned counter:** compare a frame served by a display shadow with
a frame served by a resident slot (the counters say about 7% of menu circuits are not shadows). If the checkerboard is
the same in both, the shadow is exonerated. If it appears only in shadow frames, the fault is in the saved rectangle or
its stride, not in the raster.

### 12.3 Would the reference show the same checkerboard?

**Confirmed: no.** Nothing in the reference's 6x image is indexed by the native pixel (dither, blend, tone HLE draw
and sampler all run per output pixel), so a 4-native-pixel structure cannot exist there at all. The owner's GSdx
screenshot is therefore a genuinely different image, and the difference is in our content, not in a filter.

**Confirmed: the present filter cannot be the explanation.** A 0.63x bilinear reduction of a 24-pixel, roughly
one-5-bit-step checkerboard attenuates it but leaves most of its contrast; the owner would still see it.

**Confirmed: the per-game GT4 entry cannot be the explanation.** It has four items and none of them concerns 2D or
menus (section 11.1).

### 12.4 The split that has not been made yet

Every measurement so far compares the *composed* 6x frame. The missing split is composed frame versus **the slot's own
scaled image** and versus **the shadow's scaled copy** for the same VBlank. The dump machinery already exists (the tone
diagnostic writes a saved scaled source and a rebuilt scaled image).

- Checkerboard already in the slot's scaled image: it is the scaled raster, and the period-4 source is a 4x4-tiled 1x
  source — a texture sampled at scale, the scaled texture copy, or an atlas origin. **Hypothesis**; falsification is
  the amplitude below.
- Checkerboard only in the shadow copy: the saved rectangle or its stride. **Hypothesis**; falsification is a byte
  comparison of the shadow against the slot rectangle it was copied from.
- Checkerboard only in the composed frame: the compose's read offset. **Hypothesis**; falsification is the compose
  parameter block printed for that VBlank (read width/height, placement width/height, offsets).

**The one number to read first: the difference between the two greys.** Exactly 8 levels in 8-bit units is one 5-bit
step and is the signature of a 16-bit quantisation or dither, which would put the dither back on the table despite the
A/B (and would mean the dither is applied to a 1x word the scaled pass then magnifies). One or two levels is a
sampling or texture artifact, which points at the 4x4-tiled source above. This single number splits the two families
of causes and is cheaper than any counter.

**Two items, not one:** with the deinterlacer off the deviation halves but the 329-value noise remains, so about half
of the 4.47 is the field weave and half is this pattern. Fixing the weave will not remove the checkerboard and vice
versa; the owner will report whichever is left.

---

## 14. 1x texture sources at scale (IN-387)

New evidence from the implementer, same scene (GT Mode map + "Info" dialog, 480i, 6x): a lattice whose cell is exactly
one native pixel (6 x 12 output pixels after the field stretch) with an amplitude of one 8-bit level; the 1x words of
the same slot read 138.33 on average, the 6x image reads 137.3-137.7 in the cell structure; the map background is a
1024 x 1024 texture from guest memory, TFX = 0, FST = 1; the menu layer is a quad whose texture is a resident-slot
copy; forcing linear on both atlas kinds changes 21.8% of the CT24 layer's 6x image but leaves the two displayed slots
and the composed frame byte-identical.

**Confirmed, and it corrects the premise of question 3: TFX is not a filter.** TFX is the texture *function*
(modulate, decal, highlight); the sampling filter is the magnification bit and the minification field of the TEX1
register, and mipmapping is gated by that register's maximum-level field. A trace line "TFX = 0 (nearest)" therefore
does not establish that the guest asked for nearest, and FST only says the coordinates are integers (1/16 texel
units), nothing about filtering. Read: `pcsx2/GS/GSRegs.h` (the TEX1 register and its linear-filter predicates),
`pcsx2/GS/Renderers/Common/GSVertexTrace.cpp`, `Update`.

**Confirmed: the one number that is *not* part of this question is the 0.8-level DC difference** between the 1x mean
(138.33) and the 6x mean (about 137.5). A one-level lattice can exist with or without a DC offset; the offset needs its
own explanation (section 14.4). Do not use the mean to decide the shape.

### 14.1 Question 1: scaled surface or 1x surface?

**Confirmed: both, and the deciding rule is "is the source a live render target".**

- If the texture's block range is covered by a live, compatible render target, the lookup returns that target's own
  surface. The source shares the target's texture (no copy) or receives a copy allocated at `ceil(unscaled x scale)`,
  and the source's scale is the target's scale. So the surface is the internal-resolution image, not a 1x image.
  Read: `pcsx2/GS/Renderers/HW/GSTextureCache.cpp`, `LookupSource` (the target scan) and `CreateSource` (the
  target branch), and `Target::Create` (allocation at `ceil(size x scale)`).
- If no live target covers it, the texture is decoded from guest memory into a surface of its own size and its scale
  is exactly 1, so the sampler does the magnification. Read: `GSTextureCache.cpp`, `CreateSource`, the branch with no
  target.

**Confirmed: there is no 1x copy of a target kept for sampling.** The reference has no exact 1x frame to fall back to
(section 0). It produces a 1x reduction of a scaled target in exactly two places, and both are one-way: the
palette/index conversion (an 8-bit index source is forced to scale 1 and reduced with a point sample before the
palette lookup) and the readback to guest memory. Neither is ever re-expanded into the presented frame at scale.

**Confirmed: even the "linear" case of a target source interpolates on the scaled grid.** For a 24-bit target the
reference does not use the hardware bilinear unit; it takes four point samples and mixes them in the shader, with the
half-texel offset expressed in *scaled* texels. So a target sampled with linear is smoothed at sub-native resolution,
never between two 1x texels. Read: `pcsx2/GS/Renderers/HW/GSRendererHW.cpp`, `EmulateTextureSampler` (the
shader-emulated-sampler condition, true for a 24-bit target source) and `bin/resources/shaders/vulkan/tfx.glsl`,
`sample_color`.

**Confirmed: a target source never receives mip filtering.** Trilinear is allowed only when the source is not a
palette, not a target and not a shuffle. A target is therefore always sampled from level 0 of its scaled surface.

Answer: the reference samples the *scaled copy* for a rendered target, and keeps the texture at 1x with sampler
magnification only for a texture that is not a live target. It never returns a native-resolution surface for
something it also holds as a target (with the palette/index exception above).

### 14.2 Question 2: nearest or filtered when 1x content goes into the scaled surface?

**Confirmed: neither "always nearest" nor "always filtered"; it is a per-path decision, and the default is not the
same on both paths.**

- A target that is created or recreated and preloaded from guest memory receives its 1x content with a **bilinear**
  blit for every colour format of 16 bits or wider (and always for depth). The preload asks for the filtered blit
  explicitly for those formats. Read: `GSTextureCache.cpp`, `PreloadTarget` (the linear request passed with the dirty
  rectangle) and `Target::Update` (the filter decision, `BilinearHack` = automatic).
- A partial dirty rectangle patched into a target that survives is uploaded with **nearest** under the same default,
  because the generic invalidation path does not request the filtered blit; only depth, an explicit request or the
  forced-bilinear option turn it on. Read: `GSTextureCache.cpp`, `InvalidateVideoMem` (the dirty-rectangle call) and
  `Target::Update`.
- 8-bit and palette formats are nearest on both paths.

**High confidence: this matters more than it looks.** A bilinear magnification by an exact integer factor is not a
block replication: the destination samples land at fractional positions inside each source texel, so the result is a
ramp *through* each block, and a nearly flat 1x area stays smooth. A nearest magnification is the opposite: each
source texel becomes a hard s x s block, and a one-level difference between neighbouring 1x words becomes a one-level
lattice with a cell of exactly one native pixel. So the reference's preload path cannot produce the owner's lattice
from 1x content; its nearest patch path can.

**Hypothesis (needs one log line to settle): which path GT4's transfers take.** A transfer that covers the whole valid
area removes the target, so the next use recreates it through the bilinear preload; a partial transfer patches it and
gets the nearest path. Falsification: log, for the CT24 display buffer on that frame, whether the target was
recreated or patched, and whether the dirty rectangle carried the linear request.

Answer: when the reference does resample 1x content into a scaled surface, the surface is a *filtered* magnification
by default (bilinear), not a nearest one; nearest happens only on the partial-patch path and for 8-bit/palette
formats.

**Correction to section 6:** the row "1x upload into a scaled target: bilinear by default" is true for the preload
path and needs this refinement for the partial-patch path, where the default is nearest. The distinction matters
exactly for this artifact, because only the nearest path can turn a one-level 1x difference into a one-native-pixel
lattice.

### 14.3 Question 3: filters wider than one texel, mip levels, and the 1024 x 1024 case

**Confirmed: wider-than-one-texel filtering exists only where the guest asks for it (or the user forces it).** The
shipped default is "the game's own bits"; the forced-bilinear and forced-bilinear-except-sprites options and the
forced-trilinear option exist and are off by default, and anisotropic filtering is off by default. With the default,
a 2 x 2 bilinear tap happens only when the magnification bit is set or the minification field selects a linear form,
and blending between two mip levels happens only when the minification field selects a linear mip form.

**Confirmed: there is no user LOD bias.** The guest's own fixed-LOD constant and its L term are implemented (they bias
and scale the LOD), but the reference adds nothing of its own.

**Confirmed: mipmapping is active only when three conditions hold together** — a non-zero maximum-level field, a
minification field in the mipmap range, and a computed maximum LOD above zero. Otherwise the minification field is
ignored, and with a zero maximum-level field the filter is the magnification bit alone. Read: `pcsx2/GS/GSState.cpp`,
`IsMipMapDraw`; `GSVertexTrace.cpp`, `Update`.

**Confirmed: the LOD never depends on the internal resolution.** For a draw with floating texture coordinates the LOD
is derived from the guest's own texture-coordinate range (its Q or ST extent), i.e. from the native texture-to-screen
ratio; for an integer-coordinate draw (FST = 1, which the trace reports) the LOD is the guest's fixed constant
divided by 16, with no geometric term at all. Upscaling changes neither the selected level nor the filter. Read:
`GSVertexTrace.cpp`, `Update`.

**Confirmed: therefore, for the 1024 x 1024 texture on the 640 x 224 screen, the reference samples level 0 with point
filtering when the guest asks for point** — either because the maximum-level field is zero, or because the
minification field is the plain nearest form, or because the draw is FST with a fixed LOD of zero. It does not
substitute a mip level to reduce the aliasing of a texture the guest chose to sample nearest. If the guest does ask
for mipmapping, the level is the integer LOD clamped to the maximum level (from the guest's fixed constant for an
integer-coordinate draw, or from its Q for a floating-coordinate one), and the within-level filter is nearest for the
nearest-mip forms and linear for the linear-mip forms. The internal resolution never enters that choice, so the level
at 6x is the level the reference would use at 1x; if the guest's constant is zero, that is level 0 even though the
texture is minified more than four times vertically.

**Confirmed: there is no "texture scaling for 2D art".** A texture that is not a target is allocated at its own size
and magnified by the sampler. The only scaled sources are render targets, the opt-in merged-source mode (off by
default), the native-scaling hack (off by default) and replacement texture packs (user assets).

Answer: the reference does sample with a filter wider than one texel, but only when the guest's own filter field asks
for it; for the 1024 x 1024 background with the guest asking nearest it samples level 0, point, at 1x and at 6x
alike. The internal resolution does not change the answer.

### 14.4 Question 4: what can still produce one level with a one-native-pixel cell

**(a) A nearest magnification of a 1x texture — Confirmed possible, and the only mechanism that produces both the
amplitude and the cell.** Nearest magnification makes each source texel an s x s block whose value *is* that texel,
so two neighbouring native pixels differ by exactly the source's difference (one level here) and the cell is exactly
one native pixel. In the reference this is available only where it genuinely keeps 1x content: a texture that is not
a live target, a 1x patch uploaded through the nearest path, or an index/palette source reduced to 1x.

**(b) Blend precision — Confirmed not a creator.** The reference works in 0..255 floats on 8-bit targets, uses the
hardware blend unit where the equation allows and a shader blend otherwise, truncates and adds a GPU-dependent offset
to emulate the GS's truncation; its own comments put the match at roughly 97-99% of pixels in the worst case, so a
one-level difference is inside its error budget. But the blend runs per fragment, i.e. per *output* pixel at 6x. It
can add a one-level error on top of an input that already varies per native pixel; it cannot create a period of one
native pixel by itself. Read: `bin/resources/shaders/vulkan/tfx.glsl`, `ps_blend`.

**(c) The tone/brightness HLE draw — Confirmed not this scene, and per output pixel in any case.** The reference
replaces GT4's channel-shuffle plus ramp-palette sequence with one HLE draw at the source's scale, reading the channel
and the palette per output pixel; it has no per-native-pixel indexing. Menus have no tone run (section 12), so it is
not in play here. Read: section 4 of this document and `GSRendererHW.cpp`, the per-game entry's skip-draw hook.

**(d) Something else — Confirmed nothing else in the reference has a one-native-pixel period.** Dither is indexed by
the output pixel by default (the other mode divides by the scale and has a four-native-pixel period, not one);
the deinterlacer works in output rows; the present filter is a resample and only attenuates; the CPU sprite rasteriser
is off by default; replacement textures need user assets. Read: `tfx.glsl`, `ps_dither`; sections 2, 3 and 6.

**High confidence: in the reference's model the lattice is the signature of a 1x surface that was nearest-magnified,
and it cannot come from content that passed through a rendered target at 6x.** That is the whole point: a target
sampled at scale is the 6x surface, and everything the reference writes into it is computed per output pixel. The
reference can show this artifact only where it keeps 1x content itself.

**The single measurement that settles it:** print, for the same VBlank and the same native rectangle, a per-native-
pixel *map* of the slot's 1x words and, beside it, the mean of each 6 x 6 block of the 6x target (before the field
stretch, not from the composed frame). Two outcomes, and they are exhaustive:

- the two maps have the same shape and differ by a constant: the lattice is 1x content, the scaled pass only shifted
  its level, and the magnifier is a 1x source (in the reference's model: a non-target texture or a nearest-patched
  upload);
- the 1x map is flat and the 6x block means alternate: the scaled pass computes the lattice from a per-native-pixel
  input, which in the reference's model is again a 1x source sampled at scale, never a 6x target.

Either way the question reduces to "what surface is being sampled", and the second dump (the slot's 6x image against
the atlas the menu quad reads) says which pass introduced it. The 0.8-level DC difference is a separate number and
must be explained separately: a nearest magnification cannot change the mean, so if the 6x mean really sits 0.8 below
the 1x mean there is a second mechanism in the scaled pass (an 8-bit rounding or a blend-space difference), and the
reference has the same class of difference for the same reason - its targets are 8-bit and its blend is approximate -
which is why the reference cannot be used to calibrate it, only to bound it at about one level.

---

## 15. Notched primitive edges and menu lines at 6x (IN-391)

Same scene (GT Mode map, "Info" dialog, 480i, 6x, displayed at 1920x1080). Four owner reports: text not 100% sharp,
a line cutting across the dialog, small rectangular bites on the two "wheel" billboards and on the dialog border
corners, and the same bite on the car in the race. Our facts: the dialog is composited from its own target over
1024x256 and 1024x1024 textures sampled at FST; the composed 6x menu is within one level of a bilinear magnification
of our 1x image (correlation 0.969), only the text differs; our per-primitive clip rectangle is
`floor(min+0.9) .. floor(max+0.6)`, scaled as `offset*scale + scale/2`, `width*scale`, and used as the Vulkan scissor.

The three artifacts do not share one cause. 15.1-15.3 are about the bites, 15.4 about the lines, 15.5 about the
"not a magnification" question. Every reference claim below is read from `private/slice189-pcsx2-ref` in the main
checkout.

### 15.1 Question 1: does the reference clip a primitive to a bounding rectangle?

**Confirmed: it computes a rectangle, but never uses it to clip geometry, and it is one rectangle per *draw*, not per
primitive.** `GSRendererHW::Draw` builds `m_r` from `round(min..max + 0.4)` over the vertex trace of the whole batch
(`GSVertexTrace::Update` scans the full vertex list once), intersects it with the GS scissor and returns early only if
the result is empty; `m_r` then feeds target sizing, page and block address math, dirty-rect invalidation and clear
detection. The only rectangle that reaches the GPU as a region is `m_conf.drawarea` = scaled GS scissor intersected
with `ComputeBoundingBoxRT`, and that box is the vertex extremes **expanded** by one native pixel per side (two when
coverage alpha is available) *before* the scale multiply; the render pass itself uses the full target size and the
scissor is the GS scissor alone (`GSDeviceVK::RenderHW`: `OMSetRenderTargets(..., config.scissor, ...)` and the
`render_area` selection above `BeginRenderPass`). A per-draw union also cannot open a gap *between* two primitives of
the same batch, because every primitive of the batch shares one rectangle that covers all of them.

**High confidence: our rectangle does cut, and by up to a whole native pixel, not a sub-pixel.** `floor(min+0.9)` is
never below `min`, so it drops the first covered column whenever `frac(min) >= 0.1`, and `floor(max+0.6)` exclusive
drops the last covered column whenever `frac(max) <= 0.4`; that is 0 or 1 native pixel per side, i.e. 0 or 6 output
pixels of hard, axis-aligned cut at 6x, and the `+ scale/2` offset biases the scaled rectangle half a native pixel
(3 output pixels) right and down, so one edge loses and the other gains.

**High confidence, checkable on the owner's screenshot: the bite should be at most about 5.4 output pixels wide by
3.6 tall** (0.9 and 0.6 native pixels), i.e. slightly wider than tall and quantised to output pixels. The unit of the
reported "6" is therefore the first number to settle: 6 output pixels of the 6x frame is exactly one native pixel and
fits the scissor arithmetic; 6 *native* pixels (36 output pixels) is a square the size of a sprite and would need the
rectangle to be scaled twice - the candidate is the write-back chunk rectangle of our fact 7, where a rectangle in
native units is scaled and then used again in a pass that is itself scaled.

### 15.2 Question 2: what makes rectangular bites in an alpha-tested billboard at high resolution?

**Confirmed (ours), and the only mechanism that fits: a rectangle computed in native units and applied as a scissor.**
Its signature is exactly the owner's: axis-aligned, quantised to the native grid, located where two primitives or two
rectangles meet, and completely independent of texture content, filter, alpha and depth. The dialog border proves it
independently of the 3D case: whether the border is four bars or four quads of a ring, each corner is where two
rectangles meet, and no filter, alpha test or depth rule has an axis-aligned preference at a flat corner, so nothing
but a clip rectangle can remove a square from it.

The remaining candidates, ranked, with the reason each is excluded:

1. **Texture cache dirty rectangles and partial updates - Confirmed possible in the reference, but scale-independent.**
   `GSTextureCache::Target::Update` uploads the union of the dirty rectangles (its own comment: "the union of the
   rects may also update wrong parts of the render target") and copies each one with `drect.filter = BilnIf(linear &&
   (is_depth || m_dirty[i].req_linear || override_linear))`, i.e. nearest unless the preload asked for linear. That
   produces rectangles aligned to *target* space, and it happens identically at 1x, so it cannot be "absent at 1x";
   it also needs the rim to be a live target, and it is what the `samplearea`/region clamp exists to bound.
2. **Alpha test on filtered samples - Confirmed not a creator.** `GSRendererHW::EmulateAlphaTest` discards per fragment
   on that fragment's own alpha (`PS_AFAIL::KEEP`), so the contour follows the filtered alpha ramp: on a circular rim
   it moves the radius by a fraction of a texel and stays circular. It can thin or thicken a ring; it cannot square it.
3. **Sprite and rectangle fast paths - Confirmed not a creator.** The sprite class is two triangles; `MergeSprite` is a
   paving user hack, off by default and gated on `UserHacks_MergePPSprite`, a target source and a uniform-vertex test;
   `ReplaceVerticesWithSprite` rewrites *all* of a draw's vertices into one quad, so it is all-or-nothing per draw.
4. **Z precision - Confirmed not a creator.** Depth is rasterised at the internal resolution into a float buffer, so
   precision improves with scale; a Z error shows as stitching or speckle along shared edges, not as an axis-aligned
   square, and a billboard at constant Z (`m_vt.m_eq.z`) either passes entirely or fails entirely.

**High confidence: if the bite is one native pixel, nothing in the reference can produce it and our scissor is the
whole story; if it is six native pixels, the scissor is still the shape but the scale is being applied twice.**

### 15.3 Question 3: the car in the race

**High confidence: the shared mechanism is the rectangle-as-scissor, and only that one.** The per-primitive rectangle
is computed for every draw - textured or not, 2D or 3D, opaque or alpha-tested - so on the car it appears where two
triangles of the body meet, or where a small triangle's extreme vertex rounds the rectangle inward, which is a square
bite at a vertex exactly as on the rim.

**Confirmed not shared: alpha test** (the car body is opaque, `ATE` off), **the sprite path** (a car is not a sprite),
**dirty-rectangle texture updates** (only if GT4 renders a car texture into a target - worth one check for a reflection
pass), and **Z precision** (present on the car, but with a stitching signature, not a square). The write-back chunk
rectangle of fact 7 is a second instance of the same mechanism but applies only to batches that need a readback, so it
is excluded by checking whether the car's notch sits on a chunk boundary rather than on a primitive junction.

### 15.4 Question 4: the one-texel bright lines through the translucent dialog

**Confirmed: at 1x a one-texel line cannot survive as a hard line under bilinear magnification, and mipmapping is not
what decides it.** An integer magnification lands its samples at fractional texel offsets (at 6x: 1/12, 3/12, 5/12 of
a texel from the centre), so the peak weight on the line's own texel never exceeds about 0.58: the line becomes a
roughly 12-output-pixel ramp at about 58% of its contrast, but it stays continuous. Nearest instead gives a hard 6-pixel
block at full contrast, quantised to the texel grid.

The filter is decided by TEX1 alone: `MMAG` for magnification and, when `MXL == 0`, `MMIN` is ignored outright
(`GSVertexTrace::Update`: "MXL == 0 => MMIN ignored, tested it on ps2"; `GSRegs.h`, `IsMagLinear`/`IsMinLinear`). For an
FST draw the LOD is the guest's fixed constant divided by 16 (14.3), so the internal resolution never enters the choice.
Mipmapping can only *attenuate* such a line by selecting a coarser level; it can never make it hard.

**High confidence: the reference's thin, continuous line is therefore geometry re-rasterised at 6x, not a texture
texel.** A sub-pixel-wide line primitive covers about 0.6 native pixels; at 1x the GS coverage rule ("a pixel is shaded
only if the primitive crosses its centre") quantises that to whole native pixels and breaks it into dashes, while at 6x
the same primitive is about 3.6 output pixels wide and resolved on every output row - thinner than our magnified
1-native-pixel line and continuous. That is exactly the owner's comparison. Falsification: one dump of the draw that
produces the line - a texture texel must give a soft ramp in the reference, a separate draw is re-rasterised.

### 15.5 Question 5: is the reference's 6x menu a magnification of 1x content?

**Confirmed: no. There is no 2D path (11.1): the menu is rasterised at the internal scale, and the only 1x content is a
1x *source*.** What that explains:

- **Smooth map - High confidence.** The sampler's ramp is computed per output pixel from the 1024x1024 and 1024x256
  textures, so no one-native-pixel structure can exist in its 6x image (14.4); our lattice and our dashes are the
  signature of a 1x intermediate.
- **No notches - Confirmed.** There is no per-primitive rectangle to clip with (15.1) and no 1x quantisation to
  magnify, so neither the junction gap nor the squared corner can occur.
- **Sharp text - High confidence, with a limit.** The glyph quads are positioned and rasterised at 6x with alpha test
  and blend per output pixel, which removes the 1x intermediate; it does not add glyph detail, because the glyph atlas
  is still a magnified 1x source.
- **"Text not 100% sharp" - Confirmed, and it is section 3, not the upscale.** Our deinterlace runs per *native* row
  where the reference runs per *output* row, so a one-line difference between the two fields is a 6-pixel ghost or
  blur in ours and a half-block comb in the reference. The owner's guess is right; the variable is the row
  granularity, not the weave mode.

**Confirmed: the measurement itself cannot decide this question, and should not be used as proof of a magnification.**
On smooth content, "6x equals bilinear(1x)" is expected from a *correct* 6x re-render as well, because both are
bilinear samplings of the same texture; only the high-frequency features separate them - the one-native-pixel lattice,
the dashes, and the 1-level DC offset. Read the measurement as "our 6x image carries no more information than a
bilinear magnification of our 1x image", which is the symptom of a 1x intermediate, not the proof.

### 15.6 The four measurements that settle all five questions

1. Print the scissor rectangle and the per-side primitive bounds of the rim draw or draws on the wheel; the predicted
   loss is 0 or 1 native pixel per side, larger on the min side.
2. The same for the four border bars of the dialog: each corner should be the intersection of two contracted
   rectangles, and the notch should be symmetric in the sense that the border's outer edge loses a square at all four
   corners while the inner edge does not.
3. Whether the thin map line is a texel of the 1024x1024 background or its own draw (settles 15.4).
4. Whether the car's notch lies on a chunk boundary of the write-back path or on a primitive junction (settles 15.3).

---

## 16. Ring slivers on the GT Mode event icons at 6x (IN-392)

Answers of the TASK-026 team, behaviour only.

- **Depth (Confirmed).** The reference clamps the vertex Z (format maximum for flat-Z primitives, 0xFFFFFFFF otherwise),
  converts it to float times 2^-32 into the position's depth, so the rasteriser interpolates it screen-linearly, and
  writes it into a 32-bit float depth attachment for Z24 and Z32 alike. When Z varies across a primitive that writes
  depth, the fragment depth is floored to an integer times 2^-32. GEQUAL and GREATER are the native compare operations.
  No depth bias, no tolerance, no depth option at any scale, nothing GT4-specific; identical vertices in two passes give
  bit-identical depth and the later pass wins a tie.
- **GT4's half-pixel mode ("align to native", Confirmed).** One affine mapping, no snap: gpu = s * (P - 1/320) + s/2
  with P the native position (12.4 value minus the offset, divided by 16). The 1/320 bias is in native pixels. It
  applies to every draw at scale above 1 except texture-shuffle draws; texture coordinates, colour, fog and Z are
  untouched. Points and lines keep their native width at scale. Side rules: region clamp of a texture sampled from a
  scaled target uses (1 + 0.1 s) / 0.1 texel offsets; a reduction to 1x clamps away the first s rows and columns.
- **Nothing hides slivers (Confirmed).** No conservative rasterisation, no native-resolution depth pass (the
  native-scaling option is off and GT4 does not set it), AA1 edge expansion off by default.
- **Measured on our side:** the slivers are in the mesh itself (exact simulation, IN-392), so they are expected in the
  reference too; to be confirmed with a black-box gt4-gs.exe frame at 6x.

## 17. Fractional internal scale up to 12x, and the texture replacement identity

Answers of the TASK-026 team, behaviour only. All Confirmed unless labelled.

- **One float factor s**, the same on both axes; no integer assumption anywhere.
- **Target size:** ceil(unscaled x s) per axis; a resize keeps ceil(valid x s) of old content.
- **Native rectangle to scaled pixels:** uploads draw the native rectangle times s as an unrounded float quad and let
  pixel-centre coverage decide (adjacent rectangles share the edge k x s, no gap and no overlap): output pixel j belongs
  to native pixel floor((j + 0.5) / s). Readbacks stretch nearest: native N reads scaled pixel floor((N + 0.5) x s). A
  texture read from a target samples the target directly with normalised coordinates times a reduction factor (2^TW
  over the target's unscaled width); a copy, when needed, is min(ceil(unscaled x s), target size). An 8-bit index read
  of a target is reduced to 1x first. For integer s these rules equal the s x s block rule.
- **Vertices at fractional s:** the viewport is the whole scaled surface, so the effective factor per axis is
  ceil(W x s) / W; gpu = s_eff (P - 1/320) + s_eff / 2; nothing is rounded except the GPU's sub-pixel snap.
- **Scissor:** the native inclusive scissor made exclusive, times s as floats, truncated, clipped to the surface, and
  intersected with the draw's bounding box (vertex extremes +- 1 native pixel, times s). There is **no +s/2 term**: in
  the align-to-native mode native pixel N owns [sN, sN + s). Our record_scaled_draws adds h = s/2 (and the tone carry's
  "drawn" pixel uses the same shift), which the team flags as half a native pixel off (High confidence for ours).
- **Sampling a scaled target:** normalised coordinates times the reduction factor; point = floor(u x scaled width),
  linear = hardware bilinear (four taps at half a scaled texel for 24-bit targets). REPEAT/CLAMP in hardware;
  REGION_CLAMP in normalised space; REGION_REPEAT in integer native texels, then back. The scale never enters the wrap.
- **Present:** each circuit's native rectangle times the float scale is stretched into the merge, deinterlaced in
  output rows, presented with box steps plus one bilinear blit. Dither indexes the output coordinate by default.
- **Limits:** max factor = max(1, floor(max image dimension / 1280)) (16384 -> 12), clamped with a warning. The
  reference's UI lists whole numbers; fractional values work when configured. Non-1x special cases are all binary (1x
  or not); nothing is disabled for fractional values.
- **Texture replacement identity (pack compatibility):** XXH3-64 seed 0 over the base level (and further mip levels
  into the same state when mipmapping is active). The raw path hashes 256-byte blocks in block-raster order unless the
  texture is smaller than a block, the format's bit mask is not all 32 bits, an X region exists or the Y region starts
  above 0; otherwise the expanded path hashes rows of one-byte indices (palette formats) or 32-bit RGBA with TEXA, with
  the byte-offset quirk for non-palette formats. CLUT hash: XXH3-64 seed 0 over the effective palette (16 or 256 32-bit
  entries; CSA applied, CSM1 unswizzled, 16-bit expanded with TEXA), 0 for non-palette formats. Bits word: PSM 0-5,
  TW 6-9, TH 10-13, bit 14 = 0, TA0 15-22, AEM 23, TA1 24-31 (TEXA only for 24/16-bit non-palette formats). File names
  in lowercase hex without zero padding for the hashes: `<tex>-<bits:08x>`, `<tex>-<clut>-<bits:08x>`,
  `<tex>-r<W>x<H>-<bits:08x>`, `<tex>-<clut>-r<W>x<H>-<bits:08x>`, plus the old `-r<64-bit hex>-` form, `.png` or
  `.dds`; dumps of mip k add `-mip<k>`. Folders: `<textures>/<SERIAL>/replacements/` recursively plus
  `<textures>/<SERIAL>.zip` (by file name), dumps in `<textures>/<SERIAL>/dumps/`, serial SCUS-97328 for GT4. Our
  `native/hw_gs/replace/` already follows this contract.

## 18. Automatic and motion-adaptive deinterlace (2026-10-27, TASK-026 team, prose)

- Automatic picks the motion-adaptive mode when FFMD is set, when exactly one circuit changed its DBY since the previous VBlank, or under the scan-mask rule; otherwise it does not deinterlace. Our resolve_auto_deinterlace matches.
- At an internal scale every algorithm works on output rows of the merged image. With FFMD set, the field offset of one native row (scale output rows) is applied in the merge, before the deinterlace.
- The motion-adaptive history keeps the last four fields: the current one, one back (opposite parity), two back (same parity) and three back (opposite parity). Each field is stored on its own rows only.
- For a row the current field does not hold, the reference compares:
  - the current field against two back on the rows above and below;
  - one back against three back on the row itself.
  Any channel above 0.08 counts as motion.
- **Static:** the row of one back (woven).
- **Moving:** the average of the current rows above and below, when those two agree within the threshold and the woven row differs from them at least 0.9 times as much; otherwise the woven row.
- The top and bottom rows are always woven.
- Our earlier rule never tested whether the woven field itself moved (IN-396).

## 19. Rendering at scale only: readbacks, textures from targets, uploads (2026-10-27, TASK-026 team, prose)

**No 1x copy.**
- The reference keeps no 1x copy. A render target is authoritative at its scale.

**When it writes GS memory.** Only on these triggers:
- **Local-to-host transfer.** Every overlapping target, depth first. Only the area drawn since that target's last
  read, intersected with the transfer rectangle (whole pages when formats differ). Skipped when the EE uploaded the
  same rectangle since the last draw.
- **Local-to-local move the GPU cannot do.** The move is first tried target to target at scale.
- **Texture formats the GPU cannot convert:** a 4-bit read of a target, or 8H from a 16-bit target. These are read
  back and decoded on the CPU.
- **Reset or savestate.**

Not triggers: end of frame, display, target ageing or deletion, an EE upload, or a CLUT load from drawn memory (the
CLUT is read from memory as it is).

**How a scaled pixel becomes a word.**
- Native pixel N takes scaled pixel floor((N + 0.5) * s): one sub-pixel, nearest, no average.
- Depth uses the same point sample with a raw 32/16-bit conversion.
- A read clears only the drawn-since-read rectangle. It never dirties or reloads the target.

**Textures from targets.**
- 24/32-bit with each other, or 16 with 16, at the same base: sampled at scale directly, no copy.
- A 16-bit read of a 32-bit target: a GPU texture shuffle at scale.
- T8 from a 32/16-bit target: a GPU point reduction to 1x, then the palette.
- Depth: sampled at scale through conversion shaders.
- A refused target is decoded from memory as it is, with no forced readback.

**EE uploads.**
- Same base, format and width: a dirty rectangle on the target.
- Another base or format: dirty pages.
- The target is deleted when its whole valid area is dirty.
- Dirty data is stretched in on next use: nearest for partial patches, bilinear for depth or a preload.
- A new target is preloaded only where recent EE uploads land. Older overlapping targets are copied in on the GPU at
  scale.

**GT4.**
- **Race colour filter.** A per-game hook replaces the T8 sequence with one at-scale draw: the scaled frame through
  the current palette, RGB channel fetch, RGB mask, in place. The alpha variant splits into three page-offset targets.
  Nothing happens at 1x and memory is not touched.
- **Flares.** Auto-flush 1 draws sampling sprites primitive by primitive, at scale. A transfer to the EE, if any, sees
  point-sampled values.
- **CT32 to CT16 display conversion.** A normal draw at scale.
- **Depth as texture.** At scale.

**Consequence.** With scale-only rendering a target must never be refreshed from words we downloaded ourselves. Only
real EE uploads enter a target.
