# CoreGS 2.0 core rewrite: target-cache specification (CORE2)

Status: specification, 2026-10-28. Owner decision: rewrite the CoreGS 2.0 core on the reference's model (ai-memory
`decisions/coregs2-scale-only-rendering`). Source: the TASK-026 specification team, Q-GSDX-COMPARISON 21, in prose
(clean room: no reference code, identifiers or line transcriptions). It describes the reference with default settings
plus GT4's per-game entry (half-pixel mode 4, auto-flush 1, blending level 3, the channel-shuffle hook). Claims are
Confirmed from the reference unless labelled otherwise.

Why: candidate 282 (dual 1x + scaled design) shows the 480p race at 1x in every frame at 3x (headless El Capitan,
frames 9800-10400, compose from a display shadow in 4 of 4 dumps). The fix ranked first by the comparison (IN-401,
clear `scaledStale` after the tone carry) changed neither the picture nor any counter. The dual design rebuilds scaled
images from 1x words on too many paths; the new core keeps one image per target at scale.

Scope: the new core replaces `native/hw_gs/writeback/hw_gs_writeback.cpp` and `hw_gs_writeback_gpu.cpp` (slots,
Load/Drop, GLM parking, tone runs, carries, display shadows). It keeps the frontend, memory/swizzle, decide, pixel,
clut, draw shaders, display/deinterlace and Vulkan setup. The old core stays selectable until the new one is better.

Terms:
- "Native" = GS pixels. "Scaled" = output pixels.
- `s` = the internal scale, one float for both axes.
- "Block" = a 256-byte GS block. "Page" = 32 blocks.

## 1. Target model

1.1 **What a target is.**
- A GPU image that is the content of a region of GS memory at scale.
- No 1x copy is kept. GS memory holds target content only after an explicit readback (section 4).

1.2 **A target's record.**
- **Key:** base block, buffer width (in 64-pixel units), format, kind (colour or depth).
- **Rectangles:**
  - the valid rectangle (native);
  - a dirty list (native rectangles newer in GS memory);
  - the drawn-since-last-read rectangle.
- **Bookkeeping:** the last draw number and an age in frames.
- **Flags:** RGB valid, alpha valid, alpha written.

1.3 **Format classes.**
- **32-bit and 24-bit colour are one class.**
  - A 24-bit view is the same target with alpha not valid.
  - A 32-bit target whose alpha was never written counts as 24-bit for compatibility tests.
- **16-bit (16 and 16S) is its own class.**
- **Depth:** Z32 and Z24 form one class; Z16 and Z16S form another.
- **Colour and depth at the same base:**
  - a colour target and a depth target may both exist there;
  - a draw may convert one into the other on the GPU when the bits per pixel match (2.7).

1.4 **Size.**
- **Surface:** native size w × h gives a surface of ceil(w × s) × ceil(h × s).
- **Width and height:** the width is the buffer width × 64. The height is the extent drawn or read, starting small and
  growing.
- **Growth:** the target grows when a draw, texture read or display needs more. The old content is copied across on
  the GPU at scale.
- **Ceilings:** 1280 × 1280 native. The scale ceiling is floor(max image dimension ÷ 1280).

1.5 **Scale.**
- Every target uses `s`, which may be fractional.
- Exceptions: an 8-bit index read reduces to 1x (3.5), and readback temporaries are 1x.

1.6 **Depth storage.**
- **Format:** a 32-bit float attachment holding Z × 2^-32.
- **Clamp to the format maximum** (Z24 0xFFFFFF, Z16 0xFFFF):
  - at the vertex for flat-Z draws;
  - in the fragment for depth-writing draws that exceed the maximum.
- **Flooring:** the fragment Z is floored to an integer when Z varies across the primitive and the draw writes depth
  (or tests GREATER).
- **Compare:** the hardware GEQUAL or GREATER.
- **Self-sampling:** a draw that also samples its own depth tests in the shader against a copy (or with barriers).

## 2. Draw-target lookup (frame and Z of each draw)

