# 0073 — Gameplay mods ship in the same bundle (owner, 2026-11-11)

**Status:** accepted (owner decision, 2026-11-11, Telegram).

**Context.** The owner is driving a real gameplay modification: an H-shifter with **direct gear application** (no
up/downshift button emulation), **reverse as a gate position** in the sequence R, N, 1, 2, ..., and a **clutch pedal**
with a clutch system the game can use. The earlier project rules said changes to the game's physics timing and remaster
features were out of scope here (a separate project or fork), that gameplay patches must not change how the game plays,
and that CoreGS 3.0 must never change game logic.

The owner's new direction (2026-11-11): "the original game already runs, now we start playing around more"; the mods go
into the same bundle, not a separate fork.

**Decision.**

- **Gameplay mods are in scope in this repository and ship in the same bundle.** They may add or change gameplay
  systems (H-shifter, direct gear application, reverse as a gear position, clutch, and later others).
- **Mods are opt-in and off by default.** The base game and the faithful modes (GSdx, CoreGS 2.0, CoreGS 3.0 without
  mods) keep exact physics, timing, AI, saves and gameplay-relevant read-backs; with every mod off the runtime behaves
  exactly as the reference.
- **The faithful renderers remain the default and the reference.** The original game running natively, optimized and
  faithful, is still the default experience; fidelity gates continue to apply to the faithful modes.
- **Mods are explicit and documented.** Each mod states what it changes, its replay/save consequences (a replay
  recorded with a mod may not replay faithfully without it), and its own acceptance checks. No mod is enabled silently.
- **A decompilation-based source port remains out of scope.** This decision is about gameplay modifications on the
  recompiled port, not a source-port rewrite.

**Consequences.**

- `AGENTS.md` mission wording updated: the "out of scope" and "game logic never changes" rules now scope to the base
  game/faithful modes; opt-in mods are allowed and shipped together, and the game-patch rule distinguishes patches
  (gameplay untouched) from mods (gameplay changed, opt-in).
- TASK-215 (H-shifter, direct gears, reverse as a gate position, clutch pedal) becomes a mod task of this repository.
- The launcher/README may later list the mods and their opt-in switches; the public summary must keep the faithful
  default clear.
- Replays and saves made with mods are documented as mod-dependent; the base game's files are never modified without
  the mod being on.
