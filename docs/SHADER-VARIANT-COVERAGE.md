# Shader variant coverage: what creates a CoreGS 2.0 variant, and the all-track map (TASK-205)

Date: 2026-11-07. Branch `task/205-variant-coverage` (from `task/204-no-ingame-compile` at c462fd45).
Renderer: CoreGS 2.0 (`GT4_HW_CORE=2`). Games: Gran Turismo 4 v2.00 and Spec II 1.10.
Tools: `scripts/coregs2_bench/variant_decode.py`, `scripts/coregs2_bench/variant_survey.py`.
Evidence and the candidate list: `private/task205-*` (not committed).

This document answers the owner's question: **why was a new pipeline generated?** It maps the shipped list, measures
the 52 Spec II Arcade tracks, adds the v2.00 pass (photo mode, showroom, license, Arcade, GT Mode races) and the
Spec II GT Mode modes, and explains the rc15 player report (44 new keys in one lap, then a driver crash inside a
compile). It also designs the player crowd-sourcing loop. It changes no runtime code and no key format; the two
key-format proposals wait for the owner.

## 1. The answer in one paragraph

A key is a **draw state**, not a track, car or scene: 30 state-block fields, the texture push word, the mip anisotropy
and the ROP word. New keys come from two places. First, **the render scale is inside the key**: bits 0-19 of the push
word hold the sampled texture's scale, and bits 28/30 mark a target drawn above 1x. The 544 shipped keys are only
**144 draw-state signatures** expanded over scales 1/2/3/6 (plus a few aniso/ROP twins); 105 signatures exist only at
1x and **no key exists at 4x**. A session at 4x therefore asks for a parallel set of every state it touches: the
player's 44 keys in one lap are the first slice of that set (the shipped storage-path list projects to 136 keys at
4x, all new). Second, **material vocabulary the recorded scenes never used**: mip sampling, SCANMSK (0 keys in the
list), region-clamp wraps, alpha-test/blend combinations, raw CT24 textures. The 52 tracks added no new value to any
of the 30 state fields, only the SCANSMK push flag and new combinations; the car slots added one field value,
`Tfx=Highlight2`. Nothing in the key is a per-draw value that grows without bound (scale 1-12, aniso 0-7, ROP
bounded), so the key space is finite and enumerable; the design problem is the scale multiplication, not an open-ended
key. The fallback (uber) path costs 1.9 ms of GPU per VBlank at 4x and 0.03 ms at 1x; at 4x it can cost a frame at the
margin, but it is not a stutter, while the 1.0 s compile stalls could be and TASK-204 removed them from play.
**The final both-games mapping** (81 runs: the 52 Spec II tracks, the Spec II modes, and a v2.00 pass) recorded
**191 keys, 100 new, 60 new draw-state signatures**; the candidate is **644 keys** (400 kept on this card after the
plain/ROP filters), under the 2048 cap. The v2.00 photo mode is the first source of `Zformat=Z16` keys; Spec II
disables photo mode, split screen needs a second pad, and replay and a Spec II GT Mode race remain blocked.

## 2. What a key is

`native/hw_gs/core2/core2_gpu.cpp` builds a key with `spec_key` (IN-437):

| Slot | Name | What it is |
|---:|---|---|
| 0-29 | `kSpecFields` | 30 draw-state fields, in this order: Format, Fba, Dither, Ate, Atst, Afail, DateOn, Datm, Ztest, Ztst, Zmsk, Zformat, BlendOn, Pabe, BlendA, BlendB, BlendC, BlendD, Colclamp, TexOn, Tfx, Tcc, TexelKind, Aem, FogOn, TexFst, TexLinear, WrapS, WrapT, KeepTopByte |
| 30 | `push` | the `tex_scale` push word: bits 0-19 the sampled texture's scale, bits 20-21 SCANMSK, 22 replacement image, 23 mip, 24 textured image, 25-26 diagnostics, 27 alpha-test-before-interlock, 28 depth from position, 29 vertex snap (cleared), 30 fine dither, 31 forced linear |
| 31 | `aniso` | `tex_origin_x` bits 21-23 of a mipmapped draw (0-7) |
| 32 | `rop` | the ROP word of a draw through colour attachments, 0 for storage writes, `0x80000000`/`0xC0000000` for the plain/sheen paths |

`spec_key` zeroes every field the draw cannot reach (texture fields without `TexOn`, blend selectors without
`BlendOn`, alpha-test fields without `Ate`, DATM without DATE, depth fields without `Ztest`, dither unless CT16, the
kept top byte unless CT24, push bit 29), so equal draws share one variant. A file is `GT4VARS1`, `shaderSet` (FNV-1a of
the SPIR-V words), `keyWords = 33`, `count`, then `count` keys of 132 bytes. The key holds **draw-state numbers only**:
no guest address, no vertex, no texture and no game data.

The variant is the **uber shader with these fields as specialization constants** (IN-437): a specialised draw and an
uber draw produce the same pixels, which is why TASK-204 can defer an unknown key and keep drawing.

## 3. The pattern: what creates a new key

### 3.1 The render scale is the multiplier

The shipped 544 keys are not 544 independent states. Ignoring the sampled scale (push bits 0-19 and 28/30) and
looking at the 30 fields plus the other push flags, they are **144 draw-state signatures**:

| Scale (push bits 0-19) | Shipped keys | Shipped signatures seen at this scale |
|---:|---:|---:|
| 1 | 364 | 137 |
| 2 | 62 | 26 |
| 3 | 86 | 39 |
| 6 | 32 | 29 |
| 4 | 0 | 0 |

(One signature can be seen at several scales, so the signature column adds up to more than 144.) Of the 144
signatures, 105 appear only at 1x and 7 only above 1x; 25 appear at four scales, 5 at three and 2 at two. The scale is
part of the key because the shader uses it to map texture coordinates and to choose the scaled-depth path; the
specialisation folds those branches. The consequence is that **every render scale is a separate variant universe**.

The shipped list has no 4x key at all. Taking the 300 storage-path keys this card prepares and replacing their sampled
scale with 4 (and setting bits 28/30) gives **136 keys, all of them new**; taking all 544 gives 255, also all new. A
4x session asks for that set as it meets each state. The player on rc15 at 4x saw 44 of them in one lap; a longer
session would have asked for more. This is the main answer to "why was this shader generated?".

The list is also uneven **within** a scale: of the survey's new keys, `31` are a state that already exists in
the shipped list at another scale, with no twin at the scale the survey ran (1x). That is the same defect seen from
the other side: the list is a union of recorded sessions at different scales, not a complete set at any scale.

### 3.2 The second source: material vocabulary

The 52 tracks added no new value to any of the 30 fields. They found combinations the recorded scenes had not used:
mip sampling (`push` bit 23) on indexed textures, SCANMSK (bits 20-21, IN-530, added after the list was recorded),
region-clamp wraps, bilinear filtering, alpha-test comparisons, blend selectors, and raw CT24 texels. The car slots
added one field value, `Tfx=Highlight2`; the v2.00 photo mode and license test added `Zformat=Z16`. Those are the only
new 30-field values in all 81 runs. The per-field values and the new combinations are in section 4.

The fields that have never varied in the shipped list are `Fba` (always off), `Pabe` (off), `Colclamp` (on), `Tfx`
(Modulate, until the car slot above), `Aem` (off), `aniso` (0) and the push flags replacement (22), force-linear (31)
and alpha-test-before-interlock (27). They are dead dimensions today, not missing values; a scene that uses one would
add keys.

### 3.3 Which scene change creates a key

