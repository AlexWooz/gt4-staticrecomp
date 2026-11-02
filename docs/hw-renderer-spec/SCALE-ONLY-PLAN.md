# CoreGS 2.0 scale-only rendering plan (TASK-027)

Owner decision, 2026-10-27 (ai-memory `decisions/coregs2-scale-only-rendering`). It supersedes PARITY-PLAN item 5
("skipping the 1x redraw: rejected", 2026-10-23).

- Draw at scale only by default, as the reference does.
- Keep exact 1x words only for the memory the game actually reads back, detected automatically.
- Provide an A/B switch to the current dual 1x + scaled mode. The new mode becomes the default only after owner tests.
- Game logic never changes: physics, timing, AI and saves stay exact.

The reference behaviour below is from the TASK-026 team, in prose (Q-GSDX-COMPARISON 19). This plan is written clean
room from that prose and from our own code.

## 1. Why

- **Blocky patches.** Our scaled images are rebuilt from 1x words: about 72,000 refreshes per 10,000 race frames,
  several per frame covering the whole 640 x 448 target. Each rebuild shows a 1x patch inside the high-resolution
  picture. This is the owner's blocky trees and grass in the video of 2026-10-27, and the IN-397 races at 1x.
- **Memory-built effects.** The race colour filter is built at 1x and carried over to the scaled image by a learned
  table (IN-379), not drawn at scale.
- **Cost.** At 6x the 1x work is about 7 ms of GPU per race frame: pass, decode, CLUT, swizzle and upload. The scaled
  pass is 8.5 ms. The scale-only path removes most of the first. The second needs item 1 of PARITY-PLAN (real
  attachments, early depth), which is folded in as stage S4.

## 2. Reference behaviour (summary of Q-GSDX-COMPARISON 19)

- **No 1x copy.** A target is authoritative at scale.
- **Memory is written only on a few triggers:**
  - a local-to-host transfer overlapping a target;
  - a local-to-local move the GPU cannot do;
  - a texture format the GPU cannot convert (4-bit, or 8H from a 16-bit target);
  - a reset or savestate.
  It reads only the area drawn since the last read. End of frame, display, ageing and EE uploads are NOT triggers.
- **Reduction.** Native pixel N takes scaled pixel floor((N + 0.5) * s), a point sample with no averaging. Depth uses
  the same point sample with a raw conversion. A read never dirties or reloads the target.
- **Textures from targets** are sampled at scale directly when the format is compatible:
  - 24/32-bit with each other, 16 with 16;
  - a 16-bit read of a 32-bit target as a GPU shuffle;
  - T8 indices from a 32/16-bit target by a point reduction to 1x plus a palette lookup;
  - depth through conversion shaders.
  A refused target is decoded from memory as it is, with no forced readback.
- **EE uploads** into a target add a dirty rectangle (or dirty pages). The target is deleted when the whole valid area
  is dirty. Dirty data is stretched into the target on next use: nearest for partial patches, bilinear for depth or
  a preload.
- **GT4 race colour filter.** A per-game hook replaces the whole T8 sequence with one draw at the source's scale: the
  scaled frame read through the current palette, RGB channel fetch, RGB mask, written in place. Nothing happens at 1x
  and nothing touches memory.
- **Display** reads targets directly. Moves, resizes and overlaps are GPU copies at scale.

## 3. The switch

`GT4_HW_RENDER_MODE=dual|scaled`:
- `dual` is today's behaviour, bit-exact 1x plus a scaled image.
- `scaled` is the new path. It grows stage by stage; each stage lands behind the switch with its own gates.
- At 1x both modes are the same renderer.

## 4. Stages

**S1: never re-upscale our own downloads (both modes keep the 1x pass).**
- A slot reloaded or re-viewed from memory we wrote ourselves keeps its scaled image. A refresh from 1x happens only
  where the EE really uploaded (dirty rectangles: nearest for partial patches, bilinear for depth).
- This is the visible fix for the blocky patches. The 1x words stay exact, so every memory gate still holds.
- Measure: refreshes per race frame, the owner's scenes, and `--check-presented`.

**S2: the race colour filter at scale.**
- The scaled image gets the reference's single at-scale draw: palette lookup of the scaled frame, RGB fetch, RGB mask.
  It replaces the IN-379 tone carry.
- The 1x tone sequence stays exact (IN-383 strips) while the 1x pass exists.

**S3: drop the 1x pass in `scaled` mode, with exact 1x only where the game reads back.**
- Track a drawn-since-read rectangle per target.
- On a reference trigger, do a point-sampled readback into the GLM/SLM.
- A memory region that the game has read back (a local-to-host transfer from a target) is marked read-back. That
  marking is sticky per block span. Draws touching marked regions are still recorded at 1x, so later readbacks there
  are exact; this is the owner's middle ground. The first readback of a region before it is marked is point-sampled.
- Textures from targets sample the scaled target as the reference does. T8 uses a point reduction plus the palette;
  4-bit uses a readback and decode.
- EE uploads become dirty rectangles.
- The display and compose read targets directly. The display shadows (IN-377) go away in this mode.

**S4: fast scaled raster (PARITY-PLAN item 1).**
- Colour and depth attachments where the state allows it, early depth, and fixed-function blend for the common cases.
  The exact uber shader is kept only for the GS cases the fixed pipeline cannot express.

## 5. Gates

- **`dual` mode:** unchanged. CTest in both interlock modes, the five memory hashes, menu/race/default dumps,
  `--check-presented`.
- **`scaled` mode, per stage:**
  - CTest;
  - the game's memory readbacks compared with `dual` on the captures. Exact for marked regions; point-sampled
    otherwise, with the difference reported;
  - visual checks on the owner's scenes (GT Mode menu, race start, trees at speed, night, rain, replays, photo);
  - GPU ms and power against `dual` and the reference.
- Never benchmark while builds run.

## 6. Status

- 2026-10-27: plan written.
- 2026-10-27: S1 first step, IN-398 (a reload keeps another view's scaled blocks): the stripes on the track are gone. Next: the remaining refreshes from 1x (EE uploads only).