2.1 **Exact match.**
- A target of the same kind at the same base is used. Among several, the most recently drawn one wins.
- If the width differs and the target is old (not drawn last frame), or the draw overwrites it without preserving,
  the old target is discarded and a new one is created.
- It is also discarded when its dirty rectangles cover the whole draw area.

2.2 **Compatible views.**
- A 24-bit draw into a 32-bit target, or the reverse, uses the same target.
- A 24-bit draw does not touch alpha. After a 32-bit draw, alpha is valid.

2.3 **Drawing inside an existing target.**
- **Same width:** when the base lies inside a target at a page-aligned offset with the same width (a double buffer's
  second half, a page-offset draw), the draw goes into that target at the translated rectangle.
  - A huge target with only a partial overlap may be split or shrunk.
- **Different width:** an overlapping older target of a different width is invalidated in the overlapped area.

2.4 **New target.** When nothing matches, a new target is created at the draw's size. Initialisation, in order:
- (a) **Older overlapping targets** are copied in on the GPU at scale, format-converted when needed, and their dirty
  rectangles move across. An overlapped target that was entirely dirty is skipped.
- (b) **Recent EE uploads.** Memory content is loaded only where EE uploads from the last 500 draws landed on its
  blocks (formats of 16 bits or more).
  - These areas become dirty rectangles with a bilinear request.
  - Loading the whole target from memory is off by default. The exception is a display target created from an
    uploaded frame (5.2).
- (c) **Everything else is undefined.** The draw is expected to cover it.
- (d) **Clears.** A clear detected from EE clear transfers, or a full-target clear draw, may just clear the surface.

2.5 **Depth lookup.** The same rules, on the depth list. When the RGB or alpha of a Z buffer is used as colour at
the same base, it gets the conversion of 2.7.

2.6 **After the draw.** The valid rectangle and the drawn-since-read rectangle grow to include the drawn rectangle.

2.7 **Colour-depth conversion.** A colour draw at the base of a depth target with the same bits per pixel, or the
reverse, converts the surface on the GPU at scale (24/32/16-bit packing), not through memory.

## 3. Texture lookup (in this order)

3.1 **Cached memory source.** A cached memory source whose key and data are still valid (3.7) is used first.

3.2 **Render-target hit.**
- **Base:** the texture base must equal a live target's base. A texture inside a target is not a hit by default.
- **Compatible formats:**
  - a 32 or 24-bit read of a 32/24 target;
  - a 16-bit read of a 16-bit target;
  - a different width only if the pages translate and the area is not dirty.
- **Valid data:**
  - the target must hold valid data for the channels read;
  - the requested area must overlap its valid rectangle, else a temporary memory decode is made.
- **The source is the target's scaled surface:** shared, no copy, normalised coordinates. The reduction factor
  2^TW ÷ the target's native width maps the texture onto it.
- **Dirty rectangles** that overlap the requested area are applied first (4.1).
- **Read past the valid size:** the target may grow, and the missing area is loaded from memory as dirty rectangles.

3.3 **Sub-rectangle or offset.**
- Region-clamp coordinates select a sub-rectangle of the same surface.
- An offset texture base inside a target is not a hit by default: it decodes from memory.

3.4 **16-bit read of a 32-bit target.** Only as a recognised channel-shuffle draw, done in the shader at scale.
Otherwise it is refused and decoded from memory.

3.5 **Paletted reads.**
- **T8 from a 32-bit or 16-bit target:** the index channel is extracted on the GPU into a 1x temporary, point-reduced,
  then looked up in the palette. The result is 1x.
- **T4, or 8H from a 16-bit target:** the target's drawn-since-read area is read back to memory, then decoded on the
  CPU at 1x.
- **4HL, 4HH or 8H against 24-bit:** no shared bits, so not a hit.

3.6 **Depth sampled as colour.**
- Sampled directly at scale. A shader converts float depth back to the integer, then extracts 32/24/16-bit channels
  or indexes the palette with depth bits.
- A draw that also writes that depth uses feedback (1.6).

