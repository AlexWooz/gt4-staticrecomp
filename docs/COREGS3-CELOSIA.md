# Celosia's native renderer, and what CoreGS 3.0 adopts from it

Date: 2026-11-09. Research note for TASK-090, written for the implementation agents. **Prose only.**

## 0. Scope, licence and clean room

- **Source of the facts:** the public documentation of `HenryKun55/celosia` (README, `docs/STATUS.md`,
  `docs/native_port_plan.md`, `docs/native_port_log.md`, `docs/native_renderer_map.md`, `docs/known_issues.md`),
  fetched on 2026-11-09. The repository was last pushed 2026-11-09 and had 55 stars, 5 forks and 6 open issues.
- **No source was opened for this note.** The earlier prose study (`private/research/celosia-study.md`, 2026-10-17,
  clone at `8a29e31d`) already read GPL code; this note does not add to that and does not quote code.
- **Licence: GPL-3.0 (C, `LICENSE` and GitHub).** Nothing here may be copied into `native/coregs3/`. The note records
  techniques and measured facts so an **untainted** writer can implement our own MIT version. The clean-room rule of
  decision 0070 applies: CoreGS 3.0 agents never read celosia source, only this note.
- The 2026-10-17 study described a 15-day-old project with 35 of 49 features native. **The project has moved since.**
  The current docs claim 50 of 50 tracked features native (C, their `docs/STATUS.md`), with no GS, VU1 or VIF emulation
  on the drawing path. That makes it the closest public reference for the end state CoreGS 3.0 is aiming at.

## 1. What celosia's renderer is, in their own terms

**Interception level.** The renderer is fed at the game's own draw system, `modelDisp(instance, matrix)`, not at
VU1/VIF/GS. It reads the loaded model header, LOD table, surface records, draw packets and VIF vertex blocks straight
from guest memory and draws them with Metal (macOS) or OpenGL ES (Vita). It also hooks the camera context, texture
requests, display-list layers, `shadowDisp` and the particle system (C, their `docs/native_renderer_map.md`).

**What is native (C, their STATUS):**

| Area | Native implementation |
|---|---|
| Geometry | Model data read from RAM; meshes cached in GPU buffers by content; per-frame geometry buffer on the Vita |
| Textures | 4/8-bit CLUT and 32-bit decoded once, cached by content; per-frame texture uploads disappear |
| Animation | CPU skinning from the bone palette |
| Lighting | The game's own per-vertex lighting formula, two-texture surfaces, cloth and hair |
| Shadows | Stencil shadow volumes from the game's own shadow models |
| Particles | Rebuilt from the particle lists |
| Fog | GS vertex fog plus the game's depth fog (`zFog`) into the sky |
| Post | Bloom, boss-death glow, depth of field, screen blur, planar reflections, water refraction, heat haze |
| 2D | Sprites, fades, cinema bars, subtitles, HUD and menus drawn from the game's sprite objects |
| Hardware | EE timers/interrupts/DMA, IOP modules, SPU2 and the pad/card run as host code; no IOP code runs |

**Coexistence.** They did not switch the GS off in one step. Their order was: VU1 to GPU batches, then GS to native
textures/state, then the native renderer at `modelDisp`, then the remaining hardware blocks (C, `native_port_plan.md`).
When the native renderer is on, the GS raster is skipped, but the DMA chains are still walked for the only GS events
the game waits on: SIGNAL, FINISH and LABEL (C). That is the same seam our plan calls level 3/4.

**Measurement gate.** Every step is a row in `native_port_log.md`: one fixed route driven by frame-clocked input, busy
ms per thread before and after, counters proving the new path did the work, and a picture diff. The rule is "no fps
drop, no picture difference that is not explained", otherwise the step is fixed or reverted (C, their plan and log).
This is the same discipline as our G-OFF/G-SHADOW/G-LOGIC/G-IMAGE gates.

**Settings.** All renderer settings (render scale, filtering, MSAA, bloom, glow, DOF, blur, shadows, fog, particles,
reflections, refraction, haze, alpha test, model detail, draw distance, 30 fps cap) apply **at once, none needs a
restart** (C, their STATUS). Their settings UI is host-drawn (F1 player menu and F10 panel), not the game's own menu;
it does not transfer to TASK-213's native in-game rows.

