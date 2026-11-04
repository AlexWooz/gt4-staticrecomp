# GS readback study: what GT4 reads back, why, and how to stop stalling on it

Date: 2026-10-21. Read-only study. Clean room: no PCSX2 code, no ARMSX2, no `gt4recomp-gs` fork, and no gt4-gs dumps or
logs were opened.

Sources:
- the gt4-decomp at `private/slice151-research/gt4-decomp` (`e0b70c4d6`, names at identical addresses);
- the translated EE units `private/slice232-ee-progressive-producer/generated/unit_*.cpp`. They are a literal C++
  rendering of the ELF and were used where the decomp has no C for a function: `func_004A5178`, `func_00497838`,
  `func_004AA4A0`;
- the ELF `private/reconstructed/SCUS_973.28.elf` (constant data);
- the donor `runtime/src/dmac.cpp`;
- our docs: FRAME-COST-STUDY, COREGS2-ROADMAP, NATIVE-VARIABLE-FPS-STUDY, GS-STREAM-PROTOCOL and
  hw-renderer-spec 10 and 15;
- `scripts/external_runtime_probe.py`.

Labels: **C** = Confirmed (read in code or data), **H** = High confidence, **Hyp** = Hypothesis.

## 0. Answer in plain words (for the owner)

The night headlight flare is **not** "the GPU renders the frame again after the CPU looked at the pixels". In one
frame the GS does the following:

1. It draws the scene.
2. It tests each light: it draws a tiny depth-tested square where the light is, copies that patch to a scratch
   buffer, and averages it down to one pixel by repeated halving. That pixel is the light's visible fraction.
3. It writes that one pixel into a 64x8 "visibility table", one pixel per light, up to 512 lights.

Then:

4. The 2 KiB table is copied to EE memory (the readback).
5. The EE **never looks at the numbers**. It only points the next DMA packet at that memory. The VU1 flare program
   then reads the 4 pixels around the light's slot and scales the flare's colour.
6. The flares are drawn on top **in the same frame**.

The CPU computes nothing from those pixels. All the "logic" that uses them already runs on the GS and VU1. The
round trip through EE RAM is only plumbing, because on a PS2 the VU1 cannot read GS memory directly.

On the PS2 that round trip costs the CPU nothing. The copy is a DMA step queued inside the frame's display list,
and the DMA engine handles it while the CPU goes on. Our recompilation runs DMA synchronously on the EE thread, so
at that point the EE waits for two things:
- all of the frame's VU1 work (3.3 ms);
- the GS process to finish the GPU work and copy the pixels back (0.9 ms at 1x, 2 ms at 3x, much more at 8x).

The other readbacks are one-shot captures:
- the car icons, made once when the race loads;
- the loading-screen picture;
- a 64x64 screen thumbnail;
- photo-mode tiles and autofocus depth.

"Unsynchronized" breaks them all for the same reason. gt4-gs then answers from its CPU copy of GS memory, and in
hardware mode that copy never received the pixels the GPU drew. The game gets zeros or old bytes:
- flares get zero brightness;
- icons and the logo picture come out blank.

## 1. Inventory of every GS-to-EE readback

All GS-to-EE transfers go through one pgl library routine, **`func_004A5178(fbp, psm, fbw, x, y, w, h, dst)`** (C).
It does not transfer anything itself. It appends to the pgl display list in scratchpad (`0x70002000`, cursor at
+504):
- a DMA tag `0x90000006` (IRQ bit, cnt, 6 qwords);
- VIF `FLUSHA` + `DIRECT 6`;
- a GIF A+D packet built by `func_004ABB88`: BITBLTBUF, TRXPOS, TRXREG, FINISH, TRXDIR=1;
- a list of (qwc, address) chunk descriptors (at most 0xFFF8 qwords each) for the destination.

When the list runs, the IRQ tag stops the chain and the game's DMA interrupt handler takes over. It waits for
FINISH, sets BUSDIR, runs VIF1 DMA in reverse into `dst`, then restores the bus. The descriptor list and the IRQ
tag are C; the handler's exact body is H (FRAME-COST counted CSR plus BUSDIR x2 per readback). In our runtime that
reverse transfer is `dmac.cpp run_normal(ch 1)` → `mtvu::sync("download")` → `gs::read_image` →
`gs_stream::read_fifo` (C).

