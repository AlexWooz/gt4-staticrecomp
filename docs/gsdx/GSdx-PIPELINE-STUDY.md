# GSdx pipeline study: how the PCSX2/ARMSX2 GS works, and what it says about CoreGS 2.0

Reference document for every future "how does GSdx do it" question in this project. Concepts and architecture only: no
GSdx, PCSX2 or ARMSX2 code or constants are copied here, and nothing of theirs may enter our tree (they are GPL). Paths are
given only as evidence of where a concept lives, so an untainted implementer can build their own MIT design from this text
without opening their source. Decision 0070 (the PCSX2 GS runs in a separate process on PC) still holds; this study informs
the in-process CoreGS 2.0/3.0 design and the Android line, never a GPL embedding in the release executable.

- Author status: **PCSX2-tainted, read-only**. This agent may read GSdx/PCSX2/ARMSX2 and our code, and writes documents only.
- Written against `origin/main` `59f27aa1` (our code) and the vendored ARMSX2 tree
  `C:\Antigravity\gt4-private-armsx2gs\third_party\armsx2` (`pcsx2/GS`, identical in size to `private/armsx2-vendor`).
- Labels: **C** = confirmed (read directly in the code cited, or measured and quoted with its source); **H** = high
  confidence; **Hyp** = hypothesis; **Unknown** = needs a measurement or a decision.
- No build, benchmark or game run was made for this page. Every number quoted is from the existing ledger, the TASK-215
  baseline page, or the owner's S23 log bundle as transcribed in the private `TASKS.md`; each is labelled with its source.
- Companion pages: `docs/hw-renderer-spec/CORE2-PERF-LEDGER.md`, `docs/tasks/COREGS2-PERF-AUDIT.md`,
  `docs/tasks/COREGS2-PERF-BASELINE.md`, `docs/GS-READBACK-STUDY.md`, `docs/GSDX-PLAN.md` (C.7), `docs/GS-SETTINGS.md`.

---

## 0. The short version

1. **They present from the same thread that runs the GS, and they submit the whole frame once.** A frame is recorded into
   one command buffer and handed to the driver at present time; there is no per-N-batch submission in the steady state
   (C). The only mid-frame submission is an "armed" one, enabled for a few frames after a synchronous readback, so the GPU
   has already finished the work when the readback asks for it (C).
2. **Their default readback is synchronous and blocks the GS thread**, like ours (C). Their escape hatch is a real mode:
   *Asynchronous* hardware downloads answer the EE from a CPU shadow of GS memory that is refreshed a few frames later, with
   a per-page generation check that throws away stale snapshots (C). That is exactly the one-frame-late answer the owner
   rejected for normal mode (decision 0073), made safe and stable; it is not a faithful-mode technique.
3. **Their draws are attachment draws.** One render pass per run of draws to the same targets, hardware blend/depth/alpha
   test where the GS state allows it, and a feedback read (sample the target you write) only when the draw needs it, with a
   declared loop instead of a copy (C). On a tile-based GPU that is the structural difference that matters: a pass end is a
   tile store, a storage-image barrier can push the driver out of tile mode, and they spend neither when nothing forces it
   (C/H).
4. **We are not slower on the desktop.** On the same recording and the same work, CoreGS 2.0 is 9.0% faster than the
   `gt4-gs.exe` reference at 3x and 11.4% at 12x, wall clock, including the separate-process transport
   (C, TASK-215 baseline page). The owner's "GSdx is much faster" observation is the **Galaxy S23 Ultra** (Adreno 740), and
   there our own numbers say why: 11.5 ms of GPU per VBlank at **both** 1x and 2x, with ~750 fragment barriers, 75 full
   barriers and 20 render passes per VBlank - fixed per-pass and per-barrier overhead on a tiler, not fill (C, owner's S23
   log bundle in the private `TASKS.md`, TASK-180).
5. Therefore the useful output of this study is **not** "copy their threading" (we are deeper: 16 slots, a second present
   queue, a pacer thread, latency control) but "copy their **pass and barrier discipline on tilers**, and their **readback
   arming**", plus a short list of small, faithful-neutral fixes.

---

## 1. Frame pipeline and threading

### 1.1 The threads

| Thread | Runs | Evidence |
|---|---|---|
| EE / CPU thread (the game) | guest code, VIF/GIF packet building, `MTGS::InitAndReadFIFO` when the game reads GS memory | `pcsx2/MTGS.cpp` |
| MTGS ("GS") thread | GIF parsing, vertex kicking, draw *submission to the back ring*, VSync, merge and **present** | `GS/GS.cpp` (`GSvsync`, `GSPresentCurrentFrame`), `GS/Renderers/Common/GSRenderer.cpp` (`VSync`, `SubmitVsync`) |
| GS **back** thread (ARMSX2, Vulkan only) | local-memory transfers, CLUT loads, the whole draw path including the texture cache and the device calls | `GS/GSBackQueue.h`, `GS/Renderers/Common/GSBackThreadPolicy.h`, `GS.cpp` (`GSDecideBackThreadFor`) |
| Screenshot / PNG and software-rasterizer workers | compression and the software rasterizer's job queue; the shader path has **no** worker - their Vulkan pipelines compile on the GS thread | `GS/GSPng.h`, `GS/GSRasterizer.h`, `GS/GSJobQueue.h` |
| Present/driver threads | driver-internal; the app never has a present thread of its own | - |

Facts that matter:

- **The front/back split is new and optional.** It is granted for a hardware renderer only on Vulkan, and refused when
  downloads read live GS memory (C: `GSBackThreadPolicy.h`). Its placement avoids cores another busy emu thread is pinned to
  and flags when it cannot; sharing a big core with EE/MTVU/MTGS/IOP measured as a net loss on a four-big-core SoC (C, same
  file). The S23 measurement the owner made therefore may not even include it - see section 9, Unknown U1.
- **The back ring carries self-contained records** (transfer, local-to-local move, CLUT load, PCRTC sync, VSync, draw), each
  with its register snapshot, so the consumer needs no live state machine (C: `GSBackQueue.h`). One SPSC ring,
  acquire/release only, backpressure when full.
- **VSync is never queued.** It drains the back ring first and runs on the MTGS thread, because present touches the device
  (C: `GSRenderer.cpp`, `SubmitVsync`). So per frame there is exactly **one drain barrier** between the two threads, at
  present.
- The EE->GS path is a **ring of packets** (8 MiB of 128-bit slots) with `WaitGS` when it fills (C: `pcsx2/MTGS.h`,
  `RingBufferSizeFactor = 19`, `MTGS.cpp`).

### 1.2 What a frame looks like

```
EE:      build GIF packets -> ring
MTGS:    parse GIF, kick vertices --------> back ring (records)
back:    local memory, CLUT, texture cache, draw recording -> Vulkan command buffer(s)
MTGS:    VSync: drain ring, merge the display, begin present (acquire), draw present,
         submit the command buffer, present, pre-acquire the next image
GPU:     executes the submission (starts only now, unless an armed mid-frame submit happened)
```

The important consequence: **in the steady state the GPU starts a frame's work at present time.** Nothing overlaps the GPU
with the CPU's recording except the previous frame's submission still executing (up to 2 submissions in flight with 3
command buffers). This is the opposite trade from ours, which submits every 32/64 batches.

