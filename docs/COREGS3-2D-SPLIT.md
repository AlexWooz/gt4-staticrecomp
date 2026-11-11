# CoreGS 3.0: the 2D overlay split (design, TASK-090 phase 4)

Status: **design only, blocked on a CoreGS 2.0 change**. The live world (`docs/COREGS3-HOOKS.md` §31) draws the
captured course over the CoreGS 2.0 composed image; the base's 2D/HUD layer is composed into that same image, so the
world covers the HUD where its geometry projects. This document fixes the seam, the measured constraints and the work
items for drawing the world (and cars) first and the 2D overlay after it, at the guest rate, without touching the
faithful modes.

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
overlay target delivered with the frame. It is **not implemented in the live-world round**; this design, the stream
census in §4 and the CoreGS 2.0 work item are the deliverables before the present composite.