`func_004A5178` has exactly **four static call sites** in the whole translated program (C, a `jal` scan of all 328
units): `0x00497F30`, `0x00105A50`, `0x003C7B30` and `0x00330954`.

### 1.1 Readbacks by consumer

**Lens flare (headlights, course lights, sun)**

- **Call path:** `RacePS2Base::render_world` (`0x0033DEA8`) → `func_0033EAB8` (`0x0033F140`) → `func_003B8848` →
  `func_00497EF8` → `func_004A5178`. Split screen uses `render_world_split` (`0x0034028C`) instead (C).
- **Data:** 64x8 PSMCT32, 2,048 B, at (0,0) of the flare result buffer (flare object +0x8C). It lands in the flare
  object itself: pointer at obj+0x104 = `(obj+0x147) & ~0x3F`, with obj = RacePS2Base+0xDA0 (C).
- **Frequency:** 1 per frame when any light is sampled (`obj+0x8`, `+0xA34` or `+0x1848` is non-zero). Night races
  1.0 per frame; day races only with the sun in view (C, FRAME-COST).
- **What the game does with it:** no EE arithmetic. VU1 scales each flare by its own pixel (section 2). C/H.
- **Effect when unsynchronized:** flares and sun glare vanish (zero visibility) (C, owner).

**Car icons (delta display, rank list)**

- **Call path:** `CarIconMaker::render` (`0x00393B70`), two calls (`0x00393C08` and `0x00393C60`) →
  `func_001059F8` → `func_004A5178` (C).
- **Data:** per car, a 96x64 render-to-target and a 64x32 one, read as psm 2 (PSMCT16) into `f70[i].a` and
  `f70[i].b`. About 12 KiB + 4 KiB per car (C for the sizes and the PSM; H for the exact rectangle, which comes
  from GSBuffer fields).
- **Frequency:** once. `self->f7C` gates it to the first call, at race load (C).
- **What the game does with it:** the image is kept in EE RAM and uploaded as a texture by the HUD each time an
  icon is drawn (H).
- **Effect when unsynchronized:** the icon is blank for the whole race, because it is captured only once (H, owner).

**Loading screen picture**

- **Call path:** `func_00104B68` (`0x00104BEC`), a virtual method with no direct caller (C).
- **Data:** the display buffer not being shown, `D_0061850C[1 - cur]`, read in two halves (`f4C` and
  `f4C + 0x78000`, about 960 KiB in total) over two frames (C).
- **Frequency:** once per loading screen (C for the state machine).
- **What the game does with it:** state 2 passes the image to the loading object (`func_00103FE8` →
  `func_001032F0`); state 3+ draws it with a fade (`func_00102EC0`) while the disc loads (H).
- **Effect when unsynchronized:** the picture or logo shown during loading is missing (H, owner).

**Screen thumbnail**

- **Call path:** `func_00330AE8` at the end of `render_world`. It runs at most when `self+0xE3D4 == 0` or under
  camera and state tests (C).
- **Data:** the frame is downsampled on the GS to 128x128, then to 64x64 PSMCT16, and read back to `obj->u4`
  (8 KiB). Then `obj->u28 = 1` (C).
- **Frequency:** rare. There were 0 per frame in the measured races (C, FRAME-COST).
- **What the game does with it:** Unknown use (replay, save or pause thumbnail, Hyp).
- **Effect when unsynchronized:** blank thumbnail (H).

**Photo mode autofocus**

- **Call paths:** `RacePhotoMode`: `func_003C7A50` → `func_003C7AE0` (`0x003C7B30`). `PhotoPause`:
  `func_003C5430` → `func_003308E8` (`0x00330954`). C.
- **Data:** 8x4 PSMZ32 (depth), 128 B, at a screen point (C).
- **Frequency:** photo mode only (C).
- **What the game does with it:** **the EE reads it.** `z = word & 0xFFFFFF`, then
  `focus = m[14] / (1 - z/2^23 + m[10])`, stored at +0x1E65C. That is the autofocus distance (C). The word is read
  right after the request is queued, so the game itself uses the value of the transfer that last executed (H).
