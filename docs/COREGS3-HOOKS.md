# CoreGS 3.0 phase 1: the observe-only EE hooks and the `.gt4scene` capture

Date: 2026-11-05. State after the first implementation round of TASK-090 phase 1 (`docs/COREGS3-PLAN.md` §4, §5.2, §8).
Scope of this document: what the hooks are, where they sit, what they may read, what the capture contains, and what the
first real captures proved and did not prove. Labels follow the plan: **C** Confirmed, **H** High confidence, **Hyp**
Hypothesis, **U** Unknown.

## 1. What was built

| Piece | File | Kind |
|---|---|---|
| Hook entry points, switch, capture writer, counters | `native/coregs3/coregs3_hooks.{hpp,cpp}` | host code linked into `gt4vulkan` behind `GT4_COREGS3` |
| Scene snapshot and `.gt4scene` format | `native/coregs3/coregs3_scene.{hpp,cpp}` | pure data, shared with the offline tools |
| Bounded VIF decoder | `native/coregs3/coregs3_vif.{hpp,cpp}` | pure C++, no game data |
| Shape channels, default role rule, mesh | `native/coregs3/coregs3_shape.{hpp,cpp}` | pure C++ |
| CPU reference rasteriser | `native/coregs3/coregs3_render.{hpp,cpp}` | pure C++ |
| PNG writer (stored DEFLATE) | `native/coregs3/coregs3_png.{hpp,cpp}` | pure C++, test tool only |
| Offline tool | `native/tools/coregs3_render_main.cpp` (`gt4_coregs3_render`) | test tool only |
| Unit insertion | `scripts/coregs3_units.py` | copies translated units, inserts the calls, proves byte-exact removal |
| Identity gate | `scripts/coregs3_bench/coregs3-check.sh` | three headless replays, clock/digest/state/frame identity |

`GT4_COREGS3=off|shadow|overlay` selects the mode. `off` (the default) makes every entry point one branch.
`shadow` captures and renders nothing visible. `overlay` **refuses to start**: the live seam is not in this build, and
refusing is better than silently running in another mode.

## 2. Hook sites and what each one reads

| Id | Guest site | Insertion | Reads |
|---|---|---|---|
| K1 pre | `0x00455A5C` (`jal 0x0049D308` of `ModelSet2___render`) | before the call statement, in `unit_00D5.cpp` | a0 = the shape's `Cmd`; PGL context at scratchpad `0x70002000` (sparse spans); VU0 `vf24..vf31` |
| K1 post | `0x00455A64` (the continuation) | after the label | the eight qwords `func_0049E548` wrote at the PGL output cursor (the matrix packet VU1 receives at `0xC1`) |
| K3 | `L_0033EBB4` (after the projection virtual call) | at the label | PGL `+0xFA0` block, camera object address, VU0 `vf24..vf31` |
| K4 | `L_0033EBF4` (after the modelview virtual call) | at the label | the same at the modelview point |
| K5 begin | `L_0033EE28` (block start before `vt+0x380`) | at the label | pass counter |
| K5 end | `L_0033EE54` (return of `vt+0x380` = `0x00340950`) | at the label | pass counter |

The bracket is confirmed against the decompiled `func_0033EAB8`: the block between the two labels contains exactly the
`vt+0x380` call, and the vtable entry resolved at run time is `0x00340950` (`RacePS2Base` `render_cars`), read from
`*(self+0x64) + 0x380 + 4` at the hook (**C**, measured: the trace prints `cars(+380) 00340950`).

**K2 (`func_0049CEA0`) is intentionally not hooked.** The item's `event` halfword already carries the 9-bit texture index
and the 7-bit material index, and the record arrays plus their external cursors are guest globals
(`0x0084AF58/5C/60/64`, `0x00624C7C/80`). The capture resolves exactly the addresses `func_0049CEA0` would resolve, from the
same words, without a second hook. This is a deliberate deviation from the plan's hook table and is recorded here.

## 3. The identity guarantee

- The hooks read guest RAM and scratchpad through bounds-checked copies and read `Cpu` fields; they never write guest
  memory, never call guest code and never touch `c.budget`, so no guest cycle is added or removed.
- Every bound is checked before a read; a read failure is counted (`guest_read_failures`) and never guessed around.
- A frame that hits a capacity limit is refused as a whole (`refused_frames`) and counted per cause
  (`CaptureCounters::overflow`), never truncated.
- `GT4_COREGS3=off` leaves the capture file closed and every entry point one branch; the identity gate compares that run
  against a build without the hooks.

Frame boundaries: `at_vblank` (the donor's per-VBlank callback) closes the frame the guest just finished and opens the
next one. The capture therefore holds exactly one VBlank interval per frame whatever the guest's render ordering is. A
run that never installs a VBlank callback would see no frames; `native/window.cpp` installs one in all three headless
paths (frame-skip, input replay, plain) and in the window.

## 4. The `.gt4scene` format

Little endian. Header: magic `GT4S`, version, target (0 = v2.00), reserved. Then frame blocks, each prefixed with its own
byte length so a reader can skip one it does not understand. A frame holds: index, guest VBlank, the camera record,
texture records (40 bytes each, deduplicated by guest address), material records (80 bytes each), the draw records and one
packet block with every item's VIF bytes. The draw record holds the `Cmd` header, up to 256 items (guest address, QWC,
`event`, resolved record indices, packet offset/length), the sparse PGL spans, `vf24..vf31`, and the matrix packet read
back after the call. `native/coregs3/coregs3_scene.hpp` is the authority; `coregs3_fixture.cpp` pins the round trip.

