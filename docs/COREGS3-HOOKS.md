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
`value` (≈ 1/4096 per car) and row 3 (`vf27`) is **not** scaled: in the observed car frames it holds a world position
(~`18.6, 86.1, 138.4`), not a projection row. The `+0xFA0` block is not a projection matrix in these frames either (it
holds screen-centre-like 2D state), so its role stays **U**. The VU1 vertex formula (which register is the projection, and
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
# milestone sheet (test tool, Pillow)
python scripts/coregs3_bench/coregs3_sheet.py out.png --title "..." <ref.png> "CoreGS 2.0" <native.png> "native"
```