| Change | Creates a new key? | Why |
|---|---|---|
| A new track | **No by identity; yes through materials** | the key has no track field. A track adds only the state combinations its surfaces, cars and effects use |
| A track section | **Yes, indirectly** | a tunnel, a bridge, a night section or a pit lane can use a material combination the earlier section did not |
| A car | **Only through materials** | no car field. A different body can add a reflection/target-texture or alpha-test state; measured: car slot 5 added `Tfx=Highlight2`, a texture function the list never had |
| Time of day | **Yes, indirectly** | night changes fog, additive lights and reflection targets, so it adds blends and target-texture states |
| Weather | **Yes, indirectly** | wet Tsukuba adds reflection/alpha states, measured in the survey |
| A menu | **Only if it uses a state the race does not** | measured: the Arcade menus add 1 key and the car list/colour screen 35, all already in the shipped list; a menu with a new format or blend still would |
| Photo mode | **Yes in v2.00; disabled in Spec II** | measured: 48 keys, 20 new, the first `Zformat=Z16` states; the Spec II map says `Modo Fotografia: Função Desativada no Spec II` |
| Replay | **Not mapped** | replay saves exist on both cards; the loader entry was not found in the GT Mode home or title menu (section 7.4) |
| Split screen | **Yes, but needs two pads** | the 2P menu and track preview are reached; the race waits for a second controller, which the harness does not feed |
| A per-draw value | **No** | no address, counter or geometry is in the key; the scale is a session/texture property, the aniso is bounded 0-7, the ROP word is a blend state |

### 3.4 What the survey actually found

The survey recorded `52` race runs and `155` distinct keys; `69` are not in
the shipped 544. Of those, `31` are scale twins (the same state at another scale) and `38` are new
state combinations, `38` of them new signatures. The per-track table is in section 7; every new key of the
whole survey (including the car slots) is in Appendix A.

The final both-games mapping is larger: 81 runs (the 52 tracks, the Spec II GT Mode modes and a v2.00 pass) recorded
**191 distinct keys, 100 new, 60 new signatures** (section 7.5). The v2.00 pass is where the first `Zformat=Z16` keys
appear.

## 4. Field-level classification

The families below group every new key by what differs from the nearest shipped key. A key can appear in one family
only; "state" means a combination the list never had.

| Family | New keys | Scenes | Example |
|---|---:|---:|---|
| scale | 40 | 71 | format=CT32 tex=Modulate/RGBA8 z=Always/Z24/masked scale=1 textured-image |
| state: push | 14 | 67 | format=CT32 tex=Modulate/RGBA8 z=Always/Z24/masked scale=1 textured-image scanmask |
| state: Zformat | 9 | 3 | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z16/masked fog scale=1 |
| state: TexLinear | 3 | 48 | format=CT32 tex=Modulate/Indexed z=GreaterEqual/Z24 fog scale=1 |
| state: WrapT | 3 | 5 | format=CT32 tex=Modulate/Indexed z=GreaterEqual/Z24/masked fog scale=1 |
| state: BlendB | 2 | 29 | format=CT32 tex=Modulate/RGBA8 blend z=Always/Z24/masked scale=1 textured-image |
| state: BlendB+BlendC | 2 | 17 | format=CT32 tex=Modulate/RGBA8 blend z=Always/Z24/masked scale=1 textured-image |
| state: Ztst | 2 | 5 | format=CT32 untextured blend z=GreaterEqual/Z24/masked scale=1 |
| state: WrapS | 2 | 7 | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24/masked fog scale=1 |
| state: WrapS+push | 2 | 10 | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24/masked fog scale=1 mip/aniso=0 |
| state: Tcc+TexelKind | 2 | 3 | format=CT32 tex=Modulate/CT24raw atest=Greater/Keep z=GreaterEqual/Z24 fog scale=1 |
| state: Zmsk | 2 | 2 | format=CT32 tex=Modulate/Indexed atest=Greater/Keep z=GreaterEqual/Z24/masked fog scale=1 |
| state: Format | 2 | 2 | format=CT16 tex=Modulate/RGBA8 blend scale=1 textured-image |
| state: Zmsk+Zformat+TexLinear | 1 | 1 | format=CT32 tex=Modulate/CT16raw z=Always/Z16 scale=1 textured-image |
| state: BlendC | 1 | 1 | format=CT32 tex=Modulate/RGBA8 blend z=Always/Z24/masked scale=1 textured-image |
| state: Zformat+BlendB+BlendC | 1 | 1 | format=CT32 untextured blend z=GreaterEqual/Z16/masked fog scale=1 |
| state: DateOn+BlendB+BlendC+TexOn | 1 | 1 | format=CT32 tex=Modulate/RGBA8 blend z=GreaterEqual/Z24/masked fog date scale=1 |
| state: Ate+Atst+push | 1 | 4 | format=CT32 tex=Modulate/Indexed blend atest=GreaterEqual/Keep z=GreaterEqual/Z24/masked fog scale=1 |
| state: Ate+Atst | 1 | 52 | format=CT32 tex=Modulate/Indexed blend atest=GreaterEqual/Keep z=GreaterEqual/Z24/masked fog scale=1 |
| state: Atst | 1 | 3 | format=CT32 untextured blend atest=Greater/Keep z=Always/Z24/masked scale=1 |
| state: FogOn | 1 | 1 | format=CT32 tex=Modulate/Indexed atest=Greater/Keep z=GreaterEqual/Z24 scale=1 |
| state: Tfx | 1 | 2 | format=CT32 tex=Highlight2/Indexed atest=Greater/Keep z=GreaterEqual/Z24 fog scale=1 |
| state: Ate+Atst+Tcc+TexelKind | 1 | 1 | format=CT32 tex=Modulate/CT24raw blend atest=Greater/Keep z=GreaterEqual/Z24/masked scale=1 |
| state: TexLinear+push | 1 | 1 | format=CT24 tex=Modulate/RGBA8 z=Always/Z24/masked scale=1 textured-image |
| state: Ztst+Zmsk+Zformat | 1 | 1 | format=CT24 untextured blend z=GreaterEqual/Z16 scale=1 |
| state: Format+Zformat+KeepTopByte | 1 | 1 | format=CT24 tex=Modulate/Indexed atest=Greater/Keep z=GreaterEqual/Z16 fog scale=1 |
| state: TexFst | 1 | 2 | format=CT16 tex=Modulate/Indexed blend z=Always/Z32/masked scale=1 |
| state: Format+BlendB+BlendD | 1 | 1 | format=CT16 untextured blend z=Always/Z24/masked scale=1 |

Reading the families:

- **scale** - the state and flags exist in the list, only at another scale. Cause: the uneven per-scale coverage of
  section 3.1. The fix is preparation, not a new state (section 6).
- **push** - the state exists, but with a push flag it never had. In the survey that is mip (bit 23) or SCANSMK
  (bits 20-21). Both are real draw states (IN-428, IN-530); neither is a per-draw value.
- **TexLinear / WrapS / WrapT** - the texture filter or wrap differs on an indexed texture: bilinear sampling, or a
  region clamp instead of repeat/clamp. These are CLUT/tiled texture states the recorded scenes did not combine.
- **BlendB / BlendC** - a different blend selector (`Cd -> Zero`, `As -> FIX`), i.e. another translucent effect.
- **Ztst** - a depth comparison (`GreaterEqual` against `Greater`/`Always`) on a state that had another one.
- **Ate / Atst** - alpha-test comparison or fail action on cut-outs (fences, leaves, signs).
- **Tcc / TexelKind** - a raw CT24 texture (`CT24raw`) instead of RGBA8/indexed, with the colour conversion off.

The per-field table shows the closed vocabulary:

| Field | Values in the shipped list | Values the survey added |
|---|---|---|
| Format | CT32, CT24, CT16 | - |
| Fba | off | - |
| Dither | off, on | - |
| Ate | off, on | - |
| Atst | Never, Equal, GreaterEqual, Greater, NotEqual | - |
| Afail | Keep, FrameOnly, DepthOnly, RgbOnly | - |
| DateOn | off, on | - |
| Datm | off, on | - |
| Ztest | off, on | - |
| Ztst | Never, Always, GreaterEqual | - |
| Zmsk | off, on | - |
| Zformat | Z32, Z24 | Z16 |
| BlendOn | off, on | - |
| Pabe | off | - |
| BlendA | Cs, Cd | - |
| BlendB | Cs, Cd, Zero | - |
| BlendC | As, Ad, FIX | - |
| BlendD | Cs, Cd, Zero | - |
| Colclamp | on | - |
| TexOn | off, on | - |
| Tfx | Modulate | Highlight2 |
| Tcc | off, on | - |
| TexelKind | RGBA8, CT16raw, CT24raw, Indexed | - |
| Aem | off | - |
| FogOn | off, on | - |
| TexFst | off, on | - |
| TexLinear | off, on | - |
| WrapS | Repeat, Clamp, RegionClamp | - |
| WrapT | Repeat, Clamp, RegionClamp | - |
| KeepTopByte | off, on | - |
| push | scale=1, scale=1 mip, scale=1 textured_image, scale=2 textured_image, scale=3 textured_image, scale=6 textured_image, scale=1 depth_from_position, scale=1 mip depth_from_position, scale=1 textured_image depth_from_position, scale=2 textured_image depth_from_position, scale=3 textured_image depth_from_position, scale=6 textured_image depth_from_position, scale=2 textured_image depth_from_position dither_fine, scale=3 textured_image depth_from_position dither_fine, scale=6 textured_image depth_from_position dither_fine | scale=1 scanmask.1, scale=1 scanmask.1 textured_image |
| aniso | 0 | - |
| rop | none, 0xF, 0x11, 0x1F, 0xE1, 0xEF, 0xF1, 0xFF, 0x1FF, 0x80000000, 0xC0000000 | - |

In the 52 tracks no 30-field value was added; the only new value is the SCANSMK push flag (bits 20-21). The car slots
added `Tfx=Highlight2`; the v2.00 photo mode and license test added **`Zformat=Z16`**, a depth format the list never
had. That is still a closed, small vocabulary: the state space is finite and the list can be completed by enumeration
rather than by playing forever.

## 5. The push word

| Bit | Meaning | Shipped keys | Final union keys |
|---|---|---:|---:|
| `0x000FFFFF` | tex_scale: scale of the sampled texture image (its whole-texel scale, 1x..12x) | 544 | 191 |
| `0x00100000` | scanmask.0: SCANMSK bit 0 (row mask, time trial ghost / split screen) | 0 | 0 |
| `0x00200000` | scanmask.1: SCANMSK bit 1 (row mask, time trial ghost / split screen) | 0 | 3 |
| `0x00400000` | replacement_texture: a replacement texture image is sampled instead of the game texture | 0 | 0 |
| `0x00800000` | mip: texture sampled with mip levels (LOD), aniso in slot 31 | 20 | 21 |
| `0x01000000` | textured_image: the batch samples a texture image (atlas or drawn target); never set when TexOn=0 | 251 | 47 |
| `0x02000000` | diagnostic_noz: GT4_HW_SCALED_DIAG=noz diagnostic | 0 | 0 |
| `0x04000000` | diagnostic_notex: GT4_HW_SCALED_DIAG=notex diagnostic | 0 | 0 |
| `0x08000000` | alpha_test_before_interlock: alpha test may discard before the interlock | 0 | 0 |
| `0x10000000` | depth_from_position: depth from gl_FragCoord; set with dither_fine when a target is scaled above 1x | 320 | 0 |
| `0x20000000` | vertex_snap: vertex-stage native-alignment snap; cleared from the key | 0 | 0 |
| `0x40000000` | dither_fine: fine dither; set with depth_from_position when a target is scaled above 1x | 7 | 0 |
| `0x80000000` | force_linear: forced linear filter for 1x textures | 0 | 0 |
| | **distinct words** | **15** | **5** |

The low 20 bits are not a flag: they are the sampled texture's own scale, so every key has them. In the shipped list
the values are 1, 2, 3 and 6; the survey ran at 1x and produced only 1. The flag bits in use in the shipped list are
mip (23), textured image (24), depth from position (28) and fine dither (30); the survey adds SCANMSK bit 21. No key
has ever set replacement (22), forced linear (31) or alpha-test-before-interlock (27).

**State or per-draw?** All of them are draw state:

- the sampled scale is a property of the texture image a batch samples (the main target at the session scale, or a
  1x effect target inside a scaled session), not of an individual primitive;
- mip/textured/SCANMSK come from the batch's texture and primitive state;
- depth-from-position and fine dither are set together from `frame->scale > 1`;
- aniso is `tex_origin_x` bits 21-23, a texture property, bounded 0-7;
- the ROP word is a blend/attachment state, bounded by the blend formula.

**Can the key grow without bound?** No. The largest varying value is the scale (1-12, an integer option), then aniso
(0-7), then the ROP word (finite). There is no counter, address or timestamp. The key space is a product of small
enumerations.

**The design issue is bounded but multiplicative.** The key contains the scale, so the corpus grows with the number of
prepared scales: 144 signatures x 12 scales = 1,728 signatures, and up to 255 aniso/ROP twins x 12 = 3,060 keys, over
`kMaxSpecPipelines` (2048). Two proposals, neither applied here:

1. **Prepare-side scale expansion (recommended, no shader change).** The launcher passes the session's render scale to
   `--prepare-shaders`; `variants_to_prepare` adds, for every key of the shipped list, its twin at that scale (sampled
   scale = the session scale for textures from the main frame, 1 for 1x effect targets; bits 28/30 set when the frame
   scale is above 1). The prepare writes the expanded list to `variants.bin` (it already writes the list it prepared),
   so the play session queues and recreates them with no compile. `PreparedStamp` must carry the scale, so changing it
   invalidates the prepare. The shipped `.inc` stays as it is; the cap is 136-255 keys per prepared scale, not
   multiplied by 12.
2. **Take the scale out of the key (owner decision).** Drop bits 0-19 and 28/30 from the key and read them from the
   state block at draw time; one pipeline then serves every scale. It removes the multiplication for good and reduces
   pipeline memory, but changes the key format and adds a dynamic branch/read to the fragment shader. It needs a
   measured A/B before it is chosen.

TASK-205 does not change the key format or the prepare; it records the proposals for the owner.

## 6. The cost of the fallback

