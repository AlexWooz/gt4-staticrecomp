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

> **Superseded for world textures by §24 (2026-11-10).** This section describes **version 1** (one frame-end snapshot per
> VBlank). Version 2 keeps that record (kind 0) and adds pass-begin, K1-draw and per-VU1-job records, which is what the
> offline world path uses; the environment table below still applies to the vblank record, and §24.2 lists the newer
> variables.

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

## 21. Round 22 (2026-11-10, branch `task/090-track-m1`): the world shape layout, the matrix source and the sky+road M1

### 21.1 The VIF UNPACK sign rule and the course lane accumulation

The course's V3-16 position channel is a delta chain: a STROW seed (the first vertex) plus signed 8/16-bit deltas
accumulated in the VU1's 32-bit lanes under STMOD 2, and the course program runs ITOF0 on the whole lane, not the low
halfword. The decoder now honors UNPACK bit 14 (the measured USN rule: **0 sign-extends the 8/16-bit element into the
32-bit lane, 1 keeps it zero-widened**, 32-bit elements unaffected) before the MODE/STROW addition, records each
command's USN bit and executing STMOD, and the world position family reads the full int32 lane. Evidence: the final
course-transform fit (`build/analysis-coursetx/out/verify_final2.txt`: 470 items, 19,317 vertices, 99.99% within 1 px)
and the new `native_coregs3_track` fixture.

### 21.2 The descriptor and the world shape layout (C/H)

Every item starts with a TOPS 0xC0 V3-32 descriptor: lane0 `0x8000 | vertex_count`, lane1 the per-shape selector, lane2
`0x1412`. The world item path reads `count` vectors from qword 0 after the packet ran, because the position arrays tile
VU1 memory contiguously:

- a V3-8 write at qword 0 commits the STROW seed (the first vertex);
- V3-8/V3-16/V3-32 arrays at TOPS 0x01..0x3F continue the list (measured course split: 1,022 items one array, 145 with
  aux arrays, 46 with a V3-32 position array, 6 with no position);
- the UV array tiles from qword 0x40 with the same count (V2-16 signed /4096 and V2-32 floats were fitted against the
  tapped S/Q,T/Q to <1e-4; V2-8 is decoded as signed /16, Hypothesis);
- the colour array is V4-8 at 0x80, or at 0xA0 for the A0 family; the strip table is the masked V1-8 write at 0x40 and
  its bytes live in lane 3.

The car pass keeps its own per-channel path; the two are separate branches and the car frames are byte-identical before
and after the change.

### 21.3 The 0xC1 matrix source per draw (C)

The guest upload runs when `!obj->valid || obj->value != cmd->value`, evaluated on the K1 pre-call snapshot: `valid` is
byte 3 of `pgl[0]`, `value` is `pgl[8]` (PGL +0x20). When it ran, the captured `matrix_packet` qwords 1..4 are the upload
columns (already scaled by `value`); otherwise the VU1 keeps the carried block, which is the captured `vf24..27 * value`
for a draw that did not rewrite VU0 (flag byte 0), or the last uploaded packet for a draw that did. The world renderer
counts each source; a non-finite matrix refuses the draw. Selected frame: packet 9, vu0 116, last-packet 2, missing 0.

### 21.4 The world item selection (C)

Sky (2) and far underlay (3) are drawn whole. The course (5) keeps the main-view ITOP 0x80 items with tapped XYOFFSET
`0x6C08/0x7208`; the 0x7C08/0x7808 and 0x7C08/0x7C08 items are the mirror/second view and are excluded with the
unmeasured 0xB08/0xA0/0x1278 programs, items without a position and items without a tapped state. Every exclusion is
counted (`coregs3_world.hpp`). Measured on VBlank 7398: sky 1/16/940, far underlay 3/9/378, course 123/1,192/61,173;
excluded 0xB08 15, 0xA0 46, 0x1278 11, mirror 14, no-position 6, 7 empty draws; 37 items use the draw's last paired
state or the course pass journal and are counted.

### 21.5 The world renderer state (H)

- **Sky.** Its captured matrix, tapped ABE/DATE, no fog; it is drawn as the background (no depth test/write) because its
  separate skybox w is not comparable with the course's view depth (the GS far-plane equivalent).
- **Course TEST/ALPHA.** The tap re-emits duplicate records of the same geometry with different TEST states; an exact-UV
  match can land on the alpha-tested duplicate (TEST `0x5820D`, ATE GREATER 32) while the pass runs `0x5800E` (ATE off).
  With the CT32 palette's alpha 0 that rejects the road the reference shows. The K7 pass journal supplies the course
  TEST (and the per-item PRIM/FBMSK/ZMSK/fog stay per item); the journal TEST restores the road and matches the
  no-alpha-test A/B exactly.
- **Mips/filters.** A world record with TEX1.MXL > 0 is decoded at level 0 and its mixed mip filter (MMIN 2/3 vs MMAG)
  samples level 0 nearest, both counted; the car path keeps its refusal. (The two unique course records that carried the
  mixed filter also carried MXL > 0, so the measured frame reports two level-0 fallbacks and zero separate filter
  fallbacks.)
- **AFAIL.** All four write modes are implemented for world fragments (KEEP, FBONLY, ZBONLY, RGBONLY).

### 21.6 Evidence and limits (H)

`build/evidence-m1/`: fresh v5 ALL_DRAWS capture (VBlanks 7398-7402, `.gt4vram` 0x2BC0..0x3FFF, tap 7398-7406), the 3x
world image and mask, the sky-only image/mask (the skybox covers the whole viewport), `--no-sky`/`--no-course` A/Bs,
`world-metrics.json`, texture dumps, and the byte-identical car regression. Measured against the r13 reference frame
07400: PSNR 20.36 dB, SSIM 0.6138, pct_bad 77.22%, mean |diff| 20.31, coverage 100% of the scene area, car spill 1.61%,
luma 26.69 -> 31.20. The Hyp thresholds (24 dB / 0.75 / 10%) are missed: the course surfaces dominate (no-course
13.68 dB), the decoded texture pages look like atlases, and many surfaces are brighter than the night reference. The
capture's per-item texture record does not always equal the TEX0 the GS has in force at the draw (the external texture
cursor), and some materials belong to the excluded programs or the EE post chain. M2 should resolve the external
texture cursor per item before tightening the image gate.

## 22. Round 23 (2026-11-10, branch `task/090-track-textures`): the world binds the GS state the VU1 emitted

### 22.1 The per-item record is not the emitted state (C)

The M1 lead is confirmed and sharpened. The tap emits, before every texture change, a vertex-less GIF record with
`CLAMP`, `TEX2`, `TEX1` and `TEX0` in that order (measured on the course jobs: 675 TEX0, 675 TEX1, 675 CLAMP and 645
TEX2 writes for frame 0). The emitted TEX0 has **CLD = 0**; the palette bank is uploaded by the preceding TEX2 with
**CLD = 1, CSA = 0** and a load class the VU1 picks from the sampling CSA: **T4 (16 entries) when the texture samples
CSA 0, T8 (256 entries) otherwise**. The 40-byte record carries the same texture with **CLD = 1** (there is no TEX2 in
the record), so a decoder that trusts the record's CLD is reading a state the GS never had at the draw.

The record is right for most items, but not all: with the tie-break pairing below, **22 of the 1,215 textured selected
items** have a record TEX0 that differs from the emitted one (18 texture pages, 2 formats, 22 palette states; TEX1
differs on 20, CLAMP on 2). Those are the external-cursor cases the M1 report named. The other differences the first
census reported (46) were pairing artifacts: repeated quad UVs make several tapped streams tie exactly, and the old
global best-UV rule picked the first, which can be a different item's texture. The texture-state pairing now prefers,
among tied streams, the one whose TEX0 (CLD ignored) matches the item's record (210 tie-breaks); 2 ties had no
record-matching stream and keep the first stream (counted). The selection state (TEST/ALPHA/XYOFFSET/test-zero) keeps
the old global UV pairing unchanged, so the pass selection is identical to M1.

### 22.2 The decoder-from-state path (C)

`decode_texture_state` (coregs3_texture.{hpp,cpp}) builds the record fields from the tapped TEX0/TEX1/CLAMP and then
runs the same decode body as the record path (format, size, CLAMP rectangle, palette, Format G/P keys), so the two
cannot drift. For an indexed texture:

- TEX0.CLD = 1 keeps the record path's measured rule (the TEX0 write loads the palette from CBP);
- TEX0.CLD = 0 requires `TEX2` with CLD = 1, CSA = 0, CBP/CPSM/CSM equal to TEX0's, and the measured load class
  (T4 with a sampling CSA of 0, or T8 for any CSA); the sampling CSA is TEX0's;
- every other state is a counted `refused_state` naming the register (no TEX2, a different CBP/CPSM/CSM, a nonzero
  TEX2 CSA, a T4 load with a nonzero sampling CSA, CLD 2..7);
- the world path's level-0 mip fallback applies to the tapped TEX1 exactly as it did to the record.

The record path is untouched: CLD must still be 1 and every old refusal keeps its meaning. A missing TEX0/TEX1/CLAMP
in a tapped state is a counted refusal, never a record substitution; only a completely absent tap state falls back to
the record (35 selected items).

### 22.3 Measurements and evidence (C/H)

Frame 0, VBlank 7398, 3x, against the r13 reference frame 07400. Selected world items: sky 1 draw/16 items, far
underlay 3/9, course 123/1,192. Bindings: **1,180 tapped-state, 35 record fallback** of 1,215 textured (2 items have
no record and keep the vertex colour). The change touches 3,579 pixels of the 1.78 M-pixel world mask; 2,364 of them
move closer to the reference and 887 farther. World metrics: PSNR 20.36 -> 20.36 dB, SSIM 0.6138 -> 0.6137, pct_bad
77.22 -> 77.19%, mean |diff| 20.31 -> 20.29, luma 26.69 -> 31.19 (reference 26.69). The baseline is exact: the M1
`world-3x.png` is reproduced byte for byte by the M1 executable (the same branch commit).

The road itself is unchanged by the binding because its records already equal the emitted TEX0; the decoded pages are
asphalt, not atlas pages (textures 18/19/20 are grained dark asphalt with slab patches, verified by forcing the alpha
lane opaque). The remaining difference against the reference is material/lighting (flat vertex shading and the tapped
fog) plus the excluded program layers (0xA0 = draw 85's 46 black-texture items, 0xB08, 0x1278 and the EE post chain).
The bright surfaces the M1 report flagged are the same class: draw 35's pit-lane plane (texture x warm vertex colour),
the top-right grandstand and the gray PIT sign item, not missing textures.

### 22.4 Checks (C)

- CTest coregs3 11/11, including the new `native_coregs3_texture_state` (state/record byte agreement at CSA 0/2/4/6,
  the CLD = 1 state, the TEX2 load classes, the T8 palette groups, TEX1/CLAMP from the state and every refusal).
- Python `tests/python/test_coregs3_units.py` 8/8.
- The car render (no `--world`) is byte-identical between the M1 executable and this build on the same capture and
  command (PNG SHA-256 equal; the reports equal too), so the car path is untouched.
- Full build green; the 3x off/shadow identity gate (`coregs3-night-check.sh`) against the main worktree's reference
  executable passes: **COREGS3-NIGHT-CHECK-OK**, 8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences and
  13/13 frames identical for off and shadow (capture 4,477,368 B), `build/evidence-track-textures/gate-run.log`.
- Evidence under `build/evidence-track-textures/`: the before/after images and masks, the metrics JSONs, the reports
  with the per-state binding table and the record-vs-tap list, the road crop and the draw-alone renders.

The remaining image gate (24 dB / 0.75 / 10%) stays a Hyp target for M2: this round removes the texture-state
uncertainty, but it cannot add the excluded passes or change the shading without changing the selection the owner
froze for M1.

## 23. Round 24 (2026-11-10, branch `task/090-world-families`): the 0xB08/0xA0/0x1278 course programs

The world selection used to exclude 72 course items by VU1 selector (0xB08 15, 0xA0 46, 0x1278 11). This round decodes
them from the full-corpus AOT output (426 entries; the union corpus lacked 0x00A0 and 0x1278) and the v5 ALL_DRAWS
capture, and brings them into the offline world render.

### 23.1 The programs (C)

- **0x0B08** is the round-18 nine-qword first-power family. The course item carries the same nine-qword block
  (`0x6C09C0C9`), V3-16 positions at qword 0, the V3-8 normal at TOPS 0x80 and V2-16 UVs at 0x40. Formula unchanged.
- **0x1278** is a pre-pass, not an output program: the AOT body (entry key `FE920756A7E31D4D`) converts the integer
  position lanes and the V3-8 normal at 0x40 (times the microcode constant 1/127) to floats, then dispatches to the
  body/paint output routine at 0x12C8. That routine reads the six-qword body block at 0xC9..0xCE, so the lighting
  formula is round 12's body formula, with the position being the full STMOD 2 lane (the value ITOF0 converts), not the
  low halfword. Measured: the chain reproduces the tap RGBAQ on 524/524 vertices of the 11 frame-0 items.
- **0x00A0** is a two-key morph pre-pass (entry key `2C8C8E20B5ECB957`): it lerps positions at 0x00/0x20, UVs at
  0x40/0x60 and colours at 0x80/0xA0 with the factor at TOPS+0xF2 lane 3 (`vf20 = 1 - s`, `vf21 = s`), writes the
  result back and dispatches to the shared output at 0xB0. On the capture the position keys are bit-identical for every
  item (230/230 over five frames), so the geometry is independent of the factor; the colour keys differ on 10/46
  frame-0 items and the UV keys on all of them. The factor is not captured, so the decoder accepts the item on the
  equal-position rule and uses the first key for the differing UV/colour arrays, counted as substitutions.
- **0x1508** (sky) is the three-qword gain/clamp/bias entry: it computes `clamp(q2 + q0*C, 0, q1)` on the V4-8 at 0x80,
  the same formula as 0x14D8. With the sky's identity block (gain 1, clamp 255/255/255/128, bias 0) it emits the input
  colour with alpha clamped to 128; the AOT chain matches the tap on 940/940 sky vertices.

### 23.2 Decoder, selection and lighting (C)

- `WorldShapeStatus`/`world_shape_status` (coregs3_shape) checks the measured family layout per item: the 0xC0
  descriptor must carry lane0 bit 15 and the 0x1412 signature; 0x0080/0x1508 need a position list; 0x0B08 the normal at
  0x80; 0x1278 the normal at 0x40 and colour at 0x80; 0x00A0 the colour at 0x80 and the second keyframe (equal positions
  accepted; equal UV/colour exact; differing UV/colour counted). `read_world_item` applies the same rule.
- `coregs3_lighting` maps 0x1508 -> `three_qword_gain`, 0x00A0 -> `morph_passthrough` (no block), 0x1278 ->
  `body_prepass` (six-qword block, body formula). `shade_body` accepts `MeshVertex::lane_position` when the vertex has
  no raw halfword; the car path is unchanged.
- `coregs3_world` accepts the three course families when the shape rule passes; `itop_b08/a0/1278` are no longer
  exclusion reasons and `shape-refused` is a counted one. The mirror/no-state/no-position rules are unchanged.

### 23.3 Evidence (frame 0, VBlank 7398, 3x)

- **AOT vs tap RGBAQ:** 0xB08 15 items/888 vertices exact; 0x1278 11/524 exact; sky 0x1508 16/940 exact; 0xA0 46 items /
  1,380 vertices exact on the fragment's inputs and 1,358/1,380 against the tap (the 22 vertices of the 10
  colour-substituted items differ by at most 4 levels). The C++ shading equals the AOT output on every family.
- **World selection:** course 130 draws/1,263 items/63,959 vertices (0x80 1,192/61,173; 0xB08 15/888; 0xA0 45/1,374;
  0x1278 11/524), sky 16/940, far 9/378; 0 refused draws, 0 lighting fallbacks; exclusions 15 mirror (one 0xA0 mirror
  item), 6 no-position, 0 other-itop/shape-refused/empty draws; 35 first-key UV + 10 first-key colour substitutions.
  Texture census: unknown-program 0 (was 73).
- **Picture:** PSNR 20.36 -> 20.58 dB, SSIM 0.6138 -> 0.6221, pct_bad 77.22 -> 76.17%, mean |diff| 20.31 -> 19.64, luma
  31.20 -> 30.21 (reference 26.69), coverage 100%, car spill 1.61%.
- **Checks:** CTest coregs3 11/11, Python 8/8, car-only render byte-identical to the base-commit executable
  (`248ec453`), 3x off/shadow identity gate COREGS3-NIGHT-CHECK-OK (8,997 VBlanks, 1,686 racing, 0 differences, 13/13
  frames, capture 4,477,368 B). Evidence: `build/evidence-world-families/`; harness and analysis scripts under its
  `tools/` (ignored).

### 23.4 Open (H)

