# CoreGS 3.0: the bounded GS-stream capture (TASK-090)

Date: 2026-11-10. Branch `task/090-gs-stream-study` (worktree `O:/Antigavity/gt4-wt-090-gs-stream`, based on
`task/090-coregs3` at `248ec453`). Read-only study on `native/`: this document proposes a design and does not change
code (S1 was implemented afterwards; see the status section below).

Clean room: no PCSX2 source, no `gt4recomp-gs`, no `private/tooling/pcsx2-*` and no `private/slice189-pcsx2-ref` was
opened. The references are CoreGS 2.0 (`native/hw_gs`), the donor runtime (MIT, `private/references/ps2-web-recomp`),
the existing CoreGS 3.0 capture (`native/coregs3`) and the repository studies.

Labels: **C** = Confirmed (read in code or measured), **H** = High confidence, **Hyp** = Hypothesis, **U** = Unknown.

## Status (2026-11-10): S3 implemented and measured; the speed plate is drawn, the digit atlas is outside the capture window

S3 landed on branch `task/090-hud-s3` (worktree `O:/Antigavity/gt4-wt-090-hud-s3`, from `task/090-coregs3` at
`008a71c8`). The first native HUD/2D layer is `native/coregs3/coregs3_hud.{hpp,cpp}` plus the `native_coregs3_hud`
fixture and the `gt4_coregs3_render --hud` options. The measured element is the speed element of the race panel, and the
panel is **mixed**:

- **PATH2 (the `.gt4gs` stream):** the two digit sprites of scene frame 7398 (stream 7399) at seq 1102/1103, PRIM
  0x15E (sprite, TME+ABE), TEX0 TBP0 0x3A20 T4 128x64, CBP 0x3A27, CLAMP 0..127 x 0..38, TEX1 0x60 (linear/linear,
  MXL 0), ALPHA 0x44 (normal), at x 567.75..578.25 and 577.5..588.0, y 382.5..400.5, UV cells (91..105, 1..19) and
  (61..75, 1..19).
- **PATH1 (the `.gt4tap`):** the plate, job 460652, a 4-vertex tri-strip with ST 0..0.641 x 0..0.812 at
  x 550.5..612.0, y 377.5..403.625, T8 TBP0 0x3A00 CBP 0x3A08 CLAMP 0..81 x 0..25, FRAME 0x020A008C (CT16 display
  buffer), PRIM 0x05C (tri-strip, TME+ABE), ALPHA 0x44, TEST 0x3840C (ATE off, ZTST always).
- **The merged register file (C, measured).** The PATH2-only walker sees SCISSOR 0..639 x 45..76 at the digit draws
  (the position element's write, seq 1095); under that rectangle the digits are culled. The real GS order also carries
  the plate's VU1 job (job 460652 < the digits' last_job 460653), which writes SCISSOR 0..639 x 0..447; merging the tap
  writes by job id restores the full-screen rectangle. The panel builder counts the sprites the PATH2-only state would
  have culled (**2** on this frame). The same merge explains why the digits are visible in the CoreGS 2.0 reference.
