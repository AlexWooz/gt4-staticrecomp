# CoreGS 3.0: the 2D overlay split (TASK-090 phase 4)

Status: **implemented (2026-11-11)**, with one measured open item (the one-frame phase between the split base and the
faithful compose, §8.4). The live world (`docs/COREGS3-HOOKS.md` §31) draws the captured course over the CoreGS 2.0
composed image; the base's 2D/HUD layer is composed into that same image, so the world covered the HUD where its
geometry projects. This document fixes the seam, the measured constraints and the work items for drawing the world
(and cars) first and the 2D overlay after it, at the guest rate, without touching the faithful modes.

## 1. What exists (C)

- **The 2D boundary is already in the capture.** `scripts/coregs3_units.py` patches `RacePS2Base__render`'s HUD panel
  method (+0x358, guest PC `0x0033DA84`) with `pass_begin(c, 15u)` / `pass_end` (`kPassPanel`). The hooks forward the
  marker into the GS stream in stream order (`vram::note_pass_begin`), and the GS worker sees it at its exact stream
  position (`SinkRecord::PassMark` -> `Backend::coregs3_snapshot`). No new capture-side marker is needed to know where
  the panel pass begins; the `.gt4scene` journal carries the same entry.
- **The 2D content is measured.** The HUD S3 round (speed plate and digit atlas) and the HUD A-Spec round (normal-race
  panel census, tachometer, minimap, font atlas in progress) name the elements, their textures and their stream
  positions; `native/coregs3/coregs3_hud.*` already rasterises the S3 element offline.
- **The compose is one image per frame.** `Renderer::ComposeFrameGpu` (`native/hw_gs/core2/core2_renderer.cpp`) builds
  the single presentation image from the GS targets when the backend asks for it at VBlank. There is no mid-stream
  compose checkpoint and no second image for the 2D layer.
- **The present draws into that image.** `GT4_COREGS3=present` draws the world and the cars over the composed image
  (`native/hw_gs/coregs3/hw_coregs3_present.*`), saving it as the base for the host clock's later ticks.

## 2. Why the present layer alone cannot split (C)

- There is no 3D-only image to draw over: the composed image already contains the 2D layer, and the present has no
  image of the frame before the 2D pass.
- Diffing two raw GS targets or the raw target against the composed image does not give a composited overlay: the
  compose changes scale, format (CT16) and dither, so the 2D pixels must be composed by the same path that produced
  the image the present draws into.
- A screen-space scissor around the measured HUD rectangles is rejected: it hard-codes the HUD layout, breaks on
  aspect (TASK-214), other modes and effects, and is exactly the "wrong surface silently" class the plan forbids.
- Replaying the 2D natively from the live stream (the S3 HUD path, `docs/COREGS3-GSSTREAM-CAPTURE.md` S3/S4) is the
  long-term direction (it also makes the HUD aspect-aware without CoreGS 2.0), but it needs a live in-memory
  PATH2/PATH3 stream and a live 2D rasteriser; it is not the first split.

## 3. The CoreGS 2.0 change (the blocker)

At the **first 2D draw** of the frame (measured from the panel marker; see §4):