The 0xA0 morph factor at 0xF2 is the missing capture field; until a v6 capture stores it, the first-key UV (all items)
and colour (10 items) are counted substitutions, and the tap's generated S/T for these items does not equal the
captured UV arrays, so the sampled texture coordinate remains open. The 0x1508 mapping is exact for the sky; one
env_map item with the same selector differs from the fragment (max 159) outside the world selection and is not
investigated. The 11 0x1278 items carry no UV channel and stay untextured.

## 24. Round 25 (2026-11-10, branch `task/090-pass-textures`): the per-job SLM snapshot and the world's own texture pixels

The world-materials study (`docs/COREGS3-WORLD-MATERIALS.md`) measured that the frame-end `.gt4vram` snapshot is not the
world-pass content: page `0x36D0`, 53.7% of the compared mask, is an advert atlas at frame end and at every pass begin,
while the course draws sample road there. This round adds the capture that holds the pixels a draw's own VU1 job
samples, and the offline binding that uses them.

### 24.1 Why the round's first two designs fail (C, measured)

- **The tap cannot replay uploads.** `.gt4tap` is PATH1 (XGKICK) only: 80,135 records / 16,604 tags over the M1
  window, **0 IMAGE tags and 0 TRX writes**. The mission's option (b), replaying the tap's uploads over a VRAM base,
  cannot exist. A capture-only stream trace (`GT4_COREGS3_STREAM_TRACE`) over every GIF path of one race frame found no
  IMAGE descriptor either; PATH3 chunks can split a GIF packet, which that packet-level parser does not follow, so the
  upload path is **Unknown** rather than absent.
- **The K7 pass-begin snapshot is too early.** With the new v2 pass-begin record (`kRecordPassBegin`), page `0x36D0` is
  byte-identical at every pass begin of all five captured frames and at frame end (sha256 `a191c894...`). The upload
  happens inside the course pass.
- **The K1 draw marker is too early.** A draw record (`kRecordDrawTexture`) copied at K1 pre and, in a diagnostic run
  (`GT4_COREGS3_VRAM_DRAW_AT=post`), at the shape call's return, still holds the atlas for draw 35, the first course
  draw that binds `0x36D0`. The upload is posted after the shape call and before the draw's VU1 job executes.

### 24.2 The per-job snapshot (the fix)

`hw_gs_stream_sink.cpp`'s GS worker executes every VU1 job (`SinkRecord::MtvuJob`, IN-461 direct intake). With a
`.gt4vram` sidecar configured it now buffers the job's PATH1 GIF records, parses their A+D TEX0 writes and copies the
textures' page runs and CSM1 palette banks **before executing any of them** (`SnapshotJobTextures`). At that stream
position every GS command posted before the job, the draw's texture uploads included, has executed and nothing of the
job has, so the copy is the content the draw samples. The per-record work is a `std::vector` append; the parse and the
copy run on the GS worker, never on the EE, and the whole branch is off unless `GT4_COREGS3_VRAM` is set.

The block rule is shared with the EE side in `coregs3/coregs3_texwin.hpp`: TBP0..TBP0 + pages (T4 128x128, T8 128x64,
CT32 64x32 texels, 32 blocks per page, contiguous, pad pages included) plus four CSM1 palette blocks at CBP, plus the
level-1..MXL runs the tap's TEX1.MXL and MIPTBP1/2 (or the MTBA layout) name, which the mip recapture added. The record
carries the **job id**, which the tap's source records already pair with each item's packet address, so the offline
table binds an item to the pixels of its own job with no new EE-side hook and no per-draw marker.

The writer gained `MaybeJobTextures` and `GT4_COREGS3_VRAM_JOBS=0` (the A/B switch; the per-draw records have
`GT4_COREGS3_VRAM_DRAWS=0`). `GT4_COREGS3_VRAM_MAX_FRAMES` bounds records, not frames (a captured frame holds one
vblank record, one per kept pass marker and one per textured job).

### 24.3 The sidecar format, version 2

The header is 48 bytes: `magic`, `version`, `vblank`, `first_block`, `block_count`, `byte_count`, `record_kind`,
`pass`, `label`, `window_count`, `reserved`; then `window_count` x `{u32 first_block, u32 block_count}`; then
`block_count * 256` bytes, the windows concatenated. Kinds: `0` vblank (the version 1 semantic: the worker's counter at
the flush, one ahead of the capture frame), `1` pass-begin (`vblank` = the capture frame's own, `pass` = PassId), `2` K1
draw (`label` = CapturedDraw::sequence; evidence and secondary fallback) and `3` VU1 job (`label` = job id; the fix).
Version 1 files still parse as one vblank window, and every version 1 path keeps working: the car render on the M1 v1
sidecar is byte-identical to the `a7a9434d` executable (SHA-256 `392dd4ed...`).

### 24.4 Capture recipe and evidence (night race, frame 0 = VBlank 7398)

```
GT4_BENCH_OUT=<wt>/build/evidence-pass-textures bash scripts/coregs2_bench/night-run.sh pt4 \
  build/pass-textures/gt4vulkan.exe GT4_COREGS3=shadow \
  GT4_COREGS3_CAPTURE=.../night-pt4.gt4scene GT4_COREGS3_CAPTURE_VBLANKS=7398-7402 GT4_COREGS3_ALL_DRAWS=1 \
  GT4_COREGS3_TAP=.../night-pt4.gt4tap GT4_COREGS3_TAP_VBLANKS=7398-7406 \
  GT4_COREGS3_VRAM=.../night-pt4.gt4vram GT4_COREGS3_VRAM_VBLANKS=7398-7403 \
  GT4_COREGS3_VRAM_BLOCKS=0x2BC0-0x3FFF GT4_COREGS3_VRAM_PASSES=5 GT4_COREGS3_VRAM_DRAWS=0 \
  GT4_COREGS3_VRAM_MAX_FRAMES=8192
```

Sidecar **498 records (6 vblank, 6 pass-begin, 486 job), 42,464,464 B**; counters: 1,530,243 draw markers filtered,
406,633 job vblanks outside the range, 0 refused. The offline world render (`gt4_coregs3_render ... --world
--dump-textures <dir>`) binds **1,296 world items on 486 job records, 0 refused, 0 on the pass image, 0 on the
frame-end image**.

### 24.5 Gates

| Gate | Result |
|---|---|
| CTest `native_coregs3` family | **12/12 passed**, including the new `native_coregs3_pass_textures` (v2 record kinds, the legacy header, every malformed variant, the snapshot channel, the pass/draw/job binding, cache isolation) |
| Python `tests/python/test_coregs3_units.py` | **8/8 passed** |
| Car-only render vs the `a7a9434d` executable (M1 v1 sidecar) | **byte-identical**, SHA-256 `392dd4eda2fa6f99467dea149c843398c4751c253bfdf582a8a2b8ce6276759b`; our own v2-sidecar car render is the same image, so the v1 fallback and the v2 reader agree |
| 3x off/shadow identity gate (`coregs3-night-check.sh`, main worktree reference exe) | **COREGS3-NIGHT-CHECK-OK**, 8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences, 13/13 frames identical for off and shadow |
| World picture vs frame 07400 | PSNR 20.578 -> **22.35 dB**, SSIM 0.62212 -> **0.65055**, pct_bad 76.173 -> **64.8%**, mean abs 19.6355 -> **14.84**, luma 30.21 -> 18.96 (reference 26.69); page `0x36D0` correlation -0.009 -> **0.111**, ref/world 1.303 -> **1.445** |
| The study's metric gates (page ratio 1.0 +/- 0.15 and correlation > 0.5; PSNR >= 24 dB, SSIM >= 0.75, pct_bad <= 10%) | **not reached**, and not relaxed: the residual is a nearly constant +12.8 luma on the road, the captured wet-road reflection pass (pass 7, additive; study fix 3) and the uncaptured post chain (fix 4), not the texture binding |

### 24.6 The texture mip recapture (2026-11-11, branch `task/090-mip-recapture`)

The same recipe plus the mip windows, run on the extended `texture_windows_from_state` (TEX0 pages + palette + levels
1..MXL) at 3x for scene VBlanks 7398-7402 and VRAM 7398-7403 (`GT4_COREGS3_VRAM_BLOCKS=0x2BC0-0x3FFF`,
`VRAM_PASSES=5`, `VRAM_DRAWS=0`, `VRAM_MAX_FRAMES=8192`). Sidecar **498 records (6 vblank, 6 pass-begin, 486 job),
42,726,864 B**, +262,400 B over the pass-textures file from the per-job mip windows. The road's job records (kind 3,
labels 460177/460345/460513/..., one per VBlank) carry both `(0x36D0, 32)` (level 0) and `(0x3662, 32)` (MIPTBP1
`003640036400B662`: level 1 TBP 0x3662, TBW 2), which no job record of the old sidecar did. The job's level-1 page
hashes `2fcb2e50...` while the frame-end record still holds the car/palette atlas `3cf0989a...`: the game uploads the
road's chain before the road job and overwrites the address later, so only the per-job snapshot sees the chain.
Decoded, the new level 1 against the road's level 0 box downscale scores **corr +0.5961 / PSNR 35.96 / mean |d| 3.22**
(the old atlas page: -0.0329 / 28.69 / 8.10). The A/B and the switch are in the journal 2026-11-11 and
[COREGS3-WORLD-MATERIALS-V2](COREGS3-WORLD-MATERIALS-V2.md); the mip path is on by default and `--no-mip` restores the
pre-round model byte for byte. A pre-recapture sidecar covers the level address with the later frame-end upload, so it
must be rendered with `--no-mip`.

### 24.7 Open