## 2. Why this validates the CoreGS 3.0 choices

1. **EE-level draw call (approach A), not VU1 input (B).** Celosia's finished renderer is fed by `modelDisp`, the game's
   own model system, with model/matrix/material knowledge. Their VU1-to-GPU phase was a stepping stone; once the native
   renderer existed, VU1 was no longer needed for drawing (C). This is direct evidence that our K1 `pgluCallShape` hook
   is the right long-term level, and that VU1 work is an oracle/fallback, not the destination.
2. **Hybrid with the faithful renderer.** They kept the emulated path behind a switch until each step was proven, then
   removed it (C). Our CoreGS 2.0 coexistence is the same strategy, with the advantage that we keep exact EE-visible
   read-backs in the same process.
3. **Shadow mode first.** Their `SOTC_NATIVE=2` draws natively without showing it for side-by-side checks (C). That is
   exactly `GT4_COREGS3=shadow`; the plan's level 0 is proven practice, not a detour.
4. **VU1 as the oracle.** They compiled VU1 ahead of time and kept `PS2X_VU1_NATIVE=verify`/`PS2X_VU1_REC_VERIFY` to
   compare native work against the interpreter (C). Our VU1 tap plays the same role for the native vertex stage; the
   difference is that we capture the GIF output instead of keeping a second implementation.
5. **Content-hash asset identity.** Their meshes and textures are cached by content, and the per-frame texture uploads
   disappear once textures live in GPU memory (C). Our plan's key `(record bytes, transfer payload hash, CLUT hash)` and
   the SLM validation are the same idea.
6. **One sprite assembler for 2D.** All of their 2D (HUD, menus, fades, subtitles) goes through a handful of sprite
   functions writing GS registers, and they rebuilt it as one native 2D path (C). GT4's HUD is the same shape: 32
   `render_main` elements and `display.gpb` textures. Phase 7 can copy the architecture, not the code.

## 3. The GS rules celosia had to reproduce, and that we must check

These are the concrete fidelity traps their docs record. They are cheap to add to the CoreGS 3.0 acceptance checklists
and expensive to discover late.

| Rule | What celosia found | GT4/CoreGS 3.0 action |
|---|---|---|
| Alpha test and depth | Under-half-alpha pixels write **no depth** (their round 38 fix) | Check GT4's `TEST`/`ATE` + `ZBUF` behaviour at car glass, foliage, shadows; add to the PGL state table (Phase 1/2) |
| Untextured colour | A textured primitive multiplies by the vertex colour (0x80 = 1); an untextured one takes the vertex colour **as is** (255 = white). Their first native build was twice as bright on untextured draws | Verify every PGL state class: textured vs untextured modulation; compare against CoreGS 2.0 |
| Blend rounding | GS blends truncate; their first exact blend biased the source to truncate without a target copy | Our native blend must reproduce the GS result, not a float-correct one; CoreGS 2.0 is the reference |
| `COLCLAMP` wrap | The shadow-volume stencil count is modulo 256; the emulated GS wrapped where they expected clamp, changing a sword shadow | Check any GT4 effect that counts in the frame buffer (flares, tone, shadows) |
| Vertex fog | Fog factor from `1/w`, mixed with `FOGCOL` by `F/255`; the sky is not fogged | GT4's per-vertex fog and `FOGCOL`; do not fog the sky (plan §6) |
| Texture LOD | GS level `log2(1/Q) * 2^L + K`; hardware derivative LOD blurred their floors | Derive GT4's LOD selection from the tap/state; do not use the GPU's default |
| UV row offset | The game moves a 16-bit ST row every frame (water, clouds); native adds `row / 4096` per draw | GT4's CLUT animation and texture scrolling; key the cache on the moved bytes |
| Draw order | Layers, second passes and screen-effect sprites over buffers; drawing a mirror layer on screen gave "grey curtains" | The stream markers and pass order matter; do not reorder draws |

## 4. Effect lessons (Phase 4)

- **Bloom/glare shape is the hard part.** Their open issue is that the native bloom saturates bright openings to a flat
  white with a narrow halo, while the PS2 shows a wide soft glow. They had to reproduce the GS chain buffer by buffer
  (mask, downsample, 12 shifted taps, history feedback), not a generic bloom (C, their `native_renderer_map.md` and
  STATUS). GT4's glare and tone sequence are the same class of effect: start Phase 4 from the game's own passes and
  parameters, and treat a "looks like bloom" approximation as a rejected candidate.