Format **version 2** (2026-11-09) adds the per-draw VU1 light block: the qwords the shape call uploads to VU1
`TOPS+0xC9` through a DMA REF tag (`func_0049D328` writes the tag into the PGL output packet buffer; its VIF UNPACK word
names the variant and its address word points at the shape's stack). The record stores up to 16 qwords, the qword count
(0 when the draw carried no such upload), the raw UNPACK command and the source address. The observed variants are six
qwords (`0x6C06C0C9`, the body/paint form: `0xC9..0xCB` light matrix, `0xCC` light position, `0xCD` C3, `0xCE` C4), nine
(`0x6C09C0C9`, `func_0049E698`) and three (`0x6C03C0C9`). A version 1 file has shorter draw records and is refused, not
migrated: captures are diagnostic and are recaptured with a matching build. The offline reader
(`build/analysis-lighting/gt4io.py`) follows the same layout.

Format **version 3** (2026-11-09) adds the per-draw VU1 program selector. `func_0049D328` writes one REF tag per item and
a terminating tag after the loop; word 2 of each tag is a VIF ITOP (`0x04000000 | (obj->x + obj->y)`) and word 3 a VIF
MSCAL (`0x14000000 | n`, `n` from `obj->count` at PGL+0x14 and the Cmd flag/bits). The first item's tag carries the
initial STMASK, the tags written at the end of each iteration carry the next item's ITOP/MSCAL, and the terminating tag
(the last qword before the post-call output cursor) carries the last item's pair. The record stores both raw words, the
MSCAL immediate and its byte entry (`* 8`), the ITOP dispatch entry (`(itop & 0x3FF) * 8`), `obj->count` and a validity
flag. `n` comes from `obj->count` (PGL+0x14) and the Cmd flag/bits. The MSCAL entry is a stub that sets the shared-code
return address (vi15) and the 0xD9 block pointer (vi14) and dispatches to the ITOP entry; the ITOP operands (+0x18/+0x1C)
are written inside the call by `func_0049D080` and its case helpers, so the ITOP entry is the real per-shape program and
encodes the material/program variant (the pre-call `pgl[6]/pgl[7]` are the previous call's values and must not be read as
this draw's ITOP). The six-qword body path, for example, is ITOP 0x20B -> entry 0x1058 -> shared prologue 0x1908 -> loop
0x10A8, the 0x10A0 lighting fragment. Versions 1 and 2 are refused.

Captures are derived from the user's own disc and stay under `build/evidence/` (ignored). Nothing is committed.

## 5. What the first captures show

Scene A: `licenca-b1` (v2.00, the plan's P1-A). Scene B: `night-run.sh` (P1-B, GT Mode B-Spec, six cars).

| Measurement | Scene A | Scene B |
|---|---|---|
| shape calls per frame | ~220 (course and scenery) | ~250 |
| draws inside the K5 bracket | **0** during the timed run | **116** |
| textures / materials per bracket frame | – | 169 / 39 |
| packet bytes per bracket frame | – | ~182 KB |

**Scene A has no car in the main view.** The reference frame at VBlank 2060 is a bumper camera: the road, the HUD and the
back-mirror strip, no car body. `render_cars` is called (the bracket runs) and returns without a single `pgluCallShape`
call. The car only appears at the result screen (VBlank ~3000), which is a different pass. So P1-A is a valid **identity**
scene (hooks do nothing) but not a **car** scene. Scene B is the car scene, as the plan's P1-B expected.

Channel layout of a car shape (`night-run`, VBlank 7398, draw 0, 3 items): per item one `V3-32` single vector at TOPS qword
`0xC0` (auxiliary: the first lane is the vertex count), then `V3-16` positions at TOPS `0x00`, `V4-8` at TOPS `0x80`, and
`V1-8` at TOPS `0x40` (**C** from the capture; that last channel is the strip table, decoded in §6). Some parts use
`V3-8` compressed normals at TOPS `0x80` instead of the `V4-8` colour, and the wheel/tyre parts use float `V3-32`
positions at absolute qword `0`. The course shapes add `V2-16`/`V2-32` UVs at TOPS `0x40` and use absolute `0xC0`-range
channels as well.

The matrix: the eight qwords at the PGL output cursor, read back after the call, are exactly
`tag, rows 0-2 × value, vf27, m2[0..3]` (**C**, the tag is `0x10000008`/`0x6C08C0C1`). Rows 0-2 are scaled by the shape's
`value` (measured classes on VBlank 7398: 0.000232830644, 0.000152592547 and 6.10370189e-05, i.e. `4096 * value` =
0.954, 0.625 and 0.250; a `value` of exactly zero takes the upload's unscaled branch) and row 3 (`vf27`) is **not**
scaled: in the observed car frames it holds a world position (~`18.6, 86.1, 138.4`), not a projection row. The
`+0xFA0` block is not a projection matrix in these frames either (it holds screen-centre-like 2D state), so its role
stays **U**. The VU1 vertex formula (which register is the projection, and
the exact clip mapping) is therefore still open: plan step 1d's tap is the instrument that will settle it.

The positions themselves are right: a point-cloud render of the captured car geometry (offline, no guest involvement)
shows the car silhouette. Triangle topology (strip vs list, and the strip table's location) is **not** solved: both
linear readings produce wrong surfaces, so the offline milestone picture is a point cloud, labelled as such. The offline
tool keeps every mode behind a flag so the next round can compare.

## 6. The VU1 tap (step 1d)

`native/coregs3/coregs3_tap.{hpp,cpp}` records what the VU1 microcode actually produced. The recording point is our own
MTVU adapter: `worker_gif` in `native/mtvu_runtime.cpp` receives every PATH1 (XGKICK) emission with its job id, and the
synchronous path records there too. The tap copies the bytes after the runtime accepted them and never writes back, so it is
observe-only like the EE hooks. It is gated to the capture's VBlank window (`GT4_COREGS3_TAP_VBLANKS`, defaulting to
`GT4_COREGS3_CAPTURE_VBLANKS`), bounded to 96 MB, and stops with a counter when the bound is reached; an unconfigured tap
is one branch on the normal path.

File `.gt4tap`: header `GT4T`/version, then `u32 bytes, u32 reserved, u64 job, u64 vblank, data[bytes]` per emission.

Reader and analysis: `native/coregs3/coregs3_gif.{hpp,cpp}` (a bounded GIF parser following CoreGS 2.0's own front end:
GIFtag NLOOP/EOP/PRE/PRIM/FLG/NREG/REGS, PACKED and REGLIST, XYZ2/XYZF2 with the ADC bit at high bit 47, RGBAQ, ST, UV)
and the tool `gt4_coregs3_tap` (`stats`, `dump`, `fit`).

First measurement (night race, VBlanks 7398-7399, `build/evidence/coregs3/m3/cars.gt4tap`, 3.9 MB):

| Quantity | Value |
|---|---|
| emissions (GIF packets) | 2,573 |
| vertices | 78,326 |
| vertices the GS draws (ADC = 0) | 23,705 (30%) |
| vertices marked ADC | 54,621 (70%) |
| jobs | 60, one per captured draw |

The 70% ADC share matches the plan's independent expectation for race PATH1 data ("70-80% of race vertices are ADC
no-draw", §2), which is a strong sign that the tap sees the same stream the GS sees. Per job the vertex totals match the
captured draws' position counts (e.g. job 460209 = 80 vertices = draw 1's 80), so a job is one draw, split into one or
more XGKICKs (a 56-vertex and a 24-vertex GIF packet in that example).

**The coordinate space** (owner hint, confirmed against our own front end, `hw_front_end.cpp:621`): GS XYZ is 12.4 fixed
point and the screen pixel is `(X - XYOFFSET.OFX) / 16`, `(Y - OFY) / 16`. A constant offset is *absorbed* by the
projective fit, so it was never the blocker; what the tap showed instead was that the recorded XYZ of a whole frame still
wraps over the full 16-bit range, i.e. some records are not vertex data the way this parser reads them. Two further
measurements narrowed it down:

- **The model decode is right.** Draw 3 of the captured frame has full 3D positions (e.g. `(-3042, 3413, -3207)`), while
  draws 1 and 2 are genuinely flat (`y == 0`): they are the car's ground shadows and decals. Earlier object-space
  pictures were dominated by those flat parts, which is why they looked like plates. Rendering the body only (height
  filter, one car) gives a recognisable car silhouette — the surface noise in it is the **topology**, not the positions.
- **The pass cannot be selected by job id from the EE hook.** Flushing the VIF chain at the pass boundary and reading the
  last posted job id gave `jobs 460505..460504` (empty): GT4's shape VIF packets are kicked outside the `render_cars`
  bracket, so the pass's jobs are not contiguous between the two hooks. A job tag has to come from the DMA/kick side.

**Solved: the exact draw-to-record mapping, and the G-TAP gate.** The tap now also records, for every VIF packet the EE
posts, the job it was posted as and the packet's **guest address** (`tap_job_source`, called from `post_vif_packet` and
from `flush_vif_chain` for each packet of a batch). The capture stores the same addresses per item, so a draw selects its
job exactly, with no guessing from vertex counts. The tool aligns each draw inside its job's vertex stream and searches a
window around the expected offset (clipping makes the emitted count differ), keeping the placement with the smallest
residual. Night race, VBlank 7398, `gt4_coregs3_tap fit-all`:

| G-TAP (screen XY, 1x) | Result |
|---|---|
| draws fitted | 48 of 116 (68 skipped: no single-job mapping) |
| vertex pairs | 5,868 |
| rms | **0.073 px** |
| worst | **0.222 px** (draw 41) |
| vertices within 0.25 px | **100%** (4,854 within 0.10 px, 1,014 within 0.25 px, none beyond) |
| draws with worst <= 0.25 px | **48 of 48** |

One draw fitted alone is even tighter (projective rms 0.025 px, worst 0.043 px): the transform is affine to that precision
at these distances.

**The strip table, decoded (C).** A car part carries, besides its `V3-16` positions, a `V3-8` channel at TOPS 0x80
(compressed normals), a `V2-16` channel at TOPS 0x40 (UV-like) and a `V1-8 x4` channel at TOPS 0x40. The `V1-8` write is
the one that matters: its UNPACK has **STMASK enabled (bit 28)** and the packet sets STMASK `0x3F3F3F3F`, so only lane 3
of each destination qword is written. The decoder used to ignore the mask bit and read lane 0, which is why an earlier
round saw the UV bytes `176,176,84,84` and called it a suspect bit mask. The payload itself is
`3, 6, 57, 18` for that 33-vertex part, and the format is a **strip-length list**:

- byte 0 = number of strips (3 here);
- each following byte = **three times the triangle count** of one strip, so a strip has `byte / 3 + 2` vertices
  (6/3+2 = 4, 57/3+2 = 21, 18/3+2 = 8; 4+21+8 = 33);
- a first byte of 0 means "no list": one strip over the whole item. That is the all-zero variant some shapes carry.

The reading is proven on the whole night-race frame. `gt4_coregs3_tap gate` decodes every draw without the tap, splits
the vertex array with the table and checks every triangle the VU1 draws (ADC = 0 on its last vertex) against the strips:
**116 of 116 draws, 335 strip tables, 1,549 strips, 12,146 vertices, 0 triangles crossing a strip break**. Every one of
the 3,098 strip-start vertices (the first two of each strip) is ADC, which is the structural half of the rule.

**What ADC also does (C, measured).** Within a strip, ADC is the VU1's own per-triangle cull: a vertex is ADC when the
triangle ending at it faces away (the screen-space area sign, with the strip parity folded in). That is why the raw
pattern is `1,1,0,0` for a quad strip: the first two vertices of each strip are structural, and the triangles that would
connect two quads face away. The correlation is not perfect at this precision: of 4,745 in-strip triangles the VU1
draws, 580 disagree with the sign test (268 of them have less than a quarter of a pixel of area, where the fixed-point
sign is not stable). The cull formula itself is therefore still **H**, but it does not affect the topology: the native
renderer builds the strips from the table and lets its rasteriser cull.

**Mapping every draw (C).** The tap now covers the whole car pass: the worker emits a job's GIF about two VBlanks after
the packets were posted (measured: source records at VBlank 7398, GIF records at 7400), so a tap window that stops at the
frame's own VBlank misses the tail (59 of 116 draws had a source record and no GIF record). With the window widened to
7398-7405 and the frame selected by the **source record's VBlank**, every draw maps: an item finds its GIF record by
vertex count, and when several records share a count the item's own strip table decides (the record whose strip starts
are ADC wins). A job can carry several draws and a draw can span several jobs; both cases are covered by the per-item
mapping. The gate also fits the model positions to the tapped screen positions (G-TAP affine fit): 111 of the 116 slices
fit at a mean rms of 0.97 px, and the 5 that do not (up to 37 px) are reported as ambiguous count matches, never hidden.

**Variable FPS.** The presentation timeline (`native/coregs3/coregs3_timeline.{hpp,cpp}`, `native_coregs3_timeline`) is in
place: guest timestamps, stable object identity (hash of the model's own addresses), a ring of frames, pairing with
unmatched counts, matrix lerp and the interpolation factor. Design and the CoreGS 2.0 / HUD interaction:
`docs/COREGS3-PLAN.md` section 5.5.

## 7. A measured gotcha: the capture must not write during the guest's frame

At 3x, the night-race gate first came out with 13/13 frames identical and the race clock and digest identical, but
`state.bin` differed in **512 words** at race-object offsets 0xEC0..0x16BC, and only in `shadow` (the run that writes a
capture). The reference build against itself is byte-identical, so the dump is deterministic: the capture's `fwrite` on the
EE thread at VBlank was changing the guest. The region holds coordinates in one run and the allocator's fill pattern in the
other, i.e. a streaming buffer whose double-buffer pick moved by one.

Fix: the capture now **queues the frame block in memory** and writes the whole file at shutdown, off the guest's timeline.
The EE-thread cost of a captured frame is then one `memcpy`, not a disk write. This is the same class of trap as
`GT4_HW_FLARE_DEADLINE_MS`: a capture build must not do I/O where the guest can see the delay.

## 8. Gates run in this round

| Gate | Result |
|---|---|
| G-LOGIC on P1-A (`licenca-b1`, 3,511 VBlanks, 1,576 racing) | race clock, per-VBlank digest and the 64 KiB state dump identical in ref, `off` and `shadow` |
| G-OFF / G-SHADOW frame identity | **not an oracle at 1x**: the reference build differs from itself in frame 3275 (same box). |
| G-OFF / G-SHADOW at 3x, night race (`coregs3-night-check.sh`, 8,997 VBlanks, 1,686 racing, 13 frames) | **passes**: clock, digest, `state.bin` and every frame identical for `off` and `shadow` |
| G-TAP on the night race, VBlank 7398, strip table (`gt4_coregs3_tap gate`) | **passes**: 116 of 116 draws mapped, 335 strip tables, 1,549 strips, 12,146 vertices, **0 triangles crossing a strip break**, 3,098 of 3,098 strip starts ADC; the affine slice fit is 0.97 px mean rms over the 111 draws with a unique record match |
| CTest `build/t090-v200-coregs3` | 319 tests, 100% passed, 1 skipped (PCSX2-dump test) |
| Python `tests/python` | 809 tests, OK, 57 skipped |
| G-ROBUST (capture) | 0 read failures, 0 orphan pass ends, 0 capacity refusals on the observed frames |
| G-IMAGE, G-PERF | not run yet (the live seam does not exist) |

## 9. Reproducing

```
# units with the hooks, then the EE library
python scripts/coregs3_units.py <translated units> build/coregs3/v200-units
build/t090-ee-library.cmd                     # tools/ee_producer, GT4_FAST_HEADERS=ON
# a game build that links that library (GT4_COREGS3=ON) and one reference build without the hooks
build/t090-configure.cmd t090-v200-ref
build/t090-configure.cmd t090-v200-coregs3   # T090_EELIB/T090_EESHA/T090_COREGS3=coregs3 in the environment
# the car scene (P1-B) and the identity gate (P1-A)
GT4_BENCH_OUT=... bash scripts/coregs2_bench/night-run.sh c3 <exe> GT4_COREGS3=shadow GT4_COREGS3_CAPTURE=...
GT4_BENCH_OUT=... bash scripts/coregs3_bench/coregs3-check.sh licenca-b1 <ref exe> <cand exe>
# the offline tool
gt4_coregs3_render <capture> --list
gt4_coregs3_render <capture> --frame 0 --out car.png --object --points
# the strip-table gate (needs a tap window that covers the worker's lag: see §6)
gt4_coregs3_tap gate <tap.gt4tap> <capture.gt4scene> 0 --verbose
# the same gate with the measured screen map beside each draw's matrix (since 2026-11-09, see §10)
gt4_coregs3_tap gate <tap.gt4tap> <capture.gt4scene> 0 --fits
# milestone sheet (test tool, Pillow)
python scripts/coregs3_bench/coregs3_sheet.py out.png --title "..." <ref.png> "CoreGS 2.0" <native.png> "native"
```

## 10. Round of 2026-11-09 (r7): build restored, the vertex transform measured, and how to resume

**The round was stopped early by the owner (the stage moves to another machine).** No live overlay code (`present` or
`overlay`) was written, no game build (`gt4vulkan`) was configured and **no gate was run in this round**. What exists is
the restored hook EE library, a small tools build and the measured vertex transform below. Nothing here changes `main`.

### 10.1 What was restored (all under ignored `build/`)

| Piece | Path | Result |
|---|---|---|
| Instrumented units | `build/coregs3-r7/units` | `scripts/coregs3_units.py` on the slice299 `generated` folder: 328 files, 2 instrumented, receipt `build/coregs3-r7/units.coregs3-units.json` (byte-exact removal proof) |
| Hook EE library | `build/coregs3-r7/ee-build/ps2game.lib` | 146,957,440 bytes, sha256 `b4c7ac0479e06793a147b35144aacbe7c2dae8acb7649057de11f4cd5b696575`; built by `scripts/coregs2_bench/game/ee_configure.cmd coregs3-r7/ee-build <abs units>` + `ee_build.cmd` (clang-cl 22.1.3, `GT4_FAST_HEADERS=ON`, `/O2 /clang:-msse4.1`) |
| Reference library | main checkout `private/slice299-ee-flare-producer/game-build/ps2game.lib` | 70,694,640 bytes, sha256 `d322b0aaa6a217ab8d0275d7447b8a838161d6fb3359478615c963acef616b3a`, equal to `docs/inputs/slice299-ee-libraries.json` (`profiles.flare.sha256`); no rebuild needed, same base as the hook library (hooks only) |
| Tools build (not the game) | `build/coregs3-r7/tools` | minimal root configure (`build/coregs3-r7/configure-tools.cmd`: `GT4_EE_PROFILE=production`, slice151-vu1-aot-runtime, slice153-pacing-port/-shaders, slice151-vu1-gen-smoke, slice155-vu1-full-corpus, `GT4_HW_GS=OFF`); `build/coregs3-r7/build-tools.cmd` builds `gt4_coregs3_tap.exe` and `gt4_coregs3_render.exe` |

`build/coregs3-r7/tools2` is a failed first configure (`CMake Error ... The C++ compiler ... is not able to compile a
simple test program`, `RC Pass 1 ... rc ... no such file or directory`: the `VsDevCmd.bat` environment was not in effect
for that `cmake` process); it is ignored and unused. The two `.cmd` files above are the working invocation form
(`VsDevCmd.bat` then `cmake`).

### 10.2 The VU1 vertex transform, measured (**H**, whole-file residual)

`gt4_coregs3_tap gate ... --fits` (new this round) prints, for every fitted draw, the fitted affine screen map, its
first three (object position, tapped screen position) pairs, `vf24..vf27`, `cmd.value` and the eight captured
`matrix_packet` rows. Full output: `build/coregs3-r7/gate-fits.txt` (97 draws printed; the gate itself reports
**111 of 116 draws fitted, mean rms 0.9714 px, worst 37.3771 px** — unchanged from §8). Night race, VBlank 7398,
`build/evidence/coregs3/r9/{cars.gt4tap,cars.gt4scene}`.

**The convention that reproduces the tapped coordinates** (positions in metres: a V3-16 channel's raw 16-bit value
divided by 4096 as `build_mesh` does, or a V3-32 channel's float as stored):

The VU1 receives the draw's matrix as the eight qwords `func_0049E548` writes at `0xC1`:
`tag, vf24*value, vf25*value, vf26*value, vf27, m2[0..3]`. The three linear columns are COLUMNS c0..c2 = VU0
vf24..vf26 scaled by the shape's Cmd float `value` (`Cmd + 0x04`); when `value` is exactly zero the upload stores them
unscaled (the function's zero branch, **C** in the decompiled `func_0049E548`). c3 = vf27 is the translation and is
never scaled.

```
columns: c0 = vf24 * s, c1 = vf25 * s, c2 = vf26 * s, c3 = vf27    (s = value, or 1 when value is exactly zero)
clip    = c0 * x + c1 * y + c2 * z + c3 * 1              -> (cx, cy, cz, cw)
screen  = ( (0.5 + 0.5 * cx / cw) * width, (0.5 - 0.5 * cy / cw) * height )
```

The VU1 multiplies the raw signed 16-bit model value for a V3-16 position channel and the stored float for a V3-32
channel. With `build_mesh`'s metre positions the linear columns therefore carry one more factor of 4096 for a V3-16
vertex (raw units per metre) and none for a V3-32 vertex; `native/coregs3/coregs3_vertex.hpp` builds both sets
(`vertex_columns_from_vu0_metres`), and this is the only place a shape scale enters the vertex stage.

The VU1 then adds the drawing offset before the GS: the tapped 12.4 value is `screen * 16 + offset`, and the GS
subtracts `XYOFFSET` again (`native/hw_gs/frontend/hw_front_end.cpp:621`).
Measured on the captures, the offset is about **(-2368.0, +1824.0) px** (`OFX = -37888`, `OFY = 29184` in 12.4 units;
the negative OFX is the 16-bit value 27648 read signed). It is **inferred** from the fits, not read from the register.

| Check (draw, vertex) | predicted screen (corrected) | tapped − offset | delta |
|---|---|---|---|
| draw 0, pair 0 | (358.75, 85.93) | (358.625, 86.000) | (0.12, -0.07) px |
| draw 0, pair 1 | (359.00, 85.99) | (358.875, 86.063) | (0.12, -0.07) px |
| draw 3, pair 0 | (359.65, 79.84) | (359.500, 79.875) | (0.15, -0.04) px |

The predictions are the fixture's own float32 arithmetic on the transcribed `gate-fits.txt` columns and values
(`native/coregs3/coregs3_vertex_fixture.cpp`). Two draws and three vertices were checked by hand. **Whole-file residual
(2026-11-09, r8):** the formula was then recomputed over all 291 printed (draw, vertex) pairs of `gate-fits.txt`, at
that time with the linear columns UNSCALED (see the correction below). The **41 draws that lie on screen** matched the
convention and the inferred offset; after removing one constant per draw, the residual was **rms 0.2197 px**. The other
**56 draws split in two**: **26 show the expected 16-bit wrap** (tapped y **+4085..+4122 px** away from the prediction,
one 4096 px period at 12.4, i.e. 65536 units) and **30 have small per-draw offsets (0.5-11.7 px)**, which the three
printed pairs per draw do not explain further (**Hypothesis**: partially off-screen vertices or fit bias). None is
evidence of a second transform. Three draws (95, 111 and 115) are degenerate (18-24 px centred rms) and stay unusable.
The old row reading fails the same 41 draws: 13 put `cw <= 0` behind the eye and the other 28 have **1.9204 px rms**
(worst 6.4869 px), so the column reading is the measured one. The convention is therefore **H** for on-screen vertices;
the wrap is expected 16-bit arithmetic. The live renderer must clip in clip space (`cw`) and must never consume the
wrapped 12.4 screen coordinate. The module `native/coregs3/coregs3_vertex.{hpp,cpp}` implements it (round 9; the linear
scale was corrected on 2026-11-10, below), and `coregs3_render.cpp` projects through it with near-plane clipping; the
object path is unchanged.

**Correction (2026-11-10, G-IMAGE/G-PERF round).** The round-9 reading above used the columns UNSCALED, which is wrong:
the captured packet rows are `vf24..vf26 * value` (e.g. `packet1 = 0.000474434` = `vf24[0] 2.037678 * value
0.000232830644`), so the linear part was missing a factor `4096 * value` for a V3-16 channel. The factor is not close to
one for every family: the measured classes on VBlank 7398 are `4096 * value = 0.953674` (102 body/paint draws),
`0.625019` (5) and `0.2500076` (5), plus 4 V3-32 float-position draws with `value = 0` (the upload's unscaled branch;
their stored float needs no 4096 factor). Two mathematically equivalent readings exist: metre positions with the linear
columns multiplied by `4096 * value` (chosen: it keeps the live shader's metre vertex input and is exactly the prototype
that measured the fix), or the captured packet rows with raw 16-bit positions. The captured `matrix_packet` cannot be
used as the general source: the PGL output buffer read back after the call holds this draw's upload for only 25 of 116
draws (the buffer is recycled), while the pre-call VU0 block and `cmd.value` are always captured and the upload rule is
known (**C** in `func_0049E548`).

Measured effect on the r13 night race, VBlank 7398, frame 0 (`build/analysis-gates/`; the prototype scales vf24..vf26 by
`4096 * value` before `render_frame`): whole-file, translation-centred per-draw residual median **0.489 -> 0.041 px**,
and draws within 0.5 px **58/115 -> 102/115** (draw 115 has no on-screen vertex and is excluded); diagnostic 1x coverage
of the GS car mask **95.49% -> 98.79%**, spill **31.2% -> 0.54%**; the refined 3x gate mask covers **99.26%** of the 1x
blocks (**97.49%** of strict 3x pixels), with spill **1.70%** (3x px) and **5.00%** (1x blocks). The module and both
renderers now use the corrected columns; the unscaled reading survives only in this history.

**Consequence for the existing code (before round 9):** `native/coregs3/coregs3_render.cpp` `draw_matrix()` read
`vf24..vf27` as matrix **rows** and multiplied rows 0-2 by `cmd.value`. The capture shows the packet rows are exactly
`vf * value`, so the row reading is a different transform: it is why the offline renderer's screen mode was never
validated, and every published M2/M4 picture used `--object`, which bypasses `draw_matrix`. Round 9 replaced it with the
`coregs3_vertex` module (columns, clip space); the old screen-mode pictures are row-reading evidence only. Round 9 itself
still used the linear columns unscaled; the packet relation quoted above is exactly the missing factor, corrected on
2026-11-10.

### 10.3 The live design (implemented in round 10)

**Implemented (2026-11-09, round 10).** `GT4_COREGS3=present` now has its frame ring in
`native/coregs3/coregs3_present.{hpp,cpp}` (8 slots, O(1) vector swaps, a `try_lock` producer that drops instead of
waiting, a claim-newest consumer, atomic counters) and its renderer in
`native/hw_gs/coregs3/hw_coregs3_present.{hpp,cpp}` (private command pool/buffer/fence, shaders, pipeline, D32 depth
image, host-visible vertex buffer, one triangle-list draw per captured draw with the raw `vf24..vf27` in a push
constant). It is inserted after `ComposeFrameGpu` in `Backend::end_frame` and `Backend::scanout`, submitted on
`Impl::queue` through `Impl::submit_to`; `overlay` still refuses to start, `off|shadow` stay untouched, and only
`shadow` writes a capture file. Deviations from the prepared design: the composed display image is created by CoreGS 2.0
without `COLOR_ATTACHMENT` usage on this device, so the fragment stage writes the packed R32_UINT word with an
`imageStore` (storage image) instead of a colour attachment; the D32 depth attachment, the depth test and
`depthClamp` remain, and the image barrier is `GENERAL`↔`GENERAL`. The layer refuses cleanly when the device lacks
`depthClamp`, the D32 depth attachment or R32_UINT storage. **Correction (round 12):** the compare is GEQUAL, not
strict GREATER: the GS rule measured by the CPU reference lets a later primitive win an equal-depth tie, and the
car's coincident body layers need that for the lit body to be visible (see §11).

**Evidence (2026-11-09, Avell A70, RTX 3060 Laptop, Vulkan 1.4.357).** Full 3x night run, 8,997 VBlanks / 1,686 racing:
13 passes, 1,377 draws, 338,934 vertices, 112,978 triangles, 0 refused; the guest clock, digest and state are identical
to the reference. The present frame at VBlank 7400 is `build/evidence/coregs3/r8/present-07400.png`; present against
reference differs in 36,736 px in the car box only (x 1064..1595, y 231..906). A second independent run (7,597 VBlanks,
286 racing) also reports 0 clock/digest/state differences. The off/shadow gate on the same binaries passes (8,997
VBlanks, 1,686 racing, 0 differences, 13/13 frames). The validation smoke run reports no error or warning from the new
pass; its only message is a pre-existing CoreGS 2.0 `VUID-VkSubpassDependency-srcSubpass-02243` at startup.

The design bullets below are the round-7 plan, kept as the design record.

- **Mode:** `GT4_COREGS3=present` (a fourth value beside `off|shadow|overlay`), debug only, never an acceptance mode.
  `overlay` keeps refusing to start.
- **Frame source:** the hooks already build one `FrameSnapshot` per VBlank; publish it to a ring with an O(1) vector
  swap in `finish_frame()` and expose a "copy the last complete frame" reader. No guest write, no new guest cycle.
- **Renderer:** a `CoreGS 3.0` module inside `native/hw_gs/` (the only place the `gt4_hw_*` Vulkan loader and the
  `Backend::Impl` device/queue live), initialised from `Impl` but **not** touching CoreGS 2.0's targets. A graphics
  pass into the composed display image (`R32_UINT`, integer output, no blending) with a `D32` depth image, a minimal
  vertex shader using the measured convention (`gl_Position = vec4(cx, -cy, 1.0, cw)`, depth compare GEQUAL on
  `1/cw`, corrected in round 12), no culling, vertex colour only. One draw call per captured draw, `vf24..vf27` in
  a push constant.
- **Insertion point:** after `ComposeFrameGpu` returns a valid frame, in `Backend::end_frame`
  (`native/hw_gs/hw_gs_backend.cpp:2074-2086`) for the window path and in `Backend::scanout`
  (`:1949-1955`) for the headless dump path; submit on `Impl::queue` through `Impl::submit_to` with a private command
  pool/fence, after the compose submission (same-queue order), and barrier the image `GENERAL`↔`COLOR_ATTACHMENT`.
  No second instance, device, swapchain or window.
- **Gate:** a headless 3x night run with `GT4_COREGS3=present` and `GT4_HW_DUMP_DIR`, counters for native draws and
  fallbacks, G-LOGIC unchanged (race clock/digest/state), frame saved under `build/evidence/coregs3/r7/`.

### 10.4 Resume recipe on another machine with the same `C:\Antigravity` layout

1. Worktree: branch `task/090-coregs3` (this commit). The main checkout stays read-only owner data.
2. Read-only inputs to have present in `C:\Antigravity\gt4-staticrecomp`: `private/slice299-ee-flare-producer`
   (`generated` + `game-build`), `private/references/ps2-web-recomp` (donor `1b480879…`),
   `private/slice151-vu1-aot-runtime`, `private/slice153-pacing-port`, `private/slice153-pacing-shaders`,
   `private/slice151-vu1-gen-smoke`, `private/slice155-vu1-full-corpus`, `private/tooling-venv`,
   `private/coregs2-bench` (cards and input recordings for `night-run.sh`), `private/tooling/sdl3-3.4.18`, and for the
   rc15-style game build also `private/slice200-memcard-runtime`, `private/slice365-ipu-fast-runtime`,
   `private/slice376-frame-render-license-guard-runtime`, `private/slice396-spu2-runtime`,
   `private/slice409-usb-runtime` and the Vulkan SDK (`C:/VulkanSDK/1.4.341.1`, `glslangValidator.exe`). Nothing under
   `build/` needs to travel: the units are regenerated and the libraries rebuilt.
3. Rebuild the hook library (exact commands that worked):
   ```
   python scripts/coregs3_units.py <main>/private/slice299-ee-flare-producer/generated build/coregs3-r7/units
   cmd /c scripts/coregs2_bench/game/ee_configure.cmd coregs3-r7/ee-build <abs>/build/coregs3-r7/units
   cmd /c scripts/coregs2_bench/game/ee_build.cmd coregs3-r7/ee-build
   sha256sum build/coregs3-r7/ee-build/ps2game.lib   # expect b4c7ac04…
   ```
4. Game build (the next step; **not done this round**). Follow the rc15 recipe
   `scripts/coregs2_bench/game/configure.cmd <dir> v200` (it passes `GT4_HW_GS=ON`, the Vulkan SDK, the SDL3 root and
   every runtime capsule) and add through `T134_ARGS`:
   ```
   -DGT4_EE_PROFILE=flare -DGT4_EE_LIBRARY=<abs>/build/coregs3-r7/ee-build/ps2game.lib
   -DGT4_EE_LIBRARY_SHA256=b4c7ac0479e06793a147b35144aacbe7c2dae8acb7649057de11f4cd5b696575
   -DGT4_COREGS3=ON
   ```
   then `cmake --build <build> --target gt4vulkan`. The reference build is the same line with the slice299 pinned
   library and without `GT4_COREGS3` (it can also use `-DGT4_EE_PROFILE=flare` alone, since that profile selects the
   pinned library). Note `configure.cmd` defaults still point at slice397-vu1-gen/slice262 runtime (rc15); the brief's
   r7 line used slice151-vu1-aot-runtime + slice151-vu1-gen-smoke and both CMake and the pins accept them, but the game
   build was not attempted, so the first configure must be watched for a port/runtime pin refusal.
5. Gates to re-run before any new code (same commands as §8/§9):
   ```
   GT4_BENCH_OUT=<abs>/build/evidence/coregs3/r7 NAME=r7-check SCALE=3 EVERY=200 FROM=6500 FRAMES=9000 DUMP_AT=7400 \
     bash scripts/coregs3_bench/coregs3-night-check.sh <ref exe> <cand exe>
   gt4_coregs3_tap gate <tap> <capture> 0 --verbose
   ```
   The card is copied by the bench scripts; `private/manual-play/card.ps2` is never written by an agent.

### 10.5 Risks and unknowns of this round

- The transform is measured on **two draws** and checked by hand; the whole-frame residual and the offset constants are
  **H**, not **C**. The offset may also change per pass if the game rewrites `XYOFFSET` mid-frame (it is a GS register,
  so the same formula still holds but the constant is per pass).
- `coregs3_render`'s `draw_matrix` is still the old (row) reading; screen-mode pictures from it are invalid.
- The tool's `--fits` prints 97 of the 111 "fit ok" draws: 14 are singular fits on flat parts (a y-constant model makes
  the normal equations singular) and are counted as ok without coefficients.
- No game build and no gate ran this round, so the §8 numbers remain the last measured state and nothing here proves
  they still hold on this machine.


## 11. Round 12 (2026-11-09): the measured VU1 lighting formula in both renderers

**The formula (C).** The body/paint VU1 fragment (microcode entry 0x10A0) reads the raw V3-16 model position `P` after
ITOF0 (raw model units, no `position_scale`), the V3-8 normal bytes divided by 127 (`N`), the V4-8 base colour `B` and
the six-qword block at TOPS+0xC9 (`0xC9..0xCB` matrix `M`, `0xCC` light position `LP`, `0xCD` `C3=(c3,c3,c3,1)`, `0xCE`
`C4=(c4,c4,c4,0)`), and writes, in float32 and in this order:

```
D      = P - LP
cos    = (D.x*N.x + D.y*N.y) + D.z*N.z        (lane 0, summed left to right)
t      = min(cos, 0) * (1 / |D|)              (min first, then the reciprocal length)
factor = C3 + C4 * t                          (per lane)
lit    = FTOI0(B * factor)                    (truncate toward zero)
alpha  = FTOI0(B.w * (C3.w + C4.w * t))       (the captured block gives exactly B.w)
```

`M` does **not** enter the lit colour: the fragment uses it to transform the reflected vector `D - 2*N*(D.N)` into a
separate output (two floats written at TOPS+0x40). Driving the real fragment with a different `M` and every other input
fixed leaves the colour unchanged; the float32 formula matches the fragment on all 3,914 body-family rows of the r11
capture (RGB and alpha), and the tap residual is identical to round 11.

**Module.** `native/coregs3/coregs3_lighting.{hpp,cpp}` holds `decode_light_block` and `shade_vertex`. Only the measured
six-qword variant (`light_block_qwords == 6`, command `0x6C06C0C9`) is accepted; every other block (the nine-qword
`0x6C09C0C9` form used by the glass/lamp parts, the three-qword `0x6C03C0C9` form, or no block) returns an explicit
status and the caller falls back to the base colour and counts it. A vertex without `has_normal` or
`has_raw_position`, a zero-length `P - LP`, or a non-finite value also falls back. The renderer clamps the FTOI0 result
into the RGBA8 lane (the measured block never leaves `[0,255]`).

**Offline renderer.** `coregs3_render.cpp` shades a mesh in place when the draw has the measured block and the mesh has
normals and a colour channel; `RenderStats` reports `draws_lit`, `draws_fallback`, `vertices_lit` and
`vertices_fallback`, printed by `gt4_coregs3_render`. The `--object` diagnostic path and `--flat` keep their previous
behaviour (counted as fallbacks), so their PNG hashes are unchanged (`ca070835…` points, `b485c1b7…` flat).

**Live present.** `hw_coregs3_present.cpp` shades each mesh vertex once in `build_frame` and stores the lit RGBA in the
`PresentVertex` colour (the shader stays vertex-colour only). The present counters gained `draws_unlit` and
`vertices_unlit`; the F8 report prints them. One correction was needed to make the lit body visible: the depth compare
is **GEQUAL**, not strict GREATER, because the car's coincident body layers tie on depth and the GS lets the later
primitive win (the same rule as `coregs3_render`'s w-buffer).

**Evidence (r12, Avell A70, RTX 3060 Laptop, Vulkan 1.4.357).** Module against the tap, reusing the r11 pairing:
3,914 body-family vertices, per-channel rms 0.2864, max 4 (all in the ambiguous draws 91/93); the 3,822 unambiguous
vertices stay at rms 0.1696, max 1, alpha exact 3,822/3,822. Full 3x night present run: 8,997 VBlanks / 1,686 racing,
13 passes, 1,377 draws, 338,934 vertices, 112,978 triangles, 0 refused, 1,059 unlit draws / 231,918 unlit vertices,
guest clock/digest/state identical; frame `build/evidence/coregs3/r12/present-07400.png` (and `…-09000.png`) shows the
shaded cars, present vs reference differs in the car box only (36,707 px). The 3x off/shadow gate is
COREGS3-NIGHT-CHECK-OK (8,997 VBlanks, 1,686 racing, 0 differences, 13/13 frames, capture 3,382,120 B). CTest 4/4
(the new `native_coregs3_lighting` plus the three existing) and Python 7/7.

**Open.** The nine-qword and three-qword block variants are not decoded (the no-colour glass/lamp draws keep the
`(255,255,255)` base fallback and are counted); the 91/93 sub-family (program selector) and per-item light uploads stay
open from round 11.

## 12. Round 13 (2026-11-09): the VU1 program selector and the family map

`GT4_COREGS3` captures now carry the per-draw ITOP and MSCAL words from the terminating tag of the draw's REF chain
(`CapturedDraw::program_*`, format version 3): the raw words, the MSCAL entry (`selector * 8`), the ITOP dispatch
(`(itop & 0x3FF) * 8`), `obj->count` and a validity flag. The **ITOP dispatch** is the real program selector; the MSCAL
word is a shared stub that sets the return address/0xD9 pointer and mixes families. Mapping for frame 0 (VBlank 7398),
using the read-only AOT manifest:

| ITOP dispatch | AOT entry (manifest) | chain | family | draws / vertices | block |
|---|---|---|---|---|---|
| 0x1058 | `vu1_19F8161FEF7AFDBA` | +0x10 via 0x1908 -> 0x10A8 (`vu1_6CD954479A2A5D0E`, the 0x10A0 body) | body/paint, 6-qword, lit | 27 / 3,918 | `0x6C06C0C9` |
| 0x0B08 | `vu1_10F5C9EA39C2C7CA` | +8 via 0x1878 -> 0x0B48 (`vu1_53E3859092505B4A`) | nine-qword glass/lamp (draw 98) | 44 / 3,816 | `0x6C09C0C9` |
| 0x03D8 | `vu1_752F5F48E391E2DD` | +8 via 0x1878 -> 0x0418 | nine-qword | 13 / 1,677 | 9q |
| 0x0418 | `vu1_8BE019F2A79E69D1` | different structure (goto 0x04D0) | nine-qword, no light upload | 4 / 360 | 9q |
| 0x0250 | `vu1_8D398BF1242BFD1A` | +8 via 0x1878 -> 0x0290 | nine-qword | 1 / 34 | 9q |
| 0x0C68 | `vu1_971A2A1267F9D57A` | +8 via 0x1878 -> 0x0CA8 | 6-qword but no colour, unlit | 1 / 60 | 6q |
| 0x14D8 | `vu1_ADAA0AA00ACD6D39` | +6 via 0x1810 -> 0x1508 | three-qword | 15 / 930 | `0x6C03C0C9` |
| 0x0080 | `vu1_F63D32CA15346EEC` | +6 via 0x1810 -> 0x00B0 | no block | 11 / 1,351 | none |

The 27 body draws are all lit; every other selector is unlit. **Draws 91/93 are the same body program** (ITOP 0x20B ->
0x1058 -> 0x10A8); their residual is a light-block/data effect, not a second program. Draw 98 is ITOP 0x161 -> 0x0B08
-> 0x0B48 (nine-qword, no colour). The MSCAL entry is only a stub (vi15/vi14) and mixes families (entry 0x1D40 covers
39 nine-qword + 16 six-qword draws), so the ITOP dispatch, not the MSCAL entry, is the family selector. Selector vs Cmd
flag is 1:1 in this frame (0xC2 -> 6q, 0xA1 -> 9q, 0x85 -> 3q, 0x01 -> 9q no light, 0xA4 -> 6q no colour, 0x80 ->
none). Evidence: `build/evidence/coregs3/r13/`, `build/analysis-lighting/selectors-r13.{txt,json}`. Open: the four
nine-qword programs and the three-qword/no-block programs still need their formulas; one scene only.

## 13. Round 14 (2026-11-10): the per-item light state, the per-item selector and the carried block

`.gt4scene` is **version 4**. Every draw keeps all version 3 fields; each item record adds the VU1 light state the item's
microprogram run reads at TOPS+0xC9 (16 qwords reserved, the upload's raw UNPACK word and source, and where the block came
from) and the ITOP/MSCAL pair that ran it; each draw adds the chain summary (`chain_tags`, `chain_valid`,
`program_uniform`). The K1 post hook now walks the whole PGL output chain instead of reading fixed offsets. Versions 1..3
are refused, not migrated.

### 13.1 The PGL output chain and the VIF tag2 pipeline (C)

`func_0049D328` writes one draw into the PGL output buffer (`obj->out`, PGL+0x1F8, scratchpad `0x70002000`) as a DMA1
chain of 16-byte tag qwords. The donor's tag-transfer path (`private/references/ps2-web-recomp/runtime/src/dmac.cpp:173`:
with TTE the last 8 bytes of every tag are forwarded to the VIF before the tag's data) is what makes tag words 2/3 the
VIFcode pair the VU1 executes. The chain is, in order: the `0xC1` matrix packet (CNT, QWC 8, tag2 `0x6C08C0C1`, eight
inline qwords) when `func_0049E548` ran; per item a REF tag to the model packet whose tag2 is STMASK+mask for item 0 and
the previous item's ITOP/MSCAL pair otherwise; per item, conditionally, a REF tag whose tag2 is the UNPACK into
TOPS+0xC9; per item, when `obj->bits & obj->bits2`, a CNT tag with the TOPS+0xD9 block inline (`func_0049E5F8`); and the
terminating CNT tag (0x10000000, QWC 0) whose tag2 is the last item's ITOP/MSCAL pair. `native/coregs3/coregs3_chain.cpp`
walks it with the caller's guest read function, so the CTest fixture drives hand-authored chains; `chain_valid` is 1 only
when the walk parses exactly from the pre-call cursor to the post-call cursor.

### 13.2 Per-item light blocks (C)

The light tag is copied into the packet by `func_0049D1A0`, whose copy condition is
`if (n != 0 && self->tag.w[0] != 0)` with `n` the per-draw mode that starts at 2 and decrements on every copy (its
`case 1`/`case 4` branches reset it to 2 per item only when `Obj+0x48 & 7` is set, and then re-prepare the block on the
shape stack). So a light-family draw uploads **its tag for the first two items** (and for every item of the re-preparing
kinds), not once per shape and not for every item. Measured on frame 0 (VBlank 7398, 335 items): 177 items carry their own
upload and 158 carry (153 carried + 5 uncertain):

| family (per draw) | draws | items | items with their own upload |
|---|---|---|---|
| three-qword `0x6C03C0C9` | 15 | 25 | 22 |
| six-qword `0x6C06C0C9` (body/paint) | 28 | 81 | 37 |
| nine-qword `0x6C09C0C9` (glass/lamp) | 62 | 203 | 118 |
| no block (cmd flag 0x80) | 11 | 26 | 0 |

The body family's uploaded and carried items all agree in content, as expected: items 0 and 1 write the same tag into the
two TOP buffers and later items read one of them. The nine-qword family is different: the re-preparing path
(`flag & 0x1F` 1/4 with `Obj+0x48 & 7`) pushes a new block per item, and the captured blocks differ within a draw (12
draws in frame 0, e.g. draw 14: qword 3 = `(16,16,16,256)` for item 0 and `(144,144,144,256)` for item 1); the captures
keep the per-item value. The nine-qword sources also differ per item (13 draws), as the stack pointer moves.

### 13.3 The per-item program selector (C, correcting round 13)

Round 13 read the selector from the terminating tag (the last item's pair). The per-item capture shows that the pair is
**not always uniform within a draw**:

- the **ITOP** word (the real program selector) changes inside 8 of the 116 draws of frame 0: draw 98's first 17 items
  run ITOP `0x400007B` (dispatch 0x3D8) and items 17..30 run `0x4000161` (dispatch 0x0B08); draws 29, 32, 51, 62, 65, 69
  and 73 show the same switching. The ITOP operands are recomputed every iteration from `obj->x + obj->y`
  (`func_0049D328`), so the switch means those operands changed inside the loop (the per-item callback/helpers; H);
- the **MSCAL** word (a shared stub, round 13) differs per item far more often: the pre-loop `n--` of a matrix upload
  and the per-item `n += 60` / `n -= 5` arithmetic of the 0xD9 block make the immediate vary (draw 0: item 0 `0x3A7`,
  items 1/2 `0x3AD`; draw 10: four items `0x3AD` and the rest `0x3A8`). This is the stub, not the selector, but the
  capture keeps it per item because a VU1-side consumer may need the exact `vi14`/return-address setup (43 draws in
  frame 0).

The capture therefore stores both words per item (`CapturedItem::run_itop/run_mscal`), with `run_valid`; the per-draw
`program_*` stays the terminating tag for compatibility. `program_uniform` is 1 when every item's ITOP equals the
terminating ITOP (the selector view: 108 of 116 draws in frame 0), and `program_item_mscal_variants` counts the stub
differences separately. Over the whole 7,600-frame run: 383,588 items, 0 without a run pair, **52,362 with an ITOP
different from their draw's last item** and **91,205 with a different MSCAL stub** — the per-item selector is real, not a
formality.

### 13.4 The carried block, and why the EE cannot snapshot the VU1 memory (C + H)

The VU1 data memory is double buffered: VIF1 `BASE`/`OFFSET` select two windows and every MSCAL flips them
(`private/slice262-frame-cost-runtime/vif.cpp:57`: `top = tops; dbf = !dbf; tops = base + (dbf ? ofst : 0)`), so a
program run reads the `0xC9` block written since the previous MSCAL. The capture reports the shadow registers at the end
of every interval; on this run `base 0`, **`ofst 435`**, so the two `0xC9` locations really differ and the parity matters.

The EE side cannot read the block directly:

- `ps2::Cpu` (`runtime/include/ps2/cpu.hpp:53`) holds EE registers, VU0 and COP0 only; the VU1 data memory is the global
  `g_vu1_mem[16384]` (`runtime/src/hw.hpp:17`, `memory.cpp:19`) and the microprogram receives it as its data pointer
  (`slice262-frame-cost-runtime/vu_micro.cpp:895`, `slice151-vu1-aot-runtime/vu_micro.cpp:878`).
- With MTVU on (the default), the EE thread parses VIF1 in **shadow** mode: the shadow unpacks into `g_dry_run`
  (`slice262-frame-cost-runtime/vif.cpp:33/160`) and posts the packet to the VU1 worker, which owns the real
  `g_vu1_mem`. The EE copy is the wrong state and reading it would race the worker.
- Even without MTVU, K1 post runs when `func_0049D328` returns: the draw has only been written to the PGL output buffer.
  The DMA1 kick that makes VIF1 execute it happens later, so no snapshot at K1 post can contain this draw.

The smallest alternative is a VU1-side hook inside `run_vu` (the single place a MSCAL flips TOP/OFFSET and starts a
program): snapshot `g_vu1_mem[top + 0xC9]` there. That needs a new pinned runtime capsule (the perf runtime source is
hash-pinned and this worktree carries no `private/`) plus a way to map the worker's runs to the captured items across its
lag; it was **not** added in this round.

Instead the capture tracks the state in VIF command order, which the DMA later executes in exactly the same order:
`CarriedLight` keeps one slot per MSCAL parity (`runs & 1`), every observed TOPS+0xC9 upload refreshes its slot, and an
item without an upload records the slot its own MSCAL reads. The labels are relative, so a constant unknown parity
cancels; an unseen MSCAL (another call site of `func_0049D308`, which the pinned EE library does not instrument, or a
chain the walker refuses) can shift the parity, which `gap_epoch` marks. A carried block is exact when its slot was
uploaded after the last gap, or when both slots hold the same content; otherwise the item says `carried_uncertain`.

Measured (frame 0): 158 carried items, 153 exact and **5 uncertain** (draws 25, 44, 61, 85 and 115: the tracked slot
predates a cursor discontinuity and the two slots differ). A replay of the frame's items against the two-slot model
predicts every one of the 158 carried blocks, so the values are consistent with the stream order; only the absolute parity
of those five cannot be proven from the EE side. Over the whole run: **200,339 items with their own upload, 179,944
carried (of which 3,305 uncertain), 0 with no state**; `chain_gaps` 214,244 (the PGL output buffer is flushed and rewound
between passes) but `chain_incomplete` 0 and the car-pass chains are contiguous, which is why most carried items are
exact.

### 13.5 Gates in this round

| Gate | Result |
|---|---|
| Fresh 3x night capture (`GT4_COREGS3=shadow` + tap, VBlanks 7398-7399, 7,600 frames) | per-draw counters identical to r13: lights 114,365 / missing 9,129; selectors 124,782 / missing 0; 0 read failures, 0 refusals, 0 incomplete chains; capture v4 819,792 B |
| Version 3 field identity (`build/evidence/analysis-090/compare_v3_v4.py` on the r13 v3 capture) | **passes**: 232 draws identical on every version 3 field, per-draw light block qwords included |
| CTest | 4/4 passed (`native_coregs3`, `native_coregs3_vertex`, `native_coregs3_lighting`, `native_coregs3_timeline`) |
| Python `test_coregs3_units.py` | 7/7 passed |
| 3x off/shadow identity gate (main worktree reference exe) | **passes**: 8,997 VBlanks, 1,686 racing, clock/digest/state 0 differences and 13/13 frames identical for off and shadow (capture 4,451,628 B). One earlier run of the same gate showed the known 512-word state-dump double-buffer difference (0x10EC0..0x116BC) in the `off` run under concurrent machine load; it did not reproduce in a clean run |

## 14. Round 18 (2026-11-10, branch `task/090-family-light`): the per-item lighting programs in both renderers

The round 14 formulas are now implemented in `native/coregs3/coregs3_lighting.{hpp,cpp}` and both renderers shade every
item with **its own** light block and ITOP selector from the version 4 capture. This closes the round 12/13 "every
selector other than the body is unlit" state: a night frame now lights all 335 items of the car pass, including draw 98,
whose items switch programs inside the draw (items 0..16 run 0x03D8, items 17..30 run 0x0B08).

### 14.1 The programs and their exact formulas

`LightProgram` names the body program and the seven measured families (plus `none`); the selector is the item's ITOP dispatch,
`(CapturedItem::run_itop & 0x3FF) * 8`, and the block is the item's `light_block` qwords at TOPS 0xC9. The measured
formulas, in the fragment's operation order and with its truncation:

| ITOP dispatch | program | block | formula (per colour lane, alpha = lane 3) |
|---|---|---|---|
| 0x1058 | `body_paint` | six qwords 0x6C06C0C9 | the round 12 formula: `D = P - LP`, `t = min(D.N, 0)/abs(D)`, `out = FTOI0(B * (C3 + C4*t))` |
| 0x0B08 | `nine_qword_first` | nine qwords 0x6C09C0C9 | `L_j = max(dot(M_j, N), 0)`, `out = FTOI0(clamp(C3 + C4*L0 + C5*L1 + C6*L2 + C7*L3, 0, C8))` |
| 0x0250 | `nine_qword_pow8` | nine qwords | `out = FTOI0(clamp(C3 + C4*L0 + C5*L1 + C6*L2^8 + C7*L3^8, 0, C8))`, three squarings |
| 0x03D8 | `nine_qword_pow16` | nine qwords | `out = FTOI0(clamp(C3 + C4*L0 + C5*L1 + C6*L2^16 + C7*L3^16, 0, C8))`, four squarings |
| 0x0418 | `nine_qword_direct` | nine qwords | the same power-16 formula, V3-32 positions and V3-32 unit float normals (no prologue) |
| 0x0C68 | `six_qword_no_colour` | six qwords 0x6C06C0C9 | `D = P - B`, `R = D - 2N(N.D)`, `q = RSQRT(abs(D)^2)`, `t = R*q`, `L = C9*t0 + CA*t1 + CB*t2`, `L = max(L, 0)`, `out = FTOI0(CD * L.x^8)`, alpha 128 |
| 0x14D8 | `three_qword_gain` | three qwords 0x6C03C0C9 | `out = FTOI0(clamp(bias + gain * C, 0, clamp))` per lane, C the V4-8 colour after ITOF0 |
| 0x0080 | `passthrough` | none | the V4-8 colour at TOPS+0x80, alpha included |

The nine-qword matrix rows are qwords 0xC9..0xCB and direction j is `(q0[j], q1[j], q2[j])`; C3..C8 are qwords
0xCC..0xD1. The V3-8 normal at 0x80 is converted by the 0x1878 prologue as `ITOF0(byte) * (1/127)`, the multiply the
microcode performs; for sixteen int8 values this differs by one ulp from a division, and the body formula now uses the
same multiply (it moved the matched body residual from max 2 / rms 0.188 to max 1 / rms 0.1696, the round 12 figure).

Three corrections to the round 14 summary, each measured with the real AOT fragment through the probe harness:

- **0x0250 is the power-8 variant, not first power.** With N = (127,127,115)/127 and C4..C7 = 10, first power gives
  29, power-8 gives 24 and power-16 gives 22; the fragment writes 24 for dispatch 0x0250 (loop 0x0290). `nine_qword_pow8`
  is therefore a separate program with three squarings. The round 13/14 table's "first-power" grouping was wrong.
- **The six-qword `t` is signed.** The fragment clamps only after the weighted sum: with `t0 = -0.3085` and
  `t1 = 0.9368` (measured with the harness), `C9 = 1`, `CA = 1` gives a scalar of 0.6283, not `t1`; the fragment's output
  confirms the signed sum. Clamping each `R*q` component first would be wrong.
- **The six-qword output uses `L.x` only.** The fragment broadcasts lane 0 of the clamped L before the three squarings;
  a block whose L1/L2 are non-zero but L0 is zero stores zero. The per-lane coefficient multiply `CD * scalar` is kept.

### 14.2 Channels and the per-item design

`MeshVertex` names its original draw item (`item`), and the shape decoder gained three channels beside the body normal
at 0x40 and the V4-8 colour at 0x80: `normals80` (V3-8 at 0x80), `normals80_float` (V3-32 at 0x80) and `has_color` (a
V4-8 colour was present). The channel rule is unchanged otherwise: the UV channels at 0x40 and the strip table still
classify exactly as before, and a V3-32 channel at 0x80 is the direct program's normal, not a UV. `shade_mesh_items`
decodes one `ItemLightState` per item and shades each vertex with the program and block of its own item, which is what
lets draw 98 mix two programs.

Fallbacks are explicit and counted per family and per status: an invalid run pair or unmeasured selector
(`no_program`), a missing block (`no_block`), a carried block after a chain discontinuity (`uncertain_block`), a qword
count/command that is not the form the program reads (`block_variant`), a NaN/infinite qword (`block_not_finite`), a
vertex without a channel the program reads (`missing_channel`) and zero-length/non-finite math (`invalid_math`). A
carried block is accepted (it is the VU1's carried state; a known command must match, zero is accepted because the
capture may not keep a carried tag word). `passthrough` reads no block, so its `light_source` does not gate it; the 5
`carried_uncertain` items of frame 0 are exactly this case and are shaded from their colour.

### 14.3 Evidence from the v4 capture and tap (`build/evidence/coregs3-r14/`, frame 0, VBlank 7398)

`gt4_coregs3_render --shade-dump` writes the per-vertex predicted RGBA and status; `validate_final.py` pairs the items
with the tap (the round 14 job/source rule) and compares. Items whose captured inputs reproduce the tap:

| program | vertices | rms R/G/B | max R/G/B | within 1 | alpha exact |
|---|---|---|---|---|---|
| body_paint (matched; draws 91/93 excluded) | 3,826 | 0.1696 / 0.1696 / 0.1696 | 1 / 1 / 1 | 3,826 | 3,826 |
| nine_qword_first | 2,507 | 0.1957 / 0.1635 / 0.1468 | 1 / 1 / 1 | 2,507 | 2,507 |
| nine_qword_pow8 | 636 | 0.4590 / 0.1190 / 0.6729 | 1 / 1 / 2 | 634 | 636 |
| nine_qword_pow16 (matched; draw 90 excluded) | 2,416 | 0.1678 / 0.1380 / 0.1037 | 1 / 1 / 1 | 2,416 | 2,416 |
| nine_qword_direct (matched) | 52 | 0 / 0 / 0 | 0 / 0 / 0 | 52 | 52 |
| six_qword_no_colour | 60 | 0.1291 / 0 / 0 | 1 / 0 / 0 | 60 | 60 |
| three_qword_gain | 930 | 0 / 0 / 0 | 0 / 0 / 0 | 930 | 930 |
| passthrough | 1,351 | 0 / 0 / 0 | 0 / 0 / 0 | 1,351 | 1,351 |

The exceptions are the known round 12/14 ones, and they are data, not formula: draws 91/93 (184 vertices, max 18),
draw 90's pow16 item (52 vertices, max 9) and the six direct items of draws 86/88/89/94 (308 vertices, max 17). Rerunning
the real AOT fragment on the captured inputs of each exception reproduces **this module's** colour exactly (checked on
93/1 and 90/0 against the fragment and on the two pow8 outliers), so the module is faithful to the fragment and the tap
residual is the capture's input or the tap pairing. The two pow8 vertices with max 2 are tap-pairing outliers: the
fragment with the captured normal and block writes the module's 88/72/58, not the tap's 89/72/60.

### 14.4 Renderer evidence

Offline (`gt4_coregs3_render <v4 capture> --frame 0`): 116/116 draws and 12,146/12,146 vertices lit, 0 fallback;
body 80 items/3,918 vertices (27 draws), nine-first 112/2,507 (46), pow8 21/636 (6), pow16 62/2,384 (14), direct 8/360
(4), six-no-colour 1/60 (1), three-qword 25/930 (15), passthrough 26/1,351 (11); light sources 177 uploaded, 153
carried, 5 uncertain. Draw 98 alone: 1,226 vertices, 14 nine-first items + 17 pow16 items, one draw. Images
`build/evidence/coregs3-r18/offline-frame0.png` (and `-frame1`, `draw98`, `draw0`). The evidence A/B
`lighting-ab-frame0.png` renders the same frame with `--no-programs` (all items' run pairs invalidated): 4,443 pixels
change and 272 pixels that were the `(255,255,255)` fallback become shaded. Draw 98 alone (`draw98-ab.png`) is stronger:
its 1,831 covered pixels are all `(255,255,255)` in the fallback A/B and all become the shaded glass/lamp, so the
previously white parts are lit by the new programs.

The live `present` layer now calls the same `shade_mesh_items`; its F8 counters gained per-family and per-status lines
and the lit/fallback vertex counts. The shader remains vertex-colour only.

### 14.5 Gates

| Gate | Result |
|---|---|
| CTest `native_coregs3` family | 6/6 passed (`native_coregs3`, `_vertex`, `_lighting`, `_lighting_families`, `_timeline`, `_texture`) |
| Python `test_coregs3_units.py` | 7/7 passed |
| 3x off/shadow identity gate (final candidate, main worktree reference exe) | **passes**: 8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences, 13/13 frames identical for off and shadow; capture 4,451,628 B; `COREGS3-NIGHT-CHECK-OK` |
| Present-mode guest identity run (`GT4_COREGS3=present`, same 3x night run) | **passes**: 8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences; 13 passes, 1,377 draws, 338,934 vertices, 112,978 triangles, 0 refused; family vertices lit: body 48,512, first 27,043, pow8 24,208, pow16 16,640, direct 7,872, six 432, three 9,272, pass-through 11,547; 40 unlit draws / 9,282 unlit corners (120 `no_program` items, 3,962 vertices, kept on their base colour); light sources 2,150 uploaded / 1,856 carried / 33 uncertain / 0 none; the 13/13 dumped frames differ from the reference in the overlaid car pass |

## 15. Round 19 (2026-11-10, branch `task/090-texture-decode`): the GS memory sidecar and the native car-texture decode

The `.gt4scene` capture carries the 40-byte PGLUtexture record of every item but no texture pixels, and the VU1 tap is
PATH1 only. This round makes the GS local memory (SLM) available to the offline tools and decodes the measured car
textures natively from it. The round is texture evidence only: no renderer samples a texture yet, the UV convention and
the lighting are untouched and the `.gt4scene` format stays at version 4.

### 15.1 The `.gt4vram` sidecar (writer: CoreGS 2.0 `core2::Renderer::Flush`)

`GT4_COREGS3_VRAM=<file>` makes the GS worker copy a bounded SLM block range once per captured VBlank and write the
file at shutdown (never on the guest's timeline, the rule of §7). It reads `memory_.Local()` — the same CPU shadow
memory the CoreGS 2.0 texture dump hashes — and never writes guest memory or renderer state.

| Variable | Meaning |
|---|---|
| `GT4_COREGS3_VRAM` | sidecar path; unset disables the dump |
| `GT4_COREGS3_VRAM_VBLANKS` | `a-b`, the GS worker's VBlank counter at the flush; defaults to `GT4_COREGS3_CAPTURE_VBLANKS` |
| `GT4_COREGS3_VRAM_BLOCKS` | `a-b` GS blocks (inclusive, base 0); default the measured car range `0x2BC0..0x38FF` |
| `GT4_COREGS3_VRAM_MAX_FRAMES` | record bound, default 32 |

File layout (little endian), one or more records concatenated:

```
u32 magic 'G','T','4','V' (0x56345447)   u32 version 1
u64 vblank                              u32 first_block   u32 block_count
u32 byte_count (= block_count * 256)    u32 reserved (0; refused when nonzero)
block_count * 256 bytes                 block `first_block` at offset 0
```

Sidecar alignment: the GS worker flushes after the VSYNC that ended the capture interval, so the record with
`vblank = V` covers the capture frame whose `frame.vblank = V - 1`. The tools select the record by that rule
(`gt4_coregs3_texture` does it automatically; `--vram-vblank` overrides it for diagnostics). The decoded evidence below
uses a capture at VBlanks 7398-7399 and sidecar records at 7399 and 7400.

### 15.2 The decoder (`native/coregs3/coregs3_texture.{hpp,cpp}`)

Vulkan-free, built into `gt4_coregs3` only when `GT4_HW_GS=ON` because it reuses our own MIT pieces:
`hwgs::memory::LocalMemory` (swizzle and pixel addressing), `hwgs::clut::ClutUnit::PeekLoad` and
`hwgs::clut::DecodeHalfwords` (CSM1 palette), `hw_gs_replace` (`ClutBuffer`, direct-format expansion, Format G key) and
the CoreGS 2.0 dump's RGBA loop. There is no second swizzle or palette decoder.

`decode_texture(slm, record, out)` parses TEX0/TEX1/CLAMP, decodes the CLAMP region rectangle (the same rule as
`DumpTexture`), checks that every texture and palette block is inside the sidecar, builds the CSM1 palette from CBP/CSA,
returns RGBA8 + width/height + a `hw_gs_replace::FormatGKey` (payload hash, palette hash, hashed width/height), the
Format P key/name of the reference dump, and a status. `refused_*` statuses: `refused_record` (short record),
`refused_psm` (only T4/T8/CT32), `refused_size` (TW/TH > 10), `refused_state` (mipmaps, CSM2, CLD != 1, a non-CT32
palette, CSA > 15), `refused_truncated` (the sidecar does not cover a block) and `refused_palette`. The Format G key
excludes the base addresses: moving TBP0 with the same bytes keeps it, a payload byte breaks the payload hash and a
palette byte breaks the palette hash (all three pinned by the fixture).

**Measured correction: CSA selects a palette-bank group at sampling time.** The 32-bit CSM1 palette is the 16x16 bank
at CBP; a 4-bit index reads the 16 entries of group CSA, not the bank's first group. The reference dump proves it: draw
2 (CSA 0) matched with either reading, draw 3 (CSA 6) matched the CoreGS 2.0 dump byte for byte only when the decoder
read bank group 6 (`19b46aeda67bdc9-a03e7dc7de2a8b02-00001554.png`). The decoder therefore loads the full bank unshifted
with `PeekLoad` (the T8 profile, CSA 0) and lets `DecodeHalfwords` apply the record's CSA. `hw_clut`'s `ClutUnit`
applies CSA at load time instead, which cancels out for an 8-bit index but makes a 4-bit index read the first group; that
model is left untouched here (CoreGS 2.0 has its own gates) and is the reason the decoder does not call `PeekLoad` with
the record's own T4 profile.

### 15.3 Tool and fixtures

`gt4_coregs3_texture <capture> --vram <sidecar> --frame N --out <folder> [--draw N ...] [--reference <TextureDumps>]
[--psnr] [--report <file>]` decodes the selected draws and writes one PNG per unique texture record. With `--reference`
it compares every image with the CoreGS 2.0 `GT4_HW_DUMP_TEXTURES` set by exact RGBA equality (and by PSNR against
same-size candidates with `--psnr`). `--stability --draw N` decodes the draw in every captured frame and compares each
record's key and RGBA across the frames.

`coregs3_texture_decode_fixture` (`native_coregs3_texture_decode`) is the hand-authored control: a synthetic 4 MiB SLM,
T4/T8/CT32 records with known texels and palettes (CSA 0 and 6, the T8 CSA clamp path), the key properties and every
refusal, plus the sidecar header round trip and its malformed variants.

### 15.4 Evidence (night race, 3x, headless, bench card)

`night-run.sh td-shadow` (candidate, `GT4_COREGS3=shadow`) with `GT4_COREGS3_CAPTURE_VBLANKS=7398-7399` and
`GT4_COREGS3_VRAM_VBLANKS=7398-7400`, 7,500 VBlanks: capture 819,808 B (2 frames, VBlanks 7398/7399, 116 draws, 169
texture records, 39 material records, 182,480 packet bytes each), sidecar 2,605,152 B (3 records at VBlanks 7398, 7399
and 7400; blocks 0x2BC0..0x38FF, 868,352 B each). The reference build (main worktree `coregs3-r8/ref`) ran the same
night race with `GT4_HW_DUMP_TEXTURES=1`: 1,481 PNGs in `build/evidence/coregs3-td/TextureDumps/`.

`gt4_coregs3_texture ... --frame 0` decoded the textures of all 116 draws: 204 per-draw decodes (a record used by two
draws is decoded for each), **204 ok, 0 refused**, 148 matching a reference PNG by exact RGBA equality; draw 2 and
draw 3 are exact:

| Draw | Record | Format | TBP0 / CBP / CSA | Decode key (Format G payload/palette) | Reference |
|---|---|---|---|---|---|
| 2 | 0x01AB0E70 | T4 32x32 | 0x33A0 / 0x33A2 / 0 | `aaaa07528cd1c1ea` / `003013bc5012c5f0` | exact `5d6616a3c740d56a-3013bc5012c5f0-00001554.png` |
| 3 | 0x01AAC070 | T4 32x32 | 0x3351 / 0x3348 / 6 | `34f6bba4ed22ef60` / `a03e7dc7de2a8b02` | exact `19b46aeda67bdc9-a03e7dc7de2a8b02-00001554.png` |

Frame 1 (VBlank 7399) decodes the same two records to the same keys and the same two reference files. The stability
mode compares every record of the draws across the two frames: **key and RGBA equal** for both
(`aaaa07528cd1c1ea/003013bc5012c5f0` and `34f6bba4ed22ef60/a03e7dc7de2a8b02`). The five T8 records of the frame decode
as 256-entry palettes; four match the reference exactly (draws 21, 40, 57, 81) and draw 114's has no reference name
match. The one CT32 record (the 256x256 car env map, record 3 at 0x006DDD60, TBP0 0x2BC0, TW/TH 8/8) decodes natively
(23,482 distinct RGBA values) but has no reference dump: `GT4_HW_DUMP_TEXTURES` captures memory textures only, and the
env map is a drawn target, so the reference's cache does not dump it. The 2 same-payload names with a different palette
and the 54 no-match records are reported, not hidden: the reference names by Format P, whose hash can include the upload
extent and target-backed content, so a matching name is not expected for every record; the exact-equality count is the
control that matters for the records that do match.

Images and reports: `build/evidence/coregs3-td/textures/` (`v7398_d2_*`, `v7398_d3_*`, `v7398_d5_tex3_*CT32_256x256.png`,
`report-frame0-all.txt`, `report-frame1.txt`, `stability-draw2-3.txt`); the reference set is
`build/evidence/coregs3-td/TextureDumps/`.

Gates: CTest `native_coregs3` family 7/7 (new `native_coregs3_texture_decode`), Python `test_coregs3_units.py` 7/7, and
the 3x off/shadow identity gate with the main worktree reference exe (`COREGS3-NIGHT-CHECK-OK`; see §15.5).

### 15.5 Gates

| Gate | Result |
|---|---|
| CTest `native_coregs3` family | **7/7 passed** (`native_coregs3`, `_vertex`, `_lighting`, `_lighting_families`, `_timeline`, `_texture`, and the new `_texture_decode`) |
| Python `tests/python/test_coregs3_units.py` | **7/7 passed** |
| Fixture `coregs3_texture_decode_fixture` | **ok**: synthetic 4 MiB SLM, T4 CSA 0/6, T8 CSA 2, CT32 direct, the three key properties and all seven refusals, sidecar round trip and malformed variants |
| 3x off/shadow identity gate (`coregs3-night-check.sh`, main worktree reference exe) | **COREGS3-NIGHT-CHECK-OK**: 8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences, 13/13 frames identical for off and shadow, capture 4,451,628 B |
| Real decode against the CoreGS 2.0 reference | 204 per-draw texture decodes on frame 0, 0 refused, 148 exact RGBA matches; draws 2 and 3 exact; keys and RGBA stable across the two captured frames |

## 16. Round 20 (2026-11-10, branch `task/090-textured-render`): the first textured offline car frame

The offline reference renderer now samples the decoded car textures (§15). The work is the first milestone of the
texture direction in TASKS.md TASK-090; it does not touch the E1/EE hooks, the capture format or the lighting and
vertex conventions.

### 16.1 The sampler (`native/coregs3/coregs3_sampler.{hpp,cpp}`, always built)

The measured car-pass subset, in GS terms and independent of the decoder:

- formats T4, T8 and CT32 (the decoder supplies RGBA8, so the sampler never re-decodes);
- MODULATE only: `rgb = min(255, (T * C) >> 7)`, `a = min(255, (Ta * Ca) >> 7)` when TCC = 1, else `a = Ca`;
- level 0 only (TEX1.MXL must be 0); nearest or linear per record, and MMAG must equal MMIN because the LOD that would
  choose between them is not computed (the night capture carries 156 linear/linear and 13 nearest/nearest records);
- the four CLAMP wrap modes (REPEAT, CLAMP, REGION_CLAMP, REGION_REPEAT) with the chapter 01.1 formulas; the measured
  car pass uses region clamp with the full-size region or repeat;
- bilinear with the spec's 10.4 coordinate, `(u - 0.5)` floor base, 4-bit weights and a `>> 8` sum (the same arithmetic
  as `native/hw_gs/pixel`);
- alpha test: ATE with the eight ATST comparisons and AREF; only AFAIL KEEP is reproduced (a failing fragment writes no
  colour and no depth). DATE and every other AFAIL are counted refusals;
- blends: the measured normal `(Cs - Cd) * As + Cd` and additive `(Cs - 0) * Ad + Cd`, evaluated as the GS integer
  `(((A - B) * C) >> 7) + D` and clamped (COLCLAMP = 1); another equation is refused;
- FRAME.FBMSK lane merge and ZBUF.ZMSK (no depth write).

A wrapped texel outside the decoded rectangle, a mixed filter pair, a mip level, another TFX and an unsupported PSM are
named refusals; nothing is zero-filled or guessed.

### 16.2 The frame table (`native/coregs3/coregs3_texture_frame.{hpp,cpp}`, `GT4_HW_GS` builds)

The table joins a capture, its `.gt4vram` sidecar and an optional `.gt4tap`:

- every texture record an item names is decoded once, and records sharing a Format G key share the decoded RGBA8
  (frame 0: 167 records decoded, 10 cache hits, 0 refused);
- each item binds a `SampledTexture` only when it carries a model UV channel; the body/paint family is deferred (its
  CT32 256x256 environment map is sampled with VU1 texgen ST, a later task), and the reason for every untextured item
  is counted (texgen, no record, no UV, decode refusal);
- the per-item pixel state comes from the tap with the `uv` gate's proven pairing (§15.4): the frame's jobs are the
  tap source records with `vblank == frame.vblank` (the PATH1 GIFs arrive about three VBlanks later), the A+D state is
  tracked across the whole tap in emission order (TEST, ALPHA, FRAME, ZBUF; PRIM from the vertex), and each item takes
  the next vertex record with its vertex count and TEX0 (TBP0, PSM). Frame 0: 327 of 335 items paired, 8 defaulted to
  the measured state; 116 pairings had a later candidate with the same count and TEX0 (reported).

`gt4_coregs3_tap` now reads the tap through the same `coregs3_tap_file` reader.

### 16.3 The renderer and the tool

`coregs3_render` gained the texture path: the model UV interpolates with 1/w (the VU1's Q cancels in S/Q), the fragment
is sampled with the lit vertex colour as MODULATE's C, the alpha test runs before the depth test, then the depth test,
the blend, FBMSK and the optional ZMSK depth mask. An optional output marks the textured draws' geometric coverage. The
untextured path is unchanged (the pre-texture picture is the A/B). `gt4_coregs3_render` gained `--vram`, `--tap`,
`--no-textured`, `--texture-body`, `--texture-mask` and `--mask`; it selects the sidecar record `V = frame.vblank + 1`
(the measured alignment) and reports the decode, binding and raster counters.

### 16.4 Evidence (frame 0, VBlank 7398, night race)

- 335 items: 216 textured, 119 untextured (80 texgen, 36 no record, 3 no UV), 0 refused.
- 1x: 7,196 textured fragment writes, 626 alpha-test failures, 1,563 FBMSK pixels, 596 ZMSK pixels, 0 blend pixels
  (the car PRIM has ABE off in this frame); the textured draws cover 2,653 of the 4,278 covered pixels.
- 3x: 64,325 textured fragment writes; the textured draws' mask is 23,823 px of the 38,446 covered px.
- Against the CoreGS 2.0 VBlank-7400 reference (capture +2), inside the textured draws' mask:
  PSNR 17.071 -> 17.612 dB, SSIM 0.2165 -> 0.2219, pct_bad 90.035% -> 88.381%. The 2,809 pixels the textures visibly
  change go from 12.870 to 14.929 dB. Inside the whole car-pass coverage: PSNR 18.738 -> 19.227 dB. The remaining
  difference is the pre-existing geometry/lighting gap of the CPU reference at the two-frame pairing (no fog, no
  texgen); the texture decode itself is the byte-exact one of §15.4.
- Images: `build/evidence/coregs3-textured/` (`offline-textured-frame0.png`, `offline-untextured-frame0.png`, the 3x
  frames and masks, `metrics.txt`, the tool reports).

### 16.5 Gates

| Gate | Result |
|---|---|
| CTest `native_coregs3` family | **8/8 passed** (the seven of §15.5 plus the new `native_coregs3_sampler`) |
| Python `tests/python/test_coregs3_units.py` | **7/7 passed** |
| Fixture `coregs3_sampler_fixture` | **ok**: hand-authored T4/T8/CT32 textures, MODULATE exact 8-bit, nearest/bilinear, the four wrap modes, the alpha test with KEEP and the DATE/AFAIL refusals, normal/additive blends, FBMSK and a textured quad through `render_frame` with an alpha-test-failed pixel, a blended pixel, FBMSK and ZMSK |
| Build | full `GT4_HW_GS` candidate builds clean |
| 3x off/shadow identity gate (`coregs3-night-check.sh`, main worktree reference exe) | **COREGS3-NIGHT-CHECK-OK**: 8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences, 13/13 frames identical for off and shadow, capture 4,451,628 B |

## 17. Round 21 (2026-11-10, branch `task/090-capture-v5`): the pass tag, the scene environment and the GS state journal (format v5)

One problem: make `.gt4scene` version 5 name every captured draw's pass and carry the scene environment and a bounded
per-pass GS state journal, with `GT4_COREGS3_ALL_DRAWS` tagging the whole race frame.

### 17.1 The caller return address was measured first, and it cannot separate the passes

K1 sits inside `ModelSet2___render`, whose prologue (translated `L_00455708`) saves its caller's `ra` at `sp + 168`. The
hook reads that word per draw (`CapturedDraw::caller_ra`). On the night race (VBlank 7398, ALL_DRAWS, 282 ModelSet2
draws) the whole frame comes from ten call sites:

| caller RA | draws | what it is |
|---|---|---|
| `00396F74` | 130 | `CourseData__render_course_2` -> `ModelSet2___render` (main course **and the cars**) |
| `004555F0` | 87 | `func_00455598` (sky, far underlay parts, nested model sets) |
| `0044E610` | 25 | `func_0044E660` env map -> `ModelSet2__render` |
| `00455344` | 9 | `func_00455300` |
| `00455568` | 7 | `func_00455528` (far underlay **and** the env-map model set) |
| `00452614` | 5 | `ModelSet2__render` caller in unit_00D4 |
| `00452724` | 5 | `ModelSet2__render` caller in unit_00D4 |
| `00452A58` | 5 | `ModelSet2__render` caller in unit_00D4 |
| `003B114C` | 5 | unit_00AC |
| `0039724C` | 4 | `func_003971F0` course model list |

The same low-level functions are called from several passes: `CourseData__render_course` renders the main view **and**
the back mirror, `func_00396B80` (sky) is also called by the reflection and the mirror, and the cars render through the
same `CourseData__render_course_2` path as the course. A one-level RA therefore puts the 116 car draws in the same
bucket as the 130 course draws (`00396F74`), which the existing K5 bracket already separates by time. **The RA cannot
name a pass on its own**, so the least invasive complete mechanism is:

* **K7 begin/end markers** around the pass calls of `func_0033EAB8` (env map, sky, far underlay, course, reflection,
  flares, concourse, render_after, mirror, crowd/pitmen, particles, post) and around the HUD panel dispatch: thirteen
  brackets in the already instrumented unit_008F;
* **direct K1 sites** for the three shape calls that do not go through `ModelSet2___render`: the car env map
  (`0x0044E920`, `0x0044EE24`), `BillboardSet__render` (`0x004507AC`) and the tachometer (`0x003EC4A0`). The direct call
  statements assign `a0` inside the statement, so the hook is inserted after that assignment (`inside_after`), not
  before the statement;
* the caller RA is still stored in every draw as evidence, and every draw records its `call_site` enum.

The pass ids follow the race order: `unknown`, `env_map`, `sky`, `far_underlay`, `backdrop` (PATH3 2D, no K1 draw today),
`course`, `billboards` (a direct billboard call outside a marker), `reflection`, `cars`, `render_after`, `mirror`,
`flares`, `crowd_pitmen`, `particles`, `post`, `panel`, `concourse`. A draw outside every marker and direct call site is
`unknown` and is counted, never guessed; the whole 7,600-frame drive has many menu frames, which is why the run-level
`unknown` counter is large while the captured race frames have **0 unknown**.

### 17.2 Format version 5

* `CapturedDraw` gains `call_site` (0 model set, 1 billboard, 2 env map, 3 tachometer) and `caller_ra`; `pass` is now a
  `PassId` instead of the hardcoded 1.
* `FrameSnapshot` gains the `SceneEnvironment` block and a bounded `pass_states` journal (32 entries).
* Versions 1..4 are refused with an explicit message; there is no migration (captures are recaptured with a matching
  build).

### 17.3 The scene environment block

The block is not in the PGL context at `0x70002000` (those offsets are zero in the race). It is the scene object of
`func_0033EAB8`: `course = *(*(*(self + 0x6C) + 0x80) + 4)`, `scene = *(course + 0x80)`. The script reads
`scene + 0x9C0..0xB10` (sun, four fog tables, eight lights; 84 words), `scene + 0xC10..0xC48` (reflection parameters,
14 words) and `scene + 0xC60` (lights, one word), once per frame at the first world pass begin (sky or later). The
decompiled `func_00398650` reads `scene + 0x9C0/0x9C4/0x9C8/0x9D4/0x9D8/0x9DC/0x9E0` for the sky/flare pass and
`RacePS2Base__render_reflection` reads `scene + 0xC10..0xC28` and `+0xC40`, which fixes the base and the spans. The
environment is not captured at the env-map or panel markers: those run in other functions where `r17` is not the
RacePS2Base object.

### 17.4 The per-pass GS state journal

The PGL context keeps plain 64-bit GS register mirrors. The journal captures seven at each pass begin (raw words):

| register | PGL offset | tap confirmation (277 draws, night race) |
|---|---|---|
| FOGCOL | `0x138` | exact in 220 of 253 draws whose first vertex state is readable |
| FRAME | `0x170` | exact low 32 bits in 277 of 277, full 64 in 240 (the draw's own FBMSK write) |
| XYOFFSET | `0x178` | exact in 162 of 277 |
| ZBUF | `0x188` | exact in the draws that do not rewrite it (26 of 277; nearly every draw sets ZBUF) |
| SCISSOR | `0x190` | exact in 251 of 253 |
| ALPHA | `0x198` | exact in 244 of 277 |
| TEST | `0x1A0` | exact in 176 of 277 |

The same measurement found the neighbouring mirrors (`CLAMP` at `0x1A8`, `TEX1` at `0x1B8`, `TEX0` around `0x1B0/0x1C0`)
which are not part of this journal. The journal is filled at the pass begin, so a register the pass's first draw rewrites
shows the incoming value; the tap comparison is therefore reported as "where possible" (the register the draw did not
rewrite). The pass journal is bounded at 32 entries per frame and observe-only, like the rest of the hooks.

### 17.5 Evidence and gates (night race, 3x, bench card)

`GT4_COREGS3_ALL_DRAWS=1`, VBlanks 7398-7402 (five race frames), tap 7398-7406. Each frame captured **285 draws / 1,772
items / 77,882 submitted position vectors**, with **0 unknown**:

| pass | draws | items | submitted vertices |
|---|---|---|---|
| env_map | 31 | 121 | 3,988 |
| sky | 1 | 16 | 940 |
| far_underlay | 3 | 9 | 67 |
| course | 130 | 1,284 | 60,119 |
| reflection | 4 | 7 | 292 |
| cars | 116 | 335 | 12,476 |
| unknown | 0 | 0 | 0 |

The whole 7,600-frame drive (menus included) has 827,497 shape calls, 826,893 captured, **0 guest-read failures** and 0
pass-journal refusals; the 30 frames the capture refused are the pre-existing packet-cap frames of the run (overflow
`0x4`, none inside the capture window) and the capture wrote 5 of 5 requested frames (11,118,980 B). The pass journal
has 18,086 entries, 1,557 frames with the environment block, 0 environment read failures and 0 orphan/mismatched pass
ends. Without ALL_DRAWS the car block is unchanged: 116 draws / 335 items per frame, 0 refusals (capture 2,060,288 B).

The seven journal registers are the PGL mirrors of section 17.4. A pass-level comparison against the tap (only registers
the pass's own first draw did not rewrite) is exact: **FOGCOL 15/15, SCISSOR 25/25, XYOFFSET 5/15**; the 10 XYOFFSET
differences are the cars pass, which sets the car-view offset (0x7C08/0x7C08) after the marker while the journal holds
the incoming main-course offset (0x6C08/0x7208). FRAME, ZBUF, ALPHA and TEST are written by every pass's first draw, so
the pass-begin journal cannot be compared at that vertex; their exact mirror position is the 277-draw correlation of
section 17.4.

The scene environment block is valid on every captured frame and identical across the five (static scene data):
`scene+0x9C0` floats 0.7, 0.45, 0.1, 0.8 (sun/ambient), 0.2, 0.05, 0.1 (fog densities), 10, 86.05; the reflection words
read 1, 1, 1, 0.5 and the light word 1.41.

| Gate | Result |
|---|---|
| CTest `native_coregs3` family | **8/8 passed** (the scene fixture now round-trips v5: pass/call site/RA, environment, journal and the refusals) |
| Python `tests/python/test_coregs3_units.py` | **8/8 passed** (the new pass-marker and `inside_after` controls) |
| 3x off/shadow identity gate (`coregs3-night-check.sh`, main worktree reference exe) | **COREGS3-NIGHT-CHECK-OK** (see section 17.6) |

### 17.6 The 3x identity gate

The gate runs the plain-unit reference, `GT4_COREGS3=off` and `GT4_COREGS3=shadow` over the same 9,000-VBlank drive at
3x (`scripts/coregs3_bench/coregs3-night-check.sh`, reference `O:/Antigavity/gt4-wt-090-coregs3/build/coregs3-r8/ref/gt4vulkan.exe`).
Result: **COREGS3-NIGHT-CHECK-OK**, 8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences in both runs, 13/13
frames identical for off and for shadow, capture 4,477,368 B (the car block only, as the gate does not set ALL_DRAWS).


## 20. Round 20 (2026-11-10, branch `task/090-body-reflection`): the body/paint environment-map reflection

### 20.1 What the pass is

The body/paint item's only captured draw is the reflection pass (`program_dispatch` 0x1058, six-qword block, tag PRIM
0x7C): the tap's record for the item carries TEX0 0x2BC0 (the CT32 256x256 environment map; the item's own 40-byte
record) and the measured ALPHA additive (0,2,1,1), TEST DATE/DATM, ZBUF ZMSK=1 and ZTST GEQUAL state. The **base** pass
that writes the destination alpha is a different captured draw: the nine-qword/three-qword items whose tapped vertices
share the reflection item's positions (27 exact geometry pairs in frame 0, e.g. draw 4 -> 5, 6 -> 7, 10 -> 11/12). The
reflection therefore reads the real framebuffer alpha our textured base path wrote; a DATE test on a pixel no captured
draw wrote is counted as the missing-alpha fallback (108 pixels at 3x in frame 0).

### 20.2 The formula (`native/coregs3/coregs3_reflection.{hpp,cpp}`)

The round-18 pinned float32 texgen, with the item's own six-qword block (command 0x6C06C0C9), the raw V3-16 position and
the V3-8 normal / 127:

```
D = P - LP;  c = (D.x*N.x + D.y*N.y) + D.z*N.z;  R = D - N*c - N*c
V = (C9*R.x + CA*R.y) + CB*R.z;  W = V/|V| + (0,0,1);  q = 0.5/|W|
S = 0.5 + W.x*q;  T = 0.5 - W.y*q
```

Refusals: `refused_block` (not the six-qword 0x6C06C0C9 form), `refused_position` (no V3-16 raw position),
`refused_normal` (no V3-8 normal at TOPS 0x40), `refused_non_finite`, `refused_degenerate` (|V| or |W| zero). The
fixture pins three hand-authored vertices against the real AOT fragment's TOPS+0x40 output (the harness values
0.614925206/0.0710932612, 0.310672939/0.764279068, 0.6371454/0.38020727; our std::sqrt matches within the study's EFU
rsqrt bound, < 5e-4).

### 20.3 The renderer pass

`RenderOptions::reflections` makes a body/paint item draw its triangles as the measured second pass instead of the
opaque lit colour: the map is sampled with the measured sampler (linear/linear, region clamp 0..255), MODULATE's C is
the round-18 lit vertex colour, the GS fog mixes toward FOGCOL black with the tapped per-vertex fog
(`(f*c + (255-f)*fc)>>8`), the ATE/ATST/AREF test runs first, then the destination alpha test (DATE/DATM against the
framebuffer alpha), the GEQUAL depth test, the additive blend `(((Cs-0)*Ad)>>7)+Cd` (COLCLAMP 1), FBMSK and ZBUF's no
depth write. `--reflection-mask` writes the fragments the pass wrote. `--no-reflection` is the byte-for-byte A/B (the
body items keep the lit colour). The two draws' float projections agree to ~1e-4 relative where the GS's 24-bit Z is
identical, so the reflection's GEQUAL compare uses a 1e-3 relative window (the measured per-vertex depth difference of
the exact pairs is 0).

### 20.4 Correction: PRIM.ABE is bit 6

The sampler's `prim_blend_enabled` read ABE as bit 0. The GS PRIM register and `native/hw_gs/frontend/hw_gs_types.hpp`
`primbit::kAbe` say bit 6 (TME is bit 4). All measured car PRIMs are even (base 0x3C, reflection 0x7C), so the 0x3C/0x7C
difference the reflection needs was invisible before. With the fix the measured default state is PRIM 0x3C and five
items whose ALPHA equation is outside the normal/additive pairs are explicit counted blend refusals (they keep the lit
colour).

### 20.5 The table and the capture

`build_frame_texture_table` with `TextureFrameOptions::reflections` binds body/paint items as the reflection map (no
model UV), attaches the tapped state and the record's per-vertex fog, and tracks FOGCOL (register 0x3D) in the A+D walk.
The tool gained `--no-reflection`, `--reflection-mask` and `--vram-vblank`. The capture is the night race at 3x with
`GT4_COREGS3_CAPTURE_VBLANKS=7398-7399`, `GT4_COREGS3_TAP_VBLANKS=7398-7405` (the GIF records are emitted ~3 VBlanks
after the posting) and `GT4_COREGS3_VRAM_VBLANKS=7398-7403`; the frame's environment map is the sidecar record 7402 (the
map drawn in the frame the reflection is emitted in).

### 20.6 Evidence (night race, frame 0 VBlank 7398, 3x, bench card)

80 body/paint items reflected, **0 refused** (0 PSM/TFX/filter/mip/AFAIL/blend/ZTST, 0 missing ST, 0 missing tapped
state, 0 missing fog). Raster: 20,394 reflection fragments (18,392 mask pixels), 9,013 DATE-masked, 9,884 depth-rejected,
108 DATE tests on the uncaptured background alpha. Metrics against the CoreGS 2.0 VBlank-7400 reference:

| Mask | Pixels | PSNR before -> after | SSIM before -> after | pct_bad before -> after |
|---|---|---|---|---|
| reflection mask | 18,392 | 18.115 -> 17.906 dB | 0.2504 -> 0.2611 | 90.724 -> 72.053% |
| near-car box | 133,650 | 24.218 -> 24.572 dB | 0.6113 -> 0.6235 | 61.656 -> 57.909% |
| car-pass coverage | 38,439 | 18.800 -> 18.888 dB | 0.2369 -> 0.2838 | 73.418 -> 58.867% |
| full frame | 2,580,480 | 19.475 -> 19.477 dB | 0.5572 -> 0.5579 | 70.988 -> 70.771% |

The near-car box's luma ratio moves from 1.0894 to 0.9908. The reflection is visible on the paint; the base paints now
show through the additive pass and are darker than the reference because the base pass has no fog/lighting yet (the
next fidelity item). Images: `build/evidence/coregs3-refl/` (`offline-reflected-frame0.png`, the A/B, the 3x frames,
the masks, `metrics.txt`, the reports).

### 20.7 Gates

| Gate | Result |
|---|---|
| CTest `native_coregs3` family | **9/9 passed** (the eight of Â§16.5 plus the new `native_coregs3_reflection`) |
| Python `tests/python/test_coregs3_units.py` | **7/7 passed** |
| Fixture `coregs3_reflection_fixture` | **ok**: hand-authored formula cases pinned against the AOT fragment, every formula/binding refusal, the fog mix, and a two-draw render integration (DATE/DATM, additive blend, ZBUF no-write, FBMSK) |
| Build | full `GT4_HW_GS` candidate builds clean |
| 3x off/shadow identity gate (`coregs3-night-check.sh`, main worktree reference exe) | **COREGS3-NIGHT-CHECK-OK**: 8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences, 13/13 frames identical for off and shadow, capture 4,451,628 B |
