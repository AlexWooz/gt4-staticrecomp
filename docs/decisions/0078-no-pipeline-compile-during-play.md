# 0078: a play session never compiles a pipeline; the prepare does

Date: 2026-11-07. TASK-204. From a player report on 1.3.0-rc15 (RX 6750 XT, AMD driver 25.10.30.02, CoreGS 2.0 at 4x):
the game ended with `0xC0000005` inside a pipeline compile on the `spec_worker` thread, 20 minutes into a lap.

## Decision

- **CoreGS 2.0 compiles no shader variant in a play session.** `specialised_pipeline` never calls
  `vkCreateGraphicsPipelines` for a key it has not already been given: an unknown key is recorded in `variants.bin` (the
  list of variants the sessions ask for) and the batch draws with the **uber pipeline**, which draws the same pixels
  (IN-437: the specialised shader is the uber one with the state as specialization constants).
- **The launcher's prepare (`gt4vulkan.exe --prepare-shaders`) is the only place that compiles.** It compiles the shipped
  corpus and the sessions' recorded keys, under the TASK-170 guard (`compiling.bin`, `suspects.bin`, `quarantine.bin`)
  and the TASK-170b retry loop.
- **`PreparedStamp` includes the recorded list** (`variants` FNV-1a and `variantCount` of `variants.bin`), so a session
  that recorded a new key makes the next Play run the prepare again instead of finding the stamp ready. The prepare
  writes the stamp of the list it just compiled.
- **A play session recreates only what the prepare vouched for.** On start it queues the recorded variants only when
  `prepared.bin` matches (a disk-cache hit, ~0.8 ms per variant); without a matching stamp it draws with the uber
  pipeline and records what the next prepare must compile.
- **One strike in play quarantines.** A key left in `compiling.bin` by a session that died inside its compile is
  quarantined at once when the game starts (never compiled in play again). The launcher's prepare keeps TASK-170c's two
  strikes, because an intermittent driver crash may end a prepare inside an innocent variant.
- **`GT4_CORE2_INGAME_COMPILE=1`** restores the old in-play compile (A/B and the headless gate). It is **off** by default.
  Without a shader cache (`GT4_HW_SHADER_CACHE`) there is no list to defer to, so a bare developer run keeps compiling.

## Why

- **The crash is a compile, not a draw.** The player's `game.log`: 301 variants recreated at start in 227.7 ms (0.75 ms
  each, disk-cache hits), then 44 new keys requested during play, `compile_ms` 227.7 -> 26,517.1 ms, `compile_max_ms`
  1,047.9 ms, and `compiling.bin` left with the key that was in flight when the process died.
- **The guard protected the prepare, not the play.** `begin_compile`/`end_compile` and the quarantine were already around
  every compile, but the launcher's retry loop only exists around `--prepare-shaders`: a crash in play is a lost session.
- **The stamp lied.** It hashed the shipped corpus and the card/driver, not `variants.bin`, so after a session recorded new
  keys the prepare said "ready" and the game compiled them at start (or during play) with no guard and no quarantine.
- **Doing nothing is not an option for these drivers.** AMD's shader compiler can end the process inside
  `vkCreateGraphicsPipelines`; moving every compile into a separate process (and retrying it there) contains it.

## Consequences

- A new key costs one Play drawn with the uber pipeline (same pixels, a little more fragment cost on those batches) before
  the next prepare compiles it. `[core2] specialised pipelines=... deferred=N deferred_uber_batches=M` in the game log
  counts them; `[core2] shader cache prepared: N variants (R recorded by the sessions)` and `shaders recorded R` (stdout)
  let the launcher log `shaders: R recorded keys compiled`.
- The prepare is mandatory for specialised pipelines under CoreGS 2.0. A launcher that skips it (or a failed prepare) is
  correct but slower: every variant draws with uber, and the next Play's prepare fixes it.
- Quarantined variants stay on the uber pipeline forever on that card and driver (their pixels are the same).
- Tests: `hw_core2_no_ingame_compile` (an unknown key is recorded and drawn with uber, never compiled; the stamp follows
  `variants.bin`; one strike in play quarantines, the prepare keeps two), `hw_core2_shader_cache` (record, prepare,
  recreate), and the headless night race with and without the switch (9 of 9 frames identical).