- **History feedback matters.** Their bloom keeps 0.92 of the previous blurred result, so the glow swells for a dozen
  frames. GT4's motion-blur feedback and flare deadlines have the same temporal behaviour; a single-frame implementation
  will pass a still frame and fail in motion.
- **Screen effects read the frame.** DOF, blur, refraction and shadows all copy the picture so far. In overlay/replace
  they must run after the native inject on the merged frame, or the seam shows (plan §11 already lists this).
- **2D is not an afterthought.** Their HUD/menus are native only after the sprite path exists; until then the 2D layer
  is the composite boundary (plan §5.3, level 3). Celosia confirms that splitting 2D at one point is workable.

## 5. Production lessons for the implementation (Phases 1, 6, 8)

- **Per-frame geometry buffer** (one buffer per frame, reused) moved the Vita from 8-16 to 18-25 fps in Vita3K (C). The
  same pattern applies to our phase 8 mobile work and to the desktop upload path.
- **Bounded queues and LRU release.** Their VU1 worker queue grew 40-90 MB and their texture cache filled a 160 MB Vita
  in 12 minutes until they released unused content after 30 s (C). Our plan's byte budgets must be enforced from the
  first live build, not after a leak.
- **Pipeline warm-up, texture dedupe, draw merging.** ~110 pipelines warmed in the background at start; texture objects
  deduplicated; consecutive primitives merged (draws -33%, scene CPU -25%) (C). Phase 1e/8 should include the warm-up and
  the dedupe; draw merging is a Phase 3/4 optimization.
- **Settings designed live.** Their renderer exposes every graphics option as a live setter from the start (C). CoreGS
  3.0 should do the same: build the renderer with mutable state for scale, effects and post chain, even if the launcher
  only exposes them later. This is independent of CoreGS 2.0's `const` flags (TASK-213), which need their own setters.
- **Know what not to copy.** Their game-logic decompilation, IOP removal, host menus and GPL code are out of scope here.
  GT4 keeps the recompiled EE code, the interpreted IOP and the game's own menus.

## 6. What does not transfer

| Celosia | GT4Recomp |
|---|---|
| No widescreen, no high frame rate; 4:3 fit hardcoded | 16:9 and other ratios, variable presentation rate (TASK-045/080) are requirements |
| Host-drawn settings menu (F1/ImGui) | Settings must live in GT4's own Options rows (TASK-213, owner decision) |
| Functional decompilation of game logic | Out of scope; the recompiled game stays the only logic |
| One game-specific renderer with hardcoded addresses | Same approach, but our addresses come from the decomp and the PGL/ModelSet2 study |
| Single-author, 5-week-old project, picture not hardware-verified | We need the same ideas with our gates and the owner's tests |

## 7. What to do with this note (ordered)

1. **Add the section 3 table to the Phase 1/2 acceptance checklists.** No code: the PGL state table and the image/tap
   gates must name alpha-test depth, untextured colour, blend rounding, `COLCLAMP`, fog, LOD and UV row.
2. **Phase 4 effect briefs start from the game's own pass chain**, with the bloom/glare warning in section 4 as a stop
   condition for "generic post effect" proposals.
3. **Phase 1e/8 include warm-up, dedupe, per-frame geometry buffer and LRU budgets** as explicit deliverables.
4. **Keep this note prose-only.** Any CoreGS 3.0 agent that needs more detail must ask the orchestrator; it must not open
   celosia source.

## 8. Sources (accessed 2026-11-09)

- Repository and metadata: https://github.com/HenryKun55/celosia (GPL-3.0; 55 stars; last push 2026-11-09).
- `README.md`, `docs/STATUS.md`, `docs/native_port_plan.md`, `docs/native_port_log.md`,
  `docs/native_renderer_map.md`, `docs/known_issues.md` (prose only).
- Earlier study: `private/research/celosia-study.md` (2026-10-17; source read then; ideas only).
- Our plan: `docs/COREGS3-PLAN.md`; hooks and capture: `docs/COREGS3-HOOKS.md`.
