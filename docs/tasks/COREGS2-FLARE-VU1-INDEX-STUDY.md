# CoreGS 2.0: flare path 1 - the VU1 light-index route (microcode study, 2026-11-11)

Scope: can the VU1 carry a **light index** into a field the render pipeline ignores, so a draw variant can sample the
64x8 flare table per light without a GS readback? No shader code was written; this note is the study.

Labels: **C** confirmed from code or a measurement in the tree, **H** hypothesis with the experiment named, **U** unknown.

## 1. What the renderer already receives per vertex (C)

The GIF front end decodes XYZ2/XYZF2/XYZ3 kicks into `hwgs::Vertex` (`native/hw_gs/frontend/hw_gif_parser.cpp:36`,
`DecodeRegisterXyz`): `x4, y4, z, rgba, fog (uint8), q, s, t, u, v`. The vertex becomes `gpu::GpuVertex`
(`native/hw_gs/writeback/hw_gs_writeback_gpu.hpp`, 32 bytes, static-asserted) with **F at byte 28** and a spare pad byte
at 29, and the pipeline reads F from byte 28.

**So a per-vertex byte already flows from the GIF stream to the renderer and to the shader**: no record, layout or
pipeline change is needed to *carry* an index; only a producer is missing.

## 2. Is F free for the flare draws? (C for the pipeline, H for these draws)

`native/hw_gs/shaders/hw_pixel_core.glsl:262-271`: the fog mix runs only when `fog_on` (FGE is set). With **FGE = 0** the
F byte is ignored by the colour math, so writing an arbitrary value there cannot change a pixel.

- **C**: the pipeline ignores F when FGE = 0.
- **H**: the flare sprites are drawn with FGE = 0. Experiment (cheap, one diagnostic run): print
  `state.fields[gpu::gpufield::Fog]` and the F byte of every vertex for the four flare draws (the existing
  `GT4_CORE2_FLARE_TRACE_DRAWS` block already walks them). If any flare draw has FGE = 1, this route is closed for it and
  the fallback is the marker in section 4.

## 3. What the VU1 would have to write (U: the emit point)

- **C**: the four REF windows are consumed by **one** MSCALF invocation; the light index lives in the *window address*,
  not in the draw order (see `COREGS2-FLARE-REF-MAPPING.md`, section 2), and one invocation draws both lights' meshes
  interleaved. So the index must come from the program's own state (the unrolled per-light block).
- **C**: the flare vertices reach the GS as XYZ2-class kicks, which carry **no F** (`kick.hasFog` false), so today the
  F byte is 0 for them.
- **U**: the exact emit instructions. The program is the game's VU1 microcode (XOR-encrypted in the ELF,
  `docs/VU1-MICROCODE-SOURCE.md`; translated to C++ by `tools/vu_recomp`, generated tree
  `private/slice397-vu1-gen`). The patch would change one opcode per flare vertex from XYZ2 to **XYZF2/XYZ3** and supply
  `F = light` from the register the unrolled block already uses. Finding that block needs the four window addresses:
  they are the UNPACK destinations of the four REF packets (payload offsets 11,720 / 12,752 / 14,760 / 15,792 measured
  2026-11-10); **extracting the destination word from each packet is the first half-day of the implementation**, because
  the translated C++ is searched by address.

## 4. Fallback: a marker the renderer can see (C: mechanism, H: cost)

If F is not free, the VU1 can write a **harmless GS register** (e.g. a `TEX0`/`CLAMP` write restored immediately, or a
`PRIM` write that the following kick overwrites) before each light's sprites. The renderer already parses every register
write (`FrontEnd::WriteRegister`), so a marker is a few lines there. Cost: one extra GIF packet per light per frame
(4 x ~2 qwords), i.e. negligible; risk: any register write that is not byte-exactly restored changes the draw state, so
it must be proven by the identity gate on a scene where the marker is absent.

## 5. Verdict