- The uploader and the PATH3 packet(s) that stream the road into `0x36D0` are **Unknown**; the job snapshot captures
  their result. A `.gt4gs` PATH3 sidecar with a stream-level parser (the gs-stream study's S1) would name them.
- The 0xA0 morph UV first-key substitution still misplaces the sampled coordinate for its 46 items.
- The per-draw (`kRecordDrawTexture`) and pass-begin paths stay in the format as evidence and fallbacks; they are
  measured as too early for this scene and are not used by the world picture.

## 25. Round 26 (2026-11-10, branch `task/090-gs-capture`): the `.gt4gs` PATH2/PATH3 stream (TASK-090 S1)

One problem: record every PATH2/PATH3 GIF transfer of the captured frames, with the pass and VBlank it belongs to, into a
bounded `.gt4gs` sidecar, without rendering and without changing the guest. This is S1 of
`docs/COREGS3-GSSTREAM-CAPTURE.md` (§3 format, §5.4 brief, §6 open questions); S2 (the offline post layer), S3 (the HUD
layer) and S4 (the live layer) are not implemented.

### 25.1 Where it records (C)

The writer `native/coregs3/coregs3_gsstream.{hpp,cpp}` lives in `gt4_mtvu_runtime` next to the VU1 tap and is called from
the two EE acceptance points of `native/mtvu_runtime.cpp`:

- `gif_transfer()` after the packet is scanned and its signals applied, for paths 2 and 3 (with MTVU on, PATH3 always
  arrives here; PATH2 arrives here only when VIF1 is not shadowed, which the default does not do);
- `shadow_direct()` for path 2 with the shadowed VIF1 (the MTVU default): the donor `vif.cpp` calls `gif::scan_signals`
  instead of `gif::transfer` when the unit is shadowed, so the packet is seen exactly once. The worker's later
  PATH1/PATH2 emission is a different function (`worker_gif`) and is not recorded. PATH1 stays the VU1 tap's.

The join fields are read at acceptance: `vblank` (the coregs3 VBlank the hooks stamp), `pass` (the recorder tracks the
same K7 begin/end calls the v5 journal does), `sequence` (per-file acceptance order), `last_job`
(`gt4::mtvu::last_posted_job()`, the merge point against the tap) and `source_pc`/`source_ra` (the guest site, read by
the bridge's one-line `Donor::guest_site` callback so `gt4_mtvu_runtime` stays free of donor headers). The pass records
carry the nine register words the v5 journal's `capture_gs_shadow` reads (the seven journal offsets plus CLAMP `0x1A8`
and TEX1 `0x1B8`), from **the same guest read** the journal entry uses, so G-PASS holds by construction.

The guest path is a memory copy into a preallocated ring: no file I/O, no allocation, no lock. The file is opened at
configuration time (`GT4_COREGS3_GS`) and written once at shutdown, header first, exactly like the `.gt4scene` and
`.gt4tap` sidecars. When the switch is off the recorder is one atomic load per packet and per marker.

### 25.2 Format version 1

32-byte header (`GT4G`, version 1, target 0, flags 0, first VBlank, reserved 0) and 48-byte record headers
(`bytes, kind, path, flags, pass, sequence, vblank, last_job, source_pc, source_ra, qwc, reserved`), payloads a
multiple of 16 bytes: kind 1 the GIF bytes, kinds 2/3 the nine GS register words plus a reserved word, kinds 4/5 the
frame ordinal and its PATH2/PATH3 totals, kind 8 the refused byte/record counts and cause bits. Kinds 6 and 7 are
reserved for later versions and refused by the v1 reader. The reader
`native/coregs3/coregs3_gsstream_file.{hpp,cpp}` is in `gt4_coregs3` next to `coregs3_tap_file`; the fixture pins that
the independent writer and reader agree.

Bounds: `GT4_COREGS3_GS_MAX_BYTES` (64 MiB default, the whole file including the header), `_MAX_FRAME_BYTES` (8 MiB),
`_MAX_RECORDS` (65,536), `_MAX_FRAMES` (8) and `_VBLANKS` (default `GT4_COREGS3_CAPTURE_VBLANKS`). A packet over the
frame cap is dropped and the frame is marked with a kind 8 record and a refused `frame_end` (later frames are kept); a
file over its byte or record cap stops recording and counts every dropped packet; a packet outside the VBlank window or
after the frame cap is skipped, not refused. Nothing is truncated silently.

### 25.3 Tools and controls

- `gt4_coregs3_gs --stats | --list | --dump-frame N | --check-pass <gs> <scene>`: the totals and per-frame census, the
  raw record list, one frame with its pass words and the first 64 payload bytes, and the G-PASS control (the first
  `.gt4gs` pass record of every frame against the first `.gt4scene` v5 journal entry of the same VBlank).
- `native_coregs3_gsstream` (new CTest): round trip, the four bounds, a malformed packet and every reader refusal
  (bad magic, unknown version, nonzero reserved words, reserved/unknown kinds, a record over the end, a qwc mismatch, a
  bad pass id, a trailing partial record).
- `tests/python/test_coregs3_units.py` gains an independent Python writer of the documented format and checks the
  tool's parse and refusals (`GT4_NATIVE_BUILD` must point at the build under test).
- `scripts/coregs3_bench/gs_capture_f8_check.py`: G-GS-CAP, the capture's per-frame PATH2/PATH3 bytes and records
  against the F8 `[frame] gif bytes/frame` lines, with the report offset measured rather than assumed.

### 25.4 Capture and evidence (night race, 3x, VBlanks 7398-7405)

Recipe (same as the v5 evidence; `build/evidence-gs-capture/` holds the files and logs):

```
GT4_BENCH_OUT=<evidence> SCALE=3 EVERY=100000 FROM=999999 FRAMES=7420 \
bash scripts/coregs2_bench/night-run.sh gscap build/gs-capture/gt4vulkan.exe \
  GT4_COREGS3=shadow GT4_COREGS3_ALL_DRAWS=1 \
  GT4_COREGS3_CAPTURE=<evidence>/capture.gt4scene GT4_COREGS3_CAPTURE_VBLANKS=7398-7405 \
  GT4_COREGS3_TAP=<evidence>/capture.gt4tap GT4_COREGS3_TAP_VBLANKS=7398-7406 \
  GT4_COREGS3_GS=<evidence>/capture.gt4gs GT4_COREGS3_GS_VBLANKS=7398-7405 \
  GT4_MTVU_REPORT_EVERY=1
```

The capture is **30,945,504 B**: 4,979 records (4,771 GIF, 192 pass = 96 begin + 96 end, 16 frame), 8/8 frames closed,
0 refusals. PATH2 **1,480 records / 145,536 B** (18,192 B / 185 records per frame); PATH3 **3,291 records /
30,545,072 B** (3,817,280 B / 407 records on frame 7398, 3,818,256 B / 412 on frames 7399-7405); the largest record is
491,520 B (a single PATH3 packet, `pc` 004AB548) and the largest frame 3,836,448 B (3.66 MiB, under the 8 MiB cap);
8 records carry the SIGNAL/FINISH/LABEL flag. The `.gt4scene` file of the same run is 17,783,196 B and the `.gt4tap` 28,558,088 B.

Two runs were made, one with the default IN-461 direct GS intake and one with `GT4_MTVU_DIRECT=0`; their `.gt4gs` files
are **byte-identical** (SHA-256 `df858513bd01855196e0ca00489f058ffd1d76ca3b1daab815f0b854cf6cd46a`), and so are their
`.gt4scene` files, so the capture is independent of the transport mode and the guest stream is deterministic.

### 25.5 The measured phase: the pass at acceptance is unknown (C)

The study's open question 1 is answered, and the answer changes the layer model. In every captured frame **all GIF
records precede all 24 pass records**: e.g. frame 7398 has 592 GIF records (sequences 1..592) and then the 12 pass
begins/ends (sequences 593..616), frame 7399 597 GIF and then the markers, and so on. The frame's GS stream is kicked at
the start of the VBlank interval (the previous display list), and the K7 markers of the same file frame run after it
while the next frame's display list is built. Consequently the recorder stamps `pass = unknown` on **4,771 of 4,771**
packets, and the per-pass census is one `unknown` row; the markers themselves all fire (12 passes x 8 frames, all
named). The packet `last_job` ranges are contiguous across frames (frame 7398: 460336..460503, frame 7399:
460504..460675, ..., frame 7405: 461536..461707) and the frame's markers report the next frame's first job (460504),
so the marker job ids cannot separate the passes inside a frame either.

Source-site histogram (evidence only, per record): `pc` 004AB6D8 (2,603), 004AB548 (2,040), 004ABA80 (128); `ra`
00001000 (4,587), 004ABA80 (128), 004AB8C8 (56). The low `ra` values are the translated dispatch's return address, so
the fields name the kick site, not the PGL builder: the study's hypothesis for question 5 is confirmed in form, but the
site is the frame kick loop rather than a per-pass builder.

What this means for S2/S3: the capture is complete and exact for bytes, order, VBlank and jobs, but a packet cannot be
placed in a pass from the acceptance-time marker state. S2 must either use the phase relation (the packets stamped V
are the stream built in frame V-1; the passes of V-1 are in the scene journal) or add a stream-level pass marker (the
study's optional kind 6/7 records) before the post/HUD layers can be drawn in pass order.

### 25.6 Gates

| Gate | Result |
|---|---|
| CTest coregs3 (12 existing + new `native_coregs3_gsstream`) | **13/13 passed** |
| Python `tests/python/test_coregs3_units.py` with `GT4_NATIVE_BUILD` | **14/14 passed** (8 insertion + 6 stream controls) |
| G-GS-CAP, default direct intake (PATH3; the F8 line does not see PATH2 in IN-461 mode) | **PASS**: 3,817,280..3,818,256 B and 407..412 records per frame match F8 8/8; window 30,545,072 B / 3,291 records exact |
| G-GS-CAP, `GT4_MTVU_DIRECT=0` (PATH2 and PATH3) | **PASS**: PATH2 145,536 B / 1,480 records and PATH3 30,545,072 B / 3,291 records, 8/8 frames exact; the measured report offset is line = VBlank - 1 (the F8 reset phase relative to the capture's VBlank stamp; the script measures it instead of assuming it) |
| G-PASS (first pass record per frame against the first v5 journal entry) | **8/8 frames MATCH**, pass id and 7/7 register words; CLAMP `0x000000440004000a`, TEX1 `0x0000000000000060` captured; the negative control (one corrupted FOGCOL byte) reports 6/7 and exits 1 |
| Bounds in the window | 0 refused records/bytes, 0 frames without end, 8/8 frames closed; the fixture forces a frame-cap, file-cap, record-cap and malformed-packet refusal and each is counted |
| 3x off/shadow identity gate (`coregs3-night-check.sh`, capture enabled in shadow) | **COREGS3-NIGHT-CHECK-OK**: 8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences, 13/13 frames identical for off and shadow; the gate's shadow run also wrote the same 30,945,504 B `.gt4gs` (SHA-256 `df858513...`) |

## 26. Round 26 (2026-11-10, branch `task/090-wet-reflection`): the captured reflection pass drawn and measured

The world-materials study's fix 3 asked to draw the captured wet-road reflection pass (PassId 7, 4 draws / 7 items /
292 vertices) before attributing the remaining road darkness to palettes. The pass is now drawn by the offline world
renderer; the measurement says it is **not** the additive layer: its main-view items write destination alpha only, and
its other two items render the sphere-mapper source into their own target. The colour composite is PATH3 and absent from
the PATH1 tap.

### 26.1 What the capture holds (C)

Measured on `night-pt5` frame 0 (VBlank 7398), from the tap's A+D writes and the draws' state:

| draw | items / vertices | FRAME target | FBMSK | PRIM | TEX0 | XYOFFSET |
|---|---|---|---|---|---|---|
| 165, 166 | 5 / 272 | FBP page 210 (blocks 0x1A40), FBW 10 = 640 px, CT32 | `0x00FFFFFF` (keep RGB, write alpha) | 0x3C (TME+FGE, ABE off) | T4 128x128 TBP0 `0x3F28`, CBP `0x3F70`, CLD 0 + TEX2 T4 group load | `0x6C08/0x7208` (main view) |
| 167, 168 | 2 / 20 | FBP page 394 (blocks 0x3140), FBW 2 = 128 px, CT32 | `0xFF000000` (write RGB, keep alpha) | 0x7C (ABE on) | T8 128x128 TBP0 `0x3F80`, CBP `0x3FC0`, CLD 0 + TEX2 T8 load | `0x7C08/0x7C08` (sphere mapper) |

Both pairs carry TEST `0x0005820C` (ATE off, DATE off, ZTST GEQUAL), ALPHA 0x44, ZBUF ZMSK 1, and a per-vertex fog
(202..209 main view; 255 sphere); FOGCOL is `0x27272A` and `0x7F7F7F`. The main-view items' PRIM is 0x3C with ABE off,
so the study's expected additive `(Cs-0)*Ad+Cd`/FOGCOL-black state is not this pass (that is the body/paint reflection,
round 20). `RacePS2Base__render_reflection` (`0x003411D0`, decompiled) explains the sequence: it sets the framebuffer
mask, renders the reflection models through `CourseData__render_model`, then blurs and composites with full-screen PGL
sprites (`func_004A6788`), i.e. PATH3/2D, which the `.gt4tap` (PATH1 only, 0 IMAGE tags) does not carry.

### 26.2 The renderer rule (`native/coregs3/coregs3_world.*`, `coregs3_render.*`)

- **Selection.** PassId 7 is a world pass under the measured rule: ITOP 0x0080, a position channel, a shape-rule pass, a
  tapped state with the main-view XYOFFSET `0x6C08/0x7208` and a non-zero TEST. The mirror-view (`0x7C08/0x7C08`)
  items are `mirror` exclusions; every outcome is counted (`world wet-reflection exclusions: other-itop, shape-refused,
  mirror-view, no-state, no-position, empty-draws`). The tool's `--no-wet-reflection` is the A/B.
- **Texture binding.** `coregs3_texture_frame` treats PassId 7 as a world pass: each item decodes its
  TEX0/TEX1/CLAMP/TEX2 state against its own VU1 job's `.gt4vram` v2 record (T4 `0x3F28`/CLUT `0x3F70`; TEX1.MXL 1 takes
  the world path's counted level-0 fallback). Frame 0: **5 items from the tapped state on job 460387, 0 refused**.
- **Fragments.** No new arithmetic: the world rasteriser already applies the tapped blend, the FRAME.FBMSK lane merge,
  the ZBUF.ZMSK no-write, the tapped fog and the alpha/depth tests. New counters name the pass
  (`world_reflection_{draws,items,vertices,pixels,alpha_only,rgb_only,depth_failed,sample_refused}`), and `--reflection-mask`
  writes its written-fragment mask.
- **Capture coverage.** The job windows (0x3F28, 32 blocks)+(0x3F70, 4), and for the sphere-mapper job (0x3F80, 64)+
  (0x3FC0, 4)+(0x3140, 256), cover every sampled block; 0 refused. The default car range 0x2BC0..0x38FF does **not**
  cover the reflection pages 0x3F28/0x3F80, so the pass-textures capture's extended `GT4_COREGS3_VRAM_BLOCKS=0x2BC0-0x3FFF`
  is the documented choice; the per-job windows copy the bound blocks wherever they are. No recapture was needed and no
  content was guessed.

### 26.3 Measurement (frame 0, VBlank 7398, 3x, reference frame_07400)

- Selection: wet_reflection **2 draws / 5 items / 272 vertices**; exclusions mirror-view 2, empty-draws 2 (the
  sphere-mapper draws), 0 other.
- Raster: **188 fragments, all FBMSK-alpha-only, 7,608 fragments depth-rejected, 0 sample refusals; 0 RGB pixels
  changed** and exactly 188 alpha pixels.
- Picture before == after: PSNR **22.3498 dB**, SSIM **0.65055**, pct_bad **64.776%**, mean |diff| **14.84**, luma
  26.69 -> **18.96**, ref/world 1.408, corr +0.465; whole-mask fit **`ref = 0.634*world + 14.67`** both runs. Page
  `0x36D0`: ratio 1.445, corr +0.111, fit `0.283*world + 19.31` both. Page `0x3EE1`: ratio 1.678, corr +0.543, fit
  `1.283*world + 11.08` both. The A/B (`--no-wet-reflection`) is byte-identical to the pass-textures `world-final.png`.
- The study's metric gates (PSNR >= 24, SSIM >= 0.75, pct_bad <= 10%) remain **unreached and unrelaxed**: the additive
  term is in the uncaptured PATH3 composite (and/or the post chain), not in the captured pass. The pass's mask touches
  188 of 1,780,790 compared pixels (0.011%) and 97 of 1,250,577 road-page pixels (0.008%); the luma gap is 2.93 inside
  it and 7.88 outside, so a road-wide additive term cannot be this pass.

### 26.4 Gates

| Gate | Result |
|---|---|
| CTest `native_coregs3` family | **13/13 passed**, including the new `native_coregs3_wet_reflection` (selection rule; per-job + CLD 0/TEX2 binding and the missing-TEX2 refusal; render integration pinning the alpha-only write, unchanged RGB, ZBUF no-write and the counted depth rejection) |
| Python `tests/python/test_coregs3_units.py` | **8/8 passed** |
| Car-only render vs the `e529c319` executable (M1 v1 sidecar) | **byte-identical**, SHA-256 `392dd4eda2fa6f99467dea149c843398c4751c253bfdf582a8a2b8ce6276759b`, identical reports |
| 3x off/shadow identity gate (`coregs3-night-check.sh`, main worktree reference exe) | **COREGS3-NIGHT-CHECK-OK**, 8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences, 13/13 frames identical for off and shadow |
| World A/B with the pass off | **byte-identical** to the pass-textures `world-final.png` (`345f955a...`) |
| World picture with the pass on | identical metrics to the pass off (the pass is alpha-only); the additive term stays |

### 26.5 Open

- The wet-road colour composite (blur + full-screen sprite) is **PATH3** and needs the `.gt4gs` v1 capture (the
  GS-stream study's S1) before it can be drawn; the additive term is bounded to it and/or the post chain.
- The sphere-mapper draws (167/168) target FBP page 394 (blocks 0x3140), not the main frame; their content is captured
  in the job record's windows and is not drawn in this picture.
- The pass's own colour output does not exist in the capture; nothing was inferred from it.

## 27. Round 27 (2026-11-10, branch `task/090-morph-v6`): the 0x00A0 morph factor, `.gt4scene` version 6

The world-families round decoded the course's 0x00A0 two-key morph pre-pass but could not capture its blend factor: the
program reads it at VU1 data memory TOPS+0xF2 lane 3 (`vf20 = 1 - s`, `vf21 = s`), and the v5 capture stored neither the
value nor the upload. The decoder therefore used the first key on the differing UV arrays (all 46 frame-0 items) and
colour arrays (10 items), counted as substitutions, and the tap's sampled coordinate for those items stayed open. This
round captures the factor, pins its measured semantics against the real AOT fragment and the tap, and re-measures the
family and the world picture.

### 27.1 Where the factor lives (C)

`func_0049D1A0` (the PGL item dispatcher, decomp in `private/slice151-research/gt4-decomp`) writes, once per item before
its MSCAL, a DMA CNT tag whose VIF stream is:

```
CNT QWC 1  tag words: 0x10000001, 0, 0x20000000 (STMASK), 0x3F3F3F3F (mask: only lane 3 writes)
payload qword:        0x7001C0F2 (UNPACK V1-32, TOPS-relative destination 0xF2, NUM 1, masked),
                      self->xc30, 0x20000000 (STMASK), self->x40
```

The UNPACK is a single 32-bit broadcast (vn = vl = 0) masked to lane 3, so TOPS+0xF2 lane 3 receives the raw 32-bit word
at PGL context +0xC30, which is a float bit pattern. The segment is emitted for `cmd->kind >> 5` = 1 or 2 (the Cmd flag's
bits 5..6); the value does not change during the shape call, so it is one factor per draw. `CapturedDraw` carries it in
format version 6; the chain walker recognises the segment by the STMASK pair, the UNPACK word, its format and the factor
word that follows, i.e. by what the VIF actually executes, not by the EE rule. The hook also reads PGL +0xC30 directly at
the K1 post point as a cross-check (`morph_factor_pgl`).

### 27.2 Format version 6 (C)

Four words per draw, after `caller_ra` and before the item table (the draw record's fixed part grows 984 -> 1000 bytes):

| Word | Meaning |
|---|---|
| `morph_factor` | the raw word the chain uploaded to TOPS+0xF2 lane 3 (what the VU1 read) |
| `morph_factor_pgl` | PGL +0xC30 read directly at the K1 post hook (evidence) |
| `morph_factor_valid` | bit 0 upload seen, bit 1 PGL read, bit 2 the two words agree |
| `morph_uploads` | upload segments the chain walk counted for the draw |

A factor is usable only with bit 0: the VU1 lane 3 otherwise holds a carried/stale qword. A non-finite word is refused by
`morph_blend_of`, so a damaged capture falls back instead of poisoning a mesh. The reader refuses versions 1..5 with the
message that names the missing field (and now also refuses a draw record whose field is outside its bounds, a gap the
previous reader only checked in the writer); the writer refuses the same bounds. `gt4_coregs3_render --list` prints the
per-frame morph draws, the factor word and float, the PGL word, the flags and the segment count.

### 27.3 Measured semantics (C)

The AOT fragment `vu1_2C8C8E20B5ECB957` (entry 0xA0, full-corpus generation) computes, for every vertex:

1. `vf20 = (1,1,1,1) - vf21` where `vf21 = mem[TOPS+0xF2]` (so `vf20[3] = 1 - s`);
2. `out = key1 * vf20[3]`, then `out = out + key2 * vf21[3]`, lane by lane;
3. positions (lanes 0..2 at 0x00/0x20), UVs (lanes 0/1 at 0x40/0x60) and colours (V4-8 bytes at 0x80/0xA0) use the same
   two operations; the colour keys go through ITOF0 before the blend and the result through FTOI0 (mode 0: truncation
   toward zero, saturating) before it is written back to 0x80;
4. the shared output routine at 0xB0 then emits `S = u * Q`, `T = v * Q` from the blended UV array and packs the low byte
   of each blended colour lane into RGBAQ, so the tap's `S/Q,T/Q` is the post-morph UV and its RGBAQ is the blended byte.

The host implementation keeps the same order with separate float32 roundings (no fused multiply-add): `one_minus = 1 - s`
once, then `key1 * one_minus + key2 * s` with each product rounded before the add. The colour path uses the same
truncation and low-byte packing the VU1/GS use (a negative factor can push a key past 255; the emitted byte is lane & 0xFF).

### 27.4 Capture and counters (C)

`night-run.sh morph` with `GT4_COREGS3=shadow`, ALL_DRAWS, capture VBlanks 7398-7402, tap 7398-7406, the pt5 vram recipe.
The v6 capture is 11,141,796 bytes (5 frames). Counters over the whole 11,499-frame run: **morph_uploads 121,130,
morph_draws 5,311, morph_reads 1,640,303, morph_mismatches 0, morph_value_mismatches 0, morph_missing 0** — every draw
whose chain carried the segment had the same word in the chain and in PGL +0xC30, every chain segment agreed, and no
draw that the Cmd kind rule asks for was missing. Frame 0 (VBlank 7398): draw 85, factor word `0xBF022BBF`
(-0.508480), 46 uploads, flags 0x7. The factor ramps by exactly -0.00056 per frame over the five captured frames
(-0.508480, -0.509040, -0.509600, -0.510160, -0.510720), a linear race-clock animation.

### 27.5 Family residuals against the tap (C)

Method: every 0xA0 item of the five captured frames (230 items, 6,900 vertices) is paired with its tapped GIF record at
the job's VBlank (the tap counter runs one ahead of the guest frame: every record's implied factor equals the capture
word only at `vblank + 1`). The pair is accepted only when the record's per-vertex `S/Q,T/Q` imply the captured factor.
The AOT fragment was run with the captured factor on the item's captured inputs (full-corpus generated code, the same
harness as the world-families oracle); the tap's sampled coordinates and RGBAQ were compared before (first key) and after
(blended):

| Measure | Before (first key) | After (captured factor) |
|---|---|---|
| Sampled coordinate `S/Q,T/Q` exact (<= 5e-6) | **0 / 6,900** (max error 0.511) | **6,900 / 6,900** (max 5.9e-8) |
| Tap RGBAQ exact | 3,860 / 6,900 (3,040 differ) | **6,900 / 6,900** (0 differ) |

Every differing UV key pair is V2-32 at 0x40/0x60 and every differing colour pair V4-8 at 0x80/0xA0; the position keys are
bit-equal on every item (0 differing). The real AOT fragment, run for all 46 frame-0 items with the captured factor,
reproduces the tap's `S/Q,T/Q` and RGBAQ on **1,380/1,380 vertices** (max 5.6e-8), so the model and the generated code
agree with the tap. The blend reproduces the tap's emitted coordinate and colour on every vertex of the family, so the
sampled coordinate is no longer Unknown: it is `key1 * (1 - s) + key2 * s`.

### 27.6 Decoder, pairing and render (C)

- `coregs3_shape`: with a valid factor the 0x00A0 rule blends the differing key pairs and returns `ok`; without one it
  keeps the counted first-key substitutions and the `morph_keys_differ` refusal. The blend reads the same arrays the VU1
  reads; a non-float position key (`morph-position-format`), a non-float UV key (`morph-uv-format`) and a vertex count
  above 32, where the two UV arrays would overlap past qword 0x7F (`morph-uv-region`), are refused and named.
  `MorphKeyFacts` reports which key pairs differ for the tool's census.
- `coregs3_texture_frame`: the world state pairing now compares the tap's `S/Q,T/Q` with the **post-morph** UV
  (`read_item_uv` blends when the item runs 0x00A0 and the draw has the factor). The frame-0 result: the 6-vertex item
  (draw 85 item 45) was the baseline's one "0xA0 mirror" exclusion; its own tap record (job 460537) runs under
  XYOFFSET `0x0000720800006C08` (the main view), so the first-key pairing had attached the mirror state from another
  stream. With the blended UV the item pairs with its own stream and is now selected: course **46 items / 1,380
  vertices** (was 45/1,374), mirror exclusions 14 (was 15); the tapped-state census pairs 1,567 items (was 1,530) with
  35 record fallbacks (was 72). The 46th item's state is not a guess: it is the XYOFFSET its own job's record ran under.
- `coregs3_lighting`: the `morph_passthrough` program emits the blended V4-8 colour; no block is read.
- The tool gained `--no-morph` (an evidence A/B that clears the factor so the morph items fall back to the first key) and
  the world report prints the morph census: items with a factor, blended UV/colour/position counts, the factors and the
  substitution counts.

### 27.7 Picture (H)

Two measurements on the same pixels:

- **Isolated A/B on the baseline capture.** The pass-textures `night-pt5.gt4scene` (v5) was converted to v6 byte for byte
  (only the 16-byte draw fields inserted; the conversion is verified field by field against the v5 file) with the factor
  the pt5 tap measured for its frame 0 (`0xBF022BBF`, -0.508480; the same word the fresh run captured). `--no-morph` vs
  the normal render against the same reference frame/mask: **PSNR 22.351969 -> 22.352105 dB, SSIM 0.650791 -> 0.650793,
  pct_bad 64.7744 -> 64.7747%, mean |diff| 14.837616 -> 14.837543, luma 18.946214 -> 18.946598 (reference 26.688)**, with
  675 pixels differing (max 8 levels). The pass-textures baseline is PSNR 22.349825 / SSIM 0.650555 / mean 14.839992 /
  luma 18.955046; the base-commit executable differs from it by 698 pixels (max 79, the wet-reflection merge's binding
  tweaks), which is why the same-executable A/B is the round's before/after.
- **Fresh v6 capture.** The shipped path (the fresh v6 capture, frame 0, same recipe, no conversion or injection) renders
  **byte-identical** to the converted-baseline A/B in both modes: `1ec71d3b...` before and `3fe95093...` after. The
  conversion and the injected tap-measured word are therefore validated against a real captured factor, and the fresh
  run's world content equals the baseline capture's.

The morph family is a thin part of this picture: the 675 changed pixels are the road/rail items' texture sampling plus
the one re-paired 6-vertex item, and the overall metrics move by +0.00014 dB because the sampled atlas content across the
0.5-1.0 UV shift is similar. The value of the round is the exact family: the sampled coordinate and colour now equal the
tap on all 6,900 vertices, and the item whose tapped state the UV gate could not name is now drawn with the state its own
job ran under.

### 27.8 Gates

| Gate | Result |
|---|---|
| CTest `native_coregs3` family | **14/14 passed**, with the v6 round-trip and malformed fixtures (morph flag mask, upload cap, reader refusal, non-finite word), the chain morph-segment cases (two equal segments, a differing one counted, a CNT block without the STMASK pair not read) and the shape blend/refusal cases |
| Python `tests/python/test_coregs3_units.py` | **14 ran, OK (6 skipped)**, the same suite as the base |
| Car-only render vs the aa5816ac executable | **byte-identical**, SHA-256 `b0e7082b9d0feb52da00b54d5b634d4518795b94706522abd0498e5eb338f16e`; the aa5816ac tool read the v5 M1 capture and this build the same capture converted to v6 (zero morph fields), same command, identical reports but the capture path |
| 3x off/shadow identity gate (`coregs3-night-check.sh`, `coregs3-r8/ref` reference exe) | **COREGS3-NIGHT-CHECK-OK**: 8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences, 13/13 frames identical for off and shadow, capture 4,496,552 B |
| 0xA0 family vs the tap | sampled coordinate **0 -> 6,900 / 6,900** exact; RGBAQ **3,860 -> 6,900 / 6,900** exact |

### 27.9 Open (H)

- The morph factor is captured per draw and the chain writes it once per item; no captured draw had a differing segment
  (0 value mismatches over 11,499 frames), so the per-draw representation is measured, not assumed. A capture that sees
  a differing segment would count it and keep the first word; the renderer would then be wrong for that item, and that
  case has not been observed.
- The factor's *source* (which game variable drives PGL +0xC30) is Unknown; only the context word, the VU1 upload and
  the animation ramp are measured.
- The 6-vertex item's state tie: the C++ pairing counts 2 ambiguous UV ties in the frame; item 45's own job record names
  the main view, which is the stream the blended UV selects, but the tie counter does not prove which stream each of the
  other ties should use. The picture difference from the tie is inside the 675-pixel A/B.
- The 46th item's colour is blended (10 colour-differing items); its effect on the sampled texels is included in the
  picture measurement above.

## 28. Round 27 (2026-11-10, branch `task/090-gs-post-layer`): S2, the PATH2 stream decoded and the wet-road composite drawn

One branch from `task/090-coregs3` at `aa5816ac` (worktree `O:/Antigavity/gt4-wt-090-gs-post-layer`). One problem: decode
the S1 `.gt4gs` PATH2/PATH3 stream, identify the wet-road reflection colour composite, draw it faithfully over the
offline world and measure how much of the additive term it explains. S1's S2 plan (`docs/COREGS3-GSSTREAM-CAPTURE.md`
§4-§5) named PATH3; the measurement says the composite is **PATH2**, and the answer is that it explains **none** of the
additive term.