### 1.3 Command buffers, submissions, fences, semaphores

- **3 command buffers** (`NUM_COMMAND_BUFFERS = 3`) with one fence each and a monotonically increasing **fence counter**
  (C: `GS/Renderers/Vulkan/GSDeviceVK.h`). A slot is reused only after its fence signals; `vkWaitForFences` on the slot
  being recycled is the "ring came round" wait and is deliberately not counted as a stall (C: `ActivateCommandBuffer`).
- **2 submissions in flight** in practice (3 buffers minus the one being recorded) (C/H).
- **The swapchain has 2-3 images** (2 normally, 3 with mailbox) and **4 semaphores** (buffers + 1) so an acquire never
  races a fence wait (C: `VKSwapChain.h`).
- **Present is on the same queue** as the graphics work (`m_graphics_queue`); `vkQueuePresentKHR` goes to the present
  queue, which is normally the same family/queue (C: `SubmitCommandBuffer`).
- **Acquire is bounded** (2 s), not infinite, and the next image is **pre-acquired immediately after present** so the next
  frame's `BeginPresent` usually finds an image already in hand (C: `VKSwapChain::AcquireNextImage`,
  `SubmitCommandBuffer`'s "grab the next image as soon as possible", credited there to dxvk). This is a deliberate,
  cheap trick against a blocking acquire on FIFO.

### 1.4 The draw scheduler (why: tilers)

- Draws that only write their target are **held back** and emitted together per target run, up to 4 runs, 2048 draws,
  bounded vertex/index bytes (C: `GS/Renderers/Common/GSPassScheduler.h`). Runs of the same target keep their order; runs of
  *different* targets may be reordered only while they are independent - any attachment overlap, or one run sampling what
  another writes, is a flush (C, same file).
- The stated motive is a tiler: "on Adreno or Mali each render-pass boundary is a full tile load followed by a full tile
  store" and the motivating title ping-pongs two targets 822 times per frame (C, same file's header comment). On a desktop
  GPU a pass boundary is close to free, which is why the same idea bought us so little in IN-419/IN-420 (3-13% each).
- Anything that could observe a target goes through a device entry point that flushes the queue first; the scheduler itself
  knows nothing about GS memory and relies on that (C: `GSDevice.h`, `FlushDeferredDraws`).

### 1.5 Where it blocks

| Block | Thread | Why | Evidence |
|---|---|---|---|
| `WaitGS` on ring full / on a read | EE | ring backpressure; a synchronous read waits for the GS thread | `MTGS.cpp` |
| drain at VSync | MTGS | present must see every queued draw | `GSRenderer.cpp` (`SubmitVsync`) |
| fence wait for a slot being recycled | GS thread | 3-deep ring came round | `GSDeviceVK.cpp` (`ActivateCommandBuffer`) |
| fence wait for a readback | GS thread | the download's copy must complete | `GSTextureVK.cpp` (`GSDownloadTextureVK::Flush`) |
| `vkAcquireNextImageKHR` | GS thread | FIFO, the display has not released an image | `GSDeviceVK.cpp` (`DoBeginPresent`) |
| ring/index/mutex in the back thread | back thread | backpressure between the two rings | `GSBackQueue.h` |

---

## 2. Synchronization and readbacks

### 2.1 The readback classes