- **Textures (C, captured).** The frame's own PATH3 IMAGE uploads reconstruct the GS local memory: **173 uploads,
  3,800,552 B, 0 refused** (host->local, BITBLTBUF/TRXPOS/TRXREG, T4/T8/CT32, the split packets handled). The plate page
  0x3A00 is uploaded by seq 1007 (T8 88x26, 2288 B) and its CT32 16x16 palette by seq 1009; the decoded texture is the
  exact `remote_speed_kmph` plate (the "km/h" text is readable), matching the disc entry's TEX0/CLAMP. The gear page
  0x3C80 decodes to "1 2 3 4 5 [car icon] P". **The digit glyph atlas page (T4 128x64 at 0x3A20) is not uploaded
  inside the window**: its per-frame uploads are the CT32 palette banks (0x3A20 32x16, 0x3A28 16x16, 0x3A2C 16x8,
  0x3A2E 8x8) and the small element atlases, not the font texels, which were uploaded before the capture began. The
  run's own `.gt4vram` sidecar (one vblank record at the previous rotation, worker VBlank 7398) and the cross-run
  pass-textures sidecar do not match the 7399 rotation; the font is not recoverable from the captured bytes. The
  stream-only image holds the page as zero fill, so the raster refuses the two digit draws by name ("texture page holds
  a constant value (the font atlas is uploaded before the captured window)") and counts them; no solid box is drawn.
- **Raster (C).** Sprite/strip coverage by pixel centres (the CoreGS 2.0 rule), MODULATE (`Ct*Cv >> 7`), the ALPHA
  normal blend `(Cs-Cd)*As+Cd`, TEST ATE, and the CT16 display-buffer model: the destination is read as its 5-bit value
  shifted left by 3 and the result stored the same way (the reference frame's centre samples are exactly the CT16 5-bit
  values `<< 3`, verified on frame 07400).
- **Gates.** CTest coregs3 **17/17** (new `native_coregs3_hud`), Python **14/14**; the world `--no-hud` render is
  **byte-identical** to the `008a71c8` executable on the same capture/command (PNG and report SHA-256 equal); the 3x
  off/shadow gate is **COREGS3-NIGHT-CHECK-OK** (8,997 VBlanks, 1,686 racing, 0 differences, 13/13 frames). At 1x
  against frame 07400 the plate mask is 1586 px at bbox [551,378..611,403]; the plate background (excluding the digit
  and km/h rects) is **PSNR 28.58, mean |diff| 6.37, luma 11.87 vs 6.11**; the full element mask is PSNR 13.35 /
  SSIM 0.486 because the digits are missing. **G-HUD-VALUES is open** until the font atlas is captured; G-HUD-ASPECT is
  documented as one aspect only (the bench forces widescreen, 640x448 anamorphic). Evidence and commands:
  `build/evidence-hud-s3/`.
- **Next.** Capture the font atlas (a boot/race-entry `.gt4gs` window, or the `race.fnt` decoder) and then extend to the
  remaining elements (position/lap/time, gear, fuel/tyres, tachometer, mini-map, car icon, messages/countdown, command
  bar), which the captures already carry as PATH2 sprite sets and whose background plates are the tap's other
  display-buffer tri-strips (140 PATH1 draws per frame in the window).

## Status (2026-11-10): S2 implemented and measured; the wet-road composite explains 0 of the additive term

S2 landed on branch `task/090-gs-post-layer` (worktree `O:/Antigavity/gt4-wt-090-gs-post-layer`, from `task/090-coregs3`
at `aa5816ac`). The offline decoder/census (`native/coregs3/coregs3_gsdecode.{hpp,cpp}`, the tool's
`gt4_coregs3_gs --census [--json]`, the `native_coregs3_gsdecode` fixture) and the wet-road composite
(`coregs3_wet_composite.{hpp,cpp}`, `gt4_coregs3_render --gs`, `--no-wet-composite`, `--post-chain`,
`--composite-world-alpha`, the `native_coregs3_wet_composite` fixture) are implemented and measured on the S1 capture.

- **Attribution used (C):** the S1 phase relation, stream stamped `V` belongs to scene frame `V - 1`. For scene frame 0
  (VBlank 7398) the chain is the stream stamped **7399** (records seq **920/934/936/964**); every other frame 7399-7404
  has the identical chain. No packet is placed by acceptance-time pass (all `unknown`).
- **Census (C):** 4,979 records, 4,771 GIF; **4,915 packets** (PACKED 2,637, REGLIST 896, IMAGE 1,382, disabled 0),
  register-only 2,078, primitive 928, empty 527; **9,192 A+D writes**, 8,792 register writes, 936 PRIM kicks; 928 batches
  (4,096 vertices, 2,048 drawing), **88 full-screen batches**; **1,382 IMAGE uploads / 30,403,968 B**; 1,382 packets
  split across capture records, handled. Named refusals: **0** truncated, 0 disabled-format, 0 unsupported REGLIST
  descriptor, 0 IMAGE without transfer state, 0 vertices without PRIM. Per frame: 610/615 packets, 171-173 uploads,
  11 full-screen batches (the four chain draws plus seven classified others: the frame-sample depth stamp, env-map alpha
  writes, small multiplies and the present blit).
- **The composite is PATH2, not PATH3 (C, measured):** the PATH3 records are only IMAGE uploads and BITBLTBUF/TRXPOS/
  TRXREG/TRXDIR packets; every draw in the capture is PATH2. The chain in stream 7399:
  - alpha clear **seq 920**: PRIM 0x006 (sprite, TME/ABE off), FRAME FBP 210/FBW 10 CT32, FBMSK `0x00FFFFFF`, RGBA
    alpha 0, 20 full-screen strip sprites;
  - **seq 934, the colour composite**: PRIM 0x15E (sprite, TME+ABE), FRAME FBP 210/FBW 10 CT32, FBMSK 0, TEX0 TBP0
    **0x3140 (page 394) CT32 128x128, TCC 1, TFX 0**, TEX1 MMAG/MMIN linear, MXL 0, CLAMP region 0..127, ALPHA `0x58`
    `(Cs - 0)*Ad + Cd`, TEST `0x3820C` (ATE off, ZTST ALWAYS), ZBUF ZMSK 1, 40 vertices / 20 strip sprites with vertex
    RGBA (64,64,64,128) and UVs 0..128.5 texels. It samples the **sphere-mapper source page 394**, not page 210; the
    destination alpha is the mask the reflection pass wrote into page 210 after the clear;
  - post **seq 936** FIX 127 and **seq 964** FIX 255: full-screen sprites, TME off, ABE on, ALPHA `(Cd - 0)*FIX + 0`
    (`Cd*127/128`, `Cd*255/128`);
  - present **seq 1094**: full-screen sample of page 210 into FBP 140/FBW 10 CT16 (the display buffer).
  The source page is the `.gt4vram` job record **label 460559, vblank 7399**, windows `(0x3F80,64)+(0x3FC0,4)+
  (0x3140,256)`, captured **before** that sphere-mapper job executes; the job is posted after seq 934, so this is the
  content the composite reads (the frame-0 `.gt4scene` block is byte-identical between the S1 and pass-textures runs,
  SHA-256 `0c139ab0a439f3bf...`).
- **Measurement (C):** faithful reconstruction over the pass-textures world at 3x: the reflection mask writes 188
  fragments, **70 with non-zero alpha**; the composite covers 2,573,956 pixels, samples all of them with **0 refusals**,
  and **changes 70 pixels**, all in rows 151-154 / columns 1334-1368, i.e. the thin horizon band **outside the compared
  clean mask** (the top HUD rectangle). Added luma over the 1,780,790 compared pixels: **0.000**. Before and after are
  identical: **PSNR 22.3498, SSIM 0.65055, pct_bad 64.776%, mean |diff| 14.84, luma 18.955, fit
  `ref = 0.6341*world + 14.668`**; page `0x36D0`: PSNR 25.169, SSIM 0.64213, luma 16.613, fit `0.2831*world + 19.307`.
  The additive gap is **7.733 luma** (7.397 on the road page); the composite explains **0.000%** of it. The A/B
  `--no-wet-composite` is byte-identical to the baseline (`AB769F0A92BA...`).
- **The uncaptured destination alpha is not a free variable (C):** the diagnostic `--composite-world-alpha` (skip the
  captured clear, use the world alpha as `Ad`) changes 2,543,500 pixels and gives luma **144.98**, PSNR **6.45**, SSIM
  **0.212**, inconsistent with the reference; the captured alpha clear is therefore the right `Ad`, and a road-wide
  mask of this form is excluded.
- **The post chain does not explain the residual either (C):** applying the captured multiplies (FIX 127 then FIX 255)
  gives luma **34.62** against the reference's 26.69, PSNR **20.40**, SSIM **0.6105**, fit `0.3733*world + 13.763`;
  it overshoots every metric. The remaining 7.733 luma gap is therefore not the captured composite and not a simple
  global post multiply; it stays in the material/texture/lighting difference (the pass-textures round's dominant page
  cause) or in a post operation the stream does not carry at this scene (no tone/glare draw; the 2x brighten is the only
  RGB post op and it overshoots).
- **Gates:** CTest coregs3 **16/16** (new `native_coregs3_gsdecode` and `native_coregs3_wet_composite`), Python
  `test_coregs3_units.py` **14/14**, car-only render **byte-identical** to the `aa5816ac` executable (PNG
  `B0E7082B...`, report `4F9F986A...`), `--no-wet-composite` byte-identical to the world baseline, and the 3x off/shadow
  identity gate against the `coregs3-r8/ref` executable is **COREGS3-NIGHT-CHECK-OK** (serial runs and the checks in
  `build/evidence-gs-post-layer/gate-control/gate-serial.log`).

The S2 conclusion changes the study's first milestone: M-POST-1's "apply the captured post pass" cannot be met by the
wet-road composite (it is 70 pixels at the horizon) or by the captured multiplies (they overshoot); the milestone's
`--no-post` A/B and its PSNR/luma targets stay open until the material/texture gap is addressed or a post op that the
PATH2/PATH3 stream does not carry is found. Every number, the census JSON, the masks and the A/B images are under
`build/evidence-gs-post-layer/`.

## Status (2026-11-10): S1 implemented and measured

S1 landed on branch `task/090-gs-capture` (worktree `O:/Antigavity/gt4-wt-090-gs-capture`, from `task/090-coregs3` at
`e529c319`). The `.gt4gs` v1 writer, reader, the `gt4_coregs3_gs` tool (`--stats`, `--list`, `--dump-frame`,
`--check-pass`), the `native_coregs3_gsstream` fixture and the Python control exist, and the night race was captured 3x
over VBlanks 7398-7405 with `.gt4scene`, `.gt4tap` and `.gt4gs`. Measured (method and limits in
[COREGS3-HOOKS](COREGS3-HOOKS.md) §25):

- capture **30,945,504 B**, 4,979 records (4,771 GIF, 192 pass, 16 frame), 8/8 frames closed, **0 refusals**;
  PATH2 **145,536 B / 1,480 records**, PATH3 **30,545,072 B / 3,291 records** (3.82 MB/frame, 3.66 MiB largest frame);
- **G-GS-CAP passes exactly** for both paths (8/8 frames; the F8 report line is `VBlank - 1`), and **G-PASS passes
  8/8** frames with the pass id and 7/7 register words;
- CTest coregs3 **13/13**, Python **14/14**, and the 3x off/shadow identity gate is **COREGS3-NIGHT-CHECK-OK** (the
  capture writes only at shutdown);
- open question 1 is answered (**C**): every PATH2/PATH3 packet is accepted **outside** every pass marker (`pass =
  unknown`, 4,771/4,771), because the frame's GS stream is kicked at the start of the VBlank interval and the K7 markers
  run afterwards; the packet `last_job` ranges are contiguous across frames, so a packet belongs to the stream the
  previous frame's passes built. S2/S3 need that phase relation or a stream-level pass marker before drawing the post or
  HUD layers in pass order.

Evidence directories referenced below that live in other worktrees are named in full (`build/analysis-effects/`,
`build/analysis-hud/`, `build/analysis-particles/` were produced in `O:/Antigavity/gt4-wt-090-coregs3` and
`O:/Antigavity/gt4-wt-090-track-m1`); this worktree does not carry them.

---

## 0. The short answer (for the owner)

The renderer we have captures only what the **VU1** draws (PATH1). Everything the **EE** sends to the GS directly —
the post chain (brightness, colour filter, glare/bloom, crossfade, motion blur) and the HUD — travels on **PATH3**
(immediate mode) and **PATH2** (the VIF1 direct path), and today no capture sees it at all. That is why the effects
and HUD studies stop at "blocked on a bounded GS-stream capture".

This study finds the exact place where all of it passes through one function on one thread
(`gt4::mtvu::gif_transfer`, `native/mtvu_runtime.cpp:728`, plus `shadow_direct` at `:761` for PATH2), proposes a new
bounded sidecar file **`.gt4gs` version 1** (raw PATH2/PATH3 packets, per-pass GS state, pass and VBlank markers,
the last VU1 job for ordering), and a replay path in `gt4_coregs3_render` that draws those layers **over** the native
world in pass order. The first measurable milestone is concrete and already has numbers: the offline world of the
night race is **too bright** (luma 31.20 against the reference's 26.69, PSNR 20.36 dB, `build/evidence-m1/world/
world-metrics.json`), so a captured brightness/tone operation applied to it must move both numbers toward the
reference.

Nothing here was implemented by the study. The plan is phased so that each step has a gate that can fail loudly; S1 is
now implemented and measured (status above, method in [COREGS3-HOOKS](COREGS3-HOOKS.md) §25).

---

## 1. Where the GS stream is assembled in our runtime today

### 1.1 The three GIF paths and who produces each (C)

| Path | Producer in the guest | Runtime entry point | Thread |
|---|---|---|---|
| **PATH3** | EE immediate mode: DMA channel 2 (`dmac::deliver`, donor `dmac.cpp:61`), the GIF FIFO at `0x10006000` (donor `hw.cpp:276`), both through `ps2::gif::transfer(3, …)` (donor `gif.cpp:168`) | patched `gif::transfer` → `gt4::mtvu::gif_transfer` (`native/mtvu_runtime.cpp:728`) when MTVU is on (`scripts/prepare_perf_runtime.py:227`) | EE |
| **PATH2** | VIF1 `DIRECT`/`DIRECTHL` (donor `vif.cpp:256-257`): with MTVU the EE **shadows** VIF1 and calls `gif::scan_signals(2, …)`, patched to `gt4::mtvu::shadow_direct` (`native/mtvu_runtime.cpp:761`); without the shadow the same bytes go through `gif::transfer(2, …)` → `gif_transfer` | `shadow_direct` or `gif_transfer` | EE |
| **PATH1** | VU1 `XGKICK` (donor `vu_micro.cpp:433`) → worker `gt4::mtvu::worker_gif` (`native/mtvu_runtime.cpp:938`), where the VU1 tap records it (`tap_path1`, `:958`) | `worker_gif` | VU1 worker |

Two facts matter for the capture point:

- **With MTVU on (the default, `Settings::enabled = true` in `native/mtvu_runtime.hpp:18`), every PATH2/PATH3 byte
  passes through exactly one of `gif_transfer` / `shadow_direct`, on the EE thread.** PATH2 is never seen twice:
  when VIF1 is shadowed the donor's `gif::transfer` is not called for it at all (`vif.cpp:256` chooses
  `scan_signals` instead), and the worker's own copy is emitted later through `worker_gif`, which is a different
  function.
- **PATH2 on the worker is the same packet the EE already scanned** (`shadow_direct`), so recording at the EE
  acceptance point is enough and recording in `worker_gif` would double-count.

`gif_transfer` is also where the packet's SIGNAL/FINISH/LABEL writes are applied early (`apply_accepted_signals`,
`native/mtvu_runtime.cpp:709`) and where the packet is either rendered immediately (`r.donor.render_gs`, `:733/746/753`)
or appended to the ordered journal (`EeJournal::append_ee_gif`, `native/mtvu_journal.cpp:153`). The acceptance order
therefore **is** the program order, and the journal preserves it against the worker's PATH1/PATH2 output.

### 1.2 The GS consumers (C)

The bytes above are handed to one of three consumers, all through `gt4::gs_stream::on_gif_render`
(`native/gs_stream/gs_stream_client.cpp:1297`):

1. **CoreGS 2.0 in-process** (`GT4_RENDERER=hw`, `GT4_HW_GS=ON`): `InProcessSink::gif`
   (`native/gs_stream/gs_stream_sink.hpp:42`) → `hw_gs_stream_sink.cpp:895 gif` (GS worker ring, `:884`) →
   `Backend::gif_transfer` (`native/hw_gs/hw_gs_backend.cpp:1914`) → `core2::Renderer::GifTransfer`
   (`native/hw_gs/core2/core2_renderer.cpp:5517`) → `hwgs::FrontEnd::GifTransfer`
   (`native/hw_gs/frontend/hw_front_end.cpp:192`), which parses GIF tags into register writes and vertex kicks and
   emits `Batch`es (`native/hw_gs/frontend/hw_batch.hpp:94`) to the write-back renderer.
2. **The separate `gt4-gs.exe` process** (decision 0070): `Client::gif` (`gs_stream_client.cpp:310`) splits the
   bytes into fixed-size records (`kGifChunkBytes`) of the GS stream protocol.
3. **The donor renderer** (CoreGS 1.1 / `GT4_RENDERER=donor`): `ps2::gif::render` (`gif.cpp:163`) walks the packet
   with `GsSink` (`gif.cpp:75`) into the donor's `gs::write_packed`/`gs::write_image`.

The capture must not depend on which consumer runs: this is one reason to record at the **EE acceptance point**, not
in the renderer.

### 1.3 VBlank and pass boundaries (C)

- **Host VBlank:** `window.cpp:662 on_vblank()` increments `input_vblank` and calls `gt4::coregs3::at_vblank`
  (`window.cpp:670`; the hook is `native/coregs3/coregs3_hooks.cpp:472`), then `ps2::gs::present()`.
- **MTVU VBlank:** `gt4::mtvu::vblank_sync` (`native/mtvu_runtime.cpp:891`) drains the journal and (in pcsx2 mode)
  posts the held VSYNC after the frame's output; the GS worker's `vsync_now`
  (`native/hw_gs/hw_gs_stream_sink.cpp:484`) calls `Backend::flush()` → `core2::Renderer::Flush`
  (`core2_renderer.cpp:5653`) → `FrontEnd::VBlank` (`hw_front_end.cpp:1245`). The CoreGS 2.0 SLM sidecar is written
  there (`core2_renderer.cpp:5661`).
- **Pass boundaries:** the 13 K7 markers inserted by `scripts/coregs3_units.py:169-195` call
  `coregs3::pass_begin`/`pass_end` (`coregs3_hooks.cpp:488/511`), which keep `g.current_pass` and push one
  `CapturedPassState` into the v5 journal (`coregs3_scene.hpp:345`). The car pass also flushes the VIF chain and
  records the worker job range (`car_pass_begin/end`, `coregs3_hooks.cpp:526/548`).

### 1.4 Where the bounded capture hook should sit

**Recommended (C for the call sites; the design is this study's proposal):**

```
native/mtvu_runtime.cpp
  gif_transfer()   :728   -> gs_stream_packet(path, data, bytes)   for path 3, and for path 2 when it arrives here
  shadow_direct()  :761   -> gs_stream_packet(path, data, bytes)   for path 2 (VIF1 shadowed, the MTVU default)
native/coregs3/coregs3_hooks.cpp
  pass_begin()     :488   -> gs_stream_pass(pass, begin)
  pass_end()       :511   -> gs_stream_pass(pass, end)
  at_vblank()      :472   -> gs_stream_frame(vblank)
```

with the recorder itself in a new `native/coregs3/coregs3_gsstream.{hpp,cpp}`, compiled into `gt4_mtvu_runtime` next
to the tap (`native/CMakeLists.txt:465`), and the reader next to `coregs3_tap_file.*` in `gt4_coregs3`
(`native/CMakeLists.txt:487-492`). The writer encodes its own header and records inline, the same split the tap
already uses (writer `coregs3_tap.cpp` in the runtime, reader `coregs3_tap_file.cpp` in the library); the fixture
pins that the two agree, because neither library links the other.

Why here:

- **One writer thread.** Every entry point above runs on the EE thread (`gif_transfer` and `shadow_direct` are EE
  only; `pass_*`/`at_vblank` are hooked guest calls), so the recorder needs no lock and no ordering guesswork.
- **The guest source site is available.** `ps2::g_cpu` (donor `system.cpp:37`, declared in
  `ps2/system.hpp:16`) holds `pc` and `r[31]` (`ra`) at the transfer; the v5 capture already reads guest `ra` the same
  way for `caller_ra` (`coregs3_hooks.cpp:616`). What exactly the two words name (the pgl builder or the DMA kick
  site) is open question 5.
- **Before the journal copy.** The bytes are recorded from the guest buffer the donor already has, so the sidecar is
  independent of the journal's chunking and of whether the packet is rendered now or later.
- **Renderer-independent.** It works with CoreGS 2.0 in-process, with `gt4-gs.exe` and with the donor, which is what
  a capture used as the *reference* needs.
- **Cheap when off.** Every entry point is one predictable branch; there is no allocation, no file I/O and no
  mutex on the guest path, exactly like `tap_path1` (`coregs3_tap.cpp:78`).

**Rejected alternatives:**

| Candidate | Why not |
|---|---|
| `ps2::gif::render` / `gt4::mtvu::note_render` (`mtvu_runtime.cpp:779`) | runs at render time, after the journal; the guest `ra` no longer belongs to the producer, and the hook is inside the patched donor source (a new runtime pin per change) |
| `hwgs::FrontEnd::GifTransfer` (`hw_front_end.cpp:192`) | GS worker thread, `GT4_HW_GS` builds only, sees the stream only after the journal and the transport ring |
| `InProcessSink::gif` / `Client::gif` | split per consumer (in-process vs process), and the external process path writes protocol records, not raw packets |
| The donor `GsSink` (`gif.cpp:75`) | only exists when the donor renderer draws; the capture must run with the CoreGS 2.0 reference |

**Known limitation (C):** with `GT4_MTVU=0` the patched `gif::transfer` never calls `gif_transfer`, so the capture
(and the VU1 tap) does not run. The capture therefore requires the MTVU path, which is the default and what every
gate uses. A one-line capture call in the `gif.cpp` patch would cover the single-thread A/B, at the cost of a new
runtime pin; it is not proposed for v1.

### 1.5 Why the hook cannot change guest behavior

The rules the existing observe-only hooks follow (`docs/COREGS3-HOOKS.md` §3, `coregs3_hooks.hpp:1-8`) apply
unchanged: read guest memory, never write it, never call guest code, never touch the cycle budget. Three extra rules
come from the measured lesson of round 2 (`docs/COREGS3-HOOKS.md` §7): **no file I/O on the guest thread**, **no
allocation in the hot path**, and **no lock shared with the guest**. The design meets all three: a preallocated
byte ring filled at acceptance and written to disk at shutdown, like the `.gt4scene` capture
(`coregs3_hooks.cpp:457 shutdown`) and the `.gt4vram` sidecar (`core2_renderer.cpp:153 Write`).

The identity control is the existing 3x gate: `scripts/coregs3_bench/coregs3-night-check.sh` compares
`GT4_COREGS3=off` and `GT4_COREGS3=shadow` against a plain reference in clock, per-VBlank simulation digest,
`state.bin` and 13 frames. A capture that changes any byte of guest behavior fails it.

---

## 2. What the capture records

### 2.1 Packet records (PATH2 and PATH3)

Per accepted GIF transfer: the path, the byte count, the raw bytes (16-byte aligned, as they are), and the join
fields below. The payload is the packet exactly as the GS will parse it, so it carries the register writes, the
vertex kicks and the image-transfer data (TEX0/TEX1/CLAMP/TEX2, FRAME/ZBUF/SCISSOR/ALPHA/TEST, BITBLTBUF/TRXPOS/
TRXREG/TRXDIR and the image bytes) without a second description.

Join fields, one per record:

| Field | Source | Why |
|---|---|---|
| `vblank` | `coregs3` VBlank counter (`g.vblank`, set in `at_vblank`) | the same counter the VU1 tap stores (`coregs3_tap.cpp:130`) and the capture stamps on a frame (`frame.vblank`); a record belongs to the frame with **the same** `vblank` (the tap pairing already uses that rule, `coregs3_texture_frame.hpp:24`) |
| `pass` | the pass the recorder tracks from the K7 `pass_marker` calls (the value of `g.current_pass` in `coregs3_hooks.cpp`) | ties each packet to a `PassId` (`coregs3_scene.hpp:78`): `backdrop`, `post`, `panel`, …; a packet outside every marker is `unknown` and counted, never guessed (the v5 rule) |
| `sequence` | monotonic per file | the acceptance order within a VBlank, which is the program order |
| `last_job` | `gt4::mtvu::last_posted_job()` (`mtvu_runtime.cpp:678`) | the last VU1 worker job posted when the packet was accepted; the exact merge point against the tap's PATH1 records (job ids), so the replay can interleave PATH1 and PATH3 in stream order |
| `source_pc`, `source_ra` | `ps2::g_cpu.pc`, `g_cpu.r[31]` | the guest site that kicked the transfer, evidence only (see §6, open question 5) |

The recorder keeps a per-frame byte counter and a per-pass byte/record counter for the report (the same shape as
`CaptureCounters::pass_draws`, `coregs3_scene.hpp:440`).

### 2.2 GS register state at pass begins

The v5 journal already defines the rule: seven PGL register mirrors copied at each pass begin
(`coregs3_scene.hpp:335-341`, `capture_gs_shadow` at `coregs3_hooks.cpp:160`), confirmed against the tap on 277 draws
(`docs/COREGS3-HOOKS.md` §17.4):

| Register | PGL offset (from `0x70002000`) | Exact against the tap where the draw does not rewrite it |
|---|---|---|
| FOGCOL | `0x138` | 220/253 |
| FRAME | `0x170` | 277/277 low 32 bits, 240 full 64 |
| XYOFFSET | `0x178` | 162/277 |
| ZBUF | `0x188` | 26/277 (nearly every draw rewrites it) |
| SCISSOR | `0x190` | 251/253 |
| ALPHA | `0x198` | 244/277 |
| TEST | `0x1A0` | 176/277 |

The `.gt4gs` stores the same seven words at every `pass_begin`/`pass_end` record, from the same function
(`capture_gs_shadow`), so the two files agree by construction. It also stores the two extra words that the 2D work
needs and the v5 journal does not carry: `CLAMP` (`0x1A8`) and `TEX1` (`0x1B8`), both named in §17.4 as the
neighbouring measured mirrors. They go into the pass record as raw words; interpretation belongs to the reader.

**Optional exactness level (H, proposed):** the EE shadow is the state *at the marker*, before the pass's first
packet. The measured consequence is the cars pass, whose own XYOFFSET write makes the journal differ from the tap
(5/15 in §17.5). If the replay needs the exact state at the pass's first draw, the EE journal can carry a small
**pass marker record** that the GS consumer executes in stream order (`GsSink` already has `privileged_write`,
`mtvu_journal.hpp:31`, as the precedent for an ordered out-of-band operation); on execution the front end copies its
`RegisterFile` (`hw_front_end.hpp:133`) into the sidecar. This is exact by construction but needs the GS side and
therefore belongs to the milestone that proves the EE shadow is not enough — not to v1.

### 2.3 Texture and CLUT identity

- **The packets are the identity.** Every texture change is a `TEX0`/`TEX1`/`CLAMP`/`TEX2` write inside the captured
  bytes (the track-textures round measured exactly this emission, `docs/journal/2026-11-10.md`, "Track textures"),
  and every upload is an image transfer (`TRXDIR = 0`) with its `BITBLTBUF`/`TRXPOS`/`TRXREG` and payload. The reader
  applies the captured uploads to a local GS memory model and decodes from there, using the same
  `coregs3_texture` decoder and the same CLUT rules the world path already uses.
- **`.gt4vram` stays the fallback** for pixels the capture does not carry (the `GT4_COREGS3_VRAM` sidecar,
  `native/coregs3/coregs3_vram.hpp`, written from the CoreGS 2.0 SLM at each VBlank). The two are complementary:
  `.gt4gs` has the authoritative upload bytes, `.gt4vram` has whatever is resident without an upload in the window.
- **The HUD's assets are small and known:** `display.gpb` is 267,168 B with 72 Tex1 entries (62 T4 + 10 T8, CT16 and
  CT32 palettes, MODULATE, linear, no mips, region clamp) and `race.fnt` is 184,960 B with 8 T4 atlases
  (`build/analysis-hud/display-gpb-us-tex1.txt`, `build/analysis-hud/`; TASKS.md TASK-090 HUD bullet). If GT4
  re-uploads them per frame that is a visible share of PATH3 and must be measured (§6, question 3).

### 2.4 Correlation with the VU1 tap and the `.gt4scene` passes

The three files join on two keys, both already carried by the existing formats:

1. **Frame:** `record.vblank` (this capture) = `tap` record `vblank` = `frame.vblank` (`.gt4scene`). Measured rule
   for the tap, already used by the texture pairing: the PATH1 GIFs of a frame arrive with the frame's own VBlank
   number (`docs/COREGS3-HOOKS.md` §16.2, and the "about three VBlanks later" delivery note). The `.gt4vram` sidecar
   keeps its own rule (worker counter `V` covers frame `V - 1`, `coregs3_vram.hpp:30`), which the tool applies.
2. **Order:** `.gt4gs` records carry `last_job`; tap records carry `job` (`coregs3_tap.hpp:30-32`). A `.gt4gs` record
   with `last_job = J` renders **after** job `J`'s output and before job `J+1`'s. That is the journal's own rule
   (`EeJournal::post_job` appends the placeholder before later EE entries, `mtvu_journal.cpp:134`), so the merge is
   exact without a new mechanism.
3. **Pass:** the pass id is the K7 marker at acceptance. The `.gt4scene` journal's `CapturedPassState` for the same
   `vblank` names the same passes in the same order, so a reader can cross-check the attribution: **the first
   `.gt4gs` pass record of a frame must match the first journal entry of that frame.** This is a cheap automated
   control (S1 gate).

What does **not** need a new coupling: the per-draw `caller_ra` (`coregs3_scene.hpp:268`) and the `.gt4gs`
`source_ra` are different things (a draw's caller versus a packet's kick site); the two never substitute for each
other.

---

## 3. Format proposal: `.gt4gs` version 1, a sidecar

### 3.1 Sidecar, not a `.gt4scene` extension

| Criterion | Sidecar `.gt4gs` | Extending `.gt4scene` |
|---|---|---|
| Size | megabytes per frame (§3.4) | the v5 scene block is 0.8-4.5 MB for a whole capture; the stream would dominate it |
| Producer | EE acceptance points, all paths, independent of the draw hooks | one writer, the K1/K7 hook state |
| Versioning | own magic and version, like `.gt4vram`/`.gt4tap` | every change bumps `kSceneVersion` (`coregs3_scene.hpp:70`) and refuses every existing reader |
| Bounds | its own per-frame/per-file byte caps and refusal counters | would entangle the draw caps (`kMaxDrawsPerFrame`, `kMaxPacketBytesPerFrame`) with byte caps |
| Tooling | `gt4_coregs3_render --gs <file>` next to `--vram`/`--tap` | every tool and fixture that parses v5 would change |
| Failure isolation | the stream can be dropped without losing the draw capture | one overflow would refuse the whole frame block |

Decision: **sidecar**. It follows the pattern already proven twice (`.gt4vram`, `.gt4tap`): magic, version, records
that a reader can skip when it does not know them, and bytes written at shutdown
(`docs/COREGS3-HOOKS.md` §15, §7).

### 3.2 Binary layout (little endian)

```
header (32 bytes)
  u32 magic       'G','T','4','G' in file order (0x47345447)   -- "GT4G"
  u32 version     1
  u32 target      0 (kCaptureTargetV200, coregs3_scene.hpp:71)
  u32 flags       bit0: exact GS state markers present (reserved, 0 in v1)
  u64 first_vblank
  u64 reserved    0; a nonzero value is refused, not guessed (the .gt4vram rule)

records, concatenated, each 16-byte aligned
  u32 bytes       payload byte count, a multiple of 16
  u16 kind        1 gif, 2 pass_begin, 3 pass_end, 4 frame_begin, 5 frame_end, 6 gs_state, 7 privileged, 8 overflow
  u8  path        GIF path 1..3 for kind 1, 0 otherwise
  u8  flags       bit0 payload carries SIGNAL/FINISH/LABEL, bit1 record is a refusal/partial marker
  u32 pass        PassId (0 = unknown)
  u32 sequence    per-file ordinal in acceptance order
  u64 vblank      coregs3 guest VBlank at acceptance
  u64 last_job    last VU1 worker job posted at acceptance
  u32 source_pc   ps2::g_cpu.pc at acceptance
  u32 source_ra   ps2::g_cpu.r[31] at acceptance
  u32 qwc         payload quadwords for kind 1, 0 otherwise
  u32 reserved    0
  payload:
    kind 1  the GIF bytes (qwc * 16)
    kind 2/3  u64 registers[9]: FOGCOL, FRAME, XYOFFSET, ZBUF, SCISSOR, ALPHA, TEST, CLAMP, TEX1, u64 reserved
              (the first seven are exactly CapturedPassState's, coregs3_scene.hpp:353-359; 80 bytes)
    kind 4/5  u32 frame_index, u32 reserved, u64 total_bytes_path2, u64 total_bytes_path3, u64 reserved2 (32 bytes)
    kind 8    u64 bytes_refused, u64 records_refused, u32 cause_bits, u32 reserved, u64 reserved2 (32 bytes)
```

`kind 6` (exact GS state) is reserved for the optional level of §2.2 and is not written by v1.

### 3.3 Bounds, refusals and counters

Same philosophy as the scene capture (`Overflow` flags, `coregs3_scene.hpp:380-388`): a bound is a capacity, and a
frame that would exceed it is refused as a whole and counted, never truncated silently.

| Bound | Default | Behaviour when hit |
|---|---|---|
| `GT4_COREGS3_GS_MAX_BYTES` | 64 MiB | stop recording, write what exists, count `kGsRefusedBytes` |
| `GT4_COREGS3_GS_MAX_FRAME_BYTES` | 8 MiB | drop the rest of that frame's packets, mark the frame block with `kGsRefusedFrame`, keep later frames |
| `GT4_COREGS3_GS_MAX_RECORDS` | 65536 | same as bytes |
| `GT4_COREGS3_GS_VBLANKS` | `GT4_COREGS3_CAPTURE_VBLANKS` | no record outside the window (the tap's rule, `coregs3_tap.cpp:61`) |
| `GT4_COREGS3_GS_MAX_FRAMES` | 8 | stop after N frames |

Counters, printed in the existing `[coregs3]` report (`coregs3_hooks.cpp:785`) and in the F8 `coregs3_capture`
category: records, bytes, bytes/records per path, per pass, frames closed, refused records/bytes/frames, the largest
record and the largest frame.

### 3.4 Capacity estimates per frame

Measured numbers, from code and existing evidence:

| Item | Value | Label / source |
|---|---|---|
| PATH3 bytes/frame (race, GT4Recomp 1.2.0 session) | 1,244,544 (127 records), 1,502,872 (288), 2,132,846 (415), up to 2,910,579 | C: `[frame] gif bytes/frame` in `private/manual-play/session-20261022-163408-808184/probe.log`, `native/perf_probe.cpp:289` |
| PATH3 bytes/frame (plan figure) | about 2.9 MB per 480p race frame | C as stated in `docs/COREGS3-PLAN.md:254` |
| PATH2 bytes/frame | 15,268-46,000 (5-353 records) | C: same F8 lines |
| PATH1 bytes/frame (already in the tap) | 1.4-3.3 MB in the same intervals; 24,811,512 B for 9 VBlanks = 2.76 MB/frame in the gate window | C: F8 lines; tap evidence in `docs/COREGS3-HOOKS.md` §17.5 |
| Tone strips (day race, hypothetical) | ~1,540 sprite batches per VBlank; at ~24 qwords each ≈ 0.6 MB/frame | Hyp, arithmetic from `docs/COREGS3-PLAN.md` §6 ("~1,540 sprite batches") |
| HUD assets, one upload | `display.gpb` 267,168 B + `race.fnt` 184,960 B ≈ 0.44 MB | C: `build/analysis-hud/` (T4/T8 + CT16/CT32 palette transfers, two per texture) |

Conclusion: a window of 8 frames at 1.2-3.0 MB/frame for PATH2+PATH3 is **10-24 MB**, and 64 MiB is a comfortable
bound (about 20 frames at the worst measured rate). The record overhead is 48 bytes per packet, about 5-18 KB/frame
at 100-400 packets/frame (C, arithmetic). The stream is therefore an order of magnitude larger than the `.gt4scene`
record of the same frames but an order of magnitude smaller than the tap (which is 96 MB for 9 VBlanks in the gate,
C).

---

## 4. Replay and offline path

### 4.1 The layer model

`gt4_coregs3_render` gets a new input (`--gs <file>`) and a new phase after the world render:

1. **World** (existing M1 path, `docs/journal/2026-11-10.md` "track M1"): the selected sky/far-underlay/course draws
   into the RGBA8 image and its depth buffer.
2. **2D and post layers** from `.gt4gs`, drawn in **pass order** (`PassId` is already the race order,
   `coregs3_scene.hpp:78-96`): `backdrop` before the course (the full-screen fill), `post` after everything, `panel`
   last.
   Inside a pass, the order is the merged stream order (`.gt4gs` `last_job` against the tap's `job`), so a packet
   that the game submitted after a VU1 job lands after it.
3. **HUD** (`kPassPanel`), the same mechanism as (2) with sprite primitives.

The merge is what makes the replay honest: the post chain reads the frame the VU1 drew *before* the packets that
follow it, and a HUD sprite that overlaps the 3D uses the 3D's depth (the mirror is a 3D view, the HUD is 2D with its
own ZTST).

### 4.2 Register and blend/depth rules

- **Register state per primitive** comes from the packet itself (the front end's own parser order is the model:
  `hwgs::FrontEnd::WriteRegister`, `hw_front_end.cpp:259`), with the pass-begin words (§2.2) as the starting state.
- **Blend/depth rules** reuse the measured subset `coregs3_sampler` already implements
  (`native/coregs3/coregs3_sampler.hpp`): normal and additive `(((A-B)*C)>>7)+D` with COLCLAMP, alpha test with all
  AFAIL modes, FBMSK, ZMSK, ZTST. Anything outside the subset is a counted refusal, never a silent default — the
  rule the reflection and world milestones already follow.
- **Primitive classes:** sprites (2D quads) and points are the two classes the effects/HUD studies measured; the HUD's
  ortho canvas is 640x480 (4:3) or 853.33x480 (16:9) and the P2 hook rewrites `l/r = 320 ∓ 240·R`
  (`docs/journal/2026-11-10.md`, HUD study). The replay applies the captured XYOFFSET/SCISSOR so the canvas mapping
  is the game's, not a chosen one.

### 4.3 Framebuffer-as-texture, history and depth

Three capabilities the 2D/post replay needs beyond the current renderer:

1. **The frame as a texture.** The measured main-frame target is FBP210/FBW10 CT32, the wet reflection FBP210/FBW2,
   the car env map FBP350/FBW4 256 px and the display buffers FBP0/140 CT16 (night race; `build/analysis-effects/`,
   TASKS.md TASK-090 effects bullet). A post packet whose `TEX0.TBP0` names the main frame must sample **the offline
   world image**; the rule is a small table of measured (FBP, PSM) → image, everything else is a counted refusal. This
   is how the GS itself works (the frame is just VRAM); the offline renderer needs only the mapping, not a full VRAM.
2. **The previous frame.** Crossfade/motion blur (`render_post_effect`, `func_0033F890`, blends the other display
   buffer with FIX ≤ 0.8) need the previous composed image. Keep one history image; the first milestone may count
   these passes as deferred (they need the previous frame to be meaningful in a single-frame replay).
3. **Depth for 2D.** The world path already writes a depth buffer; 2D tests read it through the captured ZTST/ZBUF.
   The mirror is not 2D (it is a second 3D view, `docs/journal/2026-11-10.md` HUD study) and stays out of this layer.

### 4.4 The first measurable milestone

**M-POST-1: the night-race frame with the captured post pass applied to the native world.**

The baseline exists and is measured (`build/evidence-m1/world/world-metrics.json`, frame 0, VBlank 7398, 3x):

| Metric | M1 world baseline |
|---|---|
| PSNR (RGB) | 20.360 dB |
| SSIM (luma) | 0.6138 |
| pct_bad | 77.22% |
| mean abs diff | 20.31 |
| luma of the world image | 31.204 |
| luma of the CoreGS 2.0 reference | 26.688 |
| luma ratio (world / reference) | 1.169 |

The native world is 17% brighter than the reference, which is exactly what a missing post chain (brightness multiply,
colour filter) produces. The milestone is passed when, on the same frame and mask:

- the captured `post` pass is recognised and its operations are applied with zero silent declines;
- `--no-post` reproduces the current baseline image **byte for byte** (the renderer's existing A/B rule);
- PSNR **rises above 20.360 dB** and the luma ratio moves **toward 1.0** (|ratio − 1| < 0.169).

Both thresholds are strict improvements on a measured baseline, so the gate cannot pass by accident. The study does
**not** promise a tone LUT at night: the effects study measured "no tone" in the night race (`docs/journal/
2026-11-10.md`, effects study; `docs/COREGS3-PLAN.md` §6), so the first milestone must apply whatever the captured
post pass actually contains (brightness and filter are the candidates, C for their existence, U for their night-race
share). If the captured post pass turns out to be empty at VBlank 7398, the milestone moves to the day race, where
the tone strips are confirmed (C, plan §6) — this is the first thing S0 measures.

---

## 5. Phased plan

### 5.1 Phases, owner-visible milestones and gates

| Phase | What is built | Owner sees | Gate | Effort (Hyp) | Risk |
|---|---|---|---|---|---|
| **S0 measurement** (read-only instrumentation) | counters only at the acceptance points: path, bytes, records, pass, `source_pc`/`ra` histogram; no file | nothing (a report) | the bytes match the F8 per-path counters; the pass census prints | 1-2 days | low |
| **S1 capture** (done 2026-11-10) | `coregs3_gsstream` module, `.gt4gs` v1 writer, `gt4_coregs3_gs` tool, fixture, hooks in `mtvu_runtime.cpp` + `coregs3_hooks.cpp` | a capture file and its statistics | CTest green; **COREGS3-NIGHT-CHECK-OK** (3x off/shadow); byte control; 0 refusals in the 8-frame window | 4-6 days | medium |
| **S2 post layer** | `gt4_coregs3_render --gs`; 2D primitive subset; post pass recognition; framebuffer-as-texture mapping | the first offline picture with the captured post chain | **M-POST-1** (§4.4) | 1-2 weeks | medium-high |
| **S3 HUD layer** | panel pass replay, sprite/font textures from the captured uploads, HUD metric mask | the offline frame with the native world plus the captured HUD | G-HUD-1: HUD-rectangle PSNR over the measured baseline, per-element counters | 2-3 weeks | high (many elements) |
| **S4 live** | the same operations in the `present` layer (`native/hw_gs/coregs3/`), optional switch | a live race with the native post chain | guest identity 0 differences; visual A/B against CoreGS 2.0 | 2-4 weeks | high |

S2 and S3 are independent of each other and can run in parallel (S2 takes the `post` pass, S3 the `panel` pass);
both depend on S1.

### 5.2 Risks and mitigations

| Risk | Why it matters | Mitigation |
|---|---|---|
| **Pass attribution at acceptance is imprecise** (a display list kicked later than the marker) | packets may land in the wrong pass or in `unknown` | S0 measures the distribution before any rendering; the optional stream marker (§2.2) makes it exact; `unknown` is counted |
| **Post chain samples the frame through an address we do not map** | the layer would read garbage | the measured FBP table plus counted refusals; every unmapped `TBP0` refusal is reported |
| **PATH3 volume** (1.2-2.9 MB/frame) | memory and file size | 64 MiB ring, 8 MiB/frame cap, per-frame refusal counting; the capture window is bounded by VBlank like the tap |
| **Capture perturbs the guest** (round 2's measured lesson) | identity gate fails | no I/O, no allocation, no locks on the guest path; write at shutdown; the 3x gate is the control |
| **Frame pairing off by one** | the post layer applies to the wrong frame | the `.gt4vram` rule is documented; the first-pass-record control (§2.4) catches it |
| **2D state outside the measured subset** (DECAL, DTD, unusual blends) | silent wrong pixels | counted refusals, never defaults; the sampler already does this for the 3D |

### 5.3 Gates (all must be automated)

1. **G-LOGIC:** the existing 3x identity gate (`scripts/coregs3_bench/coregs3-night-check.sh`), unchanged:
   clock, digest, `state.bin` and 13 frames identical for `off` and `shadow` with the capture enabled.
2. **G-GS-CAP:** the `.gt4gs` bytes for paths 2+3 equal the F8 per-path totals of the same window within the record
   padding, every frame closed, 0 refusals.
3. **G-PASS:** the first `.gt4gs` pass record of each frame equals the first `.gt4scene` journal entry of that frame.
4. **M-POST-1** (§4.4) and **G-HUD-1** (S3).
5. **Guest identity of the live layer** (S4), the `present` rule already used in round 10: 0 clock/digest/state
   differences.

### 5.4 The first task brief (for the implementation agent)

> **TASK-090 S1: the `.gt4gs` v1 bounded GS-stream capture (no rendering).**
>
> Context: `docs/COREGS3-GSSTREAM-CAPTURE.md` §1-§3 of this study; `docs/COREGS3-HOOKS.md` §7 (no writes on the
> guest timeline) and §17 (the v5 pass journal and GS state rule); `native/coregs3/coregs3_tap.*` as the pattern for
> an observe-only, bounded recorder.
>
> One problem: record every PATH2/PATH3 GIF transfer of the captured frames, with the pass and VBlank it belongs to,
> into a bounded `.gt4gs` sidecar; nothing is rendered and nothing about the guest changes.
>
> Deliverables:
> 1. `native/coregs3/coregs3_gsstream.{hpp,cpp}` (writer, compiled into `gt4_mtvu_runtime` like
>    `coregs3_tap.cpp`): the §3.2 header/record encoding, a recorder with `configure_from_environment()`
>    (`GT4_COREGS3_GS`, `_VBLANKS`, `_MAX_BYTES`, `_MAX_FRAME_BYTES`, `_MAX_RECORDS`, `_MAX_FRAMES`), `packet()`,
>    `pass_marker()`, `frame()`, `shutdown()` and counters. No I/O and no allocation in `packet()`; a preallocated
>    byte ring; the file is written at shutdown only. Every entry point is one branch when the switch is off.
>    `native/coregs3/coregs3_gsstream_file.{hpp,cpp}` (reader, `gt4_coregs3` like `coregs3_tap_file.cpp`) with
>    `parse_gs_stream()` for the tools; the fixture pins that writer and reader agree.
> 2. Hooks, each called only when `gt4::coregs3::mode() != off` and the switch is set:
>    `native/mtvu_runtime.cpp` `gif_transfer()` (paths 2 and 3) and `shadow_direct()` (path 2);
>    `native/coregs3/coregs3_hooks.cpp` `pass_begin()`, `pass_end()`, `at_vblank()`; the pass words come from
>    `capture_gs_shadow()` plus CLAMP `0x1A8` and TEX1 `0x1B8`.
> 3. `native/tools/coregs3_gs_main.cpp` (`gt4_coregs3_gs --list | --stats | --dump-frame N`), a CMake target next to
>    `gt4_coregs3_tap` (`native/CMakeLists.txt:1170`), a `native_coregs3_gsstream` fixture (round trip, bad magic,
>    unknown version, a record over the bound, a frame over the cap, a bad pass id) and a control in
>    `tests/python/test_coregs3_units.py`.
> 4. Run, headless and 3x, the night race with `GT4_COREGS3=shadow`, `GT4_COREGS3_CAPTURE`, `GT4_COREGS3_ALL_DRAWS=1`,
>    `GT4_COREGS3_TAP` and the new `GT4_COREGS3_GS`, window 7398-7405; report bytes and records per path and per
>    pass, the F8 cross-check, the first-pass control and the 3x gate.
>
> Acceptance: CTest coregs3 green (the 11 existing fixtures plus the new one); the Python control green;
> **COREGS3-NIGHT-CHECK-OK** (8,997 VBlanks, 1,686 racing, 0 differences, 13/13 frames); G-GS-CAP and G-PASS pass;
> no file I/O on the guest thread and no allocation in the packet path (review). Evidence under
> `build/evidence-gsstream/`: the capture, the tool stats, the run logs and the gate log.
>
> Do not: change guest behavior; write the file during the frame; capture PATH1 (the tap owns it); open PCSX2
> sources; merge or push. Update `docs/COREGS3-HOOKS.md` (new section), `docs/STATUS.md`, the journal and TASKS.md.

Effort: 4-6 agent-days. Risk: medium (the pass attribution may need the S0 measurement first; the brief assumes S0
ran).

---

## 6. Open questions and what must be measured before implementation

| # | Question | Why it blocks | How to measure | Label today |
|---|---|---|---|---|
| 1 | Which `PassId` do the PATH3 packets fall into at acceptance? | the whole layer model depends on it | S0 counters, night race, one 9,000-VBlank run | **U** |
| 2 | How much of PATH3 is texture upload vs 2D vs post? | capacity and what the replay must implement first | S0 counters per pass and per packet kind (TEX0/BITBLTBUF presence) | **U** |
| 3 | Are the HUD textures uploaded every frame or once? | `display.gpb` is 267 KB; a per-frame upload is ~20% of the measured PATH3 | S0: count `TRXDIR = 0` packets with `TEX0.TBP0` inside the HUD range per frame | **U** |
| 4 | Does the post chain sample the frame as a texture or via a local copy/download? | decides the framebuffer-as-texture rule (§4.3) | inspect the captured `TEX0.TBP0`/`BITBLTBUF` of the `post` pass | **Hyp** (texture read is the likely form) |
| 5 | Does `source_ra`/`pc` name the pgl builder or the DMA kick (the VIF1 IRQ handler)? | decides whether the field is useful evidence or just a grouping key | S0 histogram of the source sites in a race frame | **Hyp** (the kick handler for PATH3, from `docs/MTVU-PLAN.md` §2) |
| 6 | What are the exact PATH2/PATH3 bytes and records in the **night race** window 7398-7405? | the capacity bound and the capture size | one run with `GT4_PERF=1` and the F8 report | **U** (the 1.24-2.9 MB/frame numbers are other scenes/axes) |
| 7 | Is the night race's post pass non-empty at VBlank 7398? | the first milestone's scene | S0 capture, `post` pass record count | **U** (the tone strips are absent at night, C; brightness/filter are the candidates) |
| 8 | Which operations does the captured post pass contain (brightness, filter, crossfade, blur)? | the M-POST-1 gate is operation-specific | decode the captured `post` packets | **U** |
| 9 | Are privileged display writes (DISPFB/PMODE) needed for the post chain, or only for the final present? | decides whether a `kind 7` record is needed in v1 | S0: correlate the display-register writes with the pass markers | **U** |
| 10 | Does the frame-history requirement (crossfade/motion blur) make a single-frame replay meaningless for those passes? | the first milestone may have to defer them | inspect the post packets for two-buffer reads | **U** |
| 11 | 64 MiB ring on the target machines (bench PC fine; mobile later) | memory budget | measure on the laptop and the S23 when mobile starts | **U** |

Answers after S1 (2026-11-10): **Q1 = C** (every packet is accepted outside every pass marker: `pass = unknown` on
4,771/4,771 records, because the GS stream is kicked at the start of the interval and the K7 markers run afterwards);
**Q5 = measured** (the source sites are the frame kick loop, `pc` 004AB6D8/004AB548/004ABA80, `ra` 0x1000 for 4,587 of
4,771 records, not a per-pass builder); **Q6 = C** for the night race (PATH2 18,192 B / 185 records and PATH3 3.82 MB /
407-412 records per frame; 30.9 MB for the 8-frame window). Q2, Q3, Q4 and Q7-Q10 stay open: the capture carries the
uploads and the post-chain packets, but S1 does not decode them.

Nothing above changes the recommended capture point or format; they change the **content** of the first milestone
and the exact register set a pass record must carry.

---

## Appendix A: files and functions cited

| File | Lines | What |
|---|---|---|
| `native/mtvu_runtime.cpp` | 728, 761, 938, 779, 678, 891, 709, 533 | `gif_transfer`, `shadow_direct`, `worker_gif`, `note_render`, `last_posted_job`, `vblank_sync`, `apply_accepted_signals`, `queue_privileged` |
| `native/mtvu_journal.cpp` | 63, 134, 153, 277, 310 | `emit_gif`, `post_job`, `append_ee_gif`, `execute_ready`, `consume_job` |
| `native/mtvu_bridge.cpp` | 32 | donor `render_gs` |
| `native/coregs3/coregs3_hooks.cpp` | 160, 288, 317, 382, 457, 472, 488, 511, 582, 785 | `capture_gs_shadow`, `mark_overflow`, `finish_frame`, configure, shutdown, `at_vblank`, `pass_begin`, `pass_end`, `before_shape_call`, `print_report` |
| `native/coregs3/coregs3_scene.hpp` | 70, 78-96, 335-341, 345, 353-359, 361, 380-388 | version, `PassId`, the seven GS shadow offsets, `CapturedPassState`, its register fields, the cap, the overflow bits |
| `native/coregs3/coregs3_tap.cpp` | 78, 104, 130 | the tap's pattern (bounded records, source join, VBlank stamp) |
| `native/coregs3/coregs3_tap.hpp` | 30-32 | record kinds and join fields |
| `native/coregs3/coregs3_vram.hpp` | 26-40 | the sidecar header pattern |
| `native/hw_gs/core2/core2_renderer.cpp` | 5517, 5549, 5605, 5653, 5661 | `GifTransfer`, `WritePrivileged`, `ReadFifo`, `Flush`, the SLM sidecar |
| `native/hw_gs/frontend/hw_front_end.cpp` | 192, 259, 1245 | `GifTransfer`, `WriteRegister`, `VBlank` |
| `native/hw_gs/hw_gs_backend.cpp` | 1914 | `gif_transfer` |
| `native/hw_gs/hw_gs_stream_sink.cpp` | 176, 484, 895 | `gif_now`, `vsync_now`, `gif` |
| `native/gs_stream/gs_stream_client.cpp` | 310, 1250, 1297 | `Client::gif`, `stream_renderer`, `on_gif_render` |
| `native/gs_stream/gs_stream_sink.hpp` | 42, 62 | the in-process sink's `gif` and `mtvu_job` |
| donor `runtime/src/gs/gif.cpp` | 29, 75, 163, 168, 179 | `walk`, `GsSink`, `render`, `transfer`, `busy` |
| donor `runtime/src/dmac.cpp` | 61 | DMA ch2 → PATH3 |
| donor `runtime/src/hw.cpp` | 276 | GIF FIFO → PATH3 |
| donor `runtime/src/vif.cpp` | 256-257 | VIF1 DIRECT → PATH2 |
| donor `runtime/src/vu_micro.cpp` | 433 | XGKICK → PATH1 |
| `scripts/prepare_perf_runtime.py` | 227, 305-343 | the MTVU and GS-stream GIF patches |
| `scripts/coregs3_units.py` | 169-195 | the K7 pass markers |
| `native/perf_probe.cpp` | 102, 289 | per-path GIF byte/record counters and their report line |
| `native/tools/coregs3_render_main.cpp` | 354, 398, 400 | the renderer CLI (`--vram`, `--tap`, where `--gs` goes) |
| `native/CMakeLists.txt` | 465, 487, 1166-1172 | the tap's library placement, `gt4_coregs3`, the tool targets |

## Appendix B: measured numbers used above

| Number | Value | Source |
|---|---|---|
| Night-race world PSNR / SSIM / pct_bad | 20.360 dB / 0.6138 / 77.22% | `build/evidence-m1/world/world-metrics.json` (frame 0, VBlank 7398, 3x) |
| World luma vs reference luma | 31.204 vs 26.688 (ratio 1.169) | same |
| PATH3 / PATH2 / PATH1 bytes per frame | 1.24-2.91 MB / 15-46 KB / 1.4-3.3 MB | F8 lines, `private/manual-play/session-20261022-163408-808184/probe.log` |
| Tap size in the gate window | 24,811,512 B for 9 VBlanks | `docs/COREGS3-HOOKS.md` §17.5 |
| Scene capture size (car block) | 4,477,368 B | `docs/COREGS3-HOOKS.md` §17.6 |
| GS journal exactness vs the tap | FOGCOL 220/253, FRAME 277/277, XYOFFSET 162/277, ZBUF 26/277, SCISSOR 251/253, ALPHA 244/277, TEST 176/277 | `docs/COREGS3-HOOKS.md` §17.4 |
| HUD frame / ortho | 640x448 frame, 640x480 (4:3) or 853.33x480 (16:9) ortho | journal 2026-11-10, HUD study |
| HUD textures | `display.gpb` 267,168 B, 72 Tex1 (62 T4 + 10 T8); `race.fnt` 184,960 B | `build/analysis-hud/`; TASKS.md TASK-090 HUD bullet |
| Measured GS targets (night race) | main FBP210/FBW10 CT32; wet reflection FBP210/FBW2; env map FBP350/FBW4 256 px; sphere mapper FBP394/FBW2 128 px; display FBP0/140 CT16 | `build/analysis-effects/`; TASKS.md TASK-090 effects bullet |