### 28.1 The decoder and census (C)

`native/coregs3/coregs3_gsdecode.{hpp,cpp}` is in `gt4_coregs3` (no GT4_HW_GS dependency). It walks every capture
record with the measured CoreGS 2.0 GIF semantics, written independently: PACKED (one descriptor per qword), REGLIST
(NLOOP x NREG 64-bit words, two per qword, descriptor advancing per word - the first S2 look read NLOOP as a word count
and shifted the whole stream), IMAGE (NLOOP qwords, split across capture records), A+D register writes with the canonical
64-bit layout, and the 12.4 screen map `sx = ((x - XYOFFSET.OFX) & 0xFFFF) / 16` with the signed 12-bit range. A
primitive batch is the vertex run of one tag under one PRIM, both latching (XYZF3/XYZ3) and drawing (XYZF2/XYZ2) kicks;
a sprite is the pair. Every packet is classified (PACKED/REGLIST/IMAGE/disabled) and by content (register-only,
primitive, empty); named refusal counters cover truncation, disabled format, unsupported REGLIST descriptors, IMAGE
without BITBLTBUF/TRXREG, and vertices before any PRIM. The attribution is S1's phase relation (stream `V` is scene
frame `V - 1`), reported as `capture_vblank` and `scene_frame`; no packet is placed by acceptance-time pass.

`gt4_coregs3_gs --census [--json <out>]` prints the totals, the per-VBlank/per-scene-frame rows, every full-screen batch
with its state, and the recognised chain per scene frame. Night capture, VBlanks 7398-7405:

| Number | Value |
|---|---|
| records / GIF records | 4,979 / 4,771 (PATH2 1,480, PATH3 3,291) |
| packets | 4,915 (PACKED 2,637, REGLIST 896, IMAGE 1,382, disabled 0) |
| content | register-only 2,078, primitive 928, empty 527 |
| register traffic | 9,192 A+D writes, 8,792 register writes, 936 PRIM kicks |
| geometry | 928 batches / 4,096 vertices (2,048 drawing), 88 full-screen batches |
| images | 1,382 uploads / 30,403,968 B; 1,382 packets split across records (handled) |
| refusals | 0 truncated, 0 disabled, 0 unsupported REGLIST, 0 IMAGE without state, 0 vertices without PRIM |
| per frame | 610/615 packets, 171-173 uploads, 11 full-screen batches |

### 28.2 The chain, and the PATH3 correction (C)

The PATH3 records are **only** image uploads and transfer registers; every draw is PATH2. The chain recogniser
(`find_wet_composite`) pins the exact state signature in the stream stamped `V + 1` and refuses anything else. For scene
frame 0 (VBlank 7398, stream 7399, records seq 920/934/936/964):

| draw | state |
|---|---|
| seq 920 alpha clear | PRIM 0x006 sprite (TME/ABE off), FRAME FBP 210/FBW 10 CT32, FBMSK `0x00FFFFFF`, RGBA alpha 0, 20 full-screen strips |
| seq 934 colour composite | PRIM 0x15E sprite (TME+ABE), FRAME FBP 210/FBW 10 CT32, FBMSK 0, TEX0 TBP0 **0x3140 (page 394) CT32 128x128, TCC 1, TFX 0**, TEX1 linear/linear MXL 0, CLAMP region 0..127, ALPHA `0x58` `(Cs-0)*Ad+Cd`, TEST `0x3820C` (ATE off, ZTST ALWAYS), ZBUF ZMSK 1, 20 strip sprites, vertex RGBA (64,64,64,128) |
| seq 936 post darken | PRIM 0x04E sprite (TME off, ABE on), ALPHA `0xA9` `(Cd-0)*FIX+0`, FIX 127, full screen |
| seq 964 post brighten | same, FIX 255, full screen |
| seq 1094 present | PRIM 0x11E, TEX0 TBP0 0x1A40, FRAME FBP 140/FBW 10 CT16, full screen |

The composite samples the **sphere-mapper source page 394**, not the main frame page 210: page 210 is the destination
(its alpha is the mask) and the source is the 128x128 sphere-map target. The source pixels are the `.gt4vram` job
record **label 460559, vblank 7399**, windows `(0x3F80,64)+(0x3FC0,4)+(0x3140,256)`; the record is taken on the GS
worker **before** that job executes, and the job is posted after seq 934, so the captured page is exactly what the
composite reads. The run's frame-0 `.gt4scene` block is byte-identical to the pass-textures `night-pt5` frame
(`0c139ab0a439f3bf...`), so the sidecar belongs to the same frame.

### 28.3 The offline composite (C)

`native/coregs3/coregs3_wet_composite.{hpp,cpp}` applies the captures to the rendered world: sprite pairs are rasterised
half-open (a shared edge writes once, the S2 boundary bug), the CT32 source is sampled through the measured
`coregs3_sampler` (MODULATE with the vertex colour, linear/linear, region clamp), the blend is the measured
`(Cs-0)*Ad+Cd`, the alpha lane is written as As, and a state outside the signature is a refusal. The renderer gained
`--gs <capture.gt4gs>` (with the chain and source-page selection, refusing a missing or ambiguous draw), the A/B
`--no-wet-composite` (the world baseline byte for byte), `--post-chain` (the two captured multiplies),
`--composite-world-alpha` (a counted diagnostic that skips the alpha clear) and `--composite-mask` /
`--composite-changed` PGMs. `coregs3_render` also gained the captured alpha clear at the stream position
(`clear_alpha_before_wet_reflection`): the framebuffer alpha is zeroed immediately before the first PassId 7 draw, after
the course.

### 28.4 Measurement (C)

Night race frame 0, 3x, the pass-textures world baseline, the reference `frame_07400`, the M1 clean mask (car mask
dilated 2 px and the six HUD rectangles removed):

| Quantity | Value |
|---|---|
| reflection mask | 188 fragments at 3x, 70 with non-zero alpha |
| composite cover / samples / refusals | 2,573,956 / 2,573,956 / 0 |
| composite changed pixels | **70**, rows 151-154, columns 1334-1368 (the horizon band) |
| changed pixels inside the clean mask | **0** |
| added luma on the 1,780,790 compared pixels | **0.000** |
| before == after | PSNR 22.3498, SSIM 0.65055, pct_bad 64.776%, mean abs diff 14.84, luma 18.955, fit `0.6341*world + 14.668` |
| page `0x36D0` before == after | PSNR 25.169, SSIM 0.64213, luma 16.613, fit `0.2831*world + 19.307` |
| additive gap (whole mask / road page) | 7.733 / 7.397 luma; the composite explains **0.000%** |
| A/B `--no-wet-composite` | byte-identical to the baseline, SHA-256 `AB769F0A92BA...` |
| world-alpha diagnostic | 2,543,500 changed pixels, luma 144.98, PSNR 6.45, SSIM 0.212 (inconsistent with the reference) |
| post chain (FIX 127 then 255) | luma 34.62 (reference 26.69), PSNR 20.40, SSIM 0.6105, fit `0.3733*world + 13.763` (overshoots) |

The additive term is therefore **not** the captured wet-road composite and **not** the captured global post multiplies.
The composite's mask is a 70-pixel horizon band outside the compared mask; a road-wide destination alpha is excluded by
the explicit alpha clear and by the world-alpha diagnostic, which overshoots by an order of magnitude. The remaining
7.733 luma stays in the material/texture/lighting difference the world-materials and pass-textures rounds bounded, or in
a post operation the PATH2/PATH3 stream does not carry at this scene (no tone/glare draw; the 2x brighten is the only RGB
post op).

### 28.5 Checks

| Gate | Result |
|---|---|
| CTest coregs3 | **16/16**, including `native_coregs3_gsdecode` (PACKED/REGLIST/IMAGE, the split packet, the 12.4 map, the full-screen rule, the chain recogniser and its refusal) and `native_coregs3_wet_composite` (MODULATE, Cs*Ad+Cd, As, half-open sprites, the multiply and the refusals) |
| Python `test_coregs3_units.py` with `GT4_NATIVE_BUILD` | **14/14** |
| Car-only render vs the `aa5816ac` executable (same scene/vram/tap/command) | **byte-identical**: PNG SHA-256 `B0E7082B...`, report `4F9F986A...`, both builds |
| `--no-wet-composite` vs the world baseline | **byte-identical**, SHA-256 `AB769F0A92BA2247360B9939D9A21707D4174743202BC5C0340F0375F7388CFF` |
| 3x off/shadow identity gate (`coregs3-night-check.sh`, reference `coregs3-r8/ref/gt4vulkan.exe`) | **COREGS3-NIGHT-CHECK-OK**, 8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences, 13/13 frames identical for off and shadow |

Evidence: `build/evidence-gs-post-layer/` (census text/JSON, world baseline/composite/A-B/diagnostic/post-chain images
and reports, composite and changed masks, metrics JSON, gate logs). Limits: the destination alpha at the composite is a
reconstruction of the captured clear plus the captured reflection pass against the offline depth (188 fragments); it is
not a GS readback, and the real GS depth buffer could differ. The source page is captured, not inferred. The composite
is drawn with the measured sampler/sprite rules; no other 2D primitive class is implemented (the HUD is S3).

## 29. Round 28 (2026-11-10, branch `task/090-post-chain`): the whole PATH2 full-screen chain, the CT16 present and the residual restated (TASK-090 post layer)

One branch from `task/090-coregs3` at `0a9a7e38` (the S2 merge plus the world-materials v2 study). One problem: apply
the **whole** captured PATH2 full-screen chain in stream order over the offline world frame, model the CT16 present
that produces the reference scanout, and restate the image comparison against the raw and the CT16-corrected reference.
These are the world-materials v2 study's fixes 1 and 2 ([COREGS3-WORLD-MATERIALS-V2](COREGS3-WORLD-MATERIALS-V2.md)).

### 29.1 The chain census: eleven batches, every one applied or excluded with a reason (C)