3.7 **Memory decode (no target).**
- **Decoding:**
  - decoded from GS memory at its own native size, at scale 1, with the palette applied (or indices kept for GPU
    palette mode); the sampler magnifies it;
  - a refused target does NOT trigger a readback first, so memory may be stale.
- **Caching:**
  - sources are indexed by the pages they cover;
  - a GS memory write into a page (EE upload or readback) invalidates the overlapping parts, and the next use
    re-decodes only the invalid blocks or pages;
  - unused sources are freed after a few frames;
  - a source made from a target is removed when that target is drawn to or invalidated.
- **Palettes** are cached by content (16 or 256 entries).

3.8 **Sampling.**
- The guest's own filter (TEX1). Mip levels only under the three conditions of Q-GSDX-COMPARISON section 14.
- Region clamp and repeat are done in the shader; hardware repeat and clamp otherwise.
- A 24-bit target sampled with linear uses a four-tap filter at half a scaled texel.

## 4. Memory coherence

4.1 **EE upload (host to local).** For each target overlapping the destination:
- **Same base, compatible bits, same width:**
  - add a dirty rectangle of the upload area;
  - delete the target if the rectangle now covers its whole valid area.
- **Otherwise overlapping:** mark the overlapped pages dirty, and delete the target if it becomes fully dirty.
- **Memory sources** over those pages are invalidated.
- **Applying dirty rectangles:** lazily, at the target's next use (draw target, texture or display):
  - read the rectangle from GS memory, decoded to RGBA (or depth);
  - stretch it into the surface over the float rectangle native × s;
  - write only the dirty channels;
  - filter: nearest by default; bilinear for depth, for preload requests (2.4b) and under a forced option.

4.2 **Host readback (local to host).**
- **Area,** for every overlapping colour and depth target:
  - the drawn-since-read area ∩ the source rectangle in target space (whole pages when formats differ);
  - all of the drawn-since-read area on the second readback since a draw or for a full flush.
- **Reduction:**
  - native rectangle × s, stretched nearest into a 1x temporary, then written to GS memory in the target's format;
  - native pixel N takes scaled pixel floor((N + 0.5) × s).
- **Afterwards:** the drawn-since-read area is cleared. The target is not modified and not re-uploaded.
- **Skip:** if the EE uploaded exactly that rectangle since the last draw, the readback is skipped and the target
  invalidated.

4.3 **Local-to-local move.**
- **Compatible targets cover source and destination:** done on the GPU, target to target at scale (with translation).
- **Otherwise:**
  - read back the source rectangle (a full flush if source and destination are the same buffer);
  - do the move in memory;
  - invalidate the destination as an upload.