- **Effect when unsynchronized:** wrong focus depth (blur) (H).

**Photo capture (tiled, accumulated)**

- **Call paths:** `func_00333A00` (`0x00333B88`) and `func_00333E50` (`0x00333FD4`) → `func_001059F8` (C).
- **Data:** each tile of the high-resolution photo, for each of `interval` sub-frames. The previous accumulation
  is blended back from EE (`func_00330D60`, alpha `1/(k+1)`), then read again (C).
- **Frequency:** only while a photo is being taken (C).
- **What the game does with it:** the final image goes to the CPU for saving (H).
- **Effect when unsynchronized:** black or partial photos (H).

### 1.2 Other synchronisation points

- No other code path issues TRXDIR=1: the only GIF builder that emits it is `func_004ABB88`, and its only caller
  is `0x004A5260` (C).
- Waits on FINISH and SIGNAL outside this routine were not inventoried in this study (Unknown).
- `func_004AA4A0`, called before the flare readback and before `func_001059F8`, is a pgl list close or kick that
  writes an IRQ tag `0x91020000`. It is not a readback (H).

## 2. What the flare readback measures (from the code)

All of this is in `src/network/unit_00494578` (pgl flare module) and `src/race/unit_003B7470` (FlareClass).

**1. Begin, per frame: `func_003B80E8` → `func_00497AF0`.**
- It allocates two GS buffers: a 64x8 result buffer (obj+0x8C) and a 64x64 scratch buffer (obj+0xC8).
- It sets the sample counter to zero (`obj+8`).
- It prebuilds 16 downsample DMA packets, for sizes 2..17 (`func_00497518`).
- C.

**2. Per light: a visibility sample.**
- Callers:
  - `func_004980A0` for course lights and headlights, via `func_003B7EE8`;
  - `func_00498518` for the sun, via `func_003B8400`.
- Both call `func_00497C48(x, y, size, strength)`, which works as follows:
  - It clips the light's square on screen, size = projected radius in pixels, at least 2.
  - It draws that square depth-tested at the light's depth (`func_004A6E20(z)`), writing only to the frame
    buffer's alpha channel (`func_004A2930(0,0,0,1)` = colour mask). Occluded pixels keep their old alpha; visible
    pixels get alpha 2.0 (`func_004AA1D8(1,1,1,2)`). C for the calls, H for the exact mask semantics.
  - It copies the patch to the 64x64 scratch buffer (`func_00497298`).
  - It box-filters the patch down to 1x1 by repeated bilinear halving: prebuilt packets for sizes below 18,
    otherwise `func_004973B8` (C).
  - It draws that 1x1 average into result pixel `(n % 64, n / 64)` with colour
    `strength * w * h / 4` (`func_00497778`), then increments `n` (C).
- So each result pixel = visible fraction x per-light strength. One pixel per light, 512 at most (C).

**3. Readback: `func_003B8848`.**
- If any light was sampled, it closes the list, queues the 64x8 transfer into the flare object and resets `n = 0`
  (`func_00497EF8`) (C).

**4. Draw, later the same frame.**
- `render_world` → `func_0033F2E8` → `func_003B88C8` walks the lights **in the same order** (C).
- For a sampled light (`flag & 1`), `func_00498198` calls `func_00497F48`, then `func_00497838(.., &words[n & ~3],
  n & 3)` and increments `n`. The sun uses `func_004985F8` → `func_00497F48` (C).
- `func_00497838` builds a VIF packet (C, translated code):
  - a DMA **REF tag `0x30000001` whose address is the readback qword**;
  - `UNPACK V4-8` (4 RGBA pixels) to VU1 address `0x0CB`;
  - the lane `n & 3` and the flare colour (S-32 or V3-32 unpacks at `0x0CA`/`0x0C9`).
- The VU1 flare program applies the value to the sprite. That it scales colour or alpha is H; the exact use inside
  VU1 is Unknown.

**5. Which views use the measured value.**
- The non-sampled path, and every light in **split screen** (`func_003B7B68(obj, 1)` sets type 1 → bit 2 →
  `func_00497F98`), use the constant `D_006AF5D0` = four pixels `0x000000FF` (R = 255, full visibility; C, ELF
  data).
