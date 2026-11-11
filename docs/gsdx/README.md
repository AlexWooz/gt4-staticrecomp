# `docs/gsdx`: the GSdx/PCSX2 reference studies

Prose-only studies of how the PCSX2 GS (GSdx, and the ARMSX2 fork we use as a black box on Android) is built, written for
implementers who may not read its source. They exist so that CoreGS 2.0/3.0 decisions are informed by a working hardware GS
without ever bringing GPL code into our tree.

## Rules for every page here

- **Concepts and architecture only.** No code blocks, no file dumps, no copied constants. A short pseudocode sketch of a
  *mechanism* is allowed; a copy of their implementation is not.
- Their file paths appear only as evidence of where a concept lives.
- Every claim is labelled **C** (confirmed: read in the code, or measured and quoted with its source), **H** (high
  confidence), **Hyp** (hypothesis) or **Unknown**. No performance claim without a measurement or an explicit "unknown".
- The author of these pages is PCSX2-tainted and read-only: it may not edit code, tests, pins or data, and no agent that
  reads these pages becomes tainted (the pages carry no code).
- Decision 0070 stands: on PC the PCSX2 GS is a separate GPL process (`gt4-gs.exe`); nothing from it enters the release
  executable. These pages inform the in-process MIT renderer.

## Pages

- [`GSdx-PIPELINE-STUDY.md`](GSdx-PIPELINE-STUDY.md) - the reference: frame pipeline and threading, the readback inventory
  and the explicit "does it overlap the next frame while waiting" answer, uploads/CLUTs/targets/caching, present and vsync,
  where CoreGS 2.0 is weaker (our file:line), ranked opportunities with gates and proofs, mobile/tile-GPU notes, the full
  ledger cross-check (what was deliberately not proposed) and every unknown.
- [`GS-THREAD-CPU-COST.md`](GS-THREAD-CPU-COST.md) - the GS thread's own CPU: parse and fused vertex kick, draw buffering,
  per-draw validation and state caching, the one-command-buffer-per-frame strategy; what they do not do per draw/batch that
  we do; our per-stage numbers (license 1,930 batches / 9,197 draws / 1,553 CLUT loads / 145 uploads per VBlank); the adopted
  concepts for an MIT front end + draw recorder + texture worker, each with a bound and a gate; and why no measurement says
  their GS thread is lighter on the same CPU.

## The short answers, for when you only have a minute

1. They submit a frame **once**, at present time (3 command buffers, 2 in flight). The only mid-frame submit is a
   readback-armed "kick". We submit every 32/64 batches always.
2. Their default readback is **synchronous** and blocks the GS thread. They overlap the next frame only in the
   `Asynchronous` download mode, where the EE reads a CPU shadow 3-4 frames old with per-page validity - the lateness the
   owner rejected for normal mode (decision 0073).
3. Their draws are **attachment** draws with hardware blend/depth, one render pass per run, and feedback reads declared
   instead of copied. We use **storage images** with a shader RMW and a pixel interlock. On a tile-based GPU that is the
   difference that matters: our S23 Ultra run is 11.5 ms of GPU at both 1x and 2x with ~750 fragment barriers, 75 full
   barriers and 20 render passes per VBlank (C).
4. On the **desktop** we are not slower: CoreGS 2.0 is 9.0% faster at 3x and 11.4% faster at 12x than the `gt4-gs.exe`
   reference on the same recording (C, TASK-215 baseline).