1. **EE upload into GS memory** (GIF transfer, local-to-local move): not a readback. It writes local memory; the texture
   cache marks targets/sources dirty and later re-reads them. (C: `GSState::Transfer`, `FlushWrite`, the TC's dirty rects.)
2. **Target -> GS local memory** ("cache readback"), when a draw or a transfer needs the pixels that only the GPU holds.
   Triggers (C, `GSRendererHW.cpp`, `GSTextureCache::Read`):
   - a draw samples a target that was written since the last read and the formats/positions do not allow a direct sample;
   - a CLUT draw whose palette lives in a target (unless GPU CLUT is on);
   - an EE-visible memory read, i.e. `ReadFIFO`/`ReadLocalMemory`;
   - `ReadbackAll` on CSR reset, savestate, renderer swap, GS dump (never deferrable - "callers consume local memory
     straight after this", C: `GSTextureCache::ReadbackAll`).
3. **Frame readback to the host** (screenshot, GS dump, remote play): the merged image is downloaded, not a target.
4. **Local memory -> EE** (`TRXDIR 1`): `GSState::InitReadFIFO` reads the image from local memory *after* invalidating the
   overlapping targets; with the hardware renderer that first forces the TC readback above (C).
5. **Texture hash / unswizzle readback** used by texture replacement and the hash cache (C: `GSTextureCache` hash path).

### 2.2 The download mechanism

`GSDownloadTexture` is the common object: a staging buffer plus a copy recorded into the current command buffer
(`CopyFromTexture`), a **fence counter** taken at that moment, then `Map`/`Flush`/`Poll` (C:
`GS/Renderers/Common/GSTexture.h`, `Renderers/Vulkan/GSTextureVK.cpp`, `Renderers/OpenGL/GSTextureOGL.cpp`).

- **`Flush()`** waits for that counter (Vulkan: `WaitForFenceCounter`; OpenGL: `glClientWaitSync` with no timeout) (C).
- **`Poll()`** is a non-blocking completion test (Vulkan: compare the completed counter; OpenGL: zero-timeout client wait;
  backends without one keep the base implementation and are retired by force) (C).
- Before the copy, the device **flushes deferred draws** so the readback sees every draw the renderer has accepted (C:
  `FlushBeforeReadback`).
- Two copy shapes: a direct copy when the target is at 1x and the format matches; otherwise a format-convert pass into a
  temporary target first (C: `GSTextureCache::Read`, including a CPU RGB5A1 pack used when a 16-bit readback at 1x would
  otherwise pay a whole extra render pass inside the fence window).
- The GS thread also **spins** instead of sleeping when configured to (`HWSpinGPUForReadbacks`): a GPU-written semaphore
  plus calibrated timestamps, with a short spin budget (C: `GSDeviceVK.cpp`, `ExecuteCommandBufferForReadback`,
  `SubmitSpinCommand`). The spin exists because a blocking wait on some drivers costs more than the work it waits for.

### 2.3 Synchronous vs asynchronous: the modes

`Pcsx2Config::GSOptions::HWDownloadMode` (C: `pcsx2/Config.h`):

| Mode | What the EE sees | Same frame? | Notes |
|---|---|---|---|
| `Enabled` (default) | exact bytes, after the GS thread waits for the fence | yes, but the GS thread blocks | the accuracy reference |
| `EnabledForceFull` | exact, whole target read | yes | slower, safest |
| `NoReadbacks` | the read falls through to the synchronizing path (i.e. still accurate) | yes | not a speed mode |
| `Unsynchronized` | **live** local memory, read on the EE thread with no lock and no drain | no guarantee | refuses the front/back split (C: back-thread policy) |
| `Disabled` | zeros | - | "game does not care" |
| `Asynchronous` (ARMSX2 addition) | a **CPU shadow** of GS memory, read under a mutex | **no: a few frames late** | see below |

The `Asynchronous` machinery (all C):

- A second `GSLocalMemory` is allocated as the shadow (`GS: asynchronous HW download shadow allocated`), seeded by a full
  copy (`SyncAsyncReadbackMemory`), with a **per-page generation counter** (`GSState.cpp`).
- `MTGS::InitAndReadFIFO`: on the EE thread, if the mode is Asynchronous and the renderer is hardware, it answers from the
  shadow **immediately** and posts an `AsyncReadFIFO` packet so the GS thread issues the real GPU download later. **The EE
  never waits on the GS thread for a read** (C: `MTGS.cpp`).
- The download goes into a throwaway staging texture and is queued (`m_pending_downloads`, max 8) together with the page
  generations captured at queue time (C: `GSTextureCache.cpp`, `GSTextureCache.h`).
- At **VSync** the queue is retired (`ProcessPendingDownloads`): each entry is `Poll()`ed; the completed ones are
  **unswizzled into the shadow**; if the covered pages have moved on (an EE upload, a move, a synchronous readback), the
  entry is **dropped instead of rolled back** (C: `ApplyPendingDownload`, `AreAsyncReadbackPagesCurrent`).
- **Only the newest snapshot of the same range is published**; intermediate versions are skipped (C:
  `ProcessPendingDownloads`).
- A **1x1 read is treated as a visibility query and given a fixed 3-frame latency** on purpose, "so variable mobile GPU
  latency cannot turn a steady effect into an on/off oscillation", and an entry that never completes is forced through
  after 4 frames (C: `GSTextureCache.h`, `VISIBILITY_QUERY_LATENCY_FRAMES`, `MAX_PENDING_DOWNLOAD_FRAMES`).

### 2.4 Does GSdx overlap the next frame while waiting for a readback?

**Default (`Enabled`): no.** The GS thread is the thread that blocks on the copy's fence, and it also parses the next
frame. What still runs is the EE, ahead in the 8 MiB ring, and the GPU, on work already submitted - but the GS thread
records nothing new until the fence signals. (C: `GSDownloadTextureVK::Flush` -> `WaitForFenceCounter`; `MTGS::WaitGS`.)

**With `Asynchronous`: yes, by up to 3-4 frames.** The EE is served from the shadow with no wait, and the GPU download is
retired at a later VSync. (C: the mode's code path above.)

**Two mechanisms shorten the default wait without deferring anything:**

1. **Mid-frame "readback kick".** For 3 frames after a synchronous readback, and only at a render-pass boundary, and only
   if the next command buffer is verifiably free, the recording is submitted early. The GPU then executes the frame while
   the GS thread keeps recording, so by the time the copy is issued the fence is usually near-done (C: `GSDeviceVK.cpp`,
   `DoRenderHW`'s kick block). Arm 8 unsubmitted passes; the code notes that with 3 command buffers only ~2 kicks per frame
   actually land, and that the threshold sweep 8 -> 16 measured -2% total GPU stall on Rogue Galaxy and +12% on OutRun 2006,
   so it is title-dependent (C, same comment).
2. **Spin instead of sleep** on the readback fence, when the device can and the option is on (C, above).

**What is lost with `Asynchronous`:** the same frame's data. The game gets a value that is 3-4 frames old, the newest
snapshot wins, and results whose pages moved on are discarded rather than delivered. For a flare visibility test that is a
delayed effect; for anything the game uses to decide geometry it would be wrong. `Unsynchronized` loses even the "stable
version" guarantee: the EE reads memory while the GPU is still writing it.

### 2.5 Readback inventory (theirs)

| Class | Trigger | Sync? | Hides the wait? | What it costs / loses |
|---|---|---|---|---|
| Target -> local memory, draw samples it | a draw whose source is a dirty target with incompatible formats | sync fence wait | kick + spin | one copy + one pass wait; the wait is the frame's pending GPU work |
| Target -> local memory, CLUT from a target | a CLUT draw whose palette is in a target | sync unless GPU CLUT is on | GPU CLUT avoids it entirely | a full CPU draw of the CLUT plus the download |
| Memory read (`ReadFIFO`) | the EE executes a local-to-host transfer | sync (EE waits for the GS thread, GS thread waits for the fence) | kick + spin + (Asynchronous) | the chain EE -> GS -> GPU -> GS -> EE; 1.4-4 ms per VBlank in our own Android normal mode (C for CoreGS's analogous chain, owner's S23 log bundle) |
| `ReadbackAll` | reset, savestate, renderer swap, GS dump | sync, never deferred | nothing | whole targets copied |
| Frame readback | screenshot, dump, remote play | sync | nothing | a whole merged image |
| Hash/unswizzle | texture replacement / hash cache | sync | nothing | the source bytes |

### 2.6 Where their readback model beats ours (detail in section 5)

- They have a **queue** of pending downloads with per-page validity and newest-wins publishing. We have one deferred ticket
  per (view, rect) in performance mode (`AnswerDelayedDownload`, `core2_renderer.cpp:2746`) and no cross-frame queue.
- They can **arm** the submission cadence around a readback. We submit every 32/64 batches always, plus a "K batches after
  a waited readback" rule on the release branch (`GT4_CORE2_SUBMIT_AFTER_WAIT`, `task/182-barrier-scope`).
- They can **spin** on the fence with calibrated timestamps. We have `GT4_HW_READBACK_SPIN` in the options but off
  (`native/hw_gs/hw_gs_options.cpp:243`), and no calibrated-timestamp spin.

---

## 3. Uploads, CLUTs, targets and caching

### 3.1 Local memory is the truth; the texture cache is a cache

Their hardware renderer keeps GS local memory authoritative for the EE, and the texture cache holds GPU copies with an
explicit validity model: `m_valid` (what the GPU copy really contains), `m_dirty` (rectangles the EE wrote since the copy),
`m_drawn_since_read` (what the GPU drew since the last write-back), plus per-channel validity for RGB and the two alpha
halves (C: `Renderers/HW/GSTextureCache.h`, `Target`). Draws consult `m_drawn_since_read` to decide whether a readback is
needed at all; a dirty rectangle that fully covers a sample lets them skip the readback (C: `GSRendererHW.cpp`, the
`req_readback` computation around line 11700).

Our model is the same idea with block granularity: `Target` with `drawn`, `dirty`, `readOnGpu`, `fullWordWritten`
(`native/hw_gs/core2/core2_renderer.cpp:264`), plus block identities for textures (IN-453). We are finer-grained on
texture identity and coarser on the alpha channels; theirs tracks alpha low/high separately, which is why some of their
readback decisions are cheaper.

### 3.2 Texture creation and palettes

- A source is decoded from local memory (swizzle undone, palette applied) into a GPU texture, cached by address, format,
  clamp, palette and region (C: `GSTextureCache::CreateSource`, `SourceMap`).
- **Palettes**: `GSClut` holds the 512-halfword CLUT in memory, tracks whether a load left it dirty, and caches the GPU CLUT
  textures it built (for 4-bit and 8-bit palettes) together with the draw they were made for, so a CLUT that lives on a
  target can be re-used without a download (C: `GSClut.h`). A `PaletteMap` caches a decoded palette texture keyed by the CLUT
  bytes and the palette format (C: `GSTextureCache.h`, `PaletteKey`, `Palette`, CBP/CPSM-aware `LookupPaletteSource`).
  Indexed textures can be expanded on the CPU, or kept indexed with the palette bound as a second texture ("paltex"), or
  converted on the GPU (C: `GSConfig.GPUPaletteConversion`).
- **GPU CLUT**: when the palette itself lives in a render target, the CLUT is read as a **texture** instead of being
  downloaded to the CPU, provided the source draw can be left on the GPU (no EE upload over the area, no dirty rect in the
  way) (C: `GSRendererHW::PossibleCLUTDraw` and the `CLUTDrawTestResult::CLUTDrawOnGPU` path). This is the same concept as
  our IN-408/IN-409 (CLUT read on the GPU, palette rows looked up at draw time) - we are not behind here.
- **Hash cache**: a content hash can identify an unchanged texture across frames (C: `GSTextureCache::HashCache`,
  `LookupHashCache`). We tried the content-keyed cache and reverted it (IN-413: 2 MB/VBlank of hashing); our block
  identities (IN-453) are the better trade and are ahead of this.

### 3.3 Targets

- Lookup is by block address + format + width, with a scan of live targets and special handling for targets that overlap
  partially or need to be expanded for the display (`ScaleTargetForDisplay`, `m_target_heights`).
- Invalidation is per dirty rectangle with a format and channel mask; whole-rect dirty lets a draw skip a readback; a
  target that is "preloaded" pulls data from local memory only when the TC's validity says it must (C:
  `GSTextureCache::PreloadTarget`, `AddDirtyRectTarget`).
- They track the display framebuffers explicitly (PCRTC state, `PCRTCDisplays`, framebuffer offsets, scanmask) and pick
  field/direct-present paths per video mode (C: `GSBackQueue.h`'s `PcrtcSyncRecord`, `GSRenderer.cpp`'s `Merge`).
- Ageing: targets and hash entries age by frame with a per-GPU tuning table (see 7.2), not a single global budget.

### 3.4 Uploads

Sources and targets are written through a **persistent stream buffer ring** (`VKStreamBuffer`) with explicit cache
maintenance at submit (`FlushStreamRingWrites`), one ring per use, and staged copies (`vkCmdCopyBufferToImage`) (C:
`Renderers/Vulkan/VKStreamBuffer.cpp`, `GSDeviceVK::SubmitCommandBuffer`). Our staging/vertex/uniform rings and the GLM do
the same; our swizzle and the block-identity cache are the extra work they do not have (IN-452/453/486).

### 3.5 Render passes, barriers and feedback reads

- One render pass per run of draws to the same attachments, kept open across draws; **load op chosen from the texture's
  state** so an untouched target is not loaded and a partially written one is (C: `GSPassScheduler`'s `Run::rt_state`,
  `GSTexture::State`).
- **Self-sampling** (a draw that reads the target it writes) is a *declared feedback loop*: the feature table decides
  between a texture barrier, framebuffer fetch, input attachments, ROAA (rasterization-order attachment access) or a copy,
  and `cheap_rt_feedback_read` marks the devices where a feedback read costs no pass break and no tile flush (C:
  `GSDevice.h` `FeatureSupport`, `GSDeviceVK.cpp` extension resolution). Our equivalent is the self-copy (IN-463); they
  avoid the copy when the device can order the read, we copy always in faithful mode.
- DATE (destination alpha test) uses the same vocabulary: stencil where possible, a "fast stencil shadow" blend path on some
  devices, ROV or a second pass otherwise (C: `GSDevice.h`'s ROV fields, `GSFastStencilShadow.h`).

### 3.6 How this differs from CoreGS 2.0, in one paragraph

We treat every target as a **storage image** and every draw as a read-modify-write inside a shader, with a pixel interlock
(or a per-primitive ordering heuristic) for GS ordering, and full memory barriers when the page tracker sees a hazard
(`native/hw_gs/core2/core2_gpu.cpp:1552` `full_barrier`, `:2009` `flush_draws`, `:1824` `barrierFirst`). They treat every
target as an **attachment** and let the ROP do blend/depth/alpha where the GS state allows it, declare a loop when a draw
reads its target, and otherwise never barrier between draws of a run. Both are exact where they claim to be; theirs matches
the hardware's own datapath, ours needs the shader to emulate it.

---

## 4. Presentation, vsync and pacing

- **Begin present** acquires a swapchain image, opens a render pass on it and draws the merged display with the present
  shader; **end present** ends the pass, submits the command buffer (with the acquire semaphore as a wait and the
  render-finished semaphore as a signal), calls `vkQueuePresentKHR`, then pre-acquires the next image (C: `GSDeviceVK.cpp`,
  `DoBeginPresent`, `EndPresent`, `SubmitCommandBuffer`).
- **The present is part of the frame's command buffer**, so "how many frames are in flight" is 3 command buffers and 2-3
  swapchain images, and the GPU never sees the frame before present (except the readback kick).
- **VSync mode** maps to a Vulkan present mode: FIFO (default), MAILBOX, IMMEDIATE, FIFO_RELAXED, preferring mailbox for
  "vsync off" (C: `VKSwapChain::SelectPresentMode`).
- **Pacing**: with FIFO the display is the clock; no sleep is added (`GSThrottlePresentation` returns early for FIFO). With
  vsync off, a manual throttle sleeps to the refresh rate so the UI does not spin at thousands of FPS (C:
  `GSDevice::ThrottlePresentation`). An optional max-presented-FPS cap skips presents on an accumulator deadline, and
  duplicate frames can be skipped (C: `GSRenderer::VSync`, `GSSetMaxPresentFps`, `SkipDuplicateFrames`).
- **Waits that appear**: `vkAcquireNextImageKHR` on the GS thread when the display has not released an image (the FIFO
  stall), and the submit path when a slot must be recycled. Present itself is asynchronous.
- **Ours is more decoupled**: 3 frame slots and 2 presents in flight (`native/hw_gs/hw_gs_backend.cpp:44-45`), a **second
  queue of the family** where the device has one (`present_on_own_queue`, `:1153`, acquire with a 0 timeout and a dropped
  present instead of a block, `:1166`), a **pacer thread** for uneven frames, `VK_KHR_present_wait2` latency control
  (IN-521) and a frametime log. On a device with a single queue we fall back to `present_frame` (`:1383`), which acquires
  with `UINT64_MAX` (`:1419`) - there the GS worker can block exactly as their MTGS thread does.
- Their **pre-acquire** (grab the next image right after present) is the one present trick we do not use on that
  single-queue path, and it is the cheapest fix for the 60 Hz stall the ledger measured (IN-521's "showroom waits 11.42
  ms").

---

## 5. Where CoreGS 2.0 is wrong or weaker

Our numbers below are from the TASK-215 baseline page (2026-11-10, desktop) unless another source is named. Their side is
labelled C/H as in the rest of this page.

### 5.1 Our model in one paragraph

The EE posts records; a GS worker thread consumes them, parses GIF, batches, translates, decodes/caches textures, records
draws into a command buffer, and submits every 32 (integrated) or 64 (discrete) batches with up to 16 slots in flight
(`native/hw_gs/core2/core2_gpu.cpp:211` slots, `:274` interval, `:2472` defaults, `:2150` waiting for the oldest slot,
`:3574` the per-batch submit). A readback reduces the target with a compute pass into a host-visible buffer and **waits for
every slot** (`core2_renderer.cpp:2643`, `core2_gpu.cpp:4089`, `:2143` `wait_all`). Present composes on the GPU and is
handed to a separate presenter (`core2_renderer.cpp:5337` `ComposeFrameGpu`, `hw_gs_backend.cpp:1747` `end_frame`,
`:1817` `present_slot`).

### 5.2 Pass and barrier overhead on tilers - **C**, the biggest gap

Owner's S23 Ultra (Adreno 740, Android 17), CoreGS 2.0 Performance mode: ~60 VBlanks/s at 1x **and** 2x, GPU 11.5 ms per
VBlank at both scales, draw passes 9.5 ms, with about **750 fragment barriers, 75 full barriers and 20 render passes per
VBlank** (C, private `TASKS.md` TASK-180, owner's log bundle). The cost does not move with resolution: it is per-pass and
per-barrier overhead, not fill. Their design spends a pass boundary only when the target changes and a barrier only when a
draw reads what it writes (C, section 1.4 and 3.5). This is the mechanical reason a GS port on a tiler can be much faster
than ours at the same settings, and it is already the subject of our TASK-182 (barrier and pass scoping) - see section 6.1.

### 5.3 Storage-image read-modify-write everywhere - **C**, mobile-specific

Every draw goes through `hw_pixel_uber.frag` with storage images bound for colour and depth, plus a pixel interlock where
available (`core2_gpu.cpp:190` `interlock`, `:340-360` pipelines). On a desktop GPU that is fine (IN-421 measured the whole
fragment stage at 0.4-0.7 ms of GPU time at 3x; IN-438 the floor of a shader allowed to write an image at 0.40 ms per 12x
pass). On a tiler it forbids the fixed-function blend/depth path, forces per-pixel read-modify-write traffic and defeats
whatever framebuffer compression the driver would use (Hyp for the compression part; C for the pass/barrier part above).
We *do* have a ROP path (`GT4_CORE2_ROP`, `_ROP_BLEND`, `_ROP_CT`, `_ROP_FILLS` switches, and `IN-481/499` attachment
draws), but it is off by default and was measured as a **loss on the desktop** (IN-523: 12x 19.73 -> 21.37 ms with ROP;
IN-438: "a ROP path would not speed up this GPU"). So this is not a desktop item and must not be re-measured there.

### 5.4 The readback is a hard stop for the whole pipeline - **C** on main, **H** that it matters at 1x-6x

`wait_all` waits for *every* slot, then the worker copies the staged words into GS memory and only then answers the EE. On
main there is no newest-fence-only wait (the release branch has `GT4_CORE2_WAIT_NEWEST_FENCE`, `task/182-barrier-scope`),
no kick and no spin. Measured cost: 3x `gpu_wait` 0.23 ms and readback stage 0.46 ms; 6x 1.24 ms; 12x 13.75 ms of waits and
11.40 ms of readback stage (C, baseline). At 12x the wait is not a defect of the readback design: the frame's fill does not
fit in 16.67 ms, so anything mid-frame must wait for it.

### 5.5 The EE chain for the flare table - **C**

The read is posted by the EE (`native/hw_gs/hw_gs_stream_sink.cpp:575` `post_flare_table`), executed by the GS worker
(`:682` `FlareRead`), which calls `ReadFifo` -> `Readback` -> GPU reduce + fence wait (`core2_renderer.cpp:5162`,
`:2643`), and the answer goes back to the EE (`:585` `read_flare_table`, `:870`/`:882`). IN-522 removed the *EE's* wait for
the worker; the worker still waits for the GPU, so at 6x the EE's critical path (8.69 ms) exceeds the GS worker (7.44 ms)
because the EE waits at that read (C, baseline). Their `Asynchronous` mode removes the whole chain, at the price of
lateness (C, section 2.3).

### 5.6 Present coupling on single-queue devices - **C**

On a device with no second queue of the family we present with `acquire_image(UINT64_MAX)` on the thread that also answers
reads (`hw_gs_backend.cpp:1419`), and the ledger measured exactly that stall at 60 Hz (IN-521 showroom waits 11.42 ms with
8 slots, 0.01 ms with the second queue). Their pre-acquire means the next frame starts with an image already in hand, so
the blocking acquire mostly disappears even on one queue (C, section 1.3).

### 5.7 Per-batch CPU cost: our CLUT-load flushes - **C** for our numbers, **Hyp** for the comparison

George V Paris 1x: 1,500-1,630 CLUT-load flushes per VBlank, 2.0 ms of the CLUT path of which 1.5 ms is the flush
(TASK-134 section 9); the license B-1 at 1x: 1,430 CLUT flushes per VBlank of 1,930 batches (C, baseline). Their paltex/GPU
CLUT model binds a different palette texture per draw instead of ending the batch, so the same scene would not pay a
per-palette flush at all (C for their mechanism, Hyp for the size of the difference on our scenes). This is TASK-168's
territory, and their model is evidence that "several palettes in one batch" is the right shape: bind the palette, keep the
run.

### 5.8 Where we are ahead (do not regress these)

- Content-independent texture identity by block hashes (IN-453) beats their hash cache, which hashes whole uploads.
- Palette rows looked up in the shader (IN-409) is the same idea as their paltex and cheaper than their GPU palette
  conversion for our case.
- Compute fills (IN-442), specialised per-state pipelines (IN-437), PGO/ThinLTO (IN-473), the shader cache (IN-489) and
  pre-compilation (IN-507). They have a pipeline cache, but their pipelines compile on the GS thread and their per-draw state
  is not specialised the way ours is (C: `VKShaderCache.cpp` has no compile thread; our IN-437 measured 1x 2.8 -> 2.1-2.3 ms).
- 16 slots and a constant submission cadence: we overlap CPU and GPU far more than they do (they submit once per frame).
- The second present queue, the pacer thread, latency control and the frametime log (IN-514..523) are ahead of their
  present path; their only edge there is the pre-acquire.
- Frame skip and the 30 FPS mode exist and are validated (IN-524/525); they have duplicate-frame skip and a max-FPS cap.

---

## 6. Opportunities, ranked

Each item: our code path, expected effect on weak/mobile hardware, effort/risk, whether it changes the faithful image, the
gate, the smallest proof, and the ledger cross-check. Items already planned or already measured are marked as such and are
**not** proposals to redo.

| Rank | Item | Status | Gate | Effort |
|---|---|---|---|---|
| 1 | Pass and barrier scope on tilers (TASK-182) | already planned | faithful, exact | in progress |
| 2 | Pre-acquire the next swapchain image on the single-queue present path | new | faithful | small |
| 3 | Pending-readback queue with per-page validity, where lateness is accepted (photo mode, TASK-162) | new | Performance Mode / photo mode | medium |
| 4 | Per-device cache and age table | new, mobile-first | faithful | small |
| 5 | Newest-fence readback wait + bounded spin | already on `task/182-barrier-scope` / `release/1.3.0-rc15` | faithful | a port, not a design |

### 6.1 (already planned, ranked first) Pass and barrier scope on tilers - TASK-182

**Our code.** Full barriers (`core2_gpu.cpp:1552`), the deferred batches and their pass grouping (`:2009` `flush_draws`,
`:2086` `end_render_pass`), the fragment barrier for the plain path (`:1937`, `:1941`, `:1953`, IN-496), the hazard tracker
(`pendingReads`/`pendingWrites`, `:1681`).

**What their design says to copy (concept, not code).** (a) One pass per run of draws with the same attachments, load ops
derived from the target's validity; (b) a feedback read declared for the draw that needs it instead of a copy or a
full-image barrier; (c) the narrowest barrier that orders the two commands that actually conflict; (d) on tilers, fewer
submits, because each submit sleeps in the driver (their `DoRenderHW` comment: "every submit sleeps on the driver on
Android's Adreno drivers, and the governor answers the sleeps by lowering the GS thread's clock").

**Effect on mobile.** Large and measured in the direction: our own S23 numbers are 750 fragment barriers / 75 full barriers
/ 20 passes per VBlank at 11.5 ms GPU at 1x and 2x (C). TASK-182's step 3 already measured `GT4_CORE2_SUBMIT_EVERY=128` on
the S23 showroom: +11.6% VBlanks/s and 32% fewer render passes (C, private `TASKS.md`). Desktop: small, as IN-419/420/422
showed (3-13% each), and it must not be re-measured there as if new.

**Effort / risk.** In progress on `task/182-barrier-scope`; the risk is exactness (a wrong barrier is a wrong pixel), which
is why it carries the frame-identity gates.

**Gate.** Faithful (exact) if the gates pass; no Performance Mode split.

**Smallest proof.** Per VBlank: full barriers, fragment barriers, render passes and descriptor writes, on Hong Kong and
Motegi at 2x and the S23 at 1x/2x, against the frame diff. Already the shape of TASK-182 step 1.

**Ledger cross-check.** IN-419 (barriers only on real dependencies), IN-420 (one pass per batch run), IN-422 (decode
without GLM/barrier), IN-464 (GLM hazards per 8 KiB page), IN-496 (no interlock for large disjoint batches) are the kept
steps; IN-523/IN-438 (ROP) are the measured dead end on desktop. This item is the *continuation* of the kept line, not a
repeat of the reverted one.

### 6.2 Pre-acquire the next swapchain image on the single-queue path - new, small

**Our code.** `hw_gs_backend.cpp:1383` `present_frame`, acquire at `:1419` with `UINT64_MAX`.

**What they do.** After `vkQueuePresentKHR`, immediately `AcquireNextImage()` and cache the result, so the next
`BeginPresent` almost never blocks; the acquire also has a finite timeout so a destroyed Android surface cannot wedge the
VM (C, `GSDeviceVK::SubmitCommandBuffer`, `VKSwapChain::AcquireNextImage`).

**Effect on weak/mobile.** Removes the FIFO acquire stall from the thread that also answers reads, on devices that have one
queue (most phones). The ledger measured that stall at 60 Hz on the virtual display: showroom waits 11.42 ms with the old
path (8 slots) and 0.01 ms with the second queue (16 slots) (IN-521). A phone with one queue has no second queue to fall
back on.

**Effort / risk.** Small (a day with the window tests). Risk low: the image is only held; the present path and the
composition are untouched. The bounded timeout is a robustness fix on its own.

**Fidelity.** No image change.

**Gate.** Faithful.

**Smallest proof.** Window, 60 Hz, showroom and race start, `GT4_HW_FRAMETIME_LOG`: the per-VBlank wait at present falls to
~0 and the presented intervals are unchanged or better; frames identical; the "presentations not shown" counter does not
rise.

**Ledger cross-check.** IN-514/515/516/517/521/522/523 covered the queue *count*, the second queue, latest-ready and the
latency control. None of them pre-acquires on the one-queue path; IN-515 (latest ready) is a different idea and was
dropped. The queue-control allowance (IN-521) and its latency cost are untouched by this.

### 6.3 A pending-readback queue with per-page validity, for the places where lateness is already accepted - new, medium

**Our code.** `AnswerDelayedDownload` (`core2_renderer.cpp:2746`) answers from the previous same download, declining on
several conditions, with no page generations and no queue; `ReduceLater`/`TakeReduced` (`core2_gpu.cpp:4178`,
`core2_renderer.cpp:2760`).

**What they do.** Queue the copy, capture page generations, publish at VSync, newest snapshot wins, drop stale snapshots,
fixed latency for 1x1 visibility queries (C, section 2.3).

**Where it is legitimate for us.** Only where the owner already accepted lateness: **photo mode** (TASK-162's
one-frame-late depth download, owner's word) and the existing Performance Mode delayed download (IN-439) if it is kept.
Not normal mode: decision 0073 and IN-439/514.

**Effect on weak/mobile.** Photo mode's 3-4 ms of GPU waits per game frame at 6x on the RTX 3060 (C, TASK-134/IN-537) would
collapse to a queue retirement; the EE stops waiting. On a phone the same wait is worse (owner: 1.4-4 ms/VBlank readback
waits in normal mode, C).

**Effort / risk.** Medium (2-3 days): a queue with generation capture, a publish step at VBlank, and a staleness rule. Risk
medium: an answer that is one frame old must be acceptable to the consumer - that is a per-use decision, not a global one.

**Fidelity.** Changes the timing of the answer, not the pixels; Performance Mode / photo mode only.

**Gate.** Performance Mode and photo mode.

**Smallest proof.** Photo mode: `[core2] gpu_wait` at the depth download to ~0, EE time at the read to 0, and the picture
identical (the answer is one frame old by design); a test that a stale snapshot is dropped when the page was rewritten.

**Ledger cross-check.** IN-439 (delayed downloads, PM only), IN-514 (one-frame-late, rejected for normal mode), IN-537/539
(Z16 into the GLM and whole-block GPU stores, off by default). This is the *mechanism* (validity + queue) those lack, not a
new lateness policy.

### 6.4 Per-device-class cache and age policy (a table, like theirs) - new, small, mobile-first

**Our code.** One global `Age()` (`core2_renderer.cpp:4949`), a texture budget by integrated/discrete (IN-431), the
submission cadence by integrated/discrete (`core2_gpu.cpp:2472`).

**What they do.** A per-GPU table: texture pool size, target age and texture age in frames, keyed by GPU model and driver
(C: `GSGPUProfileMali.cpp`, `GSGPUProfileAdreno.cpp`, `MobileGsTuning`), plus a driver-bug/workaround table (C:
`GSGPUProfile.h`).

**Effect on weak/mobile.** Target age drives how often we read targets back and how much memory we keep; on a phone with
4-8 GB shared, both matter. Their table is the evidence that the right value is device-class specific, not one global
constant.

**Effort / risk.** Small (a table plus a device query), risk low; it changes no pixels as long as the gates hold.

**Fidelity.** No image change if the age is long enough for correctness (the existing grace test does that); a too-short age
shows up as readbacks and stale targets, which the frame gates catch.

**Gate.** Faithful, with the identity gates.

**Smallest proof.** On the S23: readbacks per VBlank, GPU waits and memory high-water at 1x/2x for two age values, frames
identical. On the desktop: unchanged behaviour.

**Ledger cross-check.** IN-431 (budget by class), IN-509 (grace in VBlanks), IN-512 (target ageing rule), IN-496 (device
default). Not a repeat: those are single knobs; this is the per-device table and the age policy together.

### 6.5 Readback as "newest fence only" plus a bounded spin - already on the release branch, not on main

**Our code (main).** `wait_all` (`core2_gpu.cpp:2143`), no spin (`hw_gs_options.cpp:243` keeps the switch off).

**What exists elsewhere in our tree.** `task/182-barrier-scope` has `GT4_CORE2_WAIT_NEWEST_FENCE` (on) and the bounded spin
(`GT4_HW_READBACK_SPIN`), with new counters; `release/1.3.0-rc15` has the "K batches after a waited readback" rule
(`GT4_CORE2_SUBMIT_AFTER_WAIT`). Their design is the same three ideas (fence counter, spin with calibrated timestamps,
armed submission) (C, section 2.2/2.4).

**What this means.** Do not re-implement or re-propose them on main as new: they are a merge/port question, not a design
question. The doc records the mapping so the next agent does not read this study and "discover" them.

### 6.6 Things this study deliberately does not propose

See the table in section 8; it lists every item checked against the ledger and the two audits.

---

## 7. Mobile and tile-based GPUs

### 7.1 What a tiler charges that a desktop does not

- **A render-pass boundary is a tile load and a tile store.** 20 passes per VBlank on the S23 = 20 full-surface tile
  round trips per frame (C for the count; the cost model is standard tiler behaviour, H). Their scheduler exists to cut
  this (C, `GSPassScheduler`'s header: 822 target switches -> two passes in the motivating title).
- **A storage-image barrier can push the driver out of tile mode**, and even in-pass barriers cost (C/H, our own TASK-182
  statement: "in-pass barriers can push the driver out of tile mode"; their `cheap_rt_feedback_read` flag exists precisely to
  say "a feedback read costs nothing structural - no render-pass break, no tile flush", C: `GSDevice.h`).
- **Fixed-function blend and depth are cheaper than a shader RMW**, and they keep framebuffer compression available (H;
  our own IN-523/IN-438 measurements are desktop and say nothing about a tiler).
- **Every submit can sleep in the driver** on Adreno, and the governor lowers the submitting thread's clock in response
  (C: their `DoRenderHW` kick comment; our TASK-182 measured `SUBMIT_EVERY=128` as +11.6% VBlanks/s on the S23 showroom,
  C). Fewer, larger submissions are better on a phone - the opposite of a desktop where the flare read wants the GPU
  started early.
- **Bandwidth**: a phone's shared memory means a target readback costs the whole system, not just the GPU; their async mode
  and their cache sizes are tuned for that (C for the mechanism, H for the weight).

### 7.2 Their mobile choices worth understanding (concepts only)

1. **One pass per run, load ops from validity** (C). Directly applicable to our TASK-182.
2. **Feedback loop declared, not copied** (C). Applicable where our self-copy costs `+6%` GPU at 12x (IN-463); on a tiler
   the copy is worse than on a desktop because it is pure bandwidth.
3. **Per-GPU pool/age table and driver-bug table** (C: `GSGPUProfile*`). Applicable as in 6.4.
4. **No mid-frame submits except an armed kick** (C). Our constant cadence is desktop-tuned; a tile/unified-memory profile
   should raise it (TASK-182's K rule does exactly that: 8 batches after a waited readback on a tiler, 0 on a discrete GPU,
   C).
5. **Push descriptors are off on Mali** because their driver crashes inside the push call; per-frame descriptor sets are
   used instead (C: `GSDeviceVK.h` `UsePushDescriptors`, `GSGPUProfile` driver bugs). A device-quirk table of our own will
   be needed for Android; ours is one `integrated` boolean today.
6. **Pre-rotation of the swapchain images** so SurfaceFlinger does not compose the game layer on the GPU (C: `VKSwapChain.h`
   `GetPreRotation`). We have a rotation-safe present (TASK-179) but not this reasoning; worth checking on the S23's
   "every present reports suboptimal" (C, owner's log bundle).
7. **Fixed latency for 1x1 visibility queries** (3 frames) instead of "whatever the fence delivers", so a steady effect
   does not oscillate (C). If we ever defer the flare table, this is the rule that makes it not flicker.
8. **Native-fence export for remote play** instead of a readback (C: `GSDeviceOGL.cpp`, `EGL_ANDROID_native_fence_sync`).
   Out of scope here, but it is the mobile answer to "get the frame to the encoder without a copy".

### 7.3 Which of those would change our picture, and which cannot be faithful

- Pass/barrier scope, fewer submits, pre-acquire, per-device tables: **faithful**, no image change (6.1, 6.2, 6.4).
- Attachment blending/depth (ROP) on tilers: **not faithful** in general - their blend level and ROP path round differently
  from the GS, which is why our IN-523 measured a difference and why GSDX-PLAN's G5/G6/P3 were measured and dropped. On a
  phone this would be a Performance Mode decision with the owner's picture test, not an optimization to sneak in.
- Async readback: **not faithful** (a delayed answer). Performance Mode / photo mode only.

### 7.4 What we do not know about the S23 comparison

The private Android build's GS settings choose the **OpenGL** renderer by default (`ApplyBaseSettings` in
`android/armsx2gs/gt4gs_host.cpp`: `Renderer = OGL`) and turn **vsync off** (`VsyncEnable = false`, `FrameLimitEnable =
false`), so the measured GSdx there was most likely the GL backend, single-threaded (the back thread requires Vulkan and is
refused otherwise, C: `GSBackThreadPolicy.h`), unpaced. That matters: the comparison is "our Vulkan renderer with vsync and
a 1-4 ms readback chain" against "their GL renderer with no vsync and no back thread", not against the pipelined Vulkan
design this document mostly describes. Unknown U1.

---

## 8. Ledger cross-check: what this study does not propose, and why

Read before proposing: `docs/hw-renderer-spec/CORE2-PERF-LEDGER.md` (all of it), `docs/tasks/COREGS2-PERF-AUDIT.md`
(TASK-215b), `docs/tasks/COREGS2-PERF-BASELINE.md` (TASK-215), `docs/GSDX-PLAN.md` C.7 and the owner answers,
`docs/hw-renderer-spec/CORE2-CPU-SURVEY.md`, `docs/GS-READBACK-STUDY.md`, `docs/COREGS2-ROADMAP.md`,
`docs/FRAME-COST-STUDY.md`.

- **A note on the brief's `COREGS2-WAITS-AUDIT.md`**: it does not exist in this repository. `origin/docs/coregs2-perf-audit`
  carries `docs/tasks/COREGS2-PERF-AUDIT.md`; `origin/task/215-coregs2-perf` carries
  `docs/tasks/COREGS2-PERF-BASELINE.md`. No branch listed in this worktree has a WAITS audit. The waits analysis used here is
  the ledger's readback sections plus the baseline's per-scene numbers. (Unknown U2.)

| Not suggested | Where it was tried | Why not |
|---|---|---|
| Content-keyed texture cache (hash every upload) | IN-413 | reverted: 2 MB/VBlank of hashing; a decode is ~10 us |
| Dropping repeated identical uploads, block versions | IN-412 | reverted, decodes unchanged |
| Bigger submission intervals (64/128) globally | ledger R1 | GPU starts later, the flare read waits longer, wall worse; TASK-134 kept 64 discrete / 32 integrated |
| Halving barriers / removing render passes as a goal in itself | IN-419, IN-420, IN-422, IN-464 | kept, but each gave 3-13%; the remaining idea is *scope per hazard on tilers* (TASK-182), not "fewer" |
| ROP / depth attachment / real depth probe | IN-523, IN-438, GSDX-PLAN G5/G6, ledger "dropped" | measured: ROP 12x 19.73 -> 21.37 ms of GPU busy; "a ROP path would not speed up this GPU"; G5 -3.6%, P3 -1% in game, G6 no difference (audit section 4) |
| Read skipping at 12x | IN-436 | 21.3 -> 21.1 ms; not bandwidth, not clocks |
| D1 (hardware raster path) as the next GPU lever | IN-421, IN-472 | fragment stage 0.4-0.7 ms at 3x; exact no-feedback draws are 20-23% of the area, ~4% left after fills; decided with the laptop test |
| Performance Mode without GT4's colour filter | IN-505 | -4.4% by day on the HD 620 but the race colours change (up to 24 levels); the owner left it out |
| Large textured sprites as compute | IN-443 | 20.7 -> 20.8 ms, bound by exact bilinear |
| Hardware bilinear / gather | IN-444, IN-449 | PM-only 0.5 ms at 12x, car paint differs; gather +2.6% |
| Whole clears via `vkCmdClearColorImage` | IN-454 | Z24 read-modify-write remains; reverted |
| GLM patch kernel with shared words | IN-460 | 12x unchanged; reverted |
| Self-copy off in normal mode | IN-463 | 12x frames differ run to run; kept for exactness |
| Self-copy off in Performance Mode from 8x | IN-543 | no self-copies at 12x in that scene; reverted |
| One-download-late flare answer in normal mode | IN-439, IN-514, decision 0073 | the owner rejected the lateness |
| Flares hidden / road sheen off / alpha fills off | IN-441, IN-501/502 | kept as Performance Mode effect toggles only; not a faithful win |
| CLUT-load scope test (skip a flush the load cannot reach) | IN-541 | 54-72 loads skipped per VBlank, total batch count did not move: no gain |
| Flush-reason split (test/scissor/xyoffset) as a *change* | IN-542 | measured as diagnostics: scissor 0.9-1.1, xyoffset 0.0 per VBlank on the night race; license xyoffset 4.7-12.0 |
| Photo-mode Z16 into the GLM / whole-block GPU stores | IN-537, IN-539 | 57-58 FPS against 59.6-60.1 at 6x: measured, off by default |
| MTVU journal slot rings | IN-448 | no measurable gain; reverted |
| Thread pinning | IN-488 | no gain |
| AVX2 | IN-445 | no gain |
| Interlock off for large disjoint batches / per-primitive grouping | IN-466, IN-471, IN-496 | kept, orthogonal to this study |
| Submission thread, TASK-168, TASK-171, TASK-162 | audit section 2.8 | already planned, larger than anything here |
| A GPL component in the release executable | decision 0070 | never |

Also, from the audit and the baseline, **do not** re-measure 600-VBlank stage averages without a question: the ledger's
"What we learned" is explicit that the CPU stages come from different candidates and that stutters are spikes, and the
baseline's own run is 9.0%/11.4% faster than the GSdx reference on the desktop, which is the number to beat.

---

## 9. Unknowns

- **U1. Which renderer and threading the owner's S23 comparison used.** The private Android build defaults to the OpenGL
  renderer and vsync off, where the front/back split is refused; if the owner's session file or extra lines switched to
  Vulkan, the comparison included the back thread. This changes what "GSdx is faster" is evidence for. (C for the defaults,
  Unknown for the actual session.)
- **U2. `COREGS2-WAITS-AUDIT.md` does not exist** (see section 8). If a waits audit is written later, section 5 and 6 here
  must be reconciled with it.
- **U3. Their per-pass cost on Adreno 740 is not measured by us.** We know our 20 passes / 75 full barriers / 750 fragment
  barriers cost 11.5 ms at 1x and 2x; we do not know what the same frame would cost with their pass count. The size of the
  mobile gap is therefore H, not C.
- **U4. Whether the 1x1-visibility 3-frame latency is enough for GT4's flare animation.** GT4 reads one 2 KiB table per
  frame, not 1x1; their rule is written for a different shape. Any deferral we consider needs its own latency study (the
  owner rejected lateness in normal mode anyway).
- **U5. Whether their `Asynchronous` mode is visually acceptable to the owner in any mode.** Related to U4; the decision is
  the owner's eye, not a counter.
- **U6. The cost of a faithful attachment path on a tiler.** Our ROP path exists but was only measured on the desktop at
  12x (IN-523). Nobody has measured it on the S23.
- **U7. The number of command buffers they actually keep busy on Android.** The code is 3 with 2 in flight, but the
  kick's own comment says only ~2 kicks per frame land because the next buffer is still executing; on a phone with a slow
  driver the effective depth may be 1. Unknown without a trace.
- **U8. Whether our `wait_all` (main) versus `wait_newest` (release branch) changes any measured number.** The release
  branch has the switch and counters; no page here quotes an A/B.

---

## 10. Evidence, method and limits

Read (their side): `third_party/armsx2/pcsx2/GS/GS.cpp`, `GSState.cpp`, `GSBackQueue.h`, `GSJobQueue.h`, `GSClut.*`,
`GSTextureCache.*`, `Renderers/Common/GSDevice.{h,cpp}`, `GSPassScheduler.h`, `GSBackThreadPolicy.h`, `GSGPUProfile*.{h,cpp}`,
`GSGPUProfileMali.cpp`, `GSGPUProfileAdreno.cpp`, `GSTexture.{h,cpp}`, `GSRenderer.{h,cpp}`, `GSFieldShiftDetector.cpp`,
`Renderers/HW/GSRendererHW.{h,cpp}`, `Renderers/HW/GSTextureCache.{h,cpp}`, `Renderers/Vulkan/GSDeviceVK.{h,cpp}`,
`Renderers/Vulkan/GSTextureVK.{h,cpp}`, `Renderers/Vulkan/VKSwapChain.{h,cpp}`, `Renderers/Vulkan/VKStreamBuffer.cpp`,
`Renderers/OpenGL/GSTextureOGL.cpp`, `pcsx2/MTGS.{h,cpp}`, `pcsx2/Config.h`, and the Android host
`android/armsx2gs/gt4gs_host.cpp`.

Read (ours): `native/hw_gs/core2/core2_{renderer,gpu}.cpp/.hpp`, `frontend/hw_front_end.cpp`, `clut/hw_clut.cpp`,
`hw_gs_stream_sink.cpp`, `hw_gs_backend.cpp`, `hw_gs_options.cpp`, `native/window.cpp`, plus the ledger, the two audit pages,
the baseline, `docs/GSDX-PLAN.md`, `docs/GS-READBACK-STUDY.md`, `docs/COREGS2-ROADMAP.md` and the current `TASKS.md`
tail.

Not done, by the brief: no build, no benchmark, no game run, no code/pin/test/data change, no PR, no release, no Telegram,
no owner data, no Y:/Z:.

Limits: their tree is a fork with local additions (the back thread, the async download mode, the pass scheduler, the GPU
profile tables) that upstream PCSX2 does not have; where a statement depends on that, it says "ARMSX2". All their numbers
quoted here come from their code comments (their own measurements on their machines) and are labelled as such; none was
reproduced by us. Our numbers come from the TASK-215 baseline and the ledger, each cited. The two sides were never measured
in the same conditions except the desktop black-box comparison in the baseline, which is the only like-for-like number in
this document.
