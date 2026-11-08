# 0080: the mapped CoreGS 2.0 variants ship in both games, from one shared list

Date: 2026-11-07 (build finished after midnight, 2026-11-08). TASK-208, from `task/205-variant-coverage` (the final mapping,
commit 71452b90) merged into `task/206-rc16-package` (52fea56e, which already has TASK-204, TASK-206, the rc16 package and
the both-games AGENTS rule). Owner: every build uploaded to GitHub ships both GT4 v2.00 and Spec II (AGENTS.md, 2026-11-07).

## Decision

- **The shipped `native/hw_gs/core2/core2_known_variants.inc` becomes the TASK-205 final candidate**: **644 keys = the 544
  shipped + 100 new**, 204 draw-state signatures (30 fields plus the push word with the sampled scale in bits 0-19 and the
  frame bits 28/30 ignored; the aniso and ROP words do not take part), 60 of them new. The keys are draw state only, so the
  **same `.inc` serves both games**: one list, no per-game file, no runtime switch.
- **Both games are built and shipped from the same source and the same list**: GT4 v2.00 (`GT4_TARGET=v200`,
  `GT4_EE_PROFILE=aspect`) and Spec II (`GT4_TARGET=spec2`, `GT4_EE_PROFILE=spec2` with the TASK-166 aspect-hooked Spec II
  EE library, the slice276 IOP runtime and the Spec II VU0 tables). Both plain Release, no PGO, no ThinLTO; both with
  `GT4_RELEASE_DISTRIBUTION=ON` (shaders from `<exe folder>\shaders`). A build with only one game is invalid.
- **The TASK-206 prepare expansion is unchanged**: the prepare compiles the 644-key corpus expanded over the session's
  render scale (`--prepare-shaders --prepare-scale N`, `PreparedStamp` carries the scale). With the larger corpus the
  expansion and the card filters give **315 distinct keys at 1x** (644 written) and **406 at 2x/4x/6x** (917 written, 273
  keys with both shapes) before the card filters, and **245 on this card (RX 9060 XT) at 4x** (49 targets added) against
  175 with the 544-key corpus. Still well under `kMaxSpecPipelines` (2,048).
- **The distribution is one folder with both executables** (`gt4.exe` and `gt4-spec2.exe`), the nine shaders, SDL3, the
  CRT DLLs, the licences and the `MANIFEST.json` naming both pins (`docs/inputs/slice418-pcsx2.json` and
  `docs/inputs/slice418-spec2.json`). The installer and the portable zip are a separate packaging step.
- **The launcher is not changed by this task.** It already passes `--prepare-scale` (TASK-206) and its game list already
  carries both games; the list is compiled by the game's prepare, so a bigger `.inc` changes no launcher code.

## Why

- **The owner wants the mapping shipped, not measured again.** TASK-205 mapped 81 runs over both games and every reachable
  mode; the candidate fixes the two causes of "why was this shader generated?" that the mapping found: the scale family
  (TASK-206, already shipped behaviour) and the material states the recorded scenes never used (the 100 new keys). Without
  it, a v2.00 or Spec II session at any scale still defers those states and draws them with the uber pipeline.
- **The failure it removes is measured.** On the TASK-205/206 fallback scene (Spec II George V Paris, B-Spec, 4x, 2,400
  VBlanks) the deferred run recorded 75 keys, created 0 pipelines and drew 2,645,167 uber batches; after the prepare of the
  644-key list at 4x (245 variants, 49 added, 0 failed; 3.1 s end to end, 2.8 s of it the prepare's own start-up) the
  prepared run had **0 deferred keys, 0 deferral-caused uber batches** and **4 of 4 identical frames** (the pipelines were
  recreated from the disk cache in 193.7 ms), with `gpu_busy` 3.78 against 5.67 ms per VBlank over the race's four
  600-VBlank blocks (the TASK-205/206 numbers reproduced). The same gate on the v2.00 night race at 4x is **0 deferred, 0
  deferral-caused uber batches, 9 of 9 identical frames** (245 variants, 3.1 s end to end; recreated in 183.9 ms).
- **The list is still a union of recorded scenes, not an enumeration.** The 100 new keys come from the v2.00 photo mode and
  license test (the first `Zformat=Z16` states), the used-car showroom, the Spec II license/menus, the car materials
  (`Tfx=Highlight2`), the SCANMSK flag and material combinations. Modes that need a GT Mode card, a second controller
  (split screen) or a menu path we do not have (replay) stay unmeasured; TASK-207's player loop is the way to close them.

## Consequences

- Both game executables carry the same 644-key corpus; a session at S prepares 245 variants on this card at 4x instead of
  175, and 196 at 1x (before a session's recorded keys), so every state of the mapping is specialised after one prepare.
- The identity of both games is unchanged: the list changes only which pipeline a draw uses, and a specialised draw and the
  uber draw produce the same pixels (TASK-204). Verified against the published rc15 PGO builds: Spec II three recorded laps
  (state.bin identical, race clock 0 differs, 24/35/35 frames identical), Spec II 6x (12/12), v2.00 night 3x (state
  identical, 10/10) and v2.00 Arcade 3x (14/14); VU1/VU0 verify mismatches 0, MTVU 0.
- The manifest pins are new files; the slice413/417 pins are untouched. A rebuild changes the executable hash, so the
  packaging step must regenerate the distribution and its pin rather than reuse this one.
- A 1x spot check of the four TASK-205 tracks that asked for the most new keys (Nordschleife, Tsukuba, Infineon Sports,
  Hong Kong) with a 1x prepare found **0 deferred keys and 0 deferral-caused uber batches** in every run; the list covers
  those scenes at 1x where the 544-key list deferred 6-25 keys each.
- Not measured here: in-game play of this build (owner test), the blocked modes of TASK-205 (split screen, replay, Spec II
  GT Mode race/photo/showroom), the integrated-GPU path, and the 2x/3x/6x/12x sessions in game.