**Conditionally yes:** a route exists and it is cheaper than the original FlareLight record - the carrier (the F byte at
`GpuVertex` offset 28) is already plumbed from the GIF stream to the shader, and it is free whenever FGE = 0. It needs
(a) the FGE/fog measurement of section 2 and (b) the VU1 emit patch of section 3. It does **not** need a new record, a new
descriptor, a push-word change or a pipeline layout change; the draw variant only reads the F byte it already receives.

### 5a. Step 1 measured (2026-11-11): **the carrier holds**

`GT4_CORE2_FLARE_TRACE_DRAWS` now prints each draw's `fge=` (PRIM.FGE from the translated state) and `fog=` (the F byte of
the draw's first `GpuVertex`). Night race 3x, VBlank 9,600, the last flare job:

- **680 small draws (<= 13 px)**, and **`fge = 0` on every one of them**;
- their F bytes are already populated and vary per draw (201, 202, 205, 206, 209, 210, 214, 215) - the game writes fog
  values even with fog disabled, so the byte is live and ignored by `hw_pixel_core.glsl:262-271`.

So the pixel pipeline cannot see a change there, and the renderer already receives the byte.

### 5b. Step 2 blocker, reported instead of a half-wired patch

The "extract the four REF window addresses from the packet UNPACK words" step did not resolve as planned, and the
structure is not stable enough to patch blind:

- the word before the 16 data bytes is **`cmd=00000001`**, not a VIF UNPACK command, so the window's VU address is not in
  the payload around the REF (`apply_table_patches` trace, 2026-11-11);
- the REF-to-invocation pairing **changes between frames**: at VBlank 9,600 the four REFs went to one MSCALF (packet 542),
  while in the same run at a later frame they went to **210 / 227 / 266 / 266** with `next_code=15` (MSCALF). So the
  program consumes the windows at different points per frame, and "the block after the marker" is not a fixed location.

What that means for the patch: it must be written against the *program's own read of each window* (an LQ from the window
address) rather than against the invocation boundary. Finding those reads needs the VU addresses, which the payload does not
carry in the form the trace can see - the next attempt has to decode them from the VIF chain in `walk_vif_batch` (the REF
command word before the data, which `apply_table_patches` does not receive today).

**So: no VU1 patch was written this round.** Writing one without the addresses would be a guess against the guest's
microcode; the FGE measurement (this section) is the durable result. Remaining price for steps 2-4: 3-4 days.

**Price: 3-5 days**, broken down:

| step | content | time |
|---|---|---|
| 1 | FGE/fog diagnostic on the four flare draws (one run, no code change beyond a trace print) | 0.5 d |
| 2 | extract the four window addresses and locate the flare emit block in the translated VU1 C++ | 1 d |
| 3 | the VU1 patch (XYZ2 -> XYZF2/XYZ3 with F = light) plus its identity gate with the shader off | 1-1.5 d |
| 4 | the draw variant: sample the 64x8 table word from F, multiply colour or alpha (P1), behind `GT4_CORE2_GPU_FLARE` | 1-1.5 d |

**Picture gate** (unchanged from the S4 plan): night 3x and 6x, Arcade, 10x/12x stress points; `flare_readbacks` = 0;
flare pixels within 1 LSB of the CPU-readback frame; every other pixel byte-identical; the forced-word cross-check at
three VBlanks (words 0/1 hidden vs all-visible) to prove the shader reads the texel the CPU path would; the switch off must
reproduce today's frame exactly.

**Risk:** the VU1 patch changes the *guest* microcode, so it must be a display-only patch (no EE-visible data), and the
table's sampling point is late in the frame (the occlusion result is produced after the scene depth), so the shader must
sample the same frame's table - which it does, because the draw is recorded after the table draw in each frame.

**Status:** study delivered, **no code**. The flare stays paused until the owner approves steps 1-4; step 1 alone (0.5 d)
decides whether the route survives.