The decoder now carries TEX2, DTHE, DIMX and COLCLAMP and the per-vertex RGBAQ, and `find_post_chain`
(`native/coregs3/coregs3_gsdecode.cpp`) classifies **every** full-screen batch of the attributed stream by exact state
signature, in sequence order. Scene frame 0 (VBlank 7398, stream 7399): **11 full-screen batches, 0 unrecognised**,
across every captured stream 7398-7405 (the same eleven every frame).

| seq | op | target / state | measured effect |
|---|---|---|---|
| 672 | pre-world RGB clear | FBP 210/FBW 10/CT32, FBMSK 0, TEST 0x30000, RGBA (0,0,0,0), 10 strips | frame starts black, alpha 0; the world image is rendered into it, so it is accounted for, not re-applied |
| 675 | pre-world alpha clear | same, FBMSK 0x00FFFFFF | alpha 0, RGB kept |
| 678, 681 | pre-world TEST clear | same, FBMSK 0, TEST 0x32001 (ATE + ZTST GEQUAL) | two more black clears under the depth-tested signature |
| 684 | **frame sample / depth clear** | PRIM 0x15E, page 210 CT32 1024x512 region-clamped 0..639 x 0..447, TEX1 nearest, vertex colour **(128,77,0,128)**, ALPHA 0x44 normal blend, TEST 0x3800F (ATE ALWAYS, ZTST GEQUAL), FRAME FBMSK **0xFFFFFFFF**, ZBUF **ZMSK 0** | **writes no colour**: every colour lane is masked; with ZMSK 0 and z=1 it writes the frame's depth buffer (the depth clear). 20 sprites, 2,573,956 sampled pixels, 0 colour pixels written. **It is not a colour grade.** |
| 920 | reflection alpha clear | FBP 210, FBMSK 0x00FFFFFF, TEST 0 | applied inside the world render immediately before the PassId 7 draws (`clear_alpha_before_wet_reflection`); the world image's alpha is the reflection mask |
| 934 | wet composite | page 394 sampled, ALPHA 0x58 `(Cs - 0) * Ad + Cd` | 20 sprites, 2,573,956 px covered, **70 px changed** (the horizon band, outside the compared mask), added luma 2654 |
| 936 | post multiply FIX 127 | full-screen sprite, ALPHA 0xA9 `(Cd - 0) * FIX + 0` | 2,573,956 px written |
| 964 | post multiply FIX 255 | same, FIX 255 | 2,573,956 px written |
| 1094 | **CT16 present** | page 210 -> FBP **140**/FBW 10/**CT16**, vertex (128,128,128,128), TEST 0x3820C, FBMSK 0, ZBUF ZMSK 1 | the scanout image: DTHE **1**, DIMX **0x0001044501100554**, COLCLAMP **1**; 2,573,956 px written |
| 1207 | display alpha clear | FBP 140 CT16, FBMSK 0x00FFFFFF, TEST 0 | writes only the display buffer's 1-bit alpha; 0 RGB px changed |

The present target alternates between the two CT16 display buffers: streams 7399/7401/... write FBP 140, streams
7400/7402/... write FBP 0. Reference `frame_07400` is the scanout at VBlank 7400, i.e. the display buffer presented in
window 7399, which is the **FBP 140** present of stream 7399 (the V -> V-1 attribution).

### 29.2 The frame sample is the depth clear, not a colour transform (C)

The study read the frame sample's (128,77,0,128) vertex colour as a tint. The captured registers say otherwise:
`FRAME = 0xFFFFFFFF000A00D2` (FBMSK all ones) masks every colour and alpha lane, so the modulated-and-blended colour is
discarded; `ZBUF = 0x0000000001000046` has ZMSK 0, `TEST = 0x3800F` has ZTE 1/ZTST GEQUAL, and the 20 full-screen
sprites carry z = 1. The draw writes the Z16 depth buffer at page 70 for the whole screen, i.e. it is the frame's depth
clear; the world render's own initial depth represents the same state. It changes no colour pixel, so the (128,77,0)
modulation is **not** part of the picture. (The two pre-world clears also write depth 0 under the same ZTE/GEQUAL state.)

### 29.3 The CT16 present and the CT16 correction (C for the conversion, H for the bias)

The present samples page 210 1:1 (TEX0 `0x0007140668029A40`: TBP0 0x1A40, CT32, TW 10, TH 9, TCC 1, TFX 0; TEX1 0 =
nearest; CLAMP region 0..639 x 0..447), modulates by the neutral (128,128,128,128), writes no blend, converts to CT16
with the captured DTHE/DIMX and COLCLAMP and expands `v5 << 3`. DIMX is sixteen signed 3-bit entries; with
`GT4_HW_DITHER=native` (the reference run's option) CoreGS 2.0 divides the output pixel by the 3x scale before the 4x4
phase (`native/hw_gs/shaders/core2_fill.comp`), which the offline present reproduces.

The reference dump is that CT16 scanout: **1.0000** of its clean-mask values are divisible by 8 and its maximum is
**248** (reproduced). The study's 3.5-4.5-level bias for the *pre-present* stages is therefore a measurement choice
(+4 used, label H); the **final** stage is compared against the **raw** dump because both are the same CT16
quantisation. `scripts/coregs3_bench/gs_composite_metrics.py` now reports both in every run (JSON: `raw dump` and
`dump + 4 (CT16)`), so the raw baseline is never silently replaced.

### 29.4 Offline application (C)

`gt4_coregs3_render --gs <capture>` now applies the chain in stream order: the four pre-world clears and the depth
clear are accounted for inside the world render (the renderer starts from the same black frame and its own depth); the
reflection alpha clear is applied at its stream position by the world renderer; the composite, FIX 127, FIX 255 and the
CT16 present are applied over the world image in sequence order. `--chain-dump <dir>` writes the image after every
stage, the report prints a line per batch, and the tool's `--out` is the CT16 display buffer (the scanout image).
`--no-post-chain` turns the whole chain off; `--no-wet-composite` skips only the composite. `gt4_coregs3_gs --census`
prints the chain per scene frame and the present's DTHE/DIMX/TEX0 words, and the JSON carries every entry.

### 29.5 Measurement (C for the numbers; the interpretation follows)

Scene frame 0, 3x, the morph-v6 capture + the S1 `.gt4gs`, the pass-textures mask, the M1 clean mask (1,780,790 px),
reference `frame_07400`. `--chain-dump` stages; per stage the whole mask and page `0x36D0`:

| stage | luma | PSNR raw / +4 | SSIM raw / +4 | pct_bad raw / +4 | mean abs raw / +4 | fit raw / +4 (ref = a*world + b) |
|---|---|---|---|---|---|---|
| world (no chain) | 18.95 | 22.35 / 21.55 | 0.6508 / 0.6293 | 64.8% / 80.3% | 14.84 / 17.69 | 0.6354w + 14.649 / +18.649 |
| + wet composite | 18.95 | 22.35 / 21.55 | 0.6508 / 0.6293 | 64.8% / 80.3% | 14.84 / 17.69 | 0.6354w + 14.649 / +18.649 |
| + multiply FIX 127 | 17.94 | 22.19 / 21.34 | 0.6395 / 0.6113 | 70.3% / 83.2% | 15.48 / 18.48 | 0.6403w + 15.199 / +19.199 |
| + multiply FIX 255 | 34.60 | 20.41 / 20.78 | 0.6108 / 0.6394 | 56.5% / 43.9% | 15.00 / 13.26 | 0.3744w + 13.734 / +17.734 |
| + CT16 present (final) | **29.85** | **20.78 / 20.86** | **0.5880 / 0.5902** | **27.7% / 54.0%** | **13.47 / 13.75** | **0.3727w + 15.566 / +19.566** |

Page `0x36D0` (955,413 px): world luma 16.61, PSNR 25.17 raw / 23.80 +4; after FIX 255 luma 30.22, PSNR 24.53 / 25.20;
after the present luma **25.45**, PSNR **25.03 raw** / 24.92 +4, fit `0.1234*w + 20.870`.

- **The chain explains the level.** The world/reference luma ratio is 1.409 raw (1.620 corrected); the final CT16
  display is 29.85 against the raw reference's 26.69 (ratio **0.894**) and against the +4-corrected 30.69 (ratio
  **1.028**). The pre-present frame (34.60) is 1.127x the +4-corrected reference. Per page (raw reference / present):
  median **0.943**; 0x2FC0 0.988, 0x3EE1 0.974, 0x37D0 0.968, 0x3E26 0.966, 0x36D0 0.943; the outliers are 0x3E47
  **0.689**, 0x37B5 **0.795** and 0x3BF6 **0.466** (the known content outlier).
- **The residual is structural, not a level.** The pooled fit slope falls to **0.373** (corr 0.492): the reference's
  content is now flatter than ours. Within the correlated pages the slopes are 0.60-0.90 (0x3D36 0.856 corr 0.908,
  0x3E26 0.890 corr 0.731, 0x3DCE 0.744, 0x37B5 0.734, 0x2FC0 0.498), i.e. our contrast is 10-40% high. The road page
  still does not register (corr **0.099**, fit `0.123*w + 20.87`): the reference's road is nearly flat where our
  level-0 texture has detail, which is the study's **fix 4** (MXL 1/MMIN 4 sampled at level 0).
- **The black floor is lifted before the chain.** Deciles of the raw reference: its darkest decile (mean 4.09) maps to
  our present **16.56** (the pre-chain world was already 11.98 there); the mid deciles (23-32) map to 0.94-1.01; the
  brightest decile (mean 70.36) maps to 66.49. The lift is therefore in the material/lighting path (fix 3), not the post
  chain.
- The CT16 quantisation costs the frame 4.75 luma (34.60 -> 29.85) through 5-bit truncation and the captured dither
  (DIMX mean entry -1.125); the reference carries the same quantisation, which is why the final stage is compared raw.
- PSNR/SSIM move slightly down while pct_bad and the mean absolute error improve because the chain reaches the level
  but overdrives the contrast: saturated pixels rise from 0.68% (ours, pre-present) to the reference's 0.015%; the
  MSE is dominated by the bright-end outliers, the mean/pct_bad by the now-correct level. The honest reading is that
  the post chain is **not** the remaining picture gap; it fixes the level and leaves the material transfer curve, the
  dark floor and the road's mip pattern to fixes 3 and 4.

### 29.6 Checks

| Gate | Result |
|---|---|
| CTest coregs3 | **16/16** (new cases: the eleven-batch chain order and counts, the unknown-batch exclusion, the order refusal, the missing-present refusal, DTHE/DIMX/TEX2/COLCLAMP and the per-vertex RGBA decode; the clear/frame-sample/present arithmetic, the CT16 conversion and dither, the 1:1 sampling and the refusals) |
| Python `test_coregs3_units.py` with `GT4_NATIVE_BUILD` | **15/15** (new metrics control: the raw and +4 blocks, the raw baseline unchanged) |
| `--no-post-chain` vs the pre-branch binary's world-only render (same scene/vram/tap/gs) | **byte-identical**: PNG `34BDEE072EFAA2CFA7C3198D0742A877234C4915F5D4AAB76EA4BFDF773C4E28`, mask `2858FB62...` |
| The chain's composite stage vs the world-materials v2 `world-base.png` (composite on) | **byte-identical**: PNG `B52D0B35...` |
| Car/all-draw render vs the pre-branch binary (640x480, same capture) | **byte-identical**: PNG `B0E7082B...`, report `63FADB34...` |
| 3x off/shadow identity gate (`coregs3-night-check.sh`, reference `coregs3-r8/ref/gt4vulkan.exe`) | **COREGS3-NIGHT-CHECK-OK**, 8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences, 13/13 frames identical |

Evidence: `build/evidence-post-chain/` (census text/JSON, the chain report, per-stage images and metrics against both
references, the A/B and car hashes, the gate log). Limits: the V -> V-1 phase relation is S2's measurement (the
reference's image grouping supports it: frame 0 matches reference 7400 better than frames 1-4, 22.35 vs 22.01-22.22 dB);
the PATH1/PATH2 interleaving inside the stream is not in the sidecars, so the pre-world role of the clears and the depth
clear is taken from their sequence position and state, not from per-packet job order; the colour replay does not model
the depth buffer (the world render owns it); the per-page map mixes materials that write no depth (the ABE decals).

## 30. Round 29 (2026-11-11, branch `task/090-crash-race`): the dry-pass crash is a host heap overflow in the timeline pairing, not guest code

The host-clock round left one open lead: the removed per-VBlank counter pass, restored behind `GT4_COREGS3_DRY_PASS=1`,
died with an access violation at VBlank ~6783, and the round read the crash as a guest/emulator timing race in
recompiled guest code (`gen::unit_00E0`, guest PC 0x00482674). This round reproduces it, captures the faulting thread
and address, and closes it with a host bug in the CoreGS 3.0 presentation timeline. No guest code, guest memory
semantics or renderer gate changed.

### 30.1 The first attribution was a reporting error (C)

The host-clock handler printed the live guest register file (`ps2::g_cpu`) and the guest stack on any process fault.
Guest recompiled code runs only on the EE thread; the fault was on the **GS worker** (`GsWorker::run`). The guest PC it
reported (0x00482674, and 0x00455A64 in a traced run of this round) is simply what the EE thread was executing at that
instant, and the guest stack it walked was the EE thread's. The new diagnostic (`GT4_CRASH_DUMP=1`,
`native/crash_dump.{hpp,cpp}`) prints the faulting thread id and role, the exception record (code, access type,
faulting address), the host backtrace as PE offsets, and only then the guest state as context. Every reproduced run
reports `thread=worker`.

### 30.2 The fault (C)

Three serial dry-pass runs put the fault inside CoreGS 2.0's standard containers on the GS worker, always loading a node
link of `0xFFFFFFFFFFFFFFFF`:

| Run | Faulting chain |
|---|---|
| `build/evidence-crash-race/dry-pass-crash-1` | worker, `mov rax,[rax]` with rax = -1, in an `unordered_map<u64, ...>` rehash (PDB: core2 renderer inline chain) |
| `build/evidence-crash-race/dry-pass-crash-2` | `GsWorker::run → execute_mtvu_job → gif_now → core2::Renderer::GifTransfer → FrontEnd::GifTransfer → GifParser::Transfer → StripVertices → Kick → FlushBatch → EmitBatch → OnBatch → ProcessBatch → MemoryTexture → ContentKey → blockLists_.try_emplace` (rehash) |
| `build/evidence-crash-race/dry-noblockver` (`GT4_CORE2_BLOCK_VERSIONS=0`) | `GsWorker::run → vsync_now → hw.flush → core2::Renderer::Flush → TrimTexturePool`, walking `texturePool_` |

A pointer that reads as -1 inside a container's node list is already-corrupted heap, not the container's own logic: the
renderer runs solely on the GS worker under `backendMutex_`, so the finding is a corrupting write elsewhere.

### 30.3 The corrupting write: the timeline pairing bookkeeping (C)

`interpolate_frame` (`native/coregs3/coregs3_presentation.cpp`) pairs the older and the newer frame by object key. Its
`used` vector marks the matched objects of the **newer** frame and is indexed with `newer_index`, but it was sized by
`older.objects.size()`. A match at a newer index at or past the older list's length reads `used[newer_index]` and writes
`used[newer_index] = 1` past the end of the allocation. The draw list grows between claimed frames at the race start,
which is exactly where the dry pass (two interpolations per VBlank at 120 Hz, each on the GS worker) reaches the bad
index. ASan on the fixture (`coregs3_presentation_fixture`, `check_growing_draw_list`): **heap-buffer-overflow READ of
size 1 at coregs3_presentation.cpp:540, 0 bytes after the 2-byte region allocated at line 534**; the same check with the
size fixed prints `coregs3 presentation fixture: ok` with no ASan report. Build the ASan control with `/OPT:NOICF`: the
linker's identical-COMDAT folding merges string literals and template instantiations, and ASan's global metadata then
disagrees with the merged object (a `strtoul` read past a literal appears, an artifact that disappears with NOICF).

The fix sizes `used` by `newer.objects.size()` (the list it indexes) and is host-only: the pairing for a growing list
now yields the defined result (objects previously skipped by an out-of-bounds read are matched), it does not touch guest
memory, the simulation digest or the guest clock, and at 60 Hz every instant still draws the captured matrix exactly.
The fixture pins the semantics for older `{A, B}` / newer `{new, A, B}` (matches at newer indices 1 and 2) and for an
empty older frame.

### 30.4 Why the dry pass made it fatal (H)

At 120 Hz the dry pass interpolates every due instant on the GS worker, so the OOB fired on every VBlank for thousands
of frames; each pass damaged a small allocation, and the first container operation to touch a damaged block died with
the -1 read. The delivered 120/144 Hz window path calls the same `interpolated_transforms` once per drawn tick, so it
carried the same latent overflow; the host-clock window runs did not fail, but their heap layout and claim timing
differ. The fix covers both.

### 30.5 Reproduction and controls

- `GT4_COREGS3_DRY_PASS=1` restores the removed per-instant counter pass (`native/coregs3/coregs3_present.cpp`),
  diagnostic only; it runs on the GS worker inside `vsync_now`, exactly where the removed code ran.
- Runs of 9000 frames, 3x night race, headless, 120 Hz present, serial on a quiet machine. A run without the dry pass
  completes (`control-no-dry-pass`, 7500 frames, exit 0).
- Pre-fix ASan: the fixture report is `build/evidence-crash-race/asan/fixture-prefix-niocf.txt`; the game under ASan with the
  dry pass needs the CRT artifacts above silenced and did not reach the race-start window before an unrelated process
  exit, so the fixture is the ASan control (`asan-game-prefix/log.txt` records that limit).
- Post-fix acceptance: `GT4_COREGS3_DRY_PASS=1`, 9000 frames, two runs on the acceptance binary
  (sha256 `9ECE760F448D0DC8A29B996DF18E3A3840B534692092CE7C910D24E0CF59EB00`): `acceptance-9000-run2b` and
  `acceptance-9000-run3`, both exit 0 with 8,999 frames and 8,999 published snapshots; the third run in
  `acceptance-9000-run1b` (same solution and game code, before the crash handler's filter change) also passed; the
  final rebuild after comment-only edits (sha256 `64EA9B2B3A34FFEDBF60A73FAF2984EEF964DE20556F1195B1BE36559336CA2A`)
  completes once more (`acceptance-9000-run4`).
- Guest identity of the dry pass: two 7,500-frame dump-at-7400 runs, one with the dry pass and one without, compare the
  skip table (vblank, rendered, clock, gate, digest, race) and the 128 KiB state dump (131,072 bytes): 7,497 rows identical, 0 clock/digest differences, 0 differing words
  (`DRY-PASS-IDENTITY-OK`).
- The first acceptance attempt (`acceptance-9000-run1`) ended at frame 4683 with exit 127 and no dump; its rerun
  completed, so the attempt is recorded as an invalid run, not as a second failure mode.

### 30.6 Gates

| Gate | Result |
|---|---|
| `GT4_COREGS3_DRY_PASS=1` 3x night, 9000 frames, serial | **two runs exit 0** on the acceptance binary (`run2b`, `run3`), a third (`run1b`) on the same game code, and one on the final rebuild (`run4`) |
| Dry pass vs reference, guest clock/digest/state | `DRY-PASS-IDENTITY-OK` (see 30.5) |
| CTest coregs3 | **19/19** (new `check_growing_draw_list`) |
| Python `test_coregs3_units.py` with `GT4_NATIVE_BUILD` | **15/15** |
| 3x off/shadow identity gate (`coregs3-night-check.sh`, reference `coregs3-r8/ref/gt4vulkan.exe`) | `COREGS3-NIGHT-CHECK-OK` |
| ASan fixture, pre-fix | heap-buffer-overflow at `coregs3_presentation.cpp:540` |
| ASan fixture, post-fix (`/OPT:NOICF`) | clean: `coregs3 presentation fixture: ok`, exit 0, no report |

Evidence: `build/evidence-crash-race/`. Limits: the exact byte-level path from the overflow to the -1 node link is not
traced (the crash-site signature and the ASan proof are the evidence); the ASan game run did not reach the race-start
window; the fix corrects this overflow, not every heap bug in the renderer.

## 31. Round 30 (2026-11-11, branch `task/090-live-world`): the captured course drawn in the live present

One branch from `task/090-coregs3` at `5b82c832` (worktree `O:/Antigavity/gt4-wt-090-live-world`). The offline world
renderer selects the sky, far underlay, main-view course and reflection passes, decodes their textures and applies the
GS state to a CPU picture. This round draws that world **live**: `GT4_COREGS3=present` now composes the captured world
passes over the CoreGS 2.0 display image before the car pass, with the presentation timeline's matrices, while the
guest stays bit-exact.

### 31.1 What the live path needs that the offline tool had from files (C)

The offline world picture reads three sidecars: the `.gt4scene` frame (draws, items, packets, pass journal), the
`.gt4tap` (per-item TEX0/TEX1/CLAMP and TEST/ALPHA/FRAME/ZBUF at the draw) and the per-job `.gt4vram` record (the pixels
the draw samples). The live path has the frame in memory (the hook ring) and none of the files. It gets the other two as
follows:

- **The frame.** In `present` the hooks stored only the car pass. They now store the car pass plus the passes the world
  selection names (`g.world_draws`: sky, far underlay, course, reflection) and nothing else; `GT4_COREGS3_ALL_DRAWS=1`
  still stores every hooked call. A menu frame's ~26 unmarked shape calls are no longer copied (a `CapturedDraw` is
  ~50 KB), which was the guest-thread cost of the naive all-draws choice. The pass journal and environment were already
  captured in every mode.
- **The pixels.** `native/coregs3/coregs3_live_world.{hpp,cpp}` is the in-memory version of the `.gt4vram` job record.
  At job post the MTVU runtime reports `(job, packet guest address)` through the tap module's live sink
  (`set_live_job_source_sink`/`live_job_source`, `coregs3_tap.*`); on the GS worker, immediately before a job's records
  execute, the sink copies the texture/palette windows that job's TEX0 writes bind from the worker-owned shadow local
  memory. `Renderer::Coregs3Slm()` (new) exposes that memory **without** the flush/readback of the public `Slm()`: the
  first implementation used `Slm()` and every VU1 job triggered a batch flush and readback, which made the present run
  unusable; the no-flush view is the same one `Coregs3Snapshot` uses. Every address the job was posted for receives a
  shared immutable record; records live for eight guest frames (4,096 records / 96 MiB bounds; the pending-source FIFO
  drops executed jobs), and `find(address)` returns the newest. The measured phase: the worker executes a capture
  frame's jobs after the VBlank that claims the frame, so the store is always one guest frame behind and the world
  samples the same pages' previous upload; the caller counts that as `texture_stale` (it is 100% of the lookups on the
  night race, 0 live, 0 missing).
- **The state.** With no tap, `build_frame_texture_table` gives every world item the K7 pass journal's TEST/ALPHA/
  FRAME/ZBUF/XYOFFSET/FOGCOL (`world_pass_default_state`, the same helper the offline tap-fallback path used) and keeps
  the measured default PRIM 0x3C (TME/FGE on, ABE off). A new `TextureFrameOptions::live_job_textures` maps a packet
  address to its job's record; the table tries it before the tap/job/draw/pass images, counts
  `world_live_job`/`world_live_job_missing`, and the base SLM is left empty on purpose so a texture page no job window
  covers is refused instead of read from a stale copy.

### 31.2 The live world pass (`native/hw_gs/coregs3/hw_coregs3_present.*`, C)

- **Selection.** The build reuses `coregs3::world_item_exclusion` and `world_shape_status` on the captured frame, with
  an item mask (`MeshOptions::item_mask`, new) so an excluded item is skipped without copying the draw. Meshes are built
  with the descriptor-driven world layout; triangles are grouped per item into one draw range each.
- **Vertex stage.** A 24-byte world vertex (captured-unit position: the STMOD 2 int32 lane, a raw V3-16 halfword or a
  V3-32 float; V4-8 colour; u,v) and a 112-byte push constant: the captured 0xC1 columns, the raw TEX0/CLAMP words, the
  atlas rect and TEST/ALPHA/FBMSK/flags. `clip = c0*x + c1*y + c2*z + c3`, `gl_Position = (cx, -cy, 1, cw)`: the same
  clip-space convention as the car pass, so the output resolution is only the viewport (no 640x448, no integer scale).
- **Fragment stage.** The item's texture is sampled from one R8G8B8A8 storage atlas (2048x2048, the used rectangle
  uploaded with a staging buffer; a full atlas is refused and counted): nearest at level 0 with the measured 10.4
  coordinate and the four CLAMP wrap modes; MODULATE with the 8-bit integer product and TCC; the alpha test
  (ATE/ATST/AREF, AFAIL KEEP -> discard); the FRAME.FBMSK lane merge. No colour attachment: the composed display image
  is written with an imageStore like the car pass.
- **Depth and order.** The world is drawn first in the same private D32 buffer, depth = 1/cw with GEQUAL, so the world
  and the cars occlude each other; the sky (when drawn) uses a second pipeline with depth test and write off, as the
  offline background rule. The presentation timeline's per-object matrix replaces the captured columns on ticks between
  two frames (alpha 0/1 returns the captured matrix).
- **Per-tick scheduling.** The world is built lazily on a claimed frame's first presentation tick and drawn in
  `draw_presentation_tick`, the per-tick function the host-clock round introduced: `render_tick` (one picture per
  display tick, base save/restore between ticks) and the whole-batch `render()` both go through it, so the world
  follows the host clock without a second scheduling path. The build and the draw share one mutex.
- **Counters.** `present::WorldFrameCounters` (items, vertices, triangles, atlas, texture live/stale/missing/refused,
  exclusions, sketches) feeds the F8 report; two occlusion queries per tick count the samples that passed the depth
  test for the world and for the cars.
- **Switches.** `GT4_COREGS3_LIVE_WORLD=0` is the A/B (the world path is not built; the snapshot is car-only, exactly
  the pre-round path). `GT4_COREGS3_WORLD_BACKGROUND=1` adds the sky and far underlay (they are background layers that
  cover the whole viewport and hide the base's HUD; the default keeps them off). `GT4_COREGS3_PRESENT_DEBUG=1` prints
  the claimed frame of each drawn tick (the frame-locked evidence instrument).

### 31.3 Measurements (night race, 3x validation scale)

- **Selection and drawing** (VBlanks 6600-7400, 5 builds): 3,853 items, 195,711 mesh vertices, 108,677 triangles,
  5,131 draw calls over 6 ticks, 338 atlas textures, 0 atlas refusals, 0 refused draws; exclusions 1 itop-other,
  12 no-position, 0 shape/no-state/mirror; skipped by design: sky 80 items, far underlay 26, reflection 61.
- **Textures**: 3,972 lookups, all stale by one guest frame, 0 missing, 0 refused; 1,510 items untextured (no
  UV/program/record) keep their vertex colour; 2,287 linear records are sampled nearest; 4 records took the level-0
  mip fallback. 3,853 items carry PRIM fog (deferred) and 0 blend.
- **Guest identity**: on/off/base runs produce the same race clock and per-VBlank digest on all 7,497 common VBlanks
  (0 differences) and the same 64 KiB state at VBlank 7400; the car-pass counters are identical (5 passes, 542 draws,
  143,163 vertices, 47,721 triangles, 16 unlit draws).
- **Frame-locked A/B** (`EVERY=1` dumps 7396-7400, the claimed frame printed): on and off claim the same sequence
  (7397, 7398, 7399, 7400, 7400, 7400). The world mask is 2,533,418-2,536,075 px (98.5-98.7% of 1920x1344) because the
  course pass includes the buildings, barriers and billboards behind the road. At claimed frame 7400 against the r8
  reference frame 07400: mean |diff| 20.80, PSNR 16.11 dB, luma world 28.78 / world-off 27.23 / reference 28.45 (the
  world brings the level closer and the structure remains the offline renderer's M1/M2 residual).
- **Merged tree**: after merging `origin/task/090-coregs3` (host clock + crash fix + HUD A-Spec + texture/mip) and
  removing the one extra brace the resolution left in `render()` (`1b9c4953`), a clean rebuild (executable SHA-256
  `78ef497f...`) reproduces the mask and reference numbers: the frame-locked window claims the same frames, world mask
  2,533,610-2,538,072 px, mean |diff| 20.834, PSNR 16.108 dB, luma 28.759 / 27.216 / 28.444 (world / off / reference);
  at 120 Hz the world draws both ticks of each guest frame (world ticks 12 over 6 dumped frames, 15,336 draw calls,
  0 refusals).
- **World-off vs base-present**: the same binary with `GT4_COREGS3_LIVE_WORLD=0` and the base executable differ by
  1,185-1,715 px per frame-locked dump, all inside the car box (x 1075-1607, y 232-862, max delta ~130): the base
  present layer's own cross-run car-overlay nondeterminism (two base runs in the same window differ by 663-1,363 px).
  The guest state, clock, digest and car-pass counters are identical; no world pixel exists in the off path.
- **Checks**: CTest coregs3 **20/20** (new `native_coregs3_live_world`; `native_coregs3_pass_textures` extended with the
live binding, journal state and missing-record cases), Python `test_coregs3_units.py` **17/17**, the 3x
off/shadow identity gate **COREGS3-NIGHT-CHECK-OK** (8,997 VBlanks, 1,686 racing, 0 clock/digest/state
differences, 13/13 frames identical for off and shadow, capture 4,496,552 B), and the frame-locked A/B and
world-mask measurements of §31.3.

### 31.4 What is deliberately simplified, and what still assumes 3x

- The sky and far underlay are not drawn by default (they are full-viewport background layers that would hide the
  CoreGS 2.0 base and its HUD); the course is the first live pass. `GT4_COREGS3_WORLD_BACKGROUND=1` restores both.
- The per-item GS state is the pass journal's, not the tap's: the 22 external-cursor items whose record TEX0 differs
  from the emitted one bind the record page; the 0xA0 morph items use the record path's first-key/decoded UVs.
- Fog, blending, bilinear filtering, the reflection pass, the post chain, effects and the HUD are later. 1,510 items
  have no usable texture binding and keep the vertex colour (the bright billboards are this class); 2,287 linear
  textures are sampled nearest.
- The HUD: the base's 2D layer is composed before the world, so the world covers it where the world draws. Splitting
  the 2D overlay to draw after the 3D is the next milestone.
- Resolution/aspect: the live path itself has no 640x448 or 3x assumption; it draws in clip space into the composed
  image's viewport, so 1080p 16:9 and 3440x1440 work without a path change. The **evidence harness** (the r8 reference
  frames and the identity gate) is 3x, and the offline CPU renderer was validated at 3x; neither is used by the live
  path. The occlusion sample counter is a pixel count and is resolution dependent by definition.


## 32. Round 31 (2026-11-11, branch `task/090-dither`): where the CT16 dither is, and the `GT4_COREGS3_DITHER` option

One branch from `task/090-coregs3` at `a59ce9c9` (worktree `O:/Antigavity/gt4-wt-090-dither`). The owner saw a dotted
pattern across the CoreGS 3.0 picture and confirmed it is the PS2's CT16 dither (2026-11-11 02:31); the modern path must
be able to remove it, while the faithful modes keep it. This round pins each source with evidence, adds the switch to
the offline and live CoreGS 3.0 paths, and measures the A/B. Clean room: no PCSX2 source, fork or dump was opened.

### 32.1 Where the dither is (C)

Three different things write the picture, and only two of them dither:

- **The offline CT16 present model** (`native/coregs3/coregs3_wet_composite.cpp`, `apply_present_ct16`). The captured
  PATH2 present is page 210 -> FBP 140/FBW 10 CT16 with **DTHE 1, DIMX `0x0001044501100554`, COLCLAMP 1** (the same
  registers as the night race, §29). Per channel it computes `(source * 128) >> 7` (MODULATE with the neutral vertex
  colour), adds the DIMX entry of the pixel's 4x4 cell - selected by `(x / 3) & 3, (y / 3) & 3` at the 3x validation
  scale, so the cells are 12x12 output pixels - clamps (COLCLAMP), truncates to five bits and expands with `<< 3`.
  Decoded, the DIMX matrix is `[-4 -3 -3 0] / [0 +1 +1 0] / [-3 -4 -4 0] / [+1 0 0 0]`: **six negative and ten
  non-negative entries, mean -1.125**. With the five-bit truncation the model is **4.4-4.8 levels darker** than the
  8-bit image on average (measured in 32.3). This is the dotted pattern.
- **The CoreGS 3.0 live present** (`native/hw_gs/coregs3/hw_coregs3_present.cpp`, its `coregs3_present.frag` and
  `coregs3_world.frag`). Both fragment stages write a packed 8-bit RGBA8 word (`level()` = round to 0..255) and never
  apply a dither or a five-bit truncation. Its pixels are dither-free today, in both switch states.
- **The CoreGS 2.0 composed base.** In `GT4_COREGS3=present` the image the CoreGS 3.0 pass draws over is CoreGS 2.0's
  composed CT16 output; CoreGS 2.0 applies its own DTHE/DIMX dither in `hw_process_pixel`
  (`native/hw_gs/shaders/hw_pixel_core.glsl`, `s.dither` from the FRAME/DTHE decision) at native 4x4 cells, which at 3x
  are the same 12x12 output blocks. That is faithful CoreGS 2.0 output and this round does not change it.

### 32.2 The switch (C)

`GT4_COREGS3_DITHER=0|1` (`native/coregs3/coregs3_dither.hpp`, one parser for both paths; absent or empty = the modern
default `0`; only `0` and `1` are accepted, anything else is refused with the value named). `gt4_coregs3_render` reads it
and also accepts `--dither 0|1` as a same-run override of the environment (the A/B instrument).

- `1` (faithful): `apply_present_ct16` reproduces the captured CT16 scanout conversion exactly - the DTHE/DIMX entry
  (when DTHE bit 0 is on), COLCLAMP, the five-bit truncation and `<< 3`.
- `0` (the modern default): the same MODULATE, vertex colour, alpha bit and full-screen write, but the channel is
  written as the modulated 8-bit value with COLCLAMP only. Both the dither and the five-bit truncation are skipped:
  they are one scanout model, and keeping the truncation without its dither would leave the 32-level banding the dither
  exists to hide (measured: 32 levels against 129 on the frame, 32.3).
- The live present parses and reports the mode in its init line, but its pixels do not change: it already writes 8-bit
  words. The base dither is CoreGS 2.0's and stays until CoreGS 3.0 owns the 3D frame (phase 4); the switch covers what
  the CoreGS 3.0 path controls.

### 32.3 The A/B (C)

Same A-Spec capture (scene frame 0 = VBlank 7600, world + the whole post chain, no HUD), same `gt4_coregs3_render`
command, `GT4_COREGS3_DITHER=1` against `=0`:

| measure (whole frame) | value |
| --- | --- |
| pixels | 2,580,480 |
| PSNR (1 vs 0) | **33.46 dB** |
| mean \|diff\| / max \|diff\| | **4.51 / 11** |
| differing pixels | **2,473,548 (95.9%)** |
| mean signed (1 minus 0) | **-4.43** (the CT16 model is darker) |
| values divisible by 8 | faithful **100.0%** (32 levels) / off **5.4%** (129 levels) |

- **The dot pattern isolated.** A flat road area at (1824,1152) 48x48 is a constant 63/53/41 in the off image; the
  faithful image is the 12x12-cell pattern of 56/64 (R) and the reference is its own 12x12 quantised pattern. On that
  crop: PSNR 33.08, mean |diff| 4.96, max 9, **100% of pixels differ**, mean signed -4.83; faithful 5 levels (all
  divisible by 8), off 3 (none). Against the CoreGS 2.0 reference frame 7600 on the same crop, faithful is **PSNR
  20.67 / mean 21.48** and off **19.22 / 26.22**: the faithful model is closer to the reference, and the absolute level
  gap is the known S2 material residual, not the dither.
- **Within-cell structure** (mean absolute deviation from each 12x12 cell's mean on the road crop): reference **4.11**,
  faithful **2.94**, off **1.04**. The faithful image recreates the reference's dot structure; the off image is smooth.
- Crops and the amplified difference image (`flat-crop-dither1.png`, `flat-crop-dither0.png`,
  `flat-crop-diff.png`, a x6 zoom and a three-up sheet) are in `build/evidence-dither/`, with the JSON metrics
  (`ab-full.json`, `ab-flat-crop.json`, `metrics-ref-vs-dither{0,1}-flat.json`).

### 32.4 The live path (C)

`GT4_COREGS3=present` at 3x, night race, six dumped frames 6600-7600, with three present runs (`GT4_COREGS3_DITHER=1`,
`=0`, and a same-config `=0` control) and a plain CoreGS 2.0 run (`GT4_COREGS3=off`):

- **The switch does not change the CoreGS 3.0 pixels.** present1 against present0 differs on **0.27-2.40%** of the
  pixels (mean |diff| 0.01-0.68, max 61-240), and the same-config control present0 against present0b differs on
  **0.25-2.30%** (mean 0.01-0.69, max 26-255): the difference is the live path's known cross-run overlay
  nondeterminism (§31.4), not the switch. On a sky block the live world draws (624,160,48,48 at 3x), present0,
  present1 and present0b are identical to three decimals (mean 23.197/21.671/15.781, **15.9%** of values divisible by
  8, 28 levels): the pass writes 8-bit words in both states.
- **The base is the faithful CT16 image.** In the `off` run the same block is **100%** divisible by 8 with 2 levels
  (a flat CT16 area; the whole frame is CoreGS 2.0's composed scanout). The base's own dither therefore stays under the
  CoreGS 3.0 pass until phase 4, exactly as the owner's decision records; the switch does not reach it.
- **`GT4_HW_DITHER=off` does not remove the base dither in this build.** `Options.dither` is parsed in
  `native/hw_gs/hw_gs_options.cpp`, but nothing assigns it to `decide::Options::dither_enabled` (which keeps its `true`
  default); the measured `off` against `off + GT4_HW_DITHER=off` comparison is **0 differing pixels** on every frame.
  That is a pre-existing CoreGS 2.0 option gap, not part of this round, and the phase-4 work does not depend on it.
- **An invalid value is refused.** `GT4_COREGS3_DITHER=2` with `GT4_COREGS3=present` logs
  `[coregs3] live present refused: GT4_COREGS3_DITHER=2 is not 0 or 1 (...)`, and the game keeps running without the
  overlay (exit 0, 697 frames). Evidence: `build/evidence-dither/live-analysis.txt`, `live-block-*.png`,
  `live-off-sky-zoom4.png`, `live-present0-sky-zoom4.png`.

### 32.5 Checks

- **CTest coregs3 20/20**, including `native_coregs3_wet_composite` with the new per-pixel switch test (every channel
  of both modes against the CT16 arithmetic, the DIMX sign mix, the alpha lane) and the parser test (absent/empty =
  default off, `0`/`1`, and `2`/`on`/padded values refused). Python `test_coregs3_units.py` **18/18** (new
  `DitherAbTest`: the A/B script's metrics, crops and the size refusal).
- **The faithful value is byte-identical to the `a59ce9c9` tool** built from the base commit before the change
  (executable SHA-256 `c1f00751...`): world+gs `0B6FF0F0...`, car `1325882D...`, world `D84BAC90...`, HUD all
  `8169B899...` (PNG SHA-256, same command, `GT4_COREGS3_DITHER=1`).
- **The 3x off/shadow identity gate passes on the final binary** (SHA-256 `404aab46...`, built 2026-11-11 02:55):
  `off` and `shadow` against the `coregs3-r8/ref` executable: 8,997 VBlanks, 1,686 racing, 0 clock/digest/state
  differences, 13/13 frames identical, capture 4,496,552 B, **COREGS3-NIGHT-CHECK-OK**.
- The offline tool refuses `GT4_COREGS3_DITHER=2` and `--dither 2` with the value named, and `--dither 0` overrides
  `GT4_COREGS3_DITHER=1` to the off hash `853A1BEA...`.

## 33. Round 31 (2026-11-11, branch `task/090-live-world`): the live tap, the emitted fog and the GS bilinear

Continuation of §31 in the same worktree. The live world drew the captured course but deferred fog (all items), sampled
everything nearest and dropped ~1,500 items on record-path refusals (bright vertex-colour billboards). This round
restores the three gaps from the GS worker's own job records, with no capture file.

### 33.1 The live in-memory tap (C)

The GS worker already buffers each VU1 job's PATH1 GIF records to snapshot its texture windows. It now also copies the
records into `coregs3_live_world` (`note_job_stream`, with a global emission sequence and the job id), and
`build_tap_snapshot(frame.vblank, addresses, ...)` synthesizes the `.gt4tap` view for the world items' packet addresses:
one source record per requested address (stamped with the captured frame, which is what the table's pairing filters on)
plus the stored GIF records. Two measured details:

- **The state-only records matter.** The VU1 emits the TEX0/TEX1/CLAMP writes in vertex-less records that may belong to
  another job, so the snapshot keeps **every** stored record in emission order (the offline tap file's walk) while the
  source records stay filtered to the world addresses. Filtering the GIF records to the items' own jobs left the
  tracked state incomplete (`incomplete tapped texture state`, 708 refusals at VBlank 7400); the full walk reaches 0.
- **The base image matters.** A carried texture can name a page the item's own job did not write; with the empty base
  the decode refused it (`sidecar does not cover texture block 12891`). The backend's VBlank flush now copies the GS
  local memory over the measured world range `0x2BC0..0x3FFF` into the store (`note_vram_base`) — the `.gt4vram` vblank
  record's role; the job records take precedence and the base serves the carried pages.

The table is built with `use_tap = true`; the pixels still come from the live per-job store. The emitted records are one
guest frame old (the same lag as the textures), counted by `texture_stale`; the snapshot is bounded (64 MiB) and kept
small by the address filter.

### 33.2 Fog (C)

The world vertex carries the emitted fog byte as a `noperspective` varying (the GS fog lane has no Q and is
interpolated screen-linearly); the fragment applies the GS integer mix toward FOGCOL,
`(f*c + (255-f)*fogcol) >> 8` with the alpha unchanged, before the alpha test and the FBMSK merge — the arithmetic of
`coregs3::apply_vertex_fog`. A vertex without an emitted fog value keeps the 255 identity and is counted
(`fog_missing`); the sky pass stays unfogged. `GT4_COREGS3_WORLD_FOG=0` is the A/B.

A formula study on the tap (`gt4_coregs3_render --fog-dump`, one line per vertex: draw, item, local, clip w, tapped fog)
found the emitted fog is **linear in the clip-space w per item/draw** (697 of 802 varying items fit within one level,
median 0.09) but with per-item constants that are not in the captured packet, the light block or the pass journal: a
global fit on the captured scene parameters (the 0.2/0.05/0.1 fog densities, the 10/86.05 floats) is 10-14 levels off.
The emitted record is therefore the tap-free source; a guessed formula is not.

### 33.3 GS bilinear (C)

A linear item (TEX1 MMAG = MMIN = linear, the only mixed-free form the binding accepts) samples with the GS bilinear:
`(u-0.5)` to a floor base, 4-bit weights, the four taps wrapped independently with the four CLAMP modes, sum >> 8, then
MODULATE — the arithmetic of `coregs3::sample_texture`. The atlas holds the decoded RGBA8 at level 0;
`GT4_COREGS3_WORLD_FILTER=nearest` is the A/B; `filter_linear` counts the items sampled bilinear.

### 33.4 Untextured items (C)

With the emitted state and the base, the record-path refusals (mip records, the external texture cursor) bind the pixels
the draw actually sampled: at VBlank 7400 the table bound **1,262 items from the emitted state** (offline 1,268), 41
record fallbacks and **0 decode/state refusals**; the pre-round record path left 2,938 of the window's 5,112 world
items untextured (2,886 refused), the same window now has 52 (8 no-record, 44 no-UV) and 0 refusals. The remaining
untextured items are skipped by name (`texture_no_record`, `texture_no_uv`, `texture_texgen`,
`texture_refused`) and leave the CoreGS 2.0 base pixel — never a constant bright vertex colour. The billboards that were
white blobs now show their textures in the present image.

### 33.5 Measurements (VBlanks 6600-7400)

VBlanks 6600-7400, 5 dumps: 3,853 items, 195,711 mesh vertices, 108,677 triangles, 328 draws, 3,825 draw calls, 749
atlas textures, 0 atlas/draw refusals; fog applied to 3,688 ranges with 13,776 vertices on the 255 identity; 3,769
bilinear items; 12 level-0 mip fallbacks; 475 blend-deferred items (ABE is still not reproduced). Frame-locked A/B at
VBlank 7400 (on and off claim the same guest frame): world mask 2,505,741 px; against the r8 reference frame 07400 mean
|diff| 18.825, PSNR 17.346 dB, luma 24.687 / 27.423 / 28.628 (world / base / reference) — the structural match improved
over the pre-round (20.80 / 16.11 dB) while the absolute level is darker than the base, which carries the post chain the
live world does not. Fog on vs `GT4_COREGS3_WORLD_FOG=0`: 2,293,927 px, mean 2.59; fogged is closer to the reference
(18.83 / 17.35 vs 19.58 / 16.86). Bilinear vs `GT4_COREGS3_WORLD_FILTER=nearest`: 990,784 px, mean 3.17; bilinear is
closer (18.83 / 17.35 vs 18.90 / 17.23). Present images: `build/evidence-live-world/night-fog-diag2/frames/frame_07400.ppm`
(the billboards textured; the pre-round image at `build/evidence-live-world/night-liveworld-race-07400.png` has the white
blobs).

### 33.6 Gates

| Gate | Result |
|---|---|
| CTest coregs3 (`native_coregs3_live_world`, the extended pass-texture fixture) | **20/20 passed** |
| Python `test_coregs3_units.py` with `GT4_NATIVE_BUILD` | **17/17 passed** |
| 3x off/shadow identity gate (`coregs3-night-check.sh`, clean rebuild SHA-256 `b3ee396aece4e31f17af945f1f2301d31c5295c9344cee4cc009c94db018340f`) | **COREGS3-NIGHT-CHECK-OK**, 8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences, 13/13 frames, capture 4,496,552 B |
| Frame-locked A/B (on/off/nofog/nofilter, the same claimed frame) | the numbers of §33.5 |

Two earlier runs of the same gate reported the 512-word race-object state difference in the reference run's
dump (0 clock/digest differences, 13/13 frames identical); the candidate's off and shadow dumps were
byte-identical to each other in every run and held the race object's float values, which a second reference
run in another worktree reproduced exactly, while those outlier reference dumps held the 0xDD pattern at that
offset. The reference dump there is not reproducible across runs; the clean rerun above (reference window with
no other game process) is the valid one and the gate is unchanged.

### 33.7 Cars in the present (finding)

The live present's car pass draws the cars with a neutral grey body; the r8 reference frame 07400 shows the player's
red Mégane with lit taillights. This is not the picture round: the world-off A/B (car pass only) and the pre-picture
merged executable show the same grey car, and the car-region world-on/off difference is 27,818 px now against 26,560
px before the round. r8's present drew a white blob and r12's grey with white patches, so the paint/texture
reproduction has been a present gap since the early rounds. Follow-up: the car pass's paint/CLUT reproduction and the
world/car depth interaction; crops under `build/evidence-live-world/` (`car-crops.png`, `car-history.png`,
`car-r8-r12-r13.png`, `car-on-off-diff.png`, `car-merged-on-off.png`).

## 34. Round 32 (2026-11-11, branch `task/090-2d-split`): the 2D overlay split

Continuation of §31/§33 in a branch from `task/090-coregs3` (`43039a19`), integrated with the picture round and the
dither option at merge `9e1257a7`. The live world drew the course over the CoreGS 2.0 composed image, whose 2D/HUD layer
is composed into the same image, so the world covered the HUD where its geometry projected. This round draws the world
and the cars first and the 2D layer after them, at the guest rate. Full design, the stream census and the measurements:
[COREGS3-2D-SPLIT](COREGS3-2D-SPLIT.md) §8.

### 34.1 The cut (C)

The EE panel marker (`kPassPanel`) is written after the HUD draws in the stream, and DISPFB names the buffer being
scanned out during the frame (the flip lands after the VBlank), so neither can cut the 2D layer. The rule arms on the
stream's measured present blit (`full-screen-present-blit`: a full-screen unblended CT16 draw that samples another
colour target) and cuts at the next batch into that target. Pure state machine in
`native/hw_gs/core2/core2_2d_split.{hpp,cpp}`, pinned by `native_coregs3_2d_split`.

### 34.2 Checkpoint, overlay and composite (C)

At the cut CoreGS 2.0 composes a 3D checkpoint through the existing merge kernel (the present target's 640x448
rectangle, internal scale, present resampling) and redirects the rest of the frame into a transparent overlay target;
the pixel stage's split mode writes RGBA8 words whose alpha is the accumulated coverage, with the ROP and compute-fill
paths bypassed. At VBlank `core2_overlay_compose.comp` composes the overlay through the same circuit geometry next to the
checkpoint and `GpuFrame` carries both. The present draws the world and the cars into the checkpoint unchanged, and
`core2_overlay_merge.comp` composites the 2D layer premultiplied source-over before the presenter or the dump reads the
image, after every host-clock tick's base restore. `GT4_COREGS3_2D_OVERLAY=0` is the A/B; the faithful modes never
enable the split. A 2D batch that samples the split framebuffer stays in the 3D target and is counted.

### 34.3 Measurements

A-Spec night race, 3x, VBlanks 7600-7608, live world, final integrated executable below: 1,564 frames split, 66,571
batches redirected (42.5/frame), 1,564 checkpoints, 9 overlay composes and 9 composites (23.22 Mpx), 0 refused samples,
0 refused frames, 0 unavailable draws, 0 approximated DATE/FBMSK/destination-alpha draws, 1 dropped frame (a held
VBlank). Frame 7600's overlay covers 301,208 of 2,580,480 px (11.67%). The picture shows the world (native course, cars,
sky, buildings) with the full A-Spec HUD over it; the A/B shows the world covering it. The split base is one stream frame
newer than the faithful compose (the GS scanout register flips after the VBlank); aligning that phase needs the pair
ping-ponged across two VBlanks and stays open (H).

### 34.4 Gates

Merged-tree executable SHA-256 `8b7d80a3...` (every edited source predates it; the merged tree builds clean).

| Gate | Result |
|---|---|
| CTest coregs3 | **21/21 passed** (new `native_coregs3_2d_split`) |
| Python `test_coregs3_units.py` / full suite | **18/18** / **820/820** (63 skipped) |
| No-present-layer CoreGS 2.0 path vs the `43039a19` tool | **9/9 frames byte-identical** |
| 3x off/shadow identity gate (`coregs3-night-check.sh`, clean rebuild) | **COREGS3-NIGHT-CHECK-OK**, 8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences, 13/13 frames, capture 4,496,552 B |
| Overlay A/B (`GT4_COREGS3_2D_OVERLAY=0`) | within the present tool's own cross-run nondeterminism, same claimed frames 7601-7607 |
| Split counters (A-Spec, 3x, 7600-7608) | 1,564 frames, 66,571 batches, 9 overlay composes + 9 composites, 0 refused samples, 0 refused frames, 1 dropped frame |

### 34.5 The 3D replace (C, owner report 2026-11-11 08:08)

The owner's photo of the split frame showed duplicated 3D (ghosting): the present drew the native world and cars over
the checkpoint, which still held the game's 3D from the CoreGS 2.0 compose, so both copies were visible. With the split
on and the live world drawing, CoreGS 2.0 now composes the checkpoint as the display clear colour alone (no circuit;
the guest's 3D still runs and reaches the present target, it is not shown) and the present layer draws the native sky
and far underlay before the course and the cars. `GT4_COREGS3_REPLACE_3D=0` keeps the base's 3D for the A/B and an
explicit `GT4_COREGS3_WORLD_BACKGROUND=0` keeps the native sky/far off; the present layer owns the decision and passes
it to core 2 with `SetCoregs3Overlay(enabled, replace3d)`. The 2D overlay redirect and composite are unchanged.

Measured (A-Spec, 3x, VBlanks 7600-7608, same claimed frames, clean rebuild `c70afbcb...`): `replace3d=1564` of 1,564
split frames, counters otherwise unchanged (66,571 batches, 9 overlay composes + 9 composites, 0 refused samples, 0
refused frames, 1 dropped frame). Old default vs `REPLACE_3D=0` 15.6k-219.9k px (the tool's cross-run nondeterminism, the same envelope as two
baseline runs); `REPLACE_3D=0` vs the new default 932k-1.04M px; the HUD region unchanged (bottom HUD box max delta 52). The faithful
CoreGS 2.0 path with no present layer is byte-identical to the 43039a19 tool (9/9 frames); CTest coregs3 21/21, Python
18/18 on the module and 820/820 on the full suite (63 skipped). Limits: with the sky/far
underlay on the 2048x2048 texture atlas is full (`atlas_refused=2679`, `texture_untextured=2777` on the frame), so the
sky and some far geometry use vertex colours (darker than the reference) and items the world does not draw (the big
course banner) are absent; world-materials follow-ups, not duplication. Before/after images:
`build/evidence-2d-split/analysis/ghost-07600-*`.

## 35. Round 33 (2026-11-11, branch `task/090-hud-aspect`): the native HUD anchored per display aspect (TASK-214)

Continuation of §34 in a branch from `task/090-coregs3` at `34c6c2c2`. The offline native HUD drew the captured
640x448 elements at their captured positions; this round makes it aspect-aware without stretching the UI. The measured
elements and the reference layout are in the 2026-11-11 journal ("The native HUD anchored per display aspect").

### 35.1 The rule (C)

The TASK-080 HUD canvas is 480 units high and 480*R wide at display aspect R (`l/r = 320 ∓ 240·R`, the ultrawide study
§3.3). The capture is the game's own wide 16:9 layout stored in the 640x448 scanout. Each element keeps its measured
anchor when R changes: the position/lap block and the minimap their distance from the left edge, the Best/Last Lap
times their distance from the right edge, and the central cluster (lap timer, message, gauges, gear, speed, trip,
G-force) their offset from the centre. The element keeps its design size (vertical scale = output height / 480; the UI
is never stretched), which is the "HUD at screen edges" generalisation of `wideOffset` the study recommends. At 16:9 the
mapping reduces exactly to the captured one.

### 35.2 Implementation (C)

`native/coregs3/coregs3_hud.{hpp,cpp}`: `HudAnchor`, `hud_classify_anchor` (the A-Spec page zones: 0x3C20..0x3C80 and
0x3B60..0x3C00 left; page 0x3000 split at x 500 into the centre timer/message and the right Best/Last; the geometric
fallback at the game's 96 px wideOffset distance), `HudAspectLayout` (the virtual canvas, `scale_x = W/(480R)`,
`scale_y = H/480` and the anchor translation) and the per-draw translation in `draw_hud_panel`, applied to the coverage
box, the scissor and the UV/sample point (the ratio is unchanged). The `speed` element is right-anchored and the `tach`
element centre-anchored by their measured positions; `all` uses the page/position rule. The tool gained
`--hud-aspect auto|W:H|WxH|R` (`auto` = the exact output W/H) and `--hud-anchor-report <csv>`; the report carries the
anchor, captured box, translation and predicted output box per draw. `native_coregs3_hud` pins the layout, the rule,
the 16:9 byte identity, the refusals and the report.

### 35.3 Measurements (C)

A-Spec scene 7600 (stream 7601) and the S3 night capture, `--dither 1`. 16:9 at 1920x1080: the predicted boxes equal
the reference layout (position/lap 65.2..385.5 x 46.0..192.3, Best/Last 1594.5..1788.0 x 102.5..251.9, timer
892.5..1092.8 x 210.9..247.1, tach 1044..1336.5 x 790.7..1015.7, speed plate 1651.5..1836.0 x 910.0..973.0); the
contribution-mask pixels lie inside them (inward deltas up to 7 px where the content sits inside its plate/quad).
3440x1440 with its exact 2.3889:1 aspect (virtual canvas 860 units, scale 4 x 1440/448): position/lap 87..514,
minimap 102..642, timer 1630..1897, Best/Last 3006..3264, tach 1832..2222, speed plate 3082..3328 x; against the old
captured stretch the anchor shift is −30..−177 px for the left block and the tach, +149/+60 px for Best/Last and
+123/+39 px for the speed plate, and the old stretch's gauges are 33% wider (ellipses) where the anchored dials are
circular. The nominal 21:9 ratio (2.3333) at the same output would stretch the UI by 2.4%; `auto` avoids it.

### 35.4 Gates

Candidate `gt4vulkan.exe` SHA-256 `19b0b8b3...` (the gt4vulkan object directory and executable deleted and rebuilt
after the last edited source; the HUD change itself is in the offline tool and the library, which gt4vulkan links).

| Gate | Result |
|---|---|
| CTest coregs3 | **21/21 passed** (the HUD fixture gained the anchor cases) |
| Python `test_coregs3_units.py` / full suite | **18/18** / **820/820** (85 skipped) |
| Captured-aspect HUD A/B vs the `34c6c2c2` tool | **7/7 images byte-identical** (all/tach 1x+3x, speed 1x+3x, world-only 3x) |
| 16:9 anchored path vs the unanchored command | **byte-identical** at 1920x1080 and 1920x1344 |
| Anchor images and measurements | `build/evidence-hud-aspect/` (16:9 1080p and 3440x1440 renders, masks, anchor CSV, crops, contact sheets, metrics JSON) |
| 3x off/shadow identity gate (`coregs3-night-check.sh`, clean rebuild) | **COREGS3-NIGHT-CHECK-OK**: 8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences for off and shadow, 13/13 frames identical, capture 4,496,552 B |

### 35.5 What the live path still needs (H)

The live `GT4_COREGS3=present` layer composites the CoreGS 2.0 2D overlay, which is the game's own HUD at its forced
16:9; it needs no anchoring and is unchanged. The native anchors matter when CoreGS 3.0 draws the HUD itself: the live
native HUD must tag each element's anchor at construction from the game's display objects (the capture carries no object
id, so the offline tool uses the measured page/position rule), then feed the same `HudAspectLayout` at the chosen output
resolution. Narrower aspects need the game's Vert+ HUD, which the 16:9 capture does not contain (refused by name). A
21:9 ground-truth capture would upgrade the 3440x1440 visual validation from High confidence to Confirmed.

## 36. Round 34 (2026-11-11, branch `task/090-live-world`): the captured post chain on the live pixels

The live present draws after CoreGS 2.0's compose, so its fragments skipped the game's post operations. This round
implements them on the pixels the pass owns and measures whether they are the missing level. Design:

- `hw_coregs3_post.hpp` packs one push word: output size (post.x), native size and flags (post.y bits 0-10 / 11-21 /
  22-27), DIMX low and high (post.z/post.w). `coregs3_post.glsl` has the epilogue both fragment stages call:
  `post_multiply` (`(c*FIX)>>7`, clamp, the offline `apply_full_screen_multiply`), `post_dither_entry` (the native 4x4
  cell at any output size; `gs_dither_entry`'s signed -4..3), `post_ct16_channel` (COLCLAMP, five bits, `<< 3`) and
  `post_epilogue`. The multiplies follow `GT4_COREGS3_POST`, the CT16 model `GT4_COREGS3_DITHER`, orthogonally.
- The frame environment is read from the renderer's front end and display layout at the VBlank flush
  (`Renderer::Coregs3FrameEnv`, `live_world::note_frame_env`) and consumed by the present's `update_post_words`; a
  faithful request with no environment is counted (`post_env_missing`) and falls back to the 8-bit output.
- The car fragment stage includes the same push block and epilogue, so the car pass and the world stay consistent.

Measured A/B (frame-locked 7396..7401, the r8 reference 07400, world mask 2,529,817 px):

| variant | luma | mean abs | PSNR | changed px vs chain-off |
|---|---|---|---|---|
| chain off (default) | 24.58 | 18.69 | 17.36 dB | 0 |
| FIX 127 only | 23.59 | 18.95 | 17.37 dB | 2,450,168 |
| FIX 255 only | 46.28 | 28.27 | 14.51 dB | 2,439,812 |
| both, 8-bit | 44.40 | 27.05 | 14.60 dB | 2,450,718 |
| both + CT16 faithful | 40.44 | 24.86 | 14.86 dB | 2,443,991 |

The reference luma on the mask is 28.54. HUD-free crops (reference / base / chain-off / both): road 29.59 / 28.08 /
24.21 / 45.31; left wall 26.68 / 25.30 / 33.11 / 50.86; right wall 19.79 / 19.12 / 14.72 / 26.78. The base matches the
reference (0.95-0.97x) while the reconstructed world is 0.74-1.24x depending on the surface, so the captured 2x brighten
overshoots; the offline chain overshoots too (luma 34.62 against 26.69, and its CT16 truncation brings the final to
29.85). **The measured right operation is the chain off by default**; `GT4_COREGS3_POST=1` exposes it with
`GT4_COREGS3_POST_FIX127=0` / `_FIX255=0` for the individual multiplies, and the default image reproduces the recorded
picture (24.578 / 18.758 / 17.346 dB). The remaining level difference is the material/lighting residual, not a missing
post op; no base-side application is needed (the base already carries the chain). Checks: CTest coregs3 20/20 (the new
live-post arithmetic in `native_coregs3_wet_composite`), Python 18/18, the 3x off/shadow gate COREGS3-NIGHT-CHECK-OK
(8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences, 13/13 frames, capture 4,496,552 B) on the clean rebuild (`e0898dc0edb12601e92c179da84f8abdd8003aed684b10248576b08767155132`). Evidence `build/evidence-live-world/` (`night-post-*`,
`post-ab.json`, `post_metrics.py`, `post_ab.sh`).

## 37. Round 35 (2026-11-11, branch `task/090-live-world`): the live world's per-item VU1 lighting

`build_world` took the capture's decoded base colour (`MeshVertex::color`) and never ran the measured VU1 lighting
programs; the offline renderer shades every draw with `coregs3_lighting.hpp`'s `shade_mesh_items` (round 13/14 formulas,
the item's own ITOP selector and TOPS+0xC9 block). The live build now makes the same call after `build_mesh`, before the
range loop, and the fragment stage MODULATEs by the lit colour. `GT4_COREGS3_WORLD_LIGHT=0` keeps the base colours (the
A/B); fallback items keep the base colour and are counted. The F8 world line gains `light_draws_lit`,
`light_draws_fallback`, `light_vertices_lit`, `light_vertices_fallback`.

Measured (night race, 3x, VBlank 7399 against the r8 reference 07400, the 2,504,546-px world mask):

| variant | luma | mean abs | PSNR | changed px vs base colours |
|---|---|---|---|---|
| base colours (A/B) | 24.67 | 18.85 | 17.34 dB | 0 |
| lit (default) | 22.86 | 17.33 | 18.55 dB | 94,269 (64,009 closer / 32,005 farther) |
| CoreGS 2.0 base | 27.43 | 3.84 | 30.15 dB | — |

Crops (base colours -> lit): left wall 12.57 -> 18.13 dB, road 25.23 -> 25.28, right wall 22.16 -> 22.15, HUD area
14.80 -> 14.83, centre buildings 16.16 -> 16.19. All 520 draws / 256,196 vertices were lit on the frame, 0 fallbacks.
The car pass is untouched.

Split check: with the 2D split on, the night B-Spec scene's 3D image is black (frame luma 2.291, only the HUD) while
`GT4_COREGS3_2D_OVERLAY=0` renders 22.377 and the A-Spec scene renders 28.02 with the split on; the incoming split code
was validated on A-Spec only, and the finding is reported to the split owner. Checks: CTest coregs3 21/21, Python 18/18,
the 3x off/shadow gate COREGS3-NIGHT-CHECK-OK (8,997 VBlanks, 1,686 racing, 0 clock/digest/state differences, 13/13 frames, capture 4,496,552 B), clean rebuild (`4c1cb1ee1e9f8cb1cc97d9e52d67a4c85a5884db981690a47f8760b555c4f7c7`). Evidence
`build/evidence-live-world/` (`night-shade-*`, `shade-ab*.log`, `shade-frame-7399.png`).

### 34.6 The B-Spec black 3D and the world-shading integration (C)

The live-world round found the 2D split (default) blacked out the night B-Spec race: frame luma 2.291 with only the HUD,
against 22.377 with `GT4_COREGS3_2D_OVERLAY=0`; the A-Spec scene rendered 28.02. Cause: after the present blit and the
HUD, the B-Spec panel draws a **full-screen unblended alpha-only sprite into the display buffer** (PRIM 0x006,
untextured, TEST 0, FRAME.FBMSK `0x00FFFFFF`, TBP0 `0x3C00`, extent 0..639 x 0..447) plus six small alpha-only sprites.
On the GS these write only the CT16 alpha lane; the split's fragment stage ignored FBMSK and wrote the draw's black RGB
with coverage 255 into the RGBA8 overlay, and the composite covered the world. Elements drawn after it survived, which
is the reported picture. Fix: FBMSK maps onto the overlay word's RGB lanes, and a draw that protects every RGB lane is
dropped entirely (no visible content; the overlay's alpha is the layer's coverage, not the GS alpha lane). Counters
`fbmsk_masked` and `alpha_only_skipped`; the A-Spec panel has 0/0.

Measured on the final merged executable `624b2233...` (3x): night B-Spec VBlanks 7398-7401 split on 24.50 luma (native
world + HUD), split off 22.36, split with `REPLACE_3D=0` 24.17; counters over the run 1,357 frames, 48,721 batches,
1,357 checkpoints, 0 refused samples/frames, `fbmsk_masked=3640`, `alpha_only_skipped=909`. A-Spec VBlanks 7600-7608
unchanged: 1,564 frames, 66,571 batches, 0 refusals, 0/0, luma 26.59 vs 26.10 split off. The faithful CoreGS 2.0 path
with no present layer is byte-identical to the 43039a19 tool (9/9 frames). `task/090-coregs3` at `fb59d776` (the world
shading round on top of the post chain, dither, picture round, HUD aspect and mip recapture) is merged into the split
branch; the 2D split, the 3D replace (default on with split+world), the post chain (off by default), the dither option,
the picture round, the mip path, the HUD-aspect work and the per-item VU1 lighting coexist. The only code conflict was
the present layer's init log line, which now reports all switches together; the documentation conflicts keep both
sides' entries (this round's §34.6 follows the 3D replace's §34.5; §35-37 are the incoming rounds). CTest coregs3 21/21, Python 18/18 on the module and
820/820 on the full suite (63 skipped), 3x off/shadow gate COREGS3-NIGHT-CHECK-OK (8,997 VBlanks, 1,686 racing, 0
differences, 13/13 frames). At frame 7400 the fixed split differs from the split-off image on 817,717 px (31.69%) and
from the pre-fix black frame on 2,254,584 px (87.37%). The first merged-tree gate run reported the documented 512-word
race-object state-dump artifact (off vs ref; the ref and shadow dumps byte-identical, the off dump holding the 0xDD
pattern at word 17328, with 0 clock/digest differences and 13/13 frames identical); a clean serial rerun reports
COREGS3-NIGHT-CHECK-OK. The artifact is the one recorded in §33.6.
