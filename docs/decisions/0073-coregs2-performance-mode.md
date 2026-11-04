# 0073 — CoreGS 2.0 performance mode: small visual differences allowed for speed

Date: 2026-10-29. Status: Accepted (owner, 2026-10-29). Task: TASK-027.

**Context.** At 12x on the RX 9060 XT the night race needs 21.4 ms of GPU per VBlank on candidate 306 (IN-437), above
the 16.7 ms budget. CoreGS 2.0 does the GS pixel pipeline in the fragment shader with exact integer arithmetic. A
fill benchmark (IN-436) shows the GPU's own colour units (ROP) write an opaque full-screen pass at 12x in 0.21 ms
against 0.41 ms for the best shader path, and blend in 0.42 ms. The GS rules differ from the ROP's in ways the ROP
cannot reproduce bit for bit: alpha 128 = 1.0 with factors up to about 2.0, integer products shifted by 7, colour
wrap without COLCLAMP, partial writes on alpha-test failure (AFAIL), destination alpha test, FBMSK bit masks, 16- and
24-bit formats. The reference renderer's public "blending accuracy" setting makes the same trade.

**Decision (owner).** CoreGS 2.0 gets two modes:
- **Faithful mode** (default): today's path, exact; the 1x identity gates stay.
- **Performance mode** (opt-in, the launcher's "Performance mode" button, which already lowers the reference
  renderer's blending accuracy and turns off its readbacks): GPU fast paths that may change pixels slightly, such as
  ROP colour writes and blending, hardware texture filtering, or fewer synchronisations, chosen by measurement at 12x
  (owner rule: what it saves there decides playability on weaker GPUs).

Owner's words: small differences here or there are fine; the problem is only what really affects gameplay.

**Limits.** Game logic never changes: physics, timing, AI, saves and anything the EE or IOP reads back that affects
gameplay stay exact (`decisions/coregs3-modern-pipeline`). A readback is relaxed only after it is shown to feed
nothing but the picture. Every relaxation keeps an A/B switch, is measured at 12x and 3x, and is listed with its
visible effect per scene (night, rain, rally, replays, photo, split screen) in CORE2-PERF-LEDGER.

**Consequences.** CoreGS 2.0's faithful mode remains the reference; performance mode is how slower GPUs (the laptop's
HD 620, later mobile) reach full speed. CoreGS 3.0 (the modern pipeline) stays a separate, larger step.