- So in split screen only the sun uses the measured value (C).

**Conclusions:**
- **The EE never loads the flare pixels.** The only references to the buffer are the pointer stored in a DMA tag
  (H: no other access found in the decomp or in the translated callers; a runtime proof is in section 5, step P0).
- **The flare's game state does not depend on the readback.** `FlareClass::updateFlare` (`0x003B8D40`) only
  advances blink phases at `+0x2250`. The TASK-082 study statement "updateFlare reads the latest result" is
  **wrong** (C).
- The readback can therefore never change the simulation, AI, timing or replays. It is purely visual (H, to be
  proven by P0).

## 3. Replacement designs, least to most invasive

### Shared numbers

Night race CSR5, per frame (FRAME-COST, C):
- MTVU download drain: 3.34 ms;
- GS round trip: 0.90 ms at 1x and 1.6-2.0 ms at 3x;
- frame: 9.9 ms at 1x, 12.1 at 3x;
- the same track without readbacks: 5.0 ms.

At 8x the owner sees 40 FPS with accurate readbacks and smooth play with unsynchronized. Unsynchronized still keeps
the 3.3 ms EE drain. So the **8x loss is on the GS side**: the accurate download makes the GS thread wait for the
GPU to finish the frame's upscaled work, which empties the CPU-to-GPU pipeline once per frame (H; the 3x growth of
the round trip fits this).

### R0. Per-site readback policy (fixes "unsynchronized" losing effects)

- **Change:** the producer tags each READ_FIFO by call site, using the EE return address of `func_004A5178`:
  `0x00497F30` = flare, everything else = one-shot. It uses the unused `ReadFifoPayload.reserved` as hint flags
  (protocol 1.4).
- **Consumer behaviour:**
  - one-shots are always accurate (icons, loading picture, thumbnail and photo are each paid once, at a load or a
    photo);
  - the flare follows the user's mode or one of the designs below.
- **Fidelity:** exact for one-shots.
- **Gain:** removes all three owner-visible losses of "unsynchronized" at zero per-frame cost.
- **Work:**
  - gt4-gs: honour the hint (fork change, tainted agent);
  - native side: the tagging, about 1 day;
  - CoreGS 2.0: the same hint as an in-process enum.
- **Risk:** low.
- **Verdict:** do first. A stopgap, and the base the others need.

### R1. Deferred, journal-ordered flare readback (TASK-084 option A)

- **Change:**
  - At the flare request, post READ_FIFO in stream order **without** the MTVU drain or a wait.
  - Copy the **previous** reply into `dst` immediately (2 KiB memcpy) and return.
  - Collect this frame's reply at the next request; on scene exit, at the next VBlank.
  - The response area holds one reply; collecting before posting the next one is enough.