4.4 **Other writes to memory.**
- The only others are:
  - the T4/8H texture case (3.5);
  - reset or savestate (every target's drawn-since-read area);
  - optional CPU paths, off by default.
- Never at end of frame, display, eviction or ageing.

4.5 **CLUT loads** read GS memory as it is. No readback for a CLUT, and the target-CLUT option is off by default.

## 5. Display

5.1 **Finding the target.** For each enabled circuit (base, width and format from DISPFB; size from the display
rectangle), in order:
- a colour target at exactly that base, the newest (an old, dirty target of the wrong width is deleted);
- a target that contains the base-plus-size region (a double buffer's second half);
- a target at that base with the same width but no valid data.
When found, the target grows to cover the display size and its dirty rectangles are applied. The read rectangle is
offset by the DBX/DBY page and row offset.

5.2 **No target found.**
- **A recent EE upload of that frame** (an exact upload at that base, or one that covers it, as in FMVs): a target is
  created and loaded from memory. With an exact match, its data is valid.
- **Otherwise** the circuit has no image. With no image at all, nothing new is presented and the previous frame
  stays.

5.3 **Merge.**
- Each circuit's native display rectangle × the target's scale is stretched into the merge image with linear
  filtering.
- Background colour, alpha blending (MMOD, ALP, AMOD) and SLBG are applied. The feedback write and the scan-mask
  offsets come from the registers.
- The FFMD field offset (scale × field rows) is added unless deinterlace is bob or off.
- Identical circuits are merged once.

5.4 **Deinterlace** works on the merged image in output rows (Q-GSDX-COMPARISON 18). A progressive (480p) frame has
none.

5.5 **Present.** Box halvings down to about half the destination size, then one bilinear blit. FXAA and post-filters
are options.

## 6. Eviction and ageing

6.1 **No target-count limit.**

6.2 **Ageing.**
- Every VSync, each target not used that frame ages by one.
- After 60 frames unused it is deleted, without readback.
- Sources age out faster.

6.3 **Other deletion:**
- full dirty coverage (4.1);
- being replaced on overlap or width change (2.1, 2.3);
- reset;
- memory-budget pressure.

6.4 **Never rebuilt from memory.** A target is never rebuilt from 1x memory. The only memory loads are:
- dirty-rectangle patches from real EE uploads;
- areas of a new target that recent EE uploads cover;
- a display target created from an uploaded frame.

## 7. GT4-specific handling

7.1 **Per-game settings:**
- half-pixel mode 4 (section 8);
- auto-flush 1: a sprite batch that samples the target it draws is split per primitive, so each primitive sees the
  previous one (the lens flare and the sun need it);
- blending accuracy 3;
- the channel-shuffle hook (7.2).

7.2 **The channel-shuffle hook** (race colour, brightness and fade filter).
- **Trigger:** a possible channel-shuffle draw:
  - T8-textured, sprite class;
  - the first sprite fits in one page (64 × 64 or smaller) and is exactly 8 wide or 2 tall, or uses region clamp; or
    it is a page-wide strip whose texture width is twice the frame width;
  - its texture base resolves to an existing colour target.
- **Frame mask not "RGB masked":**
  - skip this draw and every following shuffle draw with the same frame mask (CBP and TEX0 changes ignored), until a
    draw that is not a shuffle;
  - run ONE draw over that target's native rectangle at its scale instead. It reads the target, looks each channel up
    in the current palette (the ramp CLUT the game loaded, read as 32-bit), outputs R, G and B, and writes only RGB
    into the same target.
- **Frame mask = RGB masked (alpha only, some fades):**
  - create or find three CT32 targets, starting at block 0, at page offsets 0, 1 and 2 × (half the display height
    rounded to pages) × width;
  - extract red, green and blue as alpha into them, through the palette, at scale.
- **Result:** the temporary-page draws and the CT24 write-back never happen, and memory is not involved.

7.3 **Shadows, mirror and small effect targets.**
- Nothing specific: normal targets at full scale, sampled as textures under 3.2. They survive because there is no
  count limit.
- Shadow format and sampling: Unknown, to be measured with our draw trace.

7.4 **Display conversion** (the CT32 frame drawn into a CT16 buffer for display).
- A normal textured draw: the CT32 target is sampled at scale into the CT16 target at scale.
- Dither is applied per output pixel. The display reads the CT16 target.

## 8. Rasterisation conventions at scale (mode 4)

8.1 **Vertex position.**
- The native position is P = (X − OFX) / 16.
- The output position is s_eff × (P − 1/320) + s_eff / 2, with s_eff = surface width ÷ native width per axis.
- No snapping. W = 1. Z and colour are screen-linear; ST/Q is perspective.

8.2 **Ownership.**
- The pixel centre is at j + 0.5.
- Output pixel j samples native (j + 0.5)/s − 0.5, so the block [s·N, s·N + s) belongs to native pixel N.
- Upload and readback rectangles follow the same mapping.

8.3 **Scissor.**
- The GS scissor (x0, y0, x1 + 1, y1 + 1) × s, as floats, truncated and intersected with the surface.
- The draw area is also limited to the vertex bounding box ± 1 native pixel × s. It never clips geometry otherwise.

8.4 **Sprites, points and lines.**
- Sprites are two triangles with linearly interpolated UVs. FST UVs are U/16 ÷ 2^TW, normalised.
- At s ≠ 1, points and lines keep their native size: a point is s × s, a line s wide.
- Sprite rounding and alignment options are off.

8.5 **Texels.**
- Hardware texel centres on the source's own grid: scaled for target sources, 1x for memory sources.
- Linear filtering only when the guest asks for it.

8.6 **Dither.**
- Indexed by the output pixel by default; the "scaled" option uses floor(output ÷ s).
- 16-bit targets only.

8.7 **Filters.**
- 1x into scaled: preload bilinear; dirty-rectangle patch nearest (depth bilinear).
- Scaled to 1x: nearest at native centres.
- 8-bit index reduction: point.

## 9. Order of implementation

**Step 1: a sharp 480p race at 3x.**
- **Targets:** colour plus D32F depth, no count limit, the 32/24 class, valid / dirty / drawn-since-read rectangles,
  growth.
- **Draw lookup** by exact base and compatible view (2.1, 2.2). New targets are initialised as in 2.4 (a) and (b).
- **Rasterisation:** the vertex mapping and scissor of section 8.
- **Textures:** exact-base target hit sampled directly (3.1-3.2), and memory decode with page invalidation (3.7).
- **Memory:** EE uploads as dirty rectangles (4.1), and host readback (4.2) for what the game reads back (the flare).
- **GT4:** the channel-shuffle hook (7.2) and auto-flush for self-sampling sprite batches.
- **Display:** lookup (5.1, 5.2), merge, 480p present.
- **Acceptance:** no 1x pixels in the race, with the CT16 dither and the display conversion (7.4) correct.

**Step 2: menus at 480i.**
- The FFMD field offset, and Automatic and Adaptive deinterlace in output rows.
- Inside-target draws for double buffers (2.3) and the contained-display lookup (5.1).
- T8 and T4 menu textures from memory, and the palette cache.

**Step 3: the rest.**
- T8-from-target reduction (3.5) and the 16-from-32 shuffle (3.4).
- Depth as a texture and colour-depth conversion (2.7, 3.6).
- GPU local-to-local moves (4.3).
- The alpha-only shuffle variant (7.2).
- Fractional scales, texture replacement, and ageing / memory budget.

## 7.2 addendum: exact hook semantics (Q-GSDX-COMPARISON 22)

- **Palette.**
  - Captured once, at the first shuffle draw of a run: the GS CLUT buffer as that draw's own TEX0 write (CLD) left
    it. It is not re-read from GS memory.
  - It is read as 256 entries of 32 bits, through that draw's CPSM and CSA; a 16-bit palette is expanded with the
    current TEXA. The index is the channel value.
- **CLUT loads inside a run** still happen, so the CLUT buffer keeps the last load for later normal draws. The
  replacement draw ignores them.
- **Mapping.** R' = entry[R].red, G' = entry[G].green, B' = entry[B].blue. Alpha is kept.
- **Destination.** The colour target that the first draw's texture base resolves to, written in place.
  - It covers the target's whole native rectangle, at its scale.
  - The write mask is always RGB only. The frame mask is not applied.
  - If the texture base has no colour target, the hook does nothing and the draw runs normally.
- **Run.**
  - **Continues** while each draw is a possible shuffle with the same FBMSK as the first. Those draws are skipped.
  - **Ends** at the first draw that is not a possible shuffle; that draw is drawn.
  - **New run:** a possible shuffle with another FBMSK starts one, with its own palette and target.
  - **No effect:** CLUT changes, CBP changes and VBlanks do not end a run.
  - **GT4's copy-back strips** are possible shuffles. They are skipped when their FBMSK matches (High confidence;
    to be checked with a FBMSK log).
- **Detection.** TME, PSM T8, sprite class. The first sprite's size is |X1 − X0| >> 4 and |Y1 − Y0| >> 4 from the
  first two vertices' 12.4 coordinates.
  - **Rejected:** a batch of at most 2 vertices whose box is at most 8 × 8.
  - **Single page:** the batch's vertex box is at most 64 × 64, and the first sprite is 8 wide or 2 tall, or WMS or
    WMT is a region mode.
  - **Strip:** the box is at most 64 wide and taller than 64. It needs TBW == 2 × FBW, a page-aligned box in the
    frame's format, and the same 8-wide / 2-tall / region test.