1. flush the recorded 3D work and **compose a checkpoint** `frame_3d` through the existing presentation compose (same
   size, format, scale and display settings as the frame's final image), and deliver it to the backend as the frame's
   3D image;
2. **redirect the subsequent GS writes to a cleared transparent overlay target** (RGBA8 with alpha, the same display
   rectangle and sampling rules), leaving the 3D target readable for any 2D draw that samples it;
3. at VBlank, **compose the overlay** `frame_2d` through the same path (alpha preserved) and deliver it next to
   `frame_3d`.

The guest path is unchanged: the GS stream draws the same commands; the split is a host-side target choice. A 2D draw
that reads a framebuffer texel must read the 3D target (or the split is refused for that frame and counted); the
measured HUD census has not found one in the panel, but this is the risk class to pin with the stream census and a G-TAP
check before the change is accepted. The compose checkpoint runs on the GS worker at the marker, where the renderer's
targets are current; it must be bounded (one checkpoint per frame) and must not change CoreGS 2.0's coherence or
readback rules for the faithful path (the split is active only when the CoreGS 3.0 present asks for it).

## 4. Measured constraints to settle before cutting (H)

- **Stream order of the panel marker versus the post chain.** The S2/S3 rounds measured the frame's full-screen chain
  (pre-world clears, wet composite, FIX 127/255 multiplies, the CT16 present at stream seq 1094) and the HUD sprites at
  seq 1102/1103 in the same stream, while the panel marker is an EE-side pass marker. The cut point must be verified
  with `gt4_coregs3_gs --census` on the existing night capture: if a post-chain batch follows the marker, it belongs to
  the 3D image (the 2D split must not move the display compression into the overlay); if a 2D batch precedes it, the
  marker is too late. The split is placed at the first 2D batch after the 3D content, not blindly at the marker.
- **V -> V-1 phase.** The stream stamped V carries the display list built in scene frame V-1 (S2). The checkpoint and
  the overlay must belong to the same scene frame as the composed image the present draws over.
- **Host clock.** The overlay is composed once per guest frame and reused by every presentation tick; the present saves
  `frame_3d` as its base and restores it before each tick, then draws the world/cars and composites `frame_2d`, so the
  host-clock ticks and the 60 Hz guest HUD coexist (plan §5.5).

## 5. Present-layer work after the CoreGS 2.0 piece (C once unblocked)

- Per tick: start from `frame_3d`; draw the world and the cars (unchanged); composite `frame_2d` over the result with a
  full-screen fragment pass (alpha) before the existing display hand-off. The present already writes the composed image
  with a storage-image fragment stage, so the composite is one more pass, not a new renderer.
- Counters: overlay pixels composed, overlay refused (no image / mismatched size), and the world mask now measured
  outside the overlay.
- Switches: `GT4_COREGS3_2D_OVERLAY=0` keeps today's path (world over the full composed image) as the A/B.
- Faithful modes (`off`/`shadow`, GSdx, CoreGS 2.0 default) are untouched: the split exists only in the debug
  `GT4_COREGS3=present` mode.

## 6. Gates for the implementation round

- guest identity: off/shadow 3x gate unchanged (`COREGS3-NIGHT-CHECK-OK`); the present world-on run with the overlay has
  0 clock/digest/state differences, from a verified clean rebuild (the round checks the object/exe timestamps, not only
  the CTest result);
- a present image at 3x with the world and the A-Spec/normal HUD visible (panel, digits, tachometer) and the counters of
  the overlay pass; the HUD at the guest rate under the host clock;
- A/B with `GT4_COREGS3_2D_OVERLAY=0`: the current world-over-base image, byte-identical from the same claimed frame;
- the faithful renderers unchanged (their own gates).

## 7. Decision

The split needs the CoreGS 2.0 UI-layer change (§3): a compose checkpoint at the stream's 2D boundary and a transparent
overlay target delivered with the frame. It was **not implemented in the live-world round**; this design, the stream
census in §4 and the CoreGS 2.0 work item were the deliverables before the present composite. It is implemented in §8
(branch `task/090-2d-split`).

## 8. Implementation (2026-11-11, branch `task/090-2d-split`)

### 8.1 The cut is the stream's present blit, not the panel marker (C)

The A-Spec capture (VBlanks 7600-7607) settles §4's ordering: the EE's panel marker (`kPassPanel`, guest PC
`0x0033DA84`) is written **after** the HUD draws in the stream (global records 1405-1406 and the HUD's PATH2 batches
1304-1381 of the same frame), so the marker is too late. The display registers cannot name the target either: during the
frame DISPFB still points at the buffer being scanned out, and the game writes the frame's own buffer only at the end of
the frame (the flip the VBlank compose relies on; the gated runs print the one-frame relation once). The cut is
therefore the measured present blit itself: `gt4_coregs3_gs --census` classifies it as `full-screen-present-blit` (seq
2007, a full-screen unblended draw into the CT16 display buffer that samples the CT32 scene buffer). The rule
(`native/hw_gs/core2/core2_2d_split.{hpp,cpp}`) arms on that draw and cuts at the next batch into the same target; it has
no dependency on the guest's display registers.

### 8.2 CoreGS 2.0: checkpoint and overlay target (C)

At the first 2D batch, `core2_renderer.cpp` composes the 3D image as the checkpoint through the merge kernel
(`core2_compose.comp`: the present target's rectangle over its 1:1 placement at the internal scale, one circuit, then the
same present resampling) into the display image, and creates a transparent overlay target with the present target's view
and size. Every later batch into that view draws into the overlay target through the storage path: the fragment stage's
split branch (`hw_pixel_core.glsl`, `hw_pixel_uber.frag`, the new `GpuPixelState::SplitOverlay` field) writes an RGBA8
word whose RGB is the blended colour with the destination read as the overlay's own pixels and whose alpha is the
*accumulated coverage* (the blend factor over the previous coverage; 255 for an unblended or FBA write). The ROP and
compute-fill paths stay off for these draws. At VBlank `core2_overlay_compose.comp` composes the overlay through the same
circuit geometry into a second display image with the coverage in the alpha lane, and `GpuFrame` carries both images
(`image` = the checkpoint, `overlay` = the 2D layer, same size). A batch in the 2D phase that samples the split
framebuffer is drawn into the 3D target and counted (`RefuseSample`); the overlay target is dropped without a readback at
the VBlank. The split is active only in `GT4_COREGS3=present` (`Backend::SetCoregs3Overlay`); `GT4_COREGS3_2D_OVERLAY=0`
keeps the previous path as the A/B, and the faithful modes never set it.

### 8.3 The present composite (C)

`GT4_COREGS3=present` draws the world and the cars into the checkpoint image exactly as before (the present layer is
unchanged), then `CompositeCoregs3Overlay` composites the 2D layer over it premultiplied source-over
(`core2_overlay_merge.comp`: out = layer.rgb + image.rgb * (255 - coverage) / 255) before the presenter or the dump reads
the image. Every host-clock tick runs the same composite after the present layer restores the checkpoint as its base, so
the HUD stays at the guest rate under a variable presentation rate.

### 8.4 Measured (C) and the open phase item (H)

A-Spec night race, 3x, VBlanks 7600-7608, live world on, final executable SHA-256 `08daf60b...`: **1,564 frames split**
(42.5 batches per frame), 1,564 checkpoints, **9 overlay composes and 9 composites (23.22 Mpx)**, `refused_samples=0`,
`refused_frames=0`, `unavailable_draws=0`, `layout_changes=0`, and `approximated date=0 fbmsk=0 dst_alpha=0`; **1** frame
was dropped because its VBlank compose never ran (a held present), counted as `dropped_frames=1`. The overlay layer of
frame 7600 covers 301,208 of 2,580,480 pixels (11.67%). The split frame shows the world (the native course, cars, sky and
buildings) with the whole A-Spec HUD over it: position/lap, the lap timer, Best/Last Lap, the minimap, FULL THROTTLE,
both gauges with their needles, gear, digital speed, the trip odometer, the fuel/tyre widget, the G-force dot and the gear
indicator; the A/B frame shows the world covering that HUD. The HUD regions differ from the overlay-off image by 52,856
px (left), 79,822 px (right) and 192,498 px (bottom) at frame 7600.

The open item: at the VBlank the GS scanout register still names the *other* CT16 buffer (the game flips DISPFB after the
VBlank), so the faithful compose would show stream frame V-1 while the split base is stream frame V. The split image is
the frame's own, newer one; the world the present draws is the claimed capture frame, which follows the same stream
frame. Aligning the base with the faithful compose's one-frame latency needs the pair ping-ponged across two VBlanks
(compose frame V's overlay, deliver frame V-1's pair). Measured, not yet implemented; no gate depends on it. The HUD stays
at the guest rate and every tick reuses the composed overlay; HUD anchoring per aspect (TASK-214) and the VRR HUD
(`docs/COREGS3-PLAN.md` §5.5) remain.