Method: George V Paris, B-Spec, 4x, the same press timeline and the same 2,400-VBlank race window, twice. Run A used a
fresh cache, so every key was deferred and every batch drew with the uber pipeline. Then the renderer's prepare
(`gt4vulkan-spec2.exe --prepare-shaders`, the launcher's own path) compiled the shipped list and the keys the run had
recorded. Run B used the same cache, so every key was a disk-cache hit and every batch used its specialised pipeline.
The `[core2] wall ms/vblank` and `gpu_busy` figures are the 600-VBlank interval lines inside the race.

The two sides were repeated once each (deferred, prepared, deferred, prepared) to rule out clock drift. Each value is
one 600-VBlank block inside the race:

| Run | wall ms/VBlank (four blocks) | gpu_busy ms/VBlank (four blocks) |
|---|---|---|
| Deferred (uber), first | 8.18, 8.32, 10.00, 9.04 | 5.25, 5.37, 6.15, 5.99 |
| Prepared, first | 7.42, 7.72, 9.78, 8.84 | 3.51, 3.69, 3.97, 3.95 |
| Deferred (uber), repeat | 8.20, 8.27, 10.07, 9.10 | 5.25, 5.31, 6.17, 6.01 |
| Prepared, repeat | 7.58, 7.70, 9.68, 8.83 | 3.51, 3.69, 3.96, 3.95 |

Means over the eight intervals: **deferred 5.69 ms gpu_busy and 8.90 ms wall; prepared 3.78 ms gpu_busy and 8.44 ms
wall**. The uber path costs **1.91 ms of GPU per VBlank (+50%)** and 0.45 ms of wall (+5%) in this scene. The deferred
runs had 75 draw states, 0 specialised pipelines and 2,645,167 uber batches caused by the deferral; the prepared runs
had 323 pipelines, 0 deferred keys and 0 uber batches.

At 1x the same comparison in TASK-204's night race was 6.51 against 6.54 ms/VBlank, i.e. within noise. The cost scales
with the pixel work: at 4x the uber shader's dynamic branches are 1.9 ms of a 16.67 ms frame (11% of the budget). So
the owner's suspicion is partly right: **a first-encounter session at 4x can run about 2 ms per frame heavier** until
the next prepare. That is enough to drop frames at the margin in a GPU-bound scene. It is not a stutter: the visible
hitch in the rc15 log was the compile itself (`compile_max_ms = 1,047.9`), and TASK-204 removed every compile from
play. A remaining stutter should be looked for in presentation, readbacks and scheduling, not in the uber path.

## 7. The all-track and all-mode survey

### 7.1 Build and harness

- **Build:** plain (no PGO, no ThinLTO), `-DGT4_TARGET=spec2 -DGT4_EE_PROFILE=spec2`, the rc15 Spec II recipe of
  `scripts/coregs2_bench/game/configure.cmd`: runtime `private/slice276-spec2-iop-runtime`, guard
  `private/slice376-frame-render-license-guard-runtime`, port `private/slice200-port`, shaders
  `private/slice153-pacing-shaders`, VU1 `private/slice397-vu1-gen`, VU0 `private/slice394-spec2-vu0-gen` and
  `private/slice390-spec2-vu0-image`, IPU `slice365`, SPU2 `slice396`, USB `slice409`, memcard `slice200`, EE library
  `private/slice407-ee-spec2/ps2game.lib`, `GT4_RELEASE_DISTRIBUTION=ON`. Output
  `private/slice416-spec2-build/build/gt4vulkan-spec2.exe` (97,212,416 bytes). No pin file: this build is evidence,
  not a release input.
- **v2.00 build:** the same recipe with `-DGT4_TARGET=v200 -DGT4_EE_PROFILE=aspect`, the `slice262` runtime,
  `slice394-v200-vu0-gen` and the aspect EE library; output `private/slice417-v200-build/build/gt4vulkan.exe`
  (90,161,664 bytes). It shares the Spec II shader-set hash and key format.
- **Harness:** `private/task205-tools/` (`press205`, `route205`, `survey205`, `modes205`, `prepare205`, `cars205`,
  `replay205`, `v200-gtmode-probe`, `night-aspec`), copied and adapted from the read-only TASK-171 harness
  (`t171-*.sh`). It boots headless from a copied card (the Spec II Arcade card, the v2.00 GT Mode/Arcade cards), forces
  480p/16:9, 1x and a fresh `GT4_HW_SHADER_CACHE` per run, and presses or replays a recording to the scene. With
  TASK-204 in the branch, the fresh cache has no `prepared.bin`, so the session compiles nothing: it records every
  draw state it asks for and draws the uber pipeline. That makes the run a pure recorder.
- The route and the 52 names are TASK-171's (`t171-route.sh`, `t171-names.tsv`). Each run is 1,200 VBlanks of race
  after the race start, about 45 s of wall time at 1x.

### 7.2 The 52 tracks

B-Spec with the Ford GT LM Race Car Spec II at car-list slot 17 for World, Original and City. The Dirt & Snow
category never starts a B-Spec race (three attempts ended at the menus with `vblanks_480p=0`), so the seven Dirt &
Snow tracks were rerun in A-Spec with slot 13 and the throttle held, as TASK-171's A-Spec list does. All 52 tracks
raced in one of the two passes.

| Track | Cat | Keys | New vs shipped | Unique to this track |
|---|---:|---:|---:|---:|
| Nürburgring Nordschleife | 0 | 80 | 8 | 0 |
| Laguna Seca | 0 | 76 | 11 | 0 |
| Tsukuba | 0 | 96 | 25 | 2 |
| Tsukuba (wet) | 0 | 81 | 16 | 1 |
| Fuji 90's | 0 | 79 | 13 | 0 |
| Fuji 80's | 0 | 80 | 14 | 0 |
| Fuji 2005 | 0 | 79 | 12 | 0 |
| Fuji 2005 GT | 0 | 79 | 12 | 0 |
| Infineon Sports | 0 | 99 | 28 | 1 |
| Infineon Stock Car | 0 | 93 | 22 | 0 |
| Motegi Road Course | 0 | 87 | 17 | 0 |
| Motegi East Short | 0 | 88 | 15 | 0 |
| Motegi West Short | 0 | 82 | 15 | 0 |
| Motegi Super Speedway | 0 | 84 | 18 | 0 |
| Suzuka | 0 | 84 | 14 | 0 |
| Suzuka East | 0 | 83 | 13 | 0 |
| Suzuka West | 0 | 76 | 10 | 0 |
| Circuit de la Sarthe I | 0 | 88 | 18 | 0 |
| Circuit de la Sarthe II | 0 | 83 | 16 | 0 |
| Valencia | 0 | 71 | 6 | 1 |
| El Capitan | 1 | 80 | 10 | 0 |
| High Speed Ring | 1 | 79 | 13 | 0 |
| Trial Mountain | 1 | 81 | 13 | 0 |
| Grand Valley Speedway | 1 | 85 | 17 | 0 |
| Grand Valley East | 1 | 92 | 20 | 0 |
| Apricot Hill | 1 | 77 | 12 | 0 |
| Autumn Ring | 1 | 87 | 20 | 0 |
| Autumn Ring Mini | 1 | 86 | 20 | 0 |
| Deep Forest | 1 | 86 | 18 | 0 |
| Motorland | 1 | 72 | 9 | 0 |
| Mid-Field | 1 | 83 | 15 | 1 |
| Test Course | 1 | 87 | 16 | 0 |
| Beginner Course | 1 | 81 | 12 | 0 |
| Costa di Amalfi | 2 | 61 | 3 | 1 |
| Paris George V | 2 | 76 | 10 | 1 |
| Opera Paris | 2 | 81 | 12 | 0 |
| Hong Kong | 2 | 93 | 19 | 1 |
| Citta di Aria | 2 | 67 | 8 | 0 |
| New York | 2 | 82 | 17 | 0 |
| Tokyo R246 | 2 | 86 | 19 | 0 |
| Cote d'Azur | 2 | 76 | 10 | 1 |
| Special Stage Route 5 | 2 | 84 | 12 | 0 |
| Clubman Stage Route 5 | 2 | 80 | 10 | 0 |
| Seattle | 2 | 84 | 13 | 0 |
| Seoul Central | 2 | 84 | 14 | 0 |
| Whistler Ice Arena | 3 | 66 | 8 | 0 |
| Grand Canyon | 3 | 73 | 12 | 2 |
| Chamonix | 3 | 69 | 9 | 0 |
| Cathedral Rocks I | 3 | 73 | 10 | 0 |
| Cathedral Rocks II | 3 | 77 | 14 | 3 |
| Swiss Alps | 3 | 73 | 11 | 0 |
| Tahiti Maze | 3 | 72 | 13 | 0 |
| **Union** | | **155** | **69** | **15** |

The counts are the variants.bin of each run as written at the end. A run's file is written at most every 10 s of wall
time and at the renderer's close; a `--frames` run exits through the runtime's halt, so the last few keys of a run
(usually 0-4) can be missing from its file. The union is what was written; a rerun of the same scene recovers the tail,
which is why the second pass and the A-Spec runs matter. This is a recorder limit, not a coverage result.

### 7.3 The 52-track counts

- 52 race runs (`tracks52-*` for 45, `dirt-*` for 7).
- `155` distinct keys; `69` not in the shipped 544; `4046` duplicates across runs.
- `38` new draw-state signatures; `117` of the shipped 144 signatures were seen.
- On this card (discrete RX 9060 XT: the plain path and the colour-attachment/ROP path are off), the 52-track candidate
  keeps `372` keys (the shipped list's 300 storage-path keys plus the 72 new). The shipped list's 167 plain and
  77 ROP keys are filtered by `variants_to_prepare` on this card, exactly as the run's own keys would be.
- **Cap:** `kMaxSpecPipelines` is 2048. The 52-track candidate is `616` keys, well under it. The final
  both-games candidate is in section 7.5.
- **Priority rule if it ever grows:** do not truncate silently. First apply proposal 1 of section 5 (one prepared
  scale at a time); if the list still approaches 2048, keep keys by (a) how many scenes ask for them, (b) how many uber
  batches they save, and move the rest to a second, optionally downloaded list. A candidate key used by one scene is
  worth less than a key used by 40.

The merge tool wrote the 52-track candidate to `private/task205-variants/candidate-1x.inc` (616 keys, one per row, the
shipped list plus the 72 new). It is deliberately under `private/`: the shipped `.inc` changes only in a release, after
the owner's review.

### 7.4 The modes, both games

| Run group | Runs | Keys per run | New vs 544 |
|---|---:|---|---:|
| GT4 v2.00 photo mode | 1 | 48 | 20 |
| GT4 v2.00 used-car showroom | 1 | 28 | 7 |
| GT4 v2.00 license test B-1 | 1 | 79 | 27 |
| GT4 v2.00 Arcade (Clubman, car showroom) | 1 | 76 | 8 |
| GT4 v2.00 GT Mode B-Spec night race | 1 | 96 | 31 |
| GT4 v2.00 GT Mode A-Spec night race | 1 | 68 | 14 |
| GT4 v2.00 GT Mode Casa menu | 1 | 1 | 0 |
| Spec II license test | 1 | 66 | 19 |
| Spec II used-car dealer/tuning display | 1 | 9 | 3 |
| Spec II photo lab (album) | 1 | 1 | 0 |
| Spec II license menu | 1 | 1 | 0 |
| Spec II GT Mode event screen | 1 | 1 | 0 |

**The v2.00 pass (7 runs, 142 distinct keys, 62 new against 544).** Photo mode (48 keys, 20 new: the first
`Zformat=Z16` keys, as IN-537 predicted), used-car showroom (28, 7), license test B-1 (79, 27), Arcade Clubman with
the car showroom (76, 8), GT Mode B-Spec night race (96, 31), GT Mode A-Spec night race (68, 14) and the Casa menu
(1, 0). The v2.00 build is `private/slice417-v200-build` (plain, `GT4_EE_PROFILE=aspect`, the rc15 recipe).

**The Spec II GT Mode modes.** License test (66 keys, 19 new), used-car dealer/tuning car display (9, 3), photo lab
album (1, 0), license menu (1, 0), GT Mode event screen (1, 0) and the split-screen menus (1, 0 each). The Spec II
save is on the bench card (`BASCUS-97436GAMEDATA`), so GT Mode is reachable without the owner's 100% save; the
`Casa` menu was mapped (Garagem, Lab. de Fotos, Diário, Estatísticas, Opções, Trocas, Salvar).

**Blocked or pending, with the exact blocker:**

| Mode | State | Blocker / what the owner must provide |
|---|---|---|
| Split-screen race | menu reached (`Tela Dividida`, category preview) | the 2P accept screen waits for a second controller; the harness feeds one pad. Provide a two-pad recording or a second-controller path |
| Replay | replay saves exist on both cards (Spec II `BASCUS-97436R5MW/R8MW/R8UW/B5MW`); no replay entry found in the GT Mode home (Casa has no Replay in v2.00 or Spec II) or the title menu | name the menu path, or record the button sequence that loads a saved replay |
| Spec II GT Mode race | event screen reached (`Eventos Europeus`, first event `0 Grand Tour`) | the rules-accept and car-entry sequence was not completed; the save has a Mitsubishi Lancer Evo. Provide a recording or the eligible event/car |
| Spec II photo mode | **disabled by the mod**: the map says `Modo Fotografia: Função Desativada no Spec II` | none; use v2.00 for photo mode (measured) |
| Spec II rotating showroom | used-car dealer and tuning car display captured | the rotating showroom proper was not separately reached; v2.00 used-car showroom measured |

The v2.00 GT Mode B-Spec and A-Spec runs cover the GT Mode race draw states; the Spec II Arcade 52 tracks cover the
Spec II race states. Photo mode is the only mode that added a new depth format (`Z16`), and only v2.00 exposes it.

### 7.5 The both-games union and the cap

| Group | Runs | Keys recorded (sum) | Distinct keys | New vs 544 |
|---|---:|---:|---:|---:|
| Spec II 1x survey | 67 | 5239 | 160 | 72 |
| Spec II modes | 7 | 80 | 66 | 19 |
| v2.00 | 7 | 396 | 142 | 62 |

**Final counts:** 81 runs, **191 distinct keys, 100 new against the 544, 60 new draw-state signatures**, 5,524 repeated
keys across runs. The union with the shipped list is **644 keys**; on this card **400 are kept** (300 storage-path
shipped keys + 100 new), after filtering the shipped list's 167 plain and 77 ROP keys. `kMaxSpecPipelines` is 2048, so
the cap is **not exceeded**. The 28 keys beyond the previous 616 are the v2.00/mode additions: `Zformat=Z16` (photo
and license), CT24/CT16 format states with Z24/Z16, `DateOn` blends, mip and a few blend/keep-top-byte combinations.

The merge tool wrote the final candidate to `private/task205-variants/candidate-final.inc` (644 keys, the shipped list
plus the 100 new). It is deliberately under `private/`: the shipped `.inc` changes only in a release, after the owner's
review. The 52-track candidate (`candidate-1x.inc`, 616) remains as the narrower list.

## 8. Limits and confidence

- **Confirmed by construction:** the key fields and their zeroing (`core2_gpu.cpp`); the file format; the scale
  projection (the 136/255 4x keys are a pure transformation of the shipped keys); the absence of 4x, SCANMSK and Z16
  in the shipped list; the Spec II photo-mode disable text.
- **High confidence:** the pattern that new keys are combinations, not new field values, in the 52 tracks (only the
  SCANSMK flag); the per-track counts and the 52-track union (the recorder's own files); the card filters (the run
  prints `colour attachment path off` and `[core2] shader cache` draws only storage keys); the v2.00 pass counts.
- **Measured on a subset:** A-Spec adds no key beyond B-Spec (three Spec II tracks); the car slots add 5 keys beyond
  the 52 tracks, including `Tfx=Highlight2`; the v2.00 modes add 28 keys beyond the previous 616, including `Z16`.
- **Hypothesis:** the prepare-side scale expansion will remove the scale family without a fragment cost (not
  implemented).
- **Unknown:** the exact key set of the player's rc15 session (his `variants.bin` is not in the repository); the
  integrated-GPU plain path and a card with the ROP path on; the full 2x/3x/4x/6x/12x sets; PAL/1080i; the blocked
  modes above.
- **Not measured:** the launcher's notice and export flow (design only); a real player list from another build (the
  merge tool's `--shader-set` refusal is tested by construction, not with a foreign file); the split-screen race, the
  replay loader and a Spec II GT Mode race.

## Appendix A: every new key

The class names the state; "fields that differ" is the diff against the nearest shipped key, with values. A row that
says "another scale, aniso or ROP twin" has the same state and flags as a shipped key and differs only in those words.

| # | Scene (one that uses it) | Class of the key | Fields that differ from the nearest shipped key |
|---:|---|---|---|
| 1 | GT4 v2.00 license test B-1 | format=CT32 tex=Modulate/CT16raw z=Always/Z16 scale=1 textured-image | Zmsk on->off, Zformat Z24->Z16, TexLinear on->off |
| 2 | Motegi Road Course | format=CT32 tex=Modulate/RGBA8 z=Always/Z24/masked scale=1 textured-image | push scale=2 textured_image->scale=1 textured_image |
| 3 | Motegi West Short | format=CT32 tex=Modulate/RGBA8 z=Always/Z24/masked scale=1 textured-image | push scale=3 textured_image depth_from_position->scale=1 textured_image |
| 4 | Nürburgring Nordschleife | format=CT32 tex=Modulate/RGBA8 z=Always/Z24/masked scale=1 textured-image scanmask | push scale=1->scale=1 scanmask.1 textured_image |
| 5 | GT4 v2.00 license test B-1 | format=CT32 tex=Modulate/Indexed z=Always/Z24/masked scale=1 scanmask | push scale=1->scale=1 scanmask.1 |
| 6 | Nürburgring Nordschleife | format=CT32 tex=Modulate/Indexed z=Always/Z24/masked scale=1 scanmask | push scale=1->scale=1 scanmask.1 |
| 7 | Tsukuba (wet) | format=CT32 tex=Modulate/RGBA8 blend z=Always/Z24/masked scale=1 textured-image | BlendC As->Ad |
| 8 | Motegi Road Course | format=CT32 tex=Modulate/RGBA8 blend z=Always/Z24/masked scale=1 textured-image | BlendB Cd->Zero |
| 9 | Motegi Road Course | format=CT32 tex=Modulate/RGBA8 blend z=Always/Z24/masked scale=1 textured-image | BlendB Cd->Zero, BlendC As->Ad |
| 10 | Tsukuba | format=CT32 tex=Modulate/RGBA8 blend z=Always/Z24/masked scale=1 | push scale=1 depth_from_position->scale=1 |
| 11 | Motegi Road Course | format=CT32 tex=Modulate/Indexed z=GreaterEqual/Z24 fog scale=1 | TexLinear on->off |
| 12 | Suzuka | format=CT32 tex=Modulate/Indexed z=GreaterEqual/Z24 fog scale=1 mip/aniso=0 | push scale=1->scale=1 mip |
| 13 | Tsukuba | format=CT32 tex=Modulate/Indexed z=GreaterEqual/Z24 fog scale=1 | push scale=1 depth_from_position->scale=1 |
| 14 | Infineon Sports | format=CT32 tex=Modulate/Indexed z=GreaterEqual/Z24 fog scale=1 mip/aniso=0 | push scale=1 depth_from_position->scale=1 mip |
| 15 | Mid-Field | format=CT32 tex=Modulate/Indexed z=GreaterEqual/Z24 fog scale=1 | push scale=1 depth_from_position->scale=1 |
| 16 | Nürburgring Nordschleife | format=CT32 tex=Modulate/RGBA8 blend z=GreaterEqual/Z24 fog scale=1 textured-image | push scale=1 textured_image depth_from_position->scale=1 textured_image |
| 17 | Laguna Seca | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24 fog scale=1 | TexLinear on->off |
| 18 | Infineon Sports | format=CT32 tex=Modulate/CT24raw z=GreaterEqual/Z24/masked scale=1 textured-image | push scale=3 textured_image depth_from_position->scale=1 textured_image |
| 19 | GT4 v2.00 GT Mode B-Spec night race | format=CT32 tex=Modulate/Indexed z=GreaterEqual/Z24/masked fog scale=1 mip/aniso=0 | push scale=1 mip depth_from_position->scale=1 mip |
| 20 | Tsukuba | format=CT32 tex=Modulate/Indexed z=GreaterEqual/Z24/masked fog scale=1 mip/aniso=0 | push scale=1 mip depth_from_position->scale=1 mip |
| 21 | Valencia | format=CT32 tex=Modulate/Indexed z=GreaterEqual/Z24/masked fog scale=1 | WrapT Repeat->RegionClamp |
| 22 | Tsukuba | format=CT32 tex=Modulate/Indexed z=GreaterEqual/Z24/masked fog scale=1 mip/aniso=0 | push scale=1->scale=1 mip |
| 23 | Tsukuba | format=CT32 tex=Modulate/Indexed z=GreaterEqual/Z24/masked fog scale=1 mip/aniso=0 | push scale=1->scale=1 mip |
| 24 | Grand Canyon (A-Spec) | format=CT32 untextured blend z=GreaterEqual/Z24/masked scale=1 | Ztst Always->GreaterEqual |
| 25 | Infineon Sports | format=CT32 tex=Modulate/CT24raw blend z=GreaterEqual/Z24/masked scale=1 textured-image | push scale=3 textured_image depth_from_position->scale=1 textured_image |
| 26 | Nürburgring Nordschleife | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24/masked scale=1 | push scale=1 depth_from_position->scale=1 |
| 27 | Nürburgring Nordschleife | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24/masked fog scale=1 | push scale=1 depth_from_position->scale=1 |
| 28 | Infineon Sports | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24/masked fog scale=1 | WrapS RegionClamp->Repeat |
| 29 | Tsukuba | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24/masked fog scale=1 mip/aniso=0 | push scale=1 mip depth_from_position->scale=1 mip |
| 30 | Tsukuba | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24/masked fog scale=1 mip/aniso=0 | push scale=1->scale=1 mip |
| 31 | Infineon Sports | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24/masked fog scale=1 mip/aniso=0 | push scale=1->scale=1 mip |
| 32 | Tsukuba | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24/masked fog scale=1 mip/aniso=0 | push scale=1->scale=1 mip |
| 33 | Fuji 2005 | format=CT32 tex=Modulate/RGBA8 blend z=GreaterEqual/Z24/masked scale=1 textured-image | Ztst Always->GreaterEqual |
| 34 | Paris George V | format=CT32 untextured blend z=GreaterEqual/Z24/masked scale=1 | push scale=1 depth_from_position->scale=1 |
| 35 | Laguna Seca | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24/masked fog scale=1 | BlendB Cd->Zero |
| 36 | Paris George V | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24/masked fog scale=1 | push scale=1 depth_from_position->scale=1 |
| 37 | Laguna Seca | format=CT32 untextured blend z=GreaterEqual/Z24/masked fog scale=1 | BlendB Cd->Zero, BlendC As->Ad |
| 38 | GT4 v2.00 photo mode | format=CT32 tex=Modulate/CT16raw blend z=GreaterEqual/Z24/masked fog scale=1 textured-image | push scale=1 textured_image depth_from_position->scale=1 textured_image |
| 39 | Mid-Field | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24/masked fog scale=1 mip/aniso=0 | push scale=1->scale=1 mip |
| 40 | Tsukuba | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24/masked fog scale=1 | push scale=1 depth_from_position->scale=1 |
| 41 | Tsukuba | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24/masked fog scale=1 mip/aniso=0 | WrapS Repeat->RegionClamp, push scale=1->scale=1 mip |
| 42 | GT4 v2.00 license test B-1 | format=CT32 untextured blend z=GreaterEqual/Z16/masked fog scale=1 | Zformat Z24->Z16, BlendB Cd->Zero, BlendC As->Ad |
| 43 | GT4 v2.00 license test B-1 | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z16/masked fog scale=1 | Zformat Z24->Z16 |
| 44 | Tokyo R246, car slot 5 | format=CT32 tex=Modulate/RGBA8 blend z=GreaterEqual/Z24/masked fog date scale=1 | DateOn off->on, BlendB Cd->Zero, BlendC As->Ad, TexOn off->on |
| 45 | GT4 v2.00 photo mode | format=CT32 tex=Modulate/CT16raw blend z=GreaterEqual/Z24/masked fog date scale=1 textured-image | push scale=1 textured_image depth_from_position->scale=1 textured_image |
| 46 | Hong Kong | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24/masked fog date scale=1 | push scale=1 depth_from_position->scale=1 |
| 47 | GT4 v2.00 GT Mode B-Spec night race | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24/masked fog date scale=1 | push scale=1 depth_from_position->scale=1 |
| 48 | GT4 v2.00 license test B-1 | format=CT32 tex=Modulate/Indexed blend z=GreaterEqual/Z24/masked fog date scale=1 | push scale=1 depth_from_position->scale=1 |
| 49 | Special Stage Route 5 | format=CT32 tex=Modulate/Indexed blend atest=Equal/FrameOnly z=GreaterEqual/Z24 fog scale=1 | push scale=1 depth_from_position->scale=1 |
| 50 | Motegi West Short | format=CT32 tex=Modulate/Indexed blend atest=GreaterEqual/Keep z=GreaterEqual/Z24/masked fog scale=1 | Ate off->on, Atst Never->GreaterEqual, push scale=1 depth_from_position->scale=1 |
| 51 | Nürburgring Nordschleife | format=CT32 tex=Modulate/Indexed blend atest=GreaterEqual/Keep z=GreaterEqual/Z24/masked fog scale=1 | TexLinear on->off |
| 52 | Nürburgring Nordschleife | format=CT32 tex=Modulate/Indexed blend atest=GreaterEqual/Keep z=GreaterEqual/Z24/masked fog scale=1 | Ate off->on, Atst Never->GreaterEqual |
| 53 | GT4 v2.00 GT Mode B-Spec night race | format=CT32 tex=Modulate/Indexed blend atest=GreaterEqual/Keep z=GreaterEqual/Z24/masked fog scale=1 | push scale=1 depth_from_position->scale=1 |
| 54 | Fuji 80's | format=CT32 untextured blend atest=Greater/Keep z=Always/Z24/masked scale=1 | Atst Equal->Greater |
| 55 | Motegi Super Speedway | format=CT32 tex=Modulate/CT24raw atest=Greater/Keep z=GreaterEqual/Z24 fog scale=1 | Tcc on->off, TexelKind Indexed->CT24raw |
| 56 | Cote d'Azur | format=CT32 tex=Modulate/CT24raw atest=Greater/Keep z=GreaterEqual/Z24 fog scale=1 | Tcc on->off, TexelKind Indexed->CT24raw |
| 57 | Grand Canyon (A-Spec) | format=CT32 tex=Modulate/Indexed atest=Greater/Keep z=GreaterEqual/Z24 scale=1 | FogOn on->off |
| 58 | Infineon Sports | format=CT32 tex=Modulate/Indexed atest=Greater/Keep z=GreaterEqual/Z24 fog scale=1 | WrapT Repeat->RegionClamp |
| 59 | Tsukuba | format=CT32 tex=Modulate/Indexed atest=Greater/Keep z=GreaterEqual/Z24 fog scale=1 mip/aniso=0 | push scale=1 mip depth_from_position->scale=1 mip |
| 60 | Tsukuba | format=CT32 tex=Modulate/Indexed atest=Greater/Keep z=GreaterEqual/Z24 fog scale=1 mip/aniso=0 | push scale=1->scale=1 mip |
| 61 | Tokyo R246, car slot 10 | format=CT32 tex=Highlight2/Indexed atest=Greater/Keep z=GreaterEqual/Z24 fog scale=1 | Tfx Modulate->Highlight2 |
| 62 | GT4 v2.00 license test B-1 | format=CT32 tex=Modulate/Indexed atest=Greater/Keep z=GreaterEqual/Z16 fog scale=1 | Zformat Z24->Z16 |
| 63 | Cathedral Rocks II (A-Spec) | format=CT32 tex=Modulate/Indexed atest=Greater/Keep z=GreaterEqual/Z24/masked fog scale=1 | Zmsk off->on |
| 64 | Costa di Amalfi | format=CT32 tex=Modulate/Indexed atest=Greater/Keep z=GreaterEqual/Z24/masked fog scale=1 | push scale=1 depth_from_position->scale=1 |
| 65 | Mid-Field | format=CT32 tex=Modulate/CT24raw blend atest=Greater/Keep z=GreaterEqual/Z24/masked scale=1 | Ate off->on, Atst Never->Greater, Tcc on->off, TexelKind Indexed->CT24raw |
| 66 | Motegi Super Speedway | format=CT32 untextured blend atest=Greater/RgbOnly z=GreaterEqual/Z24 fog scale=1 | push scale=1 depth_from_position->scale=1 |
| 67 | Infineon Sports | format=CT32 tex=Modulate/Indexed blend atest=Greater/RgbOnly z=GreaterEqual/Z24 fog scale=1 | WrapT Repeat->RegionClamp |
| 68 | Tsukuba | format=CT32 tex=Modulate/Indexed blend atest=Greater/RgbOnly z=GreaterEqual/Z24 fog scale=1 mip/aniso=0 | push scale=1 mip depth_from_position->scale=1 mip |
| 69 | Autumn Ring Mini | format=CT32 tex=Modulate/Indexed blend atest=Greater/RgbOnly z=GreaterEqual/Z24 fog scale=1 mip/aniso=0 | push scale=1 mip depth_from_position->scale=1 mip |
| 70 | Circuit de la Sarthe I | format=CT32 tex=Modulate/Indexed blend atest=Greater/RgbOnly z=GreaterEqual/Z24 fog scale=1 | WrapS Repeat->RegionClamp |
| 71 | Tsukuba | format=CT32 tex=Modulate/Indexed blend atest=Greater/RgbOnly z=GreaterEqual/Z24 fog scale=1 mip/aniso=0 | WrapS Repeat->RegionClamp, push scale=1->scale=1 mip |
| 72 | Tsukuba | format=CT32 tex=Modulate/Indexed blend atest=Greater/RgbOnly z=GreaterEqual/Z24 fog scale=1 mip/aniso=0 | push scale=1->scale=1 mip |
| 73 | GT4 v2.00 photo mode | format=CT32 tex=Modulate/RGBA8 atest=NotEqual/Keep z=Always/Z24 scale=1 textured-image | Zmsk on->off |
| 74 | GT4 v2.00 license test B-1 | format=CT24 tex=Modulate/CT24raw blend z=Always/Z32/masked scale=1 textured-image | push scale=3 textured_image depth_from_position->scale=1 textured_image |
| 75 | GT4 v2.00 photo mode | format=CT24 tex=Modulate/RGBA8 z=Always/Z24/masked scale=1 textured-image | TexLinear on->off, push scale=3 textured_image depth_from_position->scale=1 textured_image |
| 76 | Infineon Sports | format=CT24 tex=Modulate/RGBA8 z=Always/Z24/masked scale=1 textured-image | push scale=3 textured_image depth_from_position->scale=1 textured_image |
| 77 | Infineon Sports | format=CT24 tex=Modulate/CT24raw z=Always/Z24/masked scale=1 textured-image | push scale=3 textured_image depth_from_position->scale=1 textured_image |
| 78 | GT4 v2.00 GT Mode A-Spec night race | format=CT24 untextured blend z=Always/Z24/masked scale=1 | Zformat Z32->Z24 |
| 79 | GT4 v2.00 GT Mode A-Spec night race | format=CT24 tex=Modulate/Indexed blend z=Always/Z24/masked scale=1 | Zformat Z32->Z24 |
| 80 | GT4 v2.00 GT Mode A-Spec night race | format=CT24 tex=Modulate/Indexed blend z=Always/Z24/masked scale=1 | Zformat Z32->Z24 |
| 81 | Infineon Sports | format=CT24 untextured blend z=Always/Z24/masked scale=1 | push scale=1 depth_from_position->scale=1 |
| 82 | GT4 v2.00 GT Mode A-Spec night race | format=CT24 tex=Modulate/Indexed blend z=Always/Z24/masked scale=1 | Zformat Z32->Z24 |
| 83 | GT4 v2.00 license test B-1 | format=CT24 tex=Modulate/RGBA8 blend z=Always/Z24/masked scale=1 textured-image | push scale=3 textured_image depth_from_position->scale=1 textured_image |
| 84 | GT4 v2.00 photo mode | format=CT24 untextured blend z=Always/Z16/masked scale=1 | Zformat Z32->Z16 |
| 85 | GT4 v2.00 photo mode | format=CT24 tex=Modulate/Indexed blend z=Always/Z16/masked scale=1 | Zformat Z32->Z16 |
| 86 | GT4 v2.00 photo mode | format=CT24 tex=Modulate/Indexed blend z=Always/Z16/masked scale=1 | Zformat Z32->Z16 |
| 87 | GT4 v2.00 photo mode | format=CT24 untextured blend z=GreaterEqual/Z16 scale=1 | Ztst Always->GreaterEqual, Zmsk on->off, Zformat Z32->Z16 |
| 88 | GT4 v2.00 license test B-1 | format=CT24 tex=Modulate/Indexed atest=Greater/Keep z=GreaterEqual/Z24 fog scale=1 | push scale=1 depth_from_position->scale=1 |
| 89 | GT4 v2.00 photo mode | format=CT24 tex=Modulate/Indexed atest=Greater/Keep z=GreaterEqual/Z16 fog scale=1 | Format CT32->CT24, Zformat Z24->Z16, KeepTopByte off->on |
| 90 | GT4 v2.00 license test B-1 | format=CT24 tex=Modulate/Indexed blend atest=Greater/RgbOnly z=GreaterEqual/Z24 fog scale=1 | push scale=1 depth_from_position->scale=1 |
| 91 | GT4 v2.00 license test B-1 | format=CT16 tex=Modulate/RGBA8 blend scale=1 textured-image | Format CT32->CT16 |
| 92 | Nürburgring Nordschleife | format=CT16 untextured blend z=Always/Z32/masked scale=1 | push scale=1 depth_from_position->scale=1 |
| 93 | GT4 v2.00 license test B-1 | format=CT16 tex=Modulate/Indexed blend z=Always/Z32/masked scale=1 | TexFst off->on |
| 94 | GT4 v2.00 photo mode | format=CT16 tex=Modulate/RGBA8 z=Always/Z24/masked scale=1 textured-image | Format CT32->CT16 |
| 95 | Tsukuba (wet) | format=CT16 tex=Modulate/RGBA8 blend z=Always/Z24/masked scale=1 | push scale=1 textured_image depth_from_position->scale=1 |
| 96 | GT4 v2.00 GT Mode B-Spec night race | format=CT16 tex=Modulate/RGBA8 blend z=Always/Z24/masked scale=1 textured-image | push scale=1 textured_image depth_from_position->scale=1 textured_image |
| 97 | Grand Valley East | format=CT16 tex=Modulate/CT16raw blend z=Always/Z24/masked scale=1 | push scale=1 depth_from_position->scale=1 |
| 98 | GT4 v2.00 Arcade (Clubman, car showroom) | format=CT16 tex=Modulate/Indexed blend z=Always/Z24/masked scale=1 | push scale=1 depth_from_position->scale=1 |
| 99 | GT4 v2.00 Arcade (Clubman, car showroom) | format=CT16 untextured blend z=Always/Z24/masked scale=1 | push scale=1 depth_from_position->scale=1 |
| 100 | GT4 v2.00 license test B-1 | format=CT16 untextured blend z=Always/Z24/masked scale=1 | Format CT32->CT16, BlendB Zero->Cs, BlendD Zero->Cs |

## Appendix B: the run record

| Track | Run | Frames | 480p VBlanks | Deferred keys | variants.bin keys |
|---|---|---:|---:|---:|---:|
| Nürburgring Nordschleife | `tracks52-0-00` | 5540 | 3608 | 80 | 80 |
| Laguna Seca | `tracks52-0-01` | 5552 | 3608 | 77 | 76 |
| Tsukuba | `tracks52-0-02` | 5564 | 3608 | 102 | 96 |
| Tsukuba (wet) | `tracks52-0-03` | 5576 | 3608 | 81 | 81 |
| Fuji 90's | `tracks52-0-04` | 5588 | 3608 | 82 | 79 |
| Fuji 80's | `tracks52-0-05` | 5600 | 3608 | 87 | 80 |
| Fuji 2005 | `tracks52-0-06` | 5612 | 3608 | 80 | 79 |
| Fuji 2005 GT | `tracks52-0-07` | 5624 | 3608 | 80 | 79 |
| Infineon Sports | `tracks52-0-08` | 5636 | 3608 | 102 | 99 |
| Infineon Stock Car | `tracks52-0-09` | 5648 | 3608 | 102 | 93 |
| Motegi Road Course | `tracks52-0-10` | 5660 | 3608 | 88 | 87 |
| Motegi East Short | `tracks52-0-11` | 5672 | 3608 | 88 | 88 |
| Motegi West Short | `tracks52-0-12` | 5684 | 3608 | 82 | 82 |
| Motegi Super Speedway | `tracks52-0-13` | 5696 | 3608 | 86 | 84 |
| Suzuka | `tracks52-0-14` | 5708 | 3608 | 86 | 84 |
| Suzuka East | `tracks52-0-15` | 5720 | 3608 | 86 | 83 |
| Suzuka West | `tracks52-0-16` | 5732 | 3608 | 76 | 76 |
| Circuit de la Sarthe I | `tracks52-0-17` | 5744 | 3608 | 89 | 88 |
| Circuit de la Sarthe II | `tracks52-0-18` | 5756 | 3608 | 83 | 83 |
| Valencia | `tracks52-0-19` | 5768 | 3608 | 71 | 71 |
| El Capitan | `tracks52-1-00` | 5640 | 3608 | 85 | 80 |
| High Speed Ring | `tracks52-1-01` | 5652 | 3608 | 79 | 79 |
| Trial Mountain | `tracks52-1-02` | 5664 | 3608 | 82 | 81 |
| Grand Valley Speedway | `tracks52-1-03` | 5676 | 3608 | 84 | 85 |
| Grand Valley East | `tracks52-1-04` | 5688 | 3608 | 92 | 92 |
| Apricot Hill | `tracks52-1-05` | 5700 | 3608 | 78 | 77 |
| Autumn Ring | `tracks52-1-06` | 5712 | 3608 | 87 | 87 |
| Autumn Ring Mini | `tracks52-1-07` | 5724 | 3608 | 90 | 86 |
| Deep Forest | `tracks52-1-08` | 5736 | 3608 | 87 | 86 |
| Motorland | `tracks52-1-09` | 5748 | 3608 | 72 | 72 |
| Mid-Field | `tracks52-1-10` | 5760 | 3608 | 83 | 83 |
| Test Course | `tracks52-1-11` | 5672 | 3608 | 87 | 87 |
| Beginner Course | `tracks52-1-12` | 5784 | 3608 | 81 | 81 |
| Costa di Amalfi | `tracks52-2-00` | 5640 | 3608 | 61 | 61 |
| Paris George V | `tracks52-2-01` | 5652 | 3608 | 77 | 76 |
| Opera Paris | `tracks52-2-02` | 5664 | 3608 | 83 | 81 |
| Hong Kong | `tracks52-2-03` | 5676 | 3608 | 98 | 93 |
| Citta di Aria | `tracks52-2-04` | 5688 | 3608 | 67 | 67 |
| New York | `tracks52-2-05` | 5700 | 3608 | 86 | 82 |
| Tokyo R246 | `tracks52-2-06` | 5712 | 3608 | 88 | 86 |
| Cote d'Azur | `tracks52-2-07` | 5624 | 3608 | 77 | 76 |
| Special Stage Route 5 | `tracks52-2-08` | 5736 | 3608 | 89 | 84 |
| Clubman Stage Route 5 | `tracks52-2-09` | 5748 | 3608 | 80 | 80 |
| Seattle | `tracks52-2-10` | 5760 | 3608 | 85 | 84 |
| Seoul Central | `tracks52-2-11` | 5772 | 3608 | 84 | 84 |
| Whistler Ice Arena | `dirt-3-00` | 5640 | 3617 | 66 | 66 |
| Grand Canyon | `dirt-3-01` | 5652 | 3617 | 73 | 73 |
| Chamonix | `dirt-3-02` | 5664 | 3617 | 69 | 69 |
| Cathedral Rocks I | `dirt-3-03` | 5676 | 3617 | 73 | 73 |
| Cathedral Rocks II | `dirt-3-04` | 5688 | 3617 | 77 | 77 |
| Swiss Alps | `dirt-3-05` | 5700 | 3617 | 73 | 73 |
| Tahiti Maze | `dirt-3-06` | 5712 | 3617 | 72 | 72 |
