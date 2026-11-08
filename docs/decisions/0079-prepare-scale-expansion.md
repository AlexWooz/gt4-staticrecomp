# 0079: the prepare expands the CoreGS 2.0 variant list over the session's render scale

Date: 2026-11-07. TASK-206, from TASK-205 (`docs/SHADER-VARIANT-COVERAGE.md`, sections 3, 5, 6). Owner approved option 1 of
section 5: the prepare-side scale expansion.

## Decision

- **The render scale stays part of the pipeline key.** No key-format change, no shader change, no change in the pipeline
  specialisation. The scale is what the shader uses to map texture coordinates and to choose the scaled-depth path, and
  folding those branches is why the specialised shader is cheaper than the uber one (IN-437).
- **`--prepare-shaders` takes the session's render scale**: `--prepare-shaders --prepare-scale N`, N the CoreGS 2.0 internal
  multiplier (`GT4_HW_SCALE`, 1..12), never the Render scale percentage. The launcher always passes the multiplier its own
  session will use (`launch::coregs20_scale`, resolved from the display and the Render scale exactly like the session's
  `GT4_HW_SCALE`). Without the option the prepare keeps its old behaviour, scale 1, and says so:
  `[shaders] preparing the variants of the 1x render scale (--prepare-scale absent: the old behaviour)`.
- **`variants_to_prepare` expands every key over that scale** (`core2_variant_scale.hpp`, `variant_scaled_keys`), for the
  shipped corpus and for the sessions' recorded keys, before the card/feature filters, the quarantine and
  `kMaxSpecPipelines`:
  - push bits 0-19 (the sampled image's scale) become **1** for the atlas, a replacement image, a mipmapped game texture and
    a 1x effect target - the *1x shape* of the key - and, when the key samples a texture image and the frame is above 1x,
    **S** for the same image as a target drawn at the scale (the main frame included) - the *target shape*;
  - push bits 28 (depth from position) and 30 (fine dither) follow the frame: set when S > 1, and only where `spec_key`
    keeps them (the draw tests depth / dithers), cleared at 1x;
  - a key recorded at another scale is not added as it is (no session at this scale can ask for it); its state is kept
    through its 1x shape and returns at any scale by expansion.
- **`PreparedStamp` carries the scale** (`scale`, magic `GT4PREP2`). The prepare writes the stamp of the list it expanded;
  a play session compares its own `GT4_HW_SCALE`, so a cache prepared for another scale is not used: the session compiles
  nothing, draws with the uber pipeline (the same pixels) and records its keys for the next prepare (TASK-204, decision
  0078). Changing the Render scale therefore asks for a new prepare at the next Play.
- **The prepare writes the expanded list** to `variants.bin` (it already wrote the list it compiled), so the play session
  queues exactly what was prepared, and logs `[core2] shader cache prepared: N variants for Sx (R recorded by the sessions,
  K added for the scale)`; stdout gains `shaders scale S` and `shaders added K`, and the launcher logs
  `shaders: N keys prepared at Sx`.
- **The shipped `.inc` is untouched.** The expansion is a prepare-time list transformation; the corpus stays the recorded
  union of scenes (TASK-205 measured that new states come from materials, not from the scale).

## Why

- **The scale multiplied the corpus.** TASK-205 (Confirmed): the 544 shipped keys are 144 draw-state signatures over scales
  1/2/3/6, there is **no 4x key at all**, and a session at 4x asks for a parallel set of every state it touches - the
  player's 44 keys in one lap were the first slice of that set. Until the next prepare, TASK-204 draws those keys with the
  uber pipeline: measured on George V Paris at 4x, 5.69 against 3.78 ms of `gpu_busy` per VBlank (1.91 ms, +50%) and 8.90
  against 8.44 ms of wall time. At 1x the difference is noise (6.51 against 6.54).
- **Enumerating scales in the list does not fit.** 144 signatures x 12 scales = 1,728, and the aniso/ROP twins take it over
  `kMaxSpecPipelines` (2,048). One prepared scale at a time is 336 keys at most on this corpus (255 shapes + 81 targets),
  and 175 on this card.
- **The launcher knows the scale before the game starts.** It already computes it for `GT4_HW_SCALE`, so the prepare can be
  given exactly that number: no guessing, no second source of truth.

## Consequences

- A prepare at S compiles the 1x shape of every known state plus the S-shaped targets, so a session at S has no deferred
  key for the states the corpus covers. First Play after a Render scale change: one session is prepared for the old scale,
  draws with uber (as TASK-204) and the next Play prepares at the new scale. The scale is part of the stamp, so this is the
  documented behaviour, not a silent mismatch.
- The prepare at 1x now compiles fewer keys than before (the 2x/3x/6x keys are not added as they are: 300 -> 136 on this
  card), so the prepare is shorter and the cache smaller; nothing is lost, because those keys could not be asked at 1x.
- Keys that the corpus has only at 2x/3x/6x now get their 1x shape (TASK-205 counted 31 such keys in the survey), which is
  new 1x coverage.
- Tests: `hw_core2_prepare_scale` (the transformation over the shipped corpus at 1x/2x/4x/6x: 255 and 336 distinct keys, the
  two shapes and their push words, no duplicate; a real prepare at 4x writes the expanded list and the stamp names the
  scale; a 1x session refuses a 4x prepare and defers, then uses a 1x prepare with nothing deferred and the same pixels; the
  cap), `hw_core2_prepare_shaders`, `hw_core2_shader_cache`, `hw_core2_no_ingame_compile` and `hw_core2_shader_guard`
  unchanged and green, and the launcher's `launch_contract` covers `--prepare-scale` and `coregs20_scale`. The headless 4x
  gate is the TASK-205 fallback scene (George V Paris, B-Spec, 2,400 VBlanks) with the numbers in TASKS.md.
- Still open (TASK-207, standby): the launcher notice after a session that deferred keys and the player export; they wait for
  player feedback on the scale expansion.
- Not measured here: the 2x/3x/6x/12x sessions in game (the fixture covers the list, not the pixels of every scale), and the
  integrated-GPU path (the plain/ROP filters differ per card).