- **Fidelity:**
  - Visibility is one frame old: 16.7 ms more lag on flare brightness.
  - **New finding:** the result is indexed by sample order `n`. When the set or order of sampled lights changes
    (a car or a light enters or leaves the view, or the sun's sample comes and goes), light `n` in frame N gets
    light `n`'s value from frame N-1, which may be a different light. That is a one-frame wrong brightness, a
    possible flicker (H).
  - Mitigation: hook `func_00497C48` (`count++` at `obj+8`) to record a source key per index (the caller's entry
    pointer, or screen position plus depth) and remap last frame's pixels by key before copying them to `dst`.
    About 1 extra day.
  - Deterministic: reply N-1 for request N, whatever the host timing.
  - No game-logic effect (section 2).
- **Saves:** the 3.34 ms drain + the 0.9-2 ms round trip on the EE, so about 4.2 ms at 1x and about 5 ms at 3x.
  The night frame drops to about 5-6 ms headless (H).
- **At 8x this is not enough on its own.** gt4-gs still stalls its GS thread on the GPU at each download. It must
  also **download asynchronously**: copy the 64x8 target to a staging buffer, record a fence, keep going, and
  answer when the fence signals (at most 1 frame). That is a fork change. With it, the 8x night race should match
  "unsynchronized" smoothness while keeping the flares (H).
- **Work:**
  - gt4-gs: deferred-read flag and async download (2-4 days, tainted agent);
  - native: producer and remap (3-4 days);
  - CoreGS 2.0: the M8 write-back with timeline waits, already in spec chapter 10.
- **Shipped in rc6 (TASK-116 step 1), and R1a below:** the native half is in `native/gs_stream/gs_stream_client.cpp`
  (`local_flare_readback`, journal-ordered, one frame old); the gt4-gs async download is TASK-116 step 2 (1.3.1).

### R1a. Answer the flare table as zeros (rc6, TASK-117)

- **Change:** the same journal-ordered answer, but `dst` is filled with zeros (`GT4_GS_HIDE_FLARES`, on in Performance
  Mode, `0` = R1's previous reply). The READ_FIFO is still posted, so the journal ordering and the deferred
  `sync("download")` are unchanged; only the bytes the EE receives differ.
- **Effect:** the VU1 scales each lamp, headlight and the sun by its own pixel (section 2), so the game draws **no**
  flare, halo or sun glare. This is the "zero visibility" case the VU1 already handles (a fully occluded light samples
  zero; split screen uses the constant `0x000000FF`).
- **Fidelity:** a presentation loss only. The EE never reads the table (section 2), so physics, timing, AI, saves and
  replays are untouched. It also removes R1's one-frame lag and the index-remap flicker, because nothing is drawn.
- **Measured (2026-10-25, night, 1x):** 3,763 of 3,763 tables answered as zeros (`nz=0..0`), no readback wait, EE
  critical path unchanged (5.87-6.52 ms against 6.00-6.44 ms with R1's previous reply); dumps before the race are
  byte-identical and the race dumps differ only on the lights and their glows.
- **Cost left:** the guest still samples each light and still queues the 2 KiB download; only a guest patch removes
  those, and a runtime-switched guest patch cannot keep the switch-off path identical (the EE clock is counted in
  instructions). The GS-side download is TASK-116 step 2's target.
- **Limitation:** split screen keeps the headlight glow. That path draws lights with the constant `0x000000FF` instead of
  the table (section 1.1), so only the sun disappears there. Single-player races are fully flare-free.

### R2. Same-frame, EE-asynchronous readback ("pending memory")

- **Change:** because only the VIF1 REF tag reads the bytes, the reverse DMA can complete *virtually*:
  - mark `dst` (2 KiB) as pending and let the EE go on;
  - let the **MTVU worker** fill the REF'd qwords when it unpacks the flare packet, waiting there for the GS reply.
- **Fidelity:** exact, same-frame values, with no index mismatch.
- **Saves:** about 3.3 ms of EE wait.
- **Costs:**
  - the worker, usually the race critical path at 5-8.5 ms, now waits the GS round trip;
  - the GS GPU sync stays unless it is made async as in R1.
- **Work:**
  - the VIF1 shadow walk and MTVU ring copy REF data at post time today; this needs a lazy REF;
  - the game's IRQ handler (BUSDIR, D1) must still run on the EE without blocking.
  - Medium-high, about 5-7 days.
- **Risk:** an EE load from a pending range would read stale data. That never happens for the flare (H) but would
  for the photo mode, so the allow-list is the flare only.
- **Verdict:** only worth it if R1's one-frame lag or remap proves visible. R3 dominates it.

### R3. GPU-driven flare (the owner's idea), with nothing leaving the GPU

**The observation that makes it cheap:**
- The "compute reduction" already exists. The game's sample draws and halving chain *are* a GPU reduction, and
  the 64x8 result is a GPU render target holding one visibility value per light.
- The only consumer is the VU1 flare program, which turns that value into sprite colour.
- No EE logic consumes it.

So nothing needs to stay on the EE for the flare. The "should we draw it" decision is not made from the value
either: the EE decides from the sample flag, which comes from projection and clipping (`func_003B7C98`), not from
pixels (C).

**Design:**
1. **Recognise.** Hook `func_00497EF8` to register the readback buffer's EE range and the 64x8 target's GS address.
   In the VIF1 shadow walk, a REF tag pointing into that range marks a flare draw. The offset gives the index; the
   `0x0CA` unpack gives the lane. No ELF patch is needed.
2. **Feed a placeholder.** Replace the REF'd qword with `0xFF` per pixel. That is exactly the constant the game
   itself uses in split screen (`D_006AF5D0`), so VU1 emits the full-intensity flare.
3. **Apply on the GPU.** Wrap that VU1 call's GIF output in a marker in journal order (`FLARE_VIS_BEGIN {target,
   texel x, y, lane}` / `END`). The renderer multiplies the scoped primitives' colour or alpha by `texel / 255`,
   fetched in the vertex shader from the 64x8 target (or from a storage-buffer copy).
4. **No readback, no GPU sync, no EE or worker wait, same frame.** The READ_FIFO is dropped. The EE copy keeps the
   last bytes, or zeros, unused.

**Precondition (P1 below):** the VU1 flare program must use the value only as a linear colour or alpha factor.
- If it also scales sprite size or culls on a threshold, the GPU must instead compute vertices (an indirect draw).
  That is much harder.
- The precondition is checkable offline: run the flare program entry through our VU1 AOT or interpreter
  differential harness with visibility 0..255 and diff the GIF output.

**Fidelity:**
- Same frame, the correct light per index.
- Colour within ±1 LSB: VU1 float then FTOI rounding versus the GPU multiply (H).
- Frame identity holds except flare pixels at ±1.

**Work:**

| Renderer | Effort | What changes |
|---|---|---|
| CoreGS 2.0 (in process) | 3-5 days after M8/M9 | The marker is a direct call on the command stream. The renderer already knows targets by GS address (surface cache, M7). The game-knowledge principle in COREGS2-ROADMAP §2 covers it. |
| gt4-gs | 5-8 days, tainted agent | A new protocol record pair (types 11/12 or a CONTROL subtype, protocol 1.4). The fork's hardware renderer then needs a "modulate by target texel" path through its draw and shader pipeline. Intrusive. |

**Recommendation for gt4-gs:** use R1 with async download there, and R3 in CoreGS 2.0.

**Compared with R1:**
- latency 0 frames instead of 1;
- no index remap needed;
- the GS-side GPU sync disappears by construction.

R3 is better on every axis except effort and renderer-specific risk.

**TASK-082 and aux frames:** R3 recomputes visibility for **every** rendered frame on the GPU, aux frames included.
Because there is no side effect, aux frames get fresh occlusion, which is better than reusing the last guest value.
R1 or R2 must instead reuse the last value on aux passes (option E in FRAME-COST).

### R4. One-shot captures kept on the GPU (GS-to-GS)

- **Car icons:** the bytes go EE → texture upload → HUD.
  - The renderer can remember "target T was downloaded to EE range R". When R is later uploaded unchanged (same
    hash), it binds T's GPU copy instead.
  - Result: a full-resolution icon at 8x, and no blur.
  - The CPU copy must still be correct, because the game owns those bytes. So keep the accurate readback (R0) and
    add the substitution as an upscaling feature.
- **Loading picture:** the same approach shows the 8x frame instead of a native-resolution copy.
- **Photo tiles:** the photo needs EE bytes for saving. Keep them accurate (one-shot).
- **Photo autofocus:** the EE reads it, so it must stay EE-visible.
  - Accurate is cheap: 128 B, photo mode only.
  - A one-frame-late answer is what the game already tolerates (H).
- **Verdict:** none of these cost per-frame time; R0 suffices. R4 is a CoreGS 2.0 quality feature (sharper icons
  and loading screen), not a performance item.

### Rejected or minor

- **CPU emulation of occlusion:** there is no depth on the CPU. Rejected.
- **Vulkan occlusion queries replacing the game's sample draws:** these change GS memory the game expects, and give
  no gain at 1x-3x.
  - At 8x the per-light sample and halving chain runs on upscaled targets. A compute reduction over the depth
    target could be cheaper, but this is only a CoreGS 2.0 optimisation inside R3, measured in M9.
- **Split screen:** the headlight values are unused (type 1 constant). There, only the sun needs the readback.
  - An R0-style skip is possible but is a niche gain.

## 4. What dropping the stall buys

| Item | Today (night CSR5) | R0 | R1 + async GS download | R3 (CoreGS 2.0) |
|---|---|---|---|---|
| EE wait per frame, 1x / 3x | 4.2 / about 5.3 ms | same | about 0 | 0 |
| GS-side GPU sync per frame | yes (dominant at 8x) | same | no | no |
| Headless frame, 1x | 9.9 ms | 9.9 | about 5-6 | about 5-6 |
| Owner 8x night race | 40 FPS | 40 (but "unsynchronized" no longer loses effects) | expected 60, like unsynchronized (H) | 60 (H) |
| Flare latency | 0 | 0 | +1 frame (+ remap) | 0 |
| Game-logic effect | - | none | none (P0) | none (P0) |

What the gains enable:
- **TASK-082:** the frame's EE and VU1 critical path stops being serialised. The render-only pass cost becomes
  max(EE render part, VU1 worker) = 4-6 ms (FRAME-COST), and that path needs no readback at all.
- **CoreGS 2.0 aux frames:** R3 makes the flare fully side-effect free. The night race then has nothing that forces
  a guest round trip; the one-shots happen only at load or in photo mode.

What a "native" pipeline looks like for each case:
- **Flare:** GPU occlusion reduction → GPU buffer → GPU-modulated flare (R3).
- **Icons and loading picture:** render-to-texture, kept on the GPU for display. The CPU copy is made once, at load,
  only because the game owns it.
- **Photo:** CPU readback once per photo, as any native game does for a screenshot.

## 5. Recommended plan

| Step | What | Where | Effort | Headless check |
|---|---|---|---|---|
| P0 | **Probe and prove.** Log each READ_FIFO with the EE return address, rectangle, PSM and a hash of the reply. Run the night schedule (CSR5) in accurate mode and with zero-filled replies. Compare a per-frame game-state hash (car positions, lap timers, RNG). | native F8 and capture, TASK-084 P0 | 1 d | the state hash is identical across modes, which proves no gameplay dependence; the inventory matches section 1 |
| P1 | **VU1 flare program check.** Find the VU1 entry used by `func_00497838` packets. Run the differential harness with visibility 0..255. | `tools/vu_recomp` fixtures | 1-2 d | the GIF output differs only in RGBA/alpha, linearly → R3 is valid |
| S1 | **R0 per-site policy** (protocol 1.4 READ_FIFO hint) | gt4-gs protocol + fork (tainted) + producer | 1-2 d | the icon, loading-picture and thumbnail bytes equal the accurate reference with the flare in fast mode; the stream hash is unchanged in accurate mode |
| S2 | **R1 deferred flare + index remap** (`GT4_GS_FLARE_READBACK=sync|deferred` A/B) | producer, TASK-084 step 1 | 3-4 d | fixture: reply delivered at N+1 = reference reply N, byte for byte; remapped visibility equals the reference per light key; frame dumps differ only inside flare sprites; headless night frame ≤ 6 ms |
| S3 | **Async download in gt4-gs** (staging + fence, answer at most 1 frame later) | gt4-gs fork (tainted) | 2-4 d | same replies as accurate; GS thread busy with no per-frame GPU-idle gap; owner 8x night race at 60 FPS with flares |
| S4 | **R3 GPU-driven flare in CoreGS 2.0** (marker + texel modulate; READ_FIFO dropped) | CoreGS 2.0 after M8 (readbacks) and M9; perf in M17 | 3-5 d | flare pixels within ±1 LSB of the accurate path, everything else frame-identical; 0 readbacks per frame in the night schedule; F8 readback wait 0 |
| S5 (optional) | R4 icon and loading-picture GPU substitution | CoreGS 2.0 upscaling features | 2 d | the icon texture at render scale; bytes in EE unchanged |
| - | R2 pending-memory readback | only if S2's lag or remap is visible and CoreGS 2.0 is far off | 5-7 d | - |

**Order:** P0 → S1 → S2 → S3 for the current gt4-gs path (TASK-084 plus the gt4-gs protocol 1.4), with P1 in
parallel. Then S4 when CoreGS 2.0 reaches M8.

**Corrections to existing docs:**
- NATIVE-VARIABLE-FPS-STUDY says `updateFlare` reads the readback result. It does not; only VU1 does, through a
  DMA REF.
- FRAME-COST's "visible lag goes from 1 to 2 frames" should read **0 to 1 frame**: today the flare uses the
  same-frame value.
