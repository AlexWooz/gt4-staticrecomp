# CoreGS 2.0 study: unified memory (UMA): every data path, what shared memory allows, and a phased plan

Date: 2026-11-05. Read-only planning study for TASK-027 (CoreGS 2.0) and TASK-180 (Android). No code changed, nothing built
or run. Agent status: **not PCSX2-tainted** (no PCSX2 or GSdx source, no `gt4recomp-gs` fork, no `private/tooling/pcsx2-*`,
no `private/slice189-pcsx2-ref`, no oracle transcript opened). Outside sources: the Vulkan specification, the MoltenVK
source (`MVKDevice.mm`) and Qualcomm's public Adreno best-practices page.

Code read (line numbers are of these trees):
- **Renderer:** worktree `C:\Antigravity\gt4-wt-178-download-wait`, branch `task/182-barrier-scope` at `e7ebb20b`
  (`native/hw_gs/core2/*`, `writeback/*`, `hw_gs_backend.cpp`, `hw_gs_stream_sink.cpp`, `hw_device_profile.hpp`,
  `hw_gs_vulkan_loader.cpp`, `frontend/hw_transfers.cpp`, `native_vulkan.cpp`).
- **Android:** worktree `C:\Antigravity\gt4-wt-180-android`, branch `task/180-android-emulator` at `9113a9e7`.
  - Read: `hw_gs_backend.cpp` (present path, swapchain), `docs/ANDROID-PORT-PLAN.md` and `docs/ANDROID-PERFORMANCE-PLAN.md`.
  - Where a line number is from this tree it says "(android)".

Labels: **C** Confirmed (file:line, or a number from a named log or study), **H** High confidence, **Hyp** Hypothesis,
**U** Unknown. "Exact" means: game logic, physics, timing, AI, saves and everything the EE or IOP reads back are unchanged,
and the normal-mode picture is bit-identical (the gates of section 7).

Phone evidence used (from the owner's S23 Ultra logs, as given in the brief):
- **Memory types** `0:L 1:L 2:L 3:LVH 4:LVC 5:LVC 6:LVCH 7:L 8:L`.
- **Nordschleife race, 2x, Performance mode:**
  - GS thread about 8.7 ms per VBlank;
  - GPU busy 11.5 ms at both 1x and 2x;
  - MTVU 4.48 ms;
  - EE critical path 7 ms;
  - `unchanged_uploads=137200` and `uploads_replayed=201177 (11870 MiB)` over the session.
- **Menus at 6x:** compose and deinterlace 43 ms.
- **Normal mode:** readback waits.

---

## 0. Summary

**The one-paragraph answer.** On the S23 every host-visible memory type is also device-local, and CoreGS 2.0 already puts
every CPU-written stream in host-visible memory that the GPU reads in place:
- vertices, uniforms, decode windows, palettes, patch words and fill states;
- that is, one CPU write and no GPU copy (C, sections 1-2).

The copies that remain are few and mostly small:
- GLM page uploads;
- CLUT writes;
- texture-replacement uploads;
- the readback memcpys;
- **one large group: the presentation chain.** It copies the final picture three or four times at screen size, on command
  buffers that no CoreGS counter sees.

So "make CoreGS UMA-aware" buys less CPU time than hoped. It is mostly bandwidth, heat and memory footprint, plus the
foundation for the asynchronous readback. The phone's large costs remain:
- per-pass and per-barrier GPU overhead in races (TASK-182);
- full-image post passes in menus;
- the readback wait in normal mode.

None of these is a copy that shared memory removes.

**Top findings.**

1. **(C) CoreGS is already "zero-copy" for its streams on the S23. (H) The CPU-side gain from more UMA work is small:**
   - every ring buffer is host-visible plus coherent, and on this phone that means device-local too;
   - the GPU reads them directly (`core2_gpu.cpp:548-570`, `3019-3068`);
   - what is left is the CPU memcpy into the ring, not a staging-then-copy;
   - the remaining CPU memcpys (vertices, windows, GLM pages) are likely a few MB per VBlank;
   - so the GS-thread gain is likely **0.1-0.5 ms per VBlank** (Hyp; phase 1 measures it).
2. **(H) The biggest copy cost is the presentation chain, about 110 MB per presented frame at the S23's 3088x1440:**
   - PresentScale writes `presentOut`;
   - a copy to an RGBA8 image (`hw_gs_backend.cpp:1151`);
   - a full black clear (`1238-1240`);
   - a blit to the picture (`1217`);
   - on the two-queue path, a second copy to the swapchain image (`1272`);
   - writing the final pass straight into the swapchain image, folded into the planned pre-rotation work, removes about
     95-110 MB per frame (about 6 GB/s at 60 fps; H for the arithmetic, Hyp for real DRAM bytes after UBWC);
   - it removes **an estimated 0.7-2.5 ms of GPU per frame** that no CoreGS counter sees today (Hyp; phase 1 adds the timer).
3. **Upload rings use cached-coherent memory on the phone (type 6 `LVCH`), a rule tuned on an Intel HD 620 (IN-476).**
   (C for the rule, `core2_gpu.cpp:3021-3025`; H that the S23 takes it, because Adreno reports an integrated GPU. The
   S23's `[core2] memory types` line ends with `upload buffers prefer cached` or `first visible`, which settles it.)
   - on a tiler the IO-coherent path may cost GPU snoop bandwidth;
   - the uncached `LVC` type is the usual mobile choice for CPU-write/GPU-read streams (H for the guidance, Hyp for the S23);
   - **the A/B switch already exists** (`GT4_CORE2_CACHED_UPLOAD=0`) and costs nothing to try in test build 4.
4. **(C) Readbacks lack the host-domain barrier the Vulkan spec requires before a host read of device writes:**
   - core 2 ends each readback with an `ALL_COMMANDS` to `ALL_COMMANDS` memory barrier (`core2_gpu.cpp:1818-1823`);
   - no `HOST` stage, no `HOST_READ` access;
   - the old core does it right (`hw_gs_writeback_gpu.cpp:3170-3171`);
   - it works on desktop drivers; it is latent risk on tilers and MoltenVK;
   - the fix is exact and ten lines (phase 1).
5. **(C/H) UMA cannot shorten the normal-mode flare wait, but it makes the cure cheap:**
   - the wait is a real data dependency: scene depth, then the 2 KiB table, then VU1;
   - what helps is keeping the GPU fed and its clock up: the TASK-178 K rule (already built, off by default), a denser
     submission cadence right before the read, a short spin on the fence;
   - later, an asynchronous read in which the GPU writes the table and a sequence word into coherent host memory the VU1
     worker reads;
   - on UMA that last step has no copy at all, but it needs the deferred write-back layer that made O3 non-exact (high risk).

**Phase 1 (handoff for Android test build 4), low difficulty, about 3-4 days, exact.**
- **Memory-shape detection:** discrete, unified all-local or unified split, with a diagnostic override.
- **A placement report:** which memory type each core 2 buffer and image got.
- **Per-path byte counters:** CPU-to-GPU bytes by kind, GPU copy bytes, host-read bytes, ring high-water.
- **The spec-correct host-read barrier.**
- **An optional GPU timer** around the backend's presentation command buffers.

No rendering change. It measures what phases 2-5 would win. The free gains for build 4 come from existing switches (phase 0,
section 7.1).

---

## 1. Inventory of every CPU and GPU data path

### 1.1 Where each object lives today

`make_buffer` always requires `HOST_VISIBLE | HOST_COHERENT` and then prefers the extra flags it is given
(`core2_gpu.cpp:534-570`). `memory_type` takes the first type, by index, with `wanted | preferred`, then with `wanted`
alone (`534-546`). Images, the GLM and the GPU CLUT require `DEVICE_LOCAL` with no preference (`775`, `3328`, `3403`). On
an integrated device (`VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU`, the rule `hw_device_profile.hpp:86`, which Adreno meets) the
upload rings prefer `HOST_CACHED` (IN-476, `core2_gpu.cpp:3021-3025`).

| Object | Size | Wanted / preferred (code) | S23 type it gets (from the logged list) | Discrete desktop (H, typical AMD/NVIDIA ordering) |
|---|---|---|---|---|
| staging ring (per share 32 MiB) | 8 shares = 256 MiB on integrated (`45`, `hw_device_profile.hpp:160-161`) | V C / H on integrated (`3066`) | **6 `LVCH`** (H: under the integrated rule; C: it is the first V+C+H type, since 3 `LVH` is not coherent; 4 `LVC` without the rule) | first V C type: system RAM, write-combined |
| vertex ring (per share 8 MiB) | 64 MiB | same (`3067`) | **6 `LVCH`** | system RAM |
| uniform ring (per share 2 MiB) | 16 MiB | same (`3068`) | **6 `LVCH`** | system RAM |
| readback buffer | 64 MiB (`48`; plan may cut to 16) | V C / H (`3080`) | **6 `LVCH`** | V C H system RAM |
| `later` buffer (IN-439) | 64 x 1024 words = 256 KiB (`335`, `3081`) | V C / H | **6 `LVCH`** | V C H |
| `glmTables` (address tables) | small, static | V C / none (`3341-3342`) | **4 `LVC`** | V C system RAM, read by every decode over PCIe (Hyp: small cost, cached in L2) |
| `inverseCt32` | 8 KiB, static | V C / none (`3416`) | **4 `LVC`** | same |
| GLM (GS memory on the GPU) | 4 MiB storage buffer | L (`3328`) | first `L` type allowed (H: 0) | VRAM |
| GPU CLUT | 2 KiB | L (`3403`) | H: 0 | VRAM |
| images (targets, textures, palettes, post) | `R32_UINT`, `OPTIMAL`, `STORAGE | TRANSFER_SRC | TRANSFER_DST`; with IN-481 also `MUTABLE_FORMAT | COLOR_ATTACHMENT` (`750-769`) | L, from 64 MiB blocks (`655-700`, `775-784`) | H: 0 | VRAM |
| atlas | 4096 x 4096 x 4 B = **64 MiB** (`50`, `3427`) | an image | as images | VRAM |
| backend `upload` image (present) | window rect, `R8G8B8A8_UNORM`, mips, `TRANSFER` only (`hw_gs_backend.cpp:542-566`) | L | H: 0 | VRAM |
| backend `upload` buffer | width x height x 4, V C (`523-540`) | V C | 4 `LVC` (allocated, unused on the core 2 path: H) | system RAM |
| backend pictures (two-queue path) | 3 x window size, swapchain format (android `613-640`) | L | H: 0 | VRAM |

Readings:
- **(C)** On the S23 all four rings and both readback buffers are in **type 6, cached and coherent** (IO-coherent).
  - Every object is device-local, because every visible type is.
  - So there is no "upload heap" to copy out of: the GPU reads the rings where the CPU wrote them.
- **(C)** About 400 MiB is mapped on a phone: 256 + 64 + 16 + 64.
  - This is the plan's cap (`hw_device_profile.hpp:157-166`).
  - It is reduced only if a quarter of the heap budget cannot hold it.
  - It is not reduced on a 12 GB S23 (H).
- **(C)** The atlas is a 64 MiB image that core 2 never uploads into: `UploadAtlas` has no caller in `core2/`. It is the
  default binding for bindings 3 and 4 when a batch has no texture (`core2_gpu.cpp:3625-3626`). Whether any draw ever reads
  it is **U**, so shrinking it is not yet provably exact.
- **(C)** The `[core2] memory types` log line prints only L, V, C and H (`3073-3075`). Types 1, 2, 7 and 8 on the S23 are
  probably lazily-allocated or protected types (Hyp), which the line does not show.

### 1.2 The data paths, one by one

Columns: direction; memory today; copies (CPU memcpy, `vkCmdCopy*`, compute); bytes per VBlank and the counter that has
them; the synchronisation; the code.

| # | Path | Direction | Memory today | Copies today | Bytes per VBlank (counter) | Synchronisation | Code |
|---|---|---|---|---|---|---|---|
| T1 | EE GIF chunks (paths 1-3, incl. path-3 image data) | EE thread to GS worker | 16 MiB `std::vector` ring | 1 memcpy into the ring (EE) | `[hw] transport ... gif_kib=` (C, `hw_gs_stream_sink.cpp:337`) | SPSC atomics and doorbells; VBlank back-pressure, 2 frames ahead | `hw_gs_stream_sink.cpp:559-672, 895-908` |
| T2 | VU1 (MTVU) GIF output | VU1 worker to GS worker | MTVU SPSC ring | none: executed in place (IN-461) | `mtvu_records` | ring and bell | `hw_gs_stream_sink.cpp:833-868` |
| T3 | IMAGE transfer bytes | GS worker | ring payload, or `TransferUnit::buffer_` | none when the whole transfer is in one run (IN-365); else a `vector::insert` | none | in stream order | `frontend/hw_transfers.cpp:139-157` |
| U1 | EE upload into the GS memory mirror | CPU only | 4 MiB CPU mirror (`memory_.Local()`) | hash of the bytes (IN-486 memo key), then a replay (identity compare, memcmp, memcpy per 256 B block) or the swizzle (`FastHostToLocal`) | **`swizzle=... (KiB)`: S23 2446 KiB, 1.12 ms**; `upload=... (n)`: 1.26 ms for 104 uploads (C, brief); `uploads_replayed (MiB)`, `unchanged_uploads` (cumulative) | none on the GPU: the mirror is CPU-only | `core2_renderer.cpp:3220-3382`, hashes `311-352` |
| U2 | GLM page upload | CPU to GPU | staging ring (type 6) to GLM (L) | memcpy per 8 KiB page into staging, then `vkCmdCopyBuffer` staging to GLM | `glm=... (calls, pages)` per VBlank (C, `5631-5637`); bytes = pages x 8 KiB | GLM page hazards (IN-464) and full barriers | `MakeGlmCurrent` `1549-1591`; `Gpu::UploadGlm` `core2_gpu.cpp:4265-4288` |
| U3 | Decode window (IN-422) | CPU to GPU | staging ring | memcpy of up to 256 pages (2 MiB) into staging; the decode **reads staging directly** | `window_decodes`, `window_mb` (cumulative; C `5329`) | none: no GLM hazard, the snapshot is private | `bind_window` `core2_gpu.cpp:253-276`; decision `core2_renderer.cpp:1784-1800` |
| U4 | Texture decode | GPU | GLM or window to an `R32_UINT` image | compute (`hw_glm_decode.comp`) | `decodes*` counters | `touch("decode")`, GLM pages | `DecodeTexture` `4290-4354`, `DecodeTextureGpuClut` `4426-4489` |
| U5 | CPU palette for a decode (palette modes 0-1) | CPU to GPU | staging | 1 KiB memcpy, read in place | none | none | `4306-4307` |
| U6 | Texture replacement (IN-470) | CPU to GPU | staging to image | memcpy in pieces of half a share, then `vkCmdCopyBufferToImage` | `replacement ... mb=` | `touch("upload image")` | `UploadImage` `3566-3597`; caller `core2_renderer.cpp:3855` |
| C1 | CPU CLUT mirror into the GPU CLUT | CPU to GPU | staging to CLUT (L) | the CPU writes 4 B per halfword into staging; `vkCmdCopyBuffer` | `clutWrites` (count) | CLUT buffer hazard (31% of full barriers in Hong Kong: STUDY-BARRIER-SCOPE 4) | `WriteClut` `4657-4676`; `FlushClutMirror` `core2_renderer.cpp:1256-1275` |
| C2 | CLUT from a target | GPU | image and GLM to CLUT | compute | `clutTargetLoads` | CLUT and GLM hazards | `ClutFromTarget` `4545-4607` |
| C3 | Palette row (IN-409) | GPU, or CPU to GPU | CLUT to palette image; or CPU words to staging | compute; or `Patch` of 256 words | `paletteRows` | row-granular image hazard | `4491-4543`; `core2_renderer.cpp:809` |
| C4 | GPU CLUT read for a CPU user (tone hook) | GPU to CPU | CLUT to readback (type 6) | `vkCmdCopyBuffer` 2 KiB, **waited submission** (drain) | `clutReads` | full drain | `ReadClut` `4678-4693`; `SyncClutToCpu` `core2_renderer.cpp:1278-1290` |
| D1 | Draw vertices | CPU to GPU | vertex ring (type 6) | 1 memcpy from the batch's vector; vertex fetch reads in place | none (32-byte `GpuVertex`; H) | ring shares per slot | `Draw` `3638-3645` |
| D2 | Draw state (uniform) | CPU to GPU | uniform ring | 256 B memcpy per batch, read in place (dynamic UBO) | none | none | `3642, 3657, 3866` |
| D3 | Descriptor writes | CPU to driver | descriptor pools per slot | `vkUpdateDescriptorSets` | trace: 800 calls / 4,139 writes per VBlank in Hong Kong (C, STUDY-BARRIER-SCOPE 5.3) | per-recording pools | `update_sets` `2703-2709` |
| F1 | Constant fills (IN-442) | GPU | state block in staging (256 B), target images | compute | `compute_fills`, `fill_mpx` | `touch("fill")`, whole image | `3872-3900` |
| F2 | Whole clears (IN-494) | GPU | images | `vkCmdClearColorImage` | `whole_clears` | `touch("clear")` | `3842-3857` |
| F3 | New or recycled image | GPU | image | `vkCmdClearColorImage` + full barrier | `clear_new_image` barriers 5.7 / 3.8 per VBlank (C, STUDY-BARRIER-SCOPE 4) | full barrier | `806-824` |
| W1 | Dirty rectangles (4.1) | GPU | GLM to target | compute (`PatchFromGlm`), after the GLM pages are made current (U2) | `dirtyPatches`, `dirtyPatchPixels` | image (whole) + GLM pages | `ApplyDirty` `core2_renderer.cpp:1964-1990`; `4695-4739` |
| W2 | CPU-word patches (IN-512 bit patch, CPU sprites, palette rows) | CPU to GPU | staging, read in place by compute | memcpy | `patches` | image (whole or one row) | `Patch` `4741-4772` |
| W3 | Target written into the GLM (IN-408) | GPU | image to GLM | compute | `materializations` | GLM pages | `MaterializeTarget` `4609-4655`; `core2_renderer.cpp:1466-1546` |
| W4 | Store before write (IN-411) | GPU to CPU | as R1 | as R1; a full drain | `stores before write` (TASK-178 line) | full drain | `StoreDrawnBeforeWrite` `core2_renderer.cpp:3024-3105` |
| R1 | Readback / download (flare table, keeps, moves) | GPU to CPU | image to readback buffer (type 6) by compute (`core2_reduce.comp`), then CPU | 1 dispatch; memcpy into a `vector`; `writeRect` swizzle into the mirror | `readback_pixels`; `px/readback_download`, `wait_ms/readback_download` (TASK-178 line) | full barrier, **`submit(true)` = wait for every slot**; no HOST barrier (finding 4) | `Readback` `core2_renderer.cpp:2841-2930`; `Reduce` `4774-4805`, `ReduceMany` `4807-4855` |
| R2 | Delayed download (IN-439, Performance mode) | GPU to CPU | image to `later` (type 6) | dispatch; fence poll later; memcpy | `delayed_downloads` | fence poll, no wait | `4865-4925`; `core2_renderer.cpp:2944-3008` |
| R3 | The flare table to the VU1 (IN-522) | GS worker to VU1 worker | 2 KiB table | `ReadLocalToHost` vector, memcpy into the posted table; the VU1 worker copies into guest memory | `[hw] flare replies` | the VU1 worker waits on the ticket | `ReadFifo` `core2_renderer.cpp:5518-5564`; `hw_gs_stream_sink.cpp:689-696, 802-808` |
| P1 | Self copy (spec 8.8) | GPU | image to pooled image | `vkCmdCopyImage`, or a draw inside the pass (R1, `GT4_CORE2_SELF_COPY_DRAW`) | `self_copies`, `self_copy_mpx`; Hong Kong 2x about 8,300 px per copy (C, STUDY-BARRIER-SCOPE 5.1) | full barrier, or two fragment barriers | `3700-3719` |
| P2 | Grow / copy image | GPU | image to image | `vkCmdCopyImage` (whole old image) | `targetsGrown` | full barrier | `3454-3521` |
| P3 | Display merge (compose) | GPU | targets to the display image (internal size) | compute | `composes`; GPU category `compose` | full barriers | `Compose` `4952-4988`; `ComposeGpu` `core2_renderer.cpp:5122-5293` |
| P4 | Deinterlace (480i) | GPU | display, history x4, woven, last output | 1-2 dispatches + 2 full-display copies (0 with R8 `GT4_CORE2_DEINT_FIELD_COMPOSE=1`) | `post_copies/vblank`; category `deinterlace` | 4 full barriers (2 with R8) | `5042-5126` |
| P5 | FXAA (opt-in) | GPU | display to FXAA image | dispatch | category `fxaa` | full barriers | `5128-5163` |
| P6 | PresentScale (EASU / Lanczos-2 / RCAS) | GPU | display to `presentOut` (window rect, `R32_UINT`) | 1-3 dispatches | category `present` | full barrier per pass | `5167-5243` |
| P7 | **Backend presentation** | GPU (backend command buffers) | `presentOut` to `upload.image` (RGBA8) to picture or swapchain (to swapchain on the second queue) | `vkCmdCopyImage` (`hw_gs_backend.cpp:1151`), full `vkCmdClearColorImage` (`1238-1240`), `vkCmdBlitImage` (`1217`), box-halving blits when the frame is larger than the rect (`1189-1204`), second-queue `vkCmdCopyImage` (`1272`) | **no counter at all** (outside `gpu_busy`: STUDY-BARRIER-SCOPE 2) | queue semaphores, fences per slot | `record_frame` `1126-1223`, `record_show` `1253-1281`; path choice android `810-824` |
| P8 | HUD layer | CPU to GPU | staging (V C) to an image, then drawn | memcpy + copy when the text changes | none | backend | `hud/hw_hud_layer.cpp:238-266` |
| X1 | Pipeline cache I/O | GPU driver to disk | `pipelines.bin`, `variants.bin` | `vkGetPipelineCacheData` + file write | none | the spec worker thread, at most every 10 s | `save_shader_cache` `1413-1435`, `1606-1610` |

Notes on the table:
- **(C)** Path T1 is the first of three or four passes over every EE texture byte:
  - the EE copy into the stream ring;
  - the hash (U1);
  - the replay compare or swizzle;
  - later, a window or GLM memcpy if the texture is decoded.

  This is CPU-side memory traffic; shared CPU/GPU memory does not change it.
- **(C)** The upload counter nests the swizzle timer (`core2_renderer.cpp:3226` wraps `3256`). On the S23, upload 1.26 ms
  includes swizzle 1.12 ms, so the upload work outside the swizzle is about 0.14 ms (H).
- **(C)** Every waited readback (`Reduce`, `ReduceMany`, `ReadClut`, `ReadDisplay`, `ReadImage`) ends with `submit(true)`,
  which calls `wait_all`. That waits for every in-flight slot, not just the one with the result (`core2_gpu.cpp:2581-2586,
  2640-2676`).
- **(C)** The donor Vulkan path (`native_vulkan.cpp`, not CoreGS 2.0) already prefers `L|V|C` for its buffers
  (`native_vulkan.cpp:604-616`) and has a correct `HOST` barrier (`838-845`). It is out of scope here.

---

## 2. What UMA allows, mechanism by mechanism

### 2.1 Detecting UMA and keeping the discrete path unchanged

- **Signals (H, Vulkan spec):**
  - `VkPhysicalDeviceProperties::deviceType`: `INTEGRATED_GPU` on Adreno, Mali, Intel and AMD APUs, and on Apple GPUs
    through MoltenVK (H). `CPU` on Lavapipe.
  - The memory shape from `VkPhysicalDeviceMemoryProperties`.
- **Do not use "a type with `DEVICE_LOCAL | HOST_VISIBLE` exists" alone.** A discrete GPU with Resizable BAR exposes `L|V|C`
  over all of its VRAM (H). Combine both signals:
  - **discrete:** `deviceType == DISCRETE`. Nothing changes.
  - **unified all-local:** integrated or CPU, and every `HOST_VISIBLE` type is also `DEVICE_LOCAL`. This covers the S23
    (C, from its type list), Intel iGPUs (H), Apple Silicon (C, MoltenVK source: on unified memory the shared type gets
    `DEVICE_LOCAL` and lives in the one device-local heap sized to system memory) and Lavapipe (H).
  - **unified split:** integrated, with host-visible types that are not device-local. AMD APUs on Windows have a BIOS
    carve-out heap plus a system heap (H). The memory is physically shared, but GPU access to the system heap is snooped
    and may be slower (Hyp).
- **Rule for every phase below:** a UMA behaviour is keyed on the memory shape and on its own A/B switch, never on the
  vendor alone. On `discrete` the code path stays byte-for-byte today's. The one exception is the host-read barrier
  (2.7), a spec fix that is exact and harmless everywhere. The owner may still choose to key it on the shape.
- **Testing UMA code on the desktop:**
  - Lavapipe in the Android emulator has one `L|V|C|H` type (H), so it exercises the all-local paths functionally.
  - A diagnostic override (`GT4_HW_MEMORY_SHAPE`, phase 1) lets the desktop's gates run the same code paths.

### 2.2 Persistently mapped `DEVICE_LOCAL | HOST_VISIBLE` buffers: writing straight into what the GPU reads

- **(C)** Already done for every stream that the CPU writes once and the GPU reads once in the same recording:
  - vertices (D1), uniforms (D2), decode windows (U3), palettes (U5);
  - patch words (W2), fill states (F1), the tone palette.

  All rings are mapped once at `Init` and never unmapped.
- **What could still go:**
  - (a) The **vector-to-ring memcpy** of vertices. The front end would emit `GpuVertex` records straight into a span of the
    mapped vertex ring. This is exact: same bytes, same offsets.
  - (b) The **readback-to-vector memcpy**: `writeRect` could read the mapped readback buffer directly (exact, trivial gain).
- **What must not change:** the **GLM** and the **GPU CLUT** stay GPU-ordered (U2, C1).
  - The CPU cannot write into them at record time: up to 8 earlier submissions may still read or write those pages.
  - The `vkCmdCopyBuffer` is how the write lands at its place on the GPU timeline.
  - The exception is a page whose last GPU use has completed. A per-page "last submission serial" plus a fence poll would
    allow a direct host write, with visibility at `vkQueueSubmit` for coherent memory (spec host-write ordering).
  - That is part of phase 5's design, not a quick win.

### 2.3 Optimal tiling, linear images, buffer-backed images, and when not to

- **(C)** Every core 2 image is `OPTIMAL`. The GS memory itself (GLM, windows) is already a *buffer* in PS2 swizzled layout,
  read through address tables (`hw_glm_decode.comp`, `glmTables`). In that sense the GPU already reads the PS2 layout
  directly, and nothing needs to become a linear image to "avoid a deswizzle".
- **Do NOT make targets, textures, palettes or post images linear or buffer-backed:**
  - (H) Qualcomm's guide: `OPTIMAL` "generally uses UBWC", and linear tiling, compute writes, CPU readback, aliased images
    and **mutable images** "almost certainly disable" UBWC. It also says to avoid `MUTABLE_FORMAT` before Adreno 750,
    which includes the 740.
  - Linear images also lose the 2D locality that the texture path and the tiler depend on.
  - Shared memory changes none of that: an `OPTIMAL` image is already in the same DRAM.
- **(H/Hyp) Compression is probably already off for most core 2 images on the S23:**
  - they are storage images written by fragment and compute storage stores;
  - with IN-481 on, they are also `MUTABLE_FORMAT` (`core2_gpu.cpp:765-769`);
  - the real fix for that is the D1 attachment path (`COREGS2-ROADMAP`, ANDROID-PORT-PLAN "hard part 1"), not UMA.
- **When linear is acceptable:**
  - a small image the CPU writes once and the GPU reads once (the HUD text; texture-replacement uploads through
    `VK_EXT_host_image_copy`, 2.4);
  - none of core 2's hot images qualify.

### 2.4 `VK_EXT_host_image_copy` (core but optional in Vulkan 1.4)

- **Mechanism (H, spec):**
  - `vkCopyMemoryToImageEXT` writes texels from host memory into an image created with `VK_IMAGE_USAGE_HOST_TRANSFER_BIT`,
    with no staging buffer and no GPU command.
  - The image must be in a layout from `pCopyDstLayouts`, and the GPU must not be using it.
  - `VkHostImageCopyDevicePerformanceQueryEXT`, chained to `vkGetPhysicalDeviceImageFormatProperties2`, reports
    `optimalDeviceAccess`. If it is false, the host-transfer usage degrades GPU access (for example it disables
    compression): **that is the per-format answer to "when not to"**.
- **Fit in core 2:**
  - **(a) Texture replacements (U6):** today a 4K replacement is 64 MiB pushed through the staging ring in pieces.
    The loader thread could write the new image directly. It is a fresh image, so no GPU use is in flight.
  - **(b) The HUD image.**
  - **Not targets or present images:** they are GPU-written, and host-transfer usage could cost compression.
- **Availability:**
  - MoltenVK implements it: (C) `MVKDevice.mm` has `populateHostImageCopyProperties`, with the comment that Metal "cannot
    use Private storage mode with host memory access". So host-copy images are Shared storage (and lose lossless
    compression on Apple GPUs: Hyp).
  - Adreno 740 and Mali: **U** (check `vulkaninfo`).
- **Value:** texture packs only. No effect on the normal path.

### 2.5 Texel buffers

- A `UNIFORM_TEXEL_BUFFER` (`R32_UINT`) view of the GLM or a window would route the decode's scattered reads through the
  texture cache instead of the storage-buffer path.
- The same reads in the same order would give an exact result.
- Gain on Adreno: **Hyp** (it needs a micro-benchmark). It is not a UMA item, and is listed in phase 6.

### 2.6 GPU writes into host-visible memory (readbacks)

- **(C)** Already the design: `core2_reduce.comp` writes the readback buffer (type 6 on the S23) through a storage-buffer
  descriptor (`4795`). The 2 KiB table is never staged twice.
- **On UMA the "download" is free; the wait is not (section 5).**
- **Possible refinement (exact, small):** the reduce shader writes the words already swizzled into a host-visible copy of
  the touched GS blocks, so `writeRect` becomes a memcpy.
  - Gain: a few µs for the flare table.
  - Gain: tens of µs for the 640x224 other-view targets of Hong Kong (TASK-178 9.5: 7,260 CT24 px per VBlank).
  - **Not worth a phase by itself.**

### 2.7 `HOST_CACHED` against `HOST_COHERENT`: flush and invalidate rules

- **Coherent (`HOST_COHERENT`):**
  - (H, spec) No `vkFlushMappedMemoryRanges` or `vkInvalidateMappedMemoryRanges` is needed.
  - Host writes made before `vkQueueSubmit` are visible to that submission's commands (the submit's host-write
    guarantee).
  - For **device writes read by the host** the spec still requires a memory dependency into the host domain:
    `dstStageMask = VK_PIPELINE_STAGE_HOST_BIT`, `dstAccessMask = VK_ACCESS_HOST_READ_BIT`, then the fence wait. "Signaling a
    fence and waiting on the host does not guarantee that the results of memory accesses will be visible to the host."
  - **(C) Core 2 never records it** (finding 4, `core2_gpu.cpp:1818-1823`, used at `4799`, `4845`, `4897`, `4687`, `5273`,
    `5305`).
- **Cached, not coherent (S23 type 3 `LVH`):**
  - Flush after CPU writes, before the submit.
  - Invalidate after the fence, before CPU reads.
  - Ranges are aligned to `nonCoherentAtomSize`.
  - (C) `make_buffer` never picks such a type, because it requires `C`. Keep it that way unless a measured win appears:
    a 2 KiB invalidate is cheap, but flushing MBs of upload rings is cache maintenance per line.
- **Uncached and coherent (S23 types 4, 5 `LVC`, "write-combined"):**
  - (H) Sequential CPU writes are fast; **CPU reads are very slow**.
  - (C) The rings are write-only from the CPU: every access is a memcpy into them or an element store (`WriteClut`).
    No code reads a ring back, so they are safe in uncached memory.
- **Cached and coherent on a phone (type 6 `LVCH`):**
  - (H) IO-coherent: the CPU runs at cached speed and the GPU snoops the CPU caches.
  - (H, Arm and Qualcomm guidance in general form) For CPU-write/GPU-read streams the uncached type is the usual choice;
    cached memory is for what the CPU reads.
  - (C) IN-476 chose cached for integrated GPUs after the HD 620, where the LLC is shared and snooping is cheap (H).
  - Whether type 6 or type 4 is faster for the S23's rings is **Hyp**. The switch to try it already exists:
    `GT4_CORE2_CACHED_UPLOAD=0`.
- **Readback buffers:** keep cached and coherent (type 6 on the S23; `V|C|H` on desktops). CPU reads of uncached memory
  would be the slow case.

### 2.8 `VK_KHR_external_memory` and `VK_EXT_external_memory_host`

- **`VK_EXT_external_memory_host` (H):**
  - It imports an existing host allocation as `VkDeviceMemory`, aligned to `minImportedHostPointerAlignment`, typically
    4 KiB.
  - It could let the GPU read the CPU's GS-memory mirror in place.
  - **It is not needed:** the same result comes from *allocating* the mirror from a mapped Vulkan allocation, which
    every driver supports.
  - Both run into the same versioning problem (phase 5).
  - Availability on Adreno, Mali and MoltenVK: **U**.
- **`VK_KHR_external_memory` (fd, `AHardwareBuffer`):**
  - For sharing with other processes or APIs: the out-of-process GSdx (`gt4-gs.exe`) on the desktop, or an Android video
    encoder.
  - Not a CoreGS item.

---

## 3. Platform notes

### 3.1 Adreno 7xx (S23 Ultra, Adreno 740)

- **Memory:** (C) `0:L 1:L 2:L 3:LVH 4:LVC 5:LVC 6:LVCH 7:L 8:L` — all visible types are local. Core 2 puts rings and
  readbacks in type 6 and static tables in type 4 (section 1.1).
- **Tiling and compression:**
  - (H, Qualcomm) GMEM binning or direct (FlexRender) rendering.
  - UBWC on `OPTIMAL` images; it is lost with linear, mutable, compute-written or CPU-read images.
  - (H) The core 2 pass has no attachments, so the tile memory gives it no benefit (ANDROID-PERFORMANCE-PLAN 5.1).
  - (H) Storage-image read-modify-write traffic goes to the caches and DRAM.
- **Interlock:** treat as absent (ANDROID-PERFORMANCE-PLAN 5.5).
  - (C) The S23 shows about 750 fragment barriers per VBlank (brief), the no-interlock order of magnitude in
    STUDY-BARRIER-SCOPE 5.2.
- **Integrated defaults:** (H) Adreno reports `INTEGRATED_GPU`. Under the rule `hw_device_profile.hpp:86` (C) it then
  gets every HD-620-tuned default:
  - 8 slots, submit every 32;
  - cached rings;
  - the IN-481 colour-attachment path if the device has `independentBlend` (U on the S23: read the log line
    `[core2] colour attachment path (IN-481) on/off`);
  - a 192 MiB texture budget;
  - no present resampling in Performance mode (IN-478).
- **Timers:** (H, Qualcomm) "Timer queries should be issued inside a render pass" — binned rendering timestamps include
  per-tile overhead.
  - Core 2 writes its slot and category timestamps outside passes, so the phone's `gpu_busy` and categories may be
    distorted (U).
  - Cross-check with Snapdragon Profiler or AGI.
- **Presentation:** (C, android `hw_gs_backend.cpp:810-824, 1023-1024`)
  - The two-queue path is chosen whenever the family has two or more queues (U for Adreno's count).
  - The swapchain format is the first of `B8G8R8A8_UNORM` or `R8G8B8A8_UNORM` that the surface lists (H: RGBA8 on Android).
  - Swapchain usage is `COLOR_ATTACHMENT | TRANSFER_DST` (android `1053`).
- **Mali (brief, H):**
  - The same shape: `L`, `L|V|C`, and `L|V|C|H` on IO-coherent SoCs or `L|V|H` otherwise.
  - AFBC is lost on storage and mutable images.
  - No interlock (Hyp).
  - Arm's guidance: uncached coherent for streamed CPU writes, cached for readbacks.

### 3.2 Apple Silicon through MoltenVK (macOS)

- **(C, `MVKDevice.mm` `initMemoryProperties`) Heaps:**
  - On unified memory, heap 0 is `DEVICE_LOCAL`, sized to system memory.
  - The shared type lives in it, with `DEVICE_LOCAL` added.
- **(C) Memory types:**
  - `Private` (`MTLStorageModePrivate`).
  - `Shared` (`MTLStorageModeShared`, host-visible and coherent).
  - `Memoryless` (`LAZILY_ALLOCATED`, Apple GPUs).
  - No `Managed` type on Apple GPUs.
  - Whether the shared type also carries `HOST_CACHED` is **U** (check `vulkaninfo`).
- **Coherence:** (H, Apple docs) Shared resources need only completion synchronisation, no cache flushes.
- **Mapping to core 2:**
  - The rings land in Shared (`L|V|C`): already UMA-optimal.
  - Images and GLM land in Private: GPU-only and compressible.
  - Readbacks land in Shared.
- **Limits that matter more than memory (U/Hyp, verify with `vulkaninfo`):**
  - Fragment shader interlock is probably absent, so the barrier path applies.
  - Render-pass self-dependency barriers map to Metal memory barriers inside render encoders, whose cost on Apple GPUs is U.
  - Apple GPUs are tile-based like Adreno: the D1 attachment path matters there too. `Memoryless` is ideal for transient
    depth in a future attachment path.
- **Host image copy:** (C) implemented in MoltenVK (2.4).
- **A future Metal backend (H):**
  - `MTLStorageModeShared` buffers with `MTLResourceCPUCacheModeWriteCombined` for the upload rings (the Metal form of the
    uncached choice).
  - Private textures, heaps.
  - Memoryless attachments.

### 3.3 iOS and iPadOS

- The same GPU family as Apple Silicon and the same MoltenVK memory model (H).
- **Memory is the hard limit:** jetsam kills by footprint; the per-app limits are a fraction of RAM (H). The 400 MiB of
  mapped rings plus a 64 MiB atlas plus 192 MiB of texture cache are a real risk there. **Ring trimming (phase 3) is a
  prerequisite for iOS** (H).
- No JIT is needed (the project is AOT), so App Store rules on executable memory do not block the design (H).

### 3.4 Intel and AMD integrated GPUs on Windows

- **Intel (Gen9 HD 620 and later):**
  - (H) One device-local heap, types `L`, `L|V|C`, `L|V|C|H`.
  - The CPU and GPU share the LLC: cached and coherent is cheap, which is IN-476's premise.
  - All-local shape.
- **AMD APUs (Windows):**
  - (H) A device-local carve-out heap plus a system heap.
  - Cached types live in the system heap, which the GPU reaches snooped.
  - Split shape.
  - With IN-476 the rings land in the system heap. Whether `L|V|C` in the carve-out is faster for vertex fetch is **Hyp**,
    and it is the same A/B switch.
- **Linux, the Steam Deck and phase 3 platforms:** RADV reports the APU shape in its own way. Re-check there.

---

## 4. Where the time and bandwidth go today, and what each change could buy

### 4.1 The phone's budget (C from the brief, readings H or Hyp)

- **Race, 2x Performance:**
  - GS thread about 8.7 ms per VBlank: GIF parse 2.45, draw 2.55, upload 1.26 (of which swizzle 1.12 for 2446 KiB),
    CLUT 0.40, translate 0.41, other about 1.6 (not split in the brief).
  - GPU busy 11.5 ms, **equal at 1x and 2x**.
- **(H) The race GPU time is not bandwidth-bound:**
  - 4x the pixels did not move it, so it is per-pass, per-barrier and latency cost (TASK-182's subject).
  - **Removing copies will not shorten race `gpu_busy` much.**
  - It will lower the hidden present cost, DRAM traffic and heat.
- **(C) Menus at 6x:**
  - 43 ms of compose and deinterlace: five full internal-resolution passes (STUDY-BARRIER-SCOPE step 2b).
  - This *is* bandwidth-bound, but by passes, not by CPU/GPU copies. R8 removes two of the five.
  - Computing the deinterlace decision once per native pixel is the next exact step there (STUDY-BARRIER-SCOPE 2b section 6).
- **(C) GS-thread CPU work that touches memory:**
  - swizzle, hash and replay: 1.12 ms;
  - the ring memcpys are not counted separately (phase 1 adds them).

### 4.2 Present chain, worked out (H for the arithmetic)

Assumptions:
- S23 swapchain 3088x1440 RGBA8: 17.8 MB.
- 16:9 present rect 2560x1440: 14.7 MB.
- A race at 2x with `presentScaled` (normal mode).
- The two-queue path.

Bytes per presented frame, read plus write:

| Step | Code | MB |
|---|---|---|
| PresentScale writes `presentOut` | `core2_gpu.cpp:5167-5243` | W 14.7 |
| copy `presentOut` to `upload.image` | `hw_gs_backend.cpp:1151` | R 14.7 + W 14.7 |
| black clear of the whole picture | `1238-1240` | W 17.8 |
| blit `upload.image` to the picture rect | `1217` | R 14.7 + W 14.7 |
| second queue: picture to swapchain image | `1272` | R 17.8 + W 17.8 |
| **total after PresentScale's source reads** | | **about 127** |
| fused: last pass renders the swapchain image, `loadOp = CLEAR` for the bars | phase 2 | W 17.8 |

Savings and costs:
- **About 109 MB per frame, about 6.5 GB/s at 60 fps (H arithmetic).**
- Real DRAM bytes are lower where UBWC compresses the RGBA8 images (Hyp).
- The single-queue path (`GT4_HW_PRESENT_QUEUE=0`) alone removes the last row: about 36 MB.
- GPU time: at an effective 30-50 GB/s for copies (Hyp), 0.7-2.5 ms per frame.
- These command buffers are outside `gpu_busy` (STUDY-BARRIER-SCOPE 2), so the phone's real GPU frame is 11.5 ms plus this
  (Hyp).
- With the identity pre-transform on a portrait-native panel, the compositor may add a rotation pass (U; test3's
  pre-rotation work addresses it).

### 4.3 Expected gain per change (per VBlank; every gain is a hypothesis until measured)

| Change (phase) | GS CPU ms | GPU ms | DRAM MB | Exact | Basis |
|---|---|---|---|---|---|
| Uncached upload rings on tilers (P0 A/B, then P3 default) | about ±0.1 | 0 to -0.3 (no snooping) | about 0 | yes | Hyp; switch exists |
| Single-queue present on Android (P0 A/B) | about 0 | -0.4 to -0.9 | -36 | yes (presentation only) | H arithmetic, Hyp time; switch exists |
| K rule after a waited readback (P0 A/B, `GT4_CORE2_SUBMIT_AFTER_WAIT=8`) | about 0 | GPU idle per drain -0.9 to -1.8 on the desktop | 0 | yes (C: TASK-178 hashes) | C on the desktop (STUDY-FRAME-DOWNLOAD-COST 9.3), Hyp on the phone |
| Phase 1 diagnostics and host barrier | +0.01 at most | about 0 | 0 | yes | H |
| Present fusion with pre-rotation (P2) | -0.05 | -0.7 to -2.5 | about -95 to -110 (vs today's two-queue path) | yes | H arithmetic, Hyp time |
| Vertices written in place (P3) | -0.05 to -0.2 | 0 | -1 to -3 (CPU copy) | yes | Hyp (needs the P1 counters) |
| Ring and atlas trim (P3) | 0 | 0 | 0 | yes (rings); U (atlas) | about -250 MiB footprint (H) |
| Pre-read submission cadence (P4) | +0.02 to 0.1 (more submits) | read wait -0.2 to -1 | 0 | yes | Hyp |
| Bounded fence spin (P4, `GT4_HW_READBACK_SPIN`) | -0.05 to -0.3 per drain (wake latency) | 0 | 0 | yes | Hyp; power cost |
| Asynchronous flare read over coherent memory (P4b) | -I_cpu: 0.3-0.75 per read on the desktop (C, TASK-178 9.3), more on the phone (Hyp) | no post-drain bubble | 0 | H, only with the deferred write-back layer | high risk |
| Shared GS-memory mirror (P5) | -(window + GLM memcpy): about 0.1-0.5 | -(GLM copy) | -(2 x GLM bytes) | H with versioning | Hyp; only if P1 shows more than about 2 MB per VBlank |
| Narrow texel formats for indexed decodes (P6) | 0 | sampling -? | texture memory /4 for T8/T4 indices | yes | Hyp |

---

## 5. The readback and serialisation problem in normal mode

### 5.1 What UMA cannot change

- **(C)** The dependency is real (STUDY-FRAME-DOWNLOAD-COST 2):
  - the 64x8 CT32 flare table needs the depth of the whole scene drawn before it;
  - the VU1 flare program needs the table to emit the flares;
  - one in-order queue holds both.

  The wait equals the GPU backlog at the read plus wake-up latency. Shared memory removes only the 2 KiB copy, which was
  never the cost (C: about 0.1 ms of a 4.75 ms read, TASK-178).
- **(H)** On the S23 in normal mode the GPU (11.5 ms) is slower than the GS thread (8.7 ms), so the backlog at the read is
  large. Only less GPU work (TASK-182 R1-R7, D1) shrinks it.

### 5.2 What keeps the GPU busy and stops its clock from dropping

- **(C) The bubble:**
  - after a waited readback, `submit(true)` waits for every slot (`2581-2586`);
  - the GPU then idles until the next submission;
  - at `submitEvery = 32` on an integrated GPU that is up to 32 batches later, or at the VBlank flush.
  - TASK-178 measured 1.4-3.5 ms of GPU idle per drain on the desktop (9.6).
- **(Hyp) The clock:**
  - Adreno's GPU governor scales the frequency from busy time over a sampling window, so drains and CPU gaps read as idle.
  - The latency-bound chain before the next read then runs at a lower clock, which feeds the next wait.
  - The same happens to the GS thread's CPU core while it sleeps in `vkWaitForFences`: the scheduler lowers its frequency
    or moves the thread.
- **Exact remedies, cheapest first:**
  1. **K rule** (TASK-178, `GT4_CORE2_SUBMIT_AFTER_WAIT`): the first submission after a drain comes K batches later.
     - (C) Exact on the desktop: hashes equal for K = 8, 16 and 32; gates `wrong=0`.
     - (C) It halved the GPU idle per drain on the desktop.
     - On the S23 try K = 8 (`submitEvery` is 32 there).
  2. **Pre-read cadence:** submit more often *before* the read, so the tail the GPU has not started is short when the
     read arrives.
     - GT4 reads once per race frame, near the end of the world pass (STUDY-FRAME-DOWNLOAD-COST 2).
     - A cheap trigger: after a frame's first download, the renderer remembers that frame's batch index of the read; in
       the next frame it uses a cadence of 4-8 batches from about 64 batches before that index until the read.
     - Submission boundaries never change output (C, TASK-134 section 9; TASK-178 9.2).
  3. **Bounded spin on the newest fence, then one wait** (O2; `GT4_HW_READBACK_SPIN` is parsed but unused by core 2 per
     STUDY-FRAME-DOWNLOAD-COST O2).
     - Keeps the core awake through short waits.
     - Cap it at about 100-200 µs: phones pay for spinning in power and heat.
  4. **Tell the system:**
     - ADPF performance hints for the GS worker (CPU; ANDROID-PERFORMANCE-PLAN 3.2).
     - On API 35 and later, the GPU part of a work duration (`AWorkDuration_setActualGpuDurationNanos` with
       `APerformanceHint_reportActualWorkDuration2`, H on the names; verify against the NDK). The governor can then
       raise GPU clocks for frames that need them.
     - Do **not** add dummy "keep-alive" GPU work: it burns power for an uncertain clock gain.
- **Smaller, earlier, finer-grained work before the wait:**
  - (C) The light-sampling chain right before the read is a run of tiny dependent passes behind full barriers (64x192
    scratch windows, 64x32 table: TASK-178 9.4).
  - (H) Every barrier and pass end that TASK-182 removes there (R1 self copies in the pass, R2 region hazards, R3 CLUT
    ranges, R4 scoped barriers) shortens the latency-bound tail the read waits for.
  - That is where the phone's normal-mode wait is won.

### 5.3 What UMA does make cheaper: the asynchronous read (phase 4b)

- **Design (H):**
  - At the flare read the GS worker records the reduce into a coherent cached buffer, followed by a sequence word written
    by the GPU: a `vkCmdFillBuffer`, or a store in the same dispatch.
  - It submits *without waiting* and keeps parsing the stream (O4 in STUDY-FRAME-DOWNLOAD-COST).
  - The VU1 worker's IN-522 `TablePatch` waits on that sequence word (or a timeline semaphore value), then copies 2 KiB.
- **What UMA adds:** with a coherent cached type the words land where the CPU reads them, so no copy step exists at all,
  and the VU1 worker can poll a word cheaply.
- **Gain (C on the desktop, Hyp on the phone):**
  - the GS worker's busy time between the read and the first flare job (I_cpu): 0.3-0.75 ms per read on the desktop;
  - and the post-read GPU bubble disappears, because the post-read draws are recorded and submitted while the GPU is
    still on the scene.
- **The blocker (C, TASK-178 9.8):**
  - today's `Readback` writes the words into GS memory and clears the target's `drawn` area at the read's stream position
    (`core2_renderer.cpp:2918-2925`);
  - O3 showed that changing when that state transition happens is **not exact**: Motegi's overlapping 64x192 scratch
    windows;
  - an exact asynchronous read needs a deferred write-back layer in front of GS memory, hooked into every reader: CLUT
    loads, GLM uploads, decode windows, downloads, moves, display, uploads, readbacks.
  - **High risk, 1-2 weeks.** Do it only after phases 0-4a show the wait still dominates on the phone.

---

## 6. Related CPU work that UMA unlocks, or does not

- **The CPU swizzle (1.12 ms, 2446 KiB per VBlank on the S23):**
  - The GPU already reads the PS2 layout directly: the GLM and windows are in swizzled order (2.3).
  - The CPU swizzle exists because many CPU consumers need the swizzled mirror:
    - CPU CLUT loads;
    - texture-cache block identities (`HashBlock`);
    - CPU sprites;
    - EE downloads;
    - local moves;
    - store-before-write;
    - window and GLM copies.
  - Moving the swizzle to the GPU would need those consumers to read back from the GPU. **No** (exactness and stalls).
  - A **lazy swizzle** is the only form left:
    - keep linear upload records;
    - swizzle into the mirror only when a CPU consumer touches those blocks;
    - upload linear bytes to the GPU for GPU-only consumers.
  - It is phase 6 research; its bookkeeping must reproduce A, then B over part of A, then A again, in order.
  - The ceiling is the 1.12 ms minus what the consumers still force (Hyp).
- **The replayed uploads (11,870 MiB in a session):**
  - (C) These are bytes *recognised* by IN-486. Each is read by `HashUploadBytes` (scalar 4-lane multiply hash,
    `core2_renderer.cpp:330-352`), then compared or copied per block (`3146-3162`).
  - **This is CPU memory traffic, not a CPU-to-GPU copy, so UMA changes nothing here.**
  - Per VBlank it is about 2.4 MB if most uploads replay. That is a Hyp: it needs the session's VBlank count, which the
    brief does not give.
  - Exact levers:
    - (a) A faster arm64 hash for the memo key only. It can be any 64-bit-strong hash, because it is not the block
      identity, and no stored data depends on it. NEON or CRC32C lanes would do.
    - (b) NEON for `HostToLocalCt32`: the Android build uses a portable 64-bit form (android `core2_upload.cpp`, TASK-180).
  - Both are CPU items outside this study's phases. A gain of about 0.2-0.5 ms is Hyp.
  - The EE-side copy into the stream ring (T1) is a third pass over the same bytes that cannot be avoided: the EE may
    overwrite its RAM before the GS worker parses.
- **Vertex memcpy:** in place in the mapped ring (phase 3), exact.
- **Descriptor writes:** persistent sets or push descriptors (TASK-182 R6).
  - CPU only. On Adreno the driver cost of descriptor writes is higher than on the desktop (Hyp).
  - `VK_EXT_descriptor_buffer` is the UMA-native form, with descriptors written straight into a host-visible buffer;
    availability on the 740 is U.

---

## 7. Phased plan, cheapest and safest first

### Gates used by every phase

- **Flare reply hashes** (`[hw] flare replies ... hash=`):
  - equal with the phase's switch on and off on the same build;
  - the TASK-182 press scenes, Spec II Hong Kong (reverse) and Motegi B-Spec, at 2x and 10x;
  - references on `build/t178-spec2`: Hong Kong 2x `2f4c1cc9f174d00e` (6835 replies), Motegi 2x `7bf932fbb8799965` (6588),
    Hong Kong 10x `61109e8f3143264a`, Motegi 10x `9ab7085aa7c6d425` (C, STUDY-BARRIER-SCOPE 3).
- **Frame and state gates:** `gates6.sh` `wrong=0`:
  - Spec II George V, El Capitan, Tokyo R246 at 1x: race clock, simulation state, frames;
  - v2.00 night race and Arcade at 3x;
  - George V lap at 6x;
  - VU1 and VU0 verify and the MTVU check at 0 mismatches.
- **CTest** full suite (320 tests at e7ebb20b, 1 skip) and the **Python** suite against the build under test.
- **For presentation changes:**
  - composed and presented frame hashes (`GT4_HW_DUMP_SOURCE=upscaled`, `--window`; method of STUDY-BARRIER-SCOPE 2b
    section 4);
  - plus the new swapchain-content fixture of phase 2.
- **UMA code paths on the desktop:** run the gates with `GT4_HW_MEMORY_SHAPE=unified_all_local` (phase 1) so the desktop
  exercises them; Lavapipe in the Android emulator for a functional pass.
- **Measurement:**
  - one variable per candidate;
  - desktop GPU A/Bs at 10x and 12x on the RX 9060 XT (owner rule);
  - phone A/Bs on the S23 with the owner's F7/F8 and the recorded drives;
  - no timing while builds run.

### 7.1 Phase 0: measure with what exists (no code; test build 4's `debug.env`)

Read from the S23 logs the owner already exported (C: all of these lines exist):
- `[core2] memory types`, `[core2] rings:`, `[core2] colour attachment path (IN-481) on/off`, `[core2] ready: ... pixel
  interlock`.
- `[hw] presentation queue`, the swapchain format, extent and pre-transform lines.
- In the 600-VBlank lines:
  - `glm=... (calls, pages)`;
  - `window_decodes` and `window_mb`, divided by VBlanks;
  - `image_mb`, `texture_mb`;
  - `uploads_replayed` with the session's VBlank count;
  - `[hw] transport gif_kib`.

A/B runs, each against the default, the Nordschleife race at 2x and a 480i menu at 2x, normal and Performance:

| Switch | Values | What it answers |
|---|---|---|
| `GT4_CORE2_CACHED_UPLOAD` | `0` (type 4 `LVC`) vs default (type 6 `LVCH`) | snooping cost of IO-coherent rings |
| `GT4_HW_PRESENT_QUEUE` | `0` (rendering queue) vs default | the second queue's full-screen copy; pacing on Android |
| `GT4_CORE2_SUBMIT_AFTER_WAIT` | `8`, `16` vs `0` | post-drain bubble in normal mode |
| `GT4_CORE2_ROP` | `0` vs `1` | whether the attachment path helps or hurts a tiler (and UBWC through `MUTABLE_FORMAT`) |
| `GT4_CORE2_SELF_COPY_DRAW` | `1` (only matters when ROP is off) | pass ends (TASK-182 R1) |
| `GT4_CORE2_DEINT_FIELD_COMPOSE` | `1` | menu post bandwidth (R8) |
| `GT4_CORE2_RINGS` | `128`, then `96` (MiB budget) vs default | whether smaller rings force submissions (the `[core2] rings:` line, `submissions`) |
| `GT4_CORE2_BARRIER_TRACE=1`, `GT4_CORE2_GPU_CATEGORIES=1` | on | the S23's pass and barrier chain and post split (diagnostics) |

### 7.2 Phase 1: UMA groundwork (handoff spec for Android test build 4)

**Goal.** Detect the memory shape, report placement, count every data path in bytes, time the backend's presentation on the
GPU, and add the spec-required host-read barrier. **No rendering change; every image and every GS memory byte stays
identical.** Branch from the current Android integration base (or `task/182-barrier-scope`), for example
`task/183-uma-groundwork`. Implementation agent: not PCSX2-tainted, never opens PCSX2 or the fork.

#### P1-A Memory shape in the device profile (`native/hw_gs/hw_device_profile.hpp`, pure arithmetic)

Add after `classify_device` (line 77-81):
```cpp
// The VkMemoryPropertyFlagBits values (copied in as plain integers like the rest of this header).
constexpr std::uint32_t kMemoryLocal = 0x01, kMemoryVisible = 0x02, kMemoryCoherent = 0x04, kMemoryCached = 0x08,
                        kMemoryLazy = 0x10, kMemoryProtected = 0x20;
struct MemoryTypeInfo { std::uint32_t flags = 0; std::uint32_t heap = 0; };
// discrete: the device's own memory. unified_all_local: an integrated or CPU device whose every host-visible type is also
// device-local (Adreno, Mali, Intel, Apple through MoltenVK, Lavapipe). unified_split: integrated, with host-visible types
// outside the device-local heap (AMD APUs on Windows). unknown: anything else (no types, virtual or other devices).
enum class MemoryShape : std::uint8_t { discrete, unified_all_local, unified_split, unknown };
const char *memory_shape_name(MemoryShape);             // "discrete", "unified-all-local", "unified-split", "unknown"
MemoryShape classify_memory(std::uint32_t device_type, const MemoryTypeInfo *types, std::uint32_t count);
std::string memory_type_letters(std::uint32_t flags);   // "L", "V", "C", "H", "Z" (lazily allocated), "P" (protected)
```

Rules for `classify_memory`, in order:
1. `count == 0`: `unknown`.
2. `device_type == kDeviceTypeDiscrete`: `discrete`.
3. `device_type` is `kDeviceTypeIntegrated` or `kDeviceTypeCpu`:
   - no visible type: `unknown`;
   - every type with `kMemoryVisible` has `kMemoryLocal`: `unified_all_local`;
   - else: `unified_split`.
4. Otherwise: `unknown`.

Changes to `DeviceProfile` (lines 95-119):
- Add `std::uint32_t type_count = 0; std::array<MemoryTypeInfo, 32> types{}; MemoryShape memory_shape =
  MemoryShape::unknown; bool memory_shape_forced = false;`.
- `profile_line` appends `", memory shape <name>, types <i>:<letters>@<heap> ..."`.

#### P1-A2 Probe (`native/hw_gs/hw_gs_vulkan_loader.cpp`, `probe_device_profile`, after the heap loop at line 194)

- Copy `memoryTypeCount` (capped at 32), and each `propertyFlags` and `heapIndex`.
- Set `profile.memory_shape = classify_memory(profile.device_type, ...)`.
- Diagnostic override `GT4_HW_MEMORY_SHAPE=discrete|unified-all-local|unified-split`:
  - sets the shape and `memory_shape_forced = true`;
  - prints `[hw] GT4_HW_MEMORY_SHAPE=<v>: memory shape forced (diagnostics)`.
- In phase 1 **nothing reads the shape except logs**.

#### P1-B Placement report (`native/hw_gs/core2/core2_gpu.cpp`)

- **Record the chosen types:**
  - `struct Buffer` (line 67): add `std::uint32_t type = UINT32_MAX;`; `make_buffer` (548-570) stores
    `allocate.memoryTypeIndex` into it.
  - `Impl`: add `std::uint32_t glmType = UINT32_MAX, clutType = UINT32_MAX, imageType = UINT32_MAX;`.
  - Set them at `3328`, `3403` and in `make_image` at `775` (the last type used).
- **Memory types line (3069-3079):** print each type as `<i>:<letters>@<heap>`, adding `Z` and `P` (use
  `gt4::hw::memory_type_letters`). Keep the line's prefix: bench scripts may read it.
- **Placement line:** after the CLUT block, before `s.ready = true` (3430), print:
  `[core2] placement (memory shape <name><, forced>): staging <i>:<letters> <MiB>, vertices ..., uniforms ..., readback ...,
  later ..., glm_tables ..., inverse_ct32 ..., glm <i>:<letters>, clut ..., images <i>:<letters>`.
  - The shape comes from `handles.profile.memory_shape`; print `unknown` when `!handles.profile.probed`.

#### P1-C Data-path counters (`core2_gpu.hpp` `Core2GpuStats`, `core2_gpu.cpp`, `core2_renderer.cpp`)

New `std::uint64_t` fields in `Core2GpuStats` (`core2_gpu.hpp:65-140`), each a byte count unless noted:

| Field | Incremented where (core2_gpu.cpp) | Amount |
|---|---|---|
| `cpuGlmBytes` | `UploadGlm` loop (4273-4280) | `size` per range |
| (existing) `windowBytes` | `bind_window` (274) | unchanged |
| `cpuPaletteBytes` | `DecodeTexture` when `palette` (4307) | 1024 |
| `cpuPatchBytes` | `Patch` (4760) | `bytes` |
| `cpuClutBytes` | `WriteClut` (4666-4668) | `count * 4` |
| `cpuToneBytes` | `Tone` (4939) | 1024 |
| `cpuFillStateBytes` | `Draw` fill (3877) | `sizeof state.fields` |
| `cpuVertexBytes` | `Draw` (3644) | `vertexBytes` |
| `cpuUniformBytes` | `Draw` (3657, and again at 3866 only if you count rewrites: do **not**, count once) | `sizeof state.fields` |
| `cpuImageBytes` | `UploadAtlas` (3552), `UploadImage` (3583) | `bytes` |
| `gpuCopyBufferBytes` | `UploadGlm` (sum of `copies`), `WriteClut` (`count*4`), `ReadClut` (2048) | as listed |
| `gpuCopyBufferToImageBytes` | `UploadAtlas`, `UploadImage` | `bytes` |
| `gpuCopyImageBytes` | `GrowImage` (3471), `CopyImage` (3516), self copy transfer (3708) and `record_self_copy`, `Deinterlace` copies (5074, 5118), `SnapshotLastOutput` (5035) | `width * height * 4` of the region |
| `gpuCopyImageToBufferBytes` | `ReadDisplay` (5272), `ReadImage` (5304) | region bytes |
| `gpuClearBytes` | `clear_new_image` (821: `out.bytes`), whole clears (3851, 3856: image `width*height*4`) | as listed |
| `hostReadBytes` | `Reduce` (4802), `ReduceMany` (4852), `TakeReduced` (4920), `ReadClut` (2048), `ReadDisplay`, `ReadImage` | bytes memcpy'd out of mapped memory |
| `ringHighStaging`, `ringHighVertex`, `ringHighUniform` | `ring()` (646): `max(high, buffer.used)` for the matching buffer | bytes (a maximum) |
| `ringFullSubmits` (count) | `ring()` before `submit` at 643 | 1 |

- **Print:** in `Renderer::Flush` (`core2_renderer.cpp`), inside `if (s.vblanks_ % 600 == 0)` right after the
  `ms/vblank` line (5643). Keep a `static` snapshot of the previous values, like the `GPU_CATEGORIES` block at 5608-5629,
  print the deltas / 600 in KiB, and reset the three high-water marks after printing:
  ```
  [core2] data paths/vblank cpu_to_gpu_kib glm=%.0f window=%.0f palette=%.0f patch=%.0f clut=%.1f tone=%.1f fill=%.1f
  vertex=%.0f uniform=%.0f image=%.0f | gpu_copy_kib buffer=%.0f buffer_to_image=%.0f image=%.0f image_to_buffer=%.0f
  clears=%.0f | host_read_kib=%.1f | ring_high_mib staging=%.1f/%.1f vertex=%.1f/%.1f uniform=%.2f/%.2f |
  ring_full_submits=%.2f
  ```
  (the `/x` after each high-water mark is the share size, `ring_share`).
- The counters are always on: integer adds next to code that already does a memcpy or records a command.

#### P1-D Host-read barrier (`core2_gpu.cpp`)

- Add next to `full_barrier` (1818):
  ```cpp
  // Vulkan's host-read rule: device writes are visible to a host read after the fence only with a dependency into the
  // host domain. Exact; GT4_CORE2_HOST_READ_BARRIER=0 leaves it out (A/B).
  const bool hostReadBarrier = std::getenv("GT4_CORE2_HOST_READ_BARRIER") == nullptr ||
                               std::strcmp(std::getenv("GT4_CORE2_HOST_READ_BARRIER"), "0") != 0;
  void host_read_barrier() {
    if (!hostReadBarrier) return;
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    gt4_hw_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    stats.hostReadBarriers++;  // new counter; NOT counted in fullBarriers, pending sets untouched
  }
  ```
- Call it right after the trailing `s.full_barrier()` in:
  - `Reduce` (4799), `ReduceMany` (4845), `ReduceLater` (4897);
  - `ReadClut` (4687), `ReadDisplay` (5273), `ReadImage` (5305).
- Do not remove the existing full barriers.
- Rationale: (C) the Vulkan spec's fence note quoted in 2.7; the old core does the same (`hw_gs_writeback_gpu.cpp:3170`).

#### P1-E Presentation GPU timer (diagnostic, backend; do last and rebase on test3's present changes)

- **A new header** `native/hw_gs/hw_present_gpu_timer.hpp`: a small class owning one `VK_QUERY_TYPE_TIMESTAMP` pool of
  2 kinds (frame, show) x 16 pairs.
  - `begin(cmd, kind)`: before writing, read the pair's previous results with `VK_QUERY_RESULT_64_BIT |
    VK_QUERY_RESULT_WITH_AVAILABILITY_BIT` and **no wait**, and add them if available; then `vkCmdResetQueryPool` the
    pair; then a `TOP_OF_PIPE` timestamp.
  - `end(cmd, kind)`: a `BOTTOM_OF_PIPE` timestamp.
  - `take_ms(kind, count&)`.
  - It is created only with `GT4_HW_PRESENT_GPU_TIMING=1`, when the queue family has `timestampValidBits != 0` and
    `timestampPeriod > 0`.
- **In `hw_gs_backend.cpp`:**
  - `record_frame` (1126): `begin` before `record_black_then`, `end` after it returns (outside any HUD render pass).
  - `record_show` (1253): `begin` after `vkBeginCommandBuffer`, `end` before `vkEndCommandBuffer`.
- **Print** every 600 presents: `[hw] present gpu ms/present frame=%.3f show=%.3f (n=%llu, %ux%u, path <name>)`.
- If test3 replaced `record_frame` with a shader present pass, put the same two calls around whatever records the
  presentation.

#### Acceptance tests for phase 1

1. **`hw_device_profile` (extend `native/hw_gs/hw_device_profile_fixture.cpp`).** `classify_memory`, letters and
   `profile_line` cases. Only the S23 list is measured; the others are inputs that exercise the rules:
   - S23 `L, L, L, LVH, LVC, LVC, LVCH, L, L`, integrated: `unified_all_local`; letters of type 6 = `LVCH`.
   - Intel-like `L, LVC, LVCH`, integrated: `unified_all_local`.
   - APU-like `L@0, VC@1, VCH@1, LVC@0`, integrated: `unified_split`.
   - ReBAR discrete `L@0, VC@1, LVC@0, VCH@1`, discrete: `discrete` (an `LVC` type does not make it unified).
   - Lavapipe-like `LVCH`, CPU: `unified_all_local`.
   - Apple-like `L, LVC, LZ`, integrated: `unified_all_local`; letters of type 2 = `LZ`.
   - No types: `unknown`. A virtual device: `unknown`.
   - The profile line contains `memory shape`.
2. **`hw_core2_data_paths` (new; add the name to the `foreach` list in `native/hw_gs/CMakeLists.txt:75-90` and a
   `TestCore2DataPaths` to `writeback/tests/hw_writeback_tests.cpp`, dispatched like `hw_core2_sync_trace` at 6850 and
   7521).**
   - **(a)** `CheckCore2Ab(..., "GT4_CORE2_HOST_READ_BARRIER", exact = false, ...)` over `CacheFixtureCapture()`,
     `Core2GpuPaletteCapture(4)` and `Core2StreamOverRenderCapture(6)`: images and GS memory identical, switch on vs off.
   - **(b)** Expose the GPU stats to the harness:
     - add `const core2::Core2GpuStats *Core2GpuCounters() const` to `writeback::Renderer`;
     - add `const Core2GpuStats &GpuStats() const` to `core2::Renderer`;
     - add `core2::Core2GpuStats gpu` to `Core2AbRun`, filled in `ReplayCore2Ab` next to line 3504.
   - **(c)** On the "on" runs, assert:
     - `cpuVertexBytes > 0` and `cpuUniformBytes > 0`;
     - `cpuGlmBytes + windowBytes > 0`;
     - `hostReadBytes >= 4 * (reduced pixels)` when `stats.readbacks > 0`;
     - each `ringHigh*` at most its share;
     - `hostReadBarriers > 0` exactly when the run had a waited readback, and `== 0` on the switch-off run.
   - **(d)** Run the whole test at scale 1 and 2.
3. **`hw_core2_sync_trace`, `hw_core2_gpu_clut`, `hw_core2_self_copy_draw`, `hw_core2_post_field_compose`** still pass
   (the counters must not change their identity results).
4. **Full CTest and Python** pass on the desktop build. `gates6.sh` `wrong=0` with the default settings and with
   `GT4_HW_MEMORY_SHAPE=unified-all-local`.
5. **Flare reply hashes** equal to the section 3 references of STUDY-BARRIER-SCOPE (Hong Kong and Motegi 2x) on a rebuilt
   `t178-spec2`-style build, and equal between `GT4_CORE2_HOST_READ_BARRIER=0` and the default.
6. **Log check, desktop headless run:** the `[hw]` profile line shows `memory shape discrete` on the RX 9060 XT; a
   `[core2] placement` line; a `[core2] data paths/vblank` line every 600 VBlanks with non-zero vertex and uniform fields.
7. **Present timer:** a windowed run with `GT4_HW_PRESENT_GPU_TIMING=1` prints `[hw] present gpu` with `frame > 0`; the
   presented-frame dump hash is equal with the switch on and off; with the switch unset, no query pool is created.
8. **Android:** the APK builds. The S23 log shows `memory shape unified-all-local`, and placement with the rings on type 6
   (5 if `GT4_CORE2_CACHED_UPLOAD=0`) and the GLM, CLUT and images on an `L` type.

- **Risks:**
  - Low. The only command-stream change is the host barrier (exact by construction; switch to remove).
  - The present timer touches backend code that test3 also edits: do it last and rebase.
- **Effort:** about 3-4 days for one agent (P1-A/B half a day each, P1-C one day, P1-D half a day, P1-E one day, tests one
  day). **Difficulty: low.**

### 7.3 Phase 2: write the final picture straight into the swapchain image (with test3's pre-rotation)

- **Change:**
  - Replace the backend's copy, clear and blit (and, on Android, the second-queue copy) with one full-screen graphics pass
    that renders the swapchain image.
  - It has `loadOp = CLEAR` (black bars in tile memory), `storeOp = STORE`, and the viewport at the present rect.
  - The fragment shader reads `presentOut` (`R32_UINT`, `texelFetch`/`imageLoad` at the matching texel), unpacks RGBA8,
    and writes the attachment.
  - Pre-rotation and mirroring are done in the vertex positions and texel coordinates. This is also test3's pre-rotation
    item.
  - Simpler variant for the PresentScale case: make PresentScale's last dispatch the fragment pass itself, so `presentOut`
    is never written.
- **Files:**
  - `hw_gs_backend.cpp`: `record_frame`, `record_black_then`, `record_show`, the swapchain usage at android line 1053 (it
    already has `COLOR_ATTACHMENT`), and `create_pictures`.
  - `core2_gpu.cpp`: `PresentScale`, `PresentImage`.
  - New shaders `core2_present_out.vert/.frag` (pinned glslang, like the other core 2 shaders).
- **Switches:**
  - `GT4_HW_PRESENT_DIRECT=0` keeps today's chain (A/B).
  - Android default for `GT4_HW_PRESENT_QUEUE` from phase 0's result.
- **Exactness:**
  - The picture bytes must equal today's when upright: unpack `k`, write `k/255.0` to a UNORM attachment, and the
    conversion rounds back to `k` (H).
  - Gate: a new CTest renders a synthetic `presentOut` through both chains into an offscreen RGBA8 and BGRA8 "swapchain"
    and compares bytes. It covers rect offsets, mirrored transforms and the quarter turns (compared as a permutation).
  - Plus the presented-frame dump hashes.
  - The composed frame and everything upstream is untouched.
- **Risks (medium):**
  - the HUD layer composes into the same image (record it in the same render pass, or after it);
  - the three present paths (`own_queue`, `shared_queue`, `rendering_queue`);
  - Performance mode on integrated GPUs uses box-halving blits of the internal image (IN-375 and IN-478) and keeps the old
    chain until it gets the same treatment.
- **Gain:** section 4.2: about 95-110 MB less DRAM per frame and an estimated 0.7-2.5 ms of hidden GPU per frame on the
  S23 (Hyp; P1-E measures before and after).
- **Effort:** 3-5 days, shared with the pre-rotation work.

### 7.4 Phase 3: UMA placement and footprint

- **(a) Upload memory type per shape, from the phase 0 A/B:**
  - In `Gpu::Init` (3021-3025), replace the device-type test with the memory shape and the measured winner (for example
    uncached `L|V|C` on `unified_all_local` tilers, cached on Intel).
  - Keep `GT4_CORE2_CACHED_UPLOAD` as the A/B.
  - Exact.
- **(b) Ring sizes from the phase 1 high-water marks:**
  - In `plan_core2_rings` (`hw_device_profile.hpp:192-236`), add a per-shape cap (for example 96-160 MiB in all on
    phones) with a margin over the measured highs.
  - Keep `GT4_CORE2_RINGS=fixed` as the A/B.
  - Exact; the gate is `ring_full_submits` near 0 on the drives.
- **(c) Vertices written in place:**
  - `Gpu::Draw` hands out a mapped span (`Gpu::ReserveVertices(count) -> GpuVertex*`) and the front end's batch builder
    writes there.
  - Files: `frontend/hw_batch.cpp`, `core2_renderer.cpp` (`ProcessBatch`), `core2_gpu.cpp` (`Draw` 3638-3645).
  - Switch `GT4_CORE2_VERTEX_IN_PLACE=0`.
  - Exact: the same bytes at the same offsets. Gate: frame hashes.
- **(d) Footprint:**
  - Prove or disprove that the atlas is only a placeholder: a counter of draws whose shader takes a texel from binding 3
    while `memoryTexture == kNoImage`, run on all drives.
  - If it stays 0, a 1x1 atlas saves 64 MiB. Keep the 4096 atlas behind `GT4_CORE2_ATLAS_SIDE=4096` for the A/B.
  - Free empty image blocks under memory pressure (`onTrimMemory`, ANDROID-PERFORMANCE-PLAN 4.4).
- **(e) Discrete-only, optional and outside UMA:** the static `glmTables` and `inverseCt32` in device-local memory with a
  one-time copy. Exact. Keep it as a separate change so the discrete path changes only when someone chooses to.
- **Effort:** 3-5 days. **Risk:** low, (c) medium (front-end plumbing).

### 7.5 Phase 4: keep the GPU fed around readbacks

- **4a (exact, low-medium risk, 2-3 days):**
  - **(1)** The K rule on by default for tilers if phase 0 confirms (`Gpu::Init` 3018).
  - **(2)** Pre-read cadence (5.2), switch `GT4_CORE2_PRE_READ_CADENCE=<batches>`, 0 = off, in `Gpu::Draw`'s submission
    rule (the `batchesSinceSubmit` logic around IN-410). The renderer passes a hint (`Gpu::ExpectReadSoon(bool)`) from
    `ReadFifo`'s history (`core2_renderer.cpp:5518`).
  - **(3)** Bounded fence spin in `wait_slot` (2516-2521): poll `vkGetFenceStatus` on the newest slot for at most
    `GT4_HW_READBACK_SPIN` µs, then wait.
  - **(4)** ADPF work-duration reports for the GS worker (Android only, `android_*`).
  - Gates: flare hashes equal for every setting (submission boundaries and wait methods never change output), `gates6`.
- **4b (high risk, 1-2 weeks):** the asynchronous flare read over a coherent sequence word (5.3), behind
  `GT4_CORE2_ASYNC_FLARE_READ`.
  - Prerequisite: a deferred write-back layer in front of GS memory, with every reader hooked (the O3 lesson).
  - Gates: the per-reply trace `GT4_HW_FLARE_REPLY_TRACE` on Hong Kong and Motegi at 2x, 6x and 10x, where Motegi's
    overlapping scratch windows are the known trap.

### 7.6 Phase 5: shared GS-memory mirror (only if phase 1 shows it is worth it)

- **Design (H):**
  - The 4 MiB CPU mirror becomes 512 logical pages mapped onto a pool of 8 KiB physical slots in one mapped `L|V|C|H`
    allocation.
  - Each GPU command that reads GS memory directly (decode, dirty patch, CLUT from memory) gets a 2 KiB page table
    (512 x 32-bit slot numbers) in the uniform or staging ring.
  - A CPU write to a page whose slot is referenced by an in-flight submission first takes a new slot (copy-on-write of
    8 KiB, then the write).
  - The decode windows (U3) and most GLM uploads (U2) disappear.
  - The GLM keeps its role for GPU-written target pages (W3).
- **Exactness:** each GPU reader sees the page versions as of its recording, which is what today's window snapshots give.
- **Do it only if** the phone's `window + glm` bytes in the P1-C line exceed about 2 MB per VBlank, or their memcpy time
  exceeds about 0.3 ms.
- **Effort:** 2+ weeks. **Risk:** high (every CPU writer of GS memory goes through copy-on-write; `hw_glm_decode.comp`
  and the patch and CLUT kernels gain page-table addressing).

### 7.7 Phase 6: research items (bandwidth and CPU, not copies)

- **Narrow texel images for indexed decodes:** T8 and T4 indices as `R8_UINT` instead of `R32_UINT` (IN-409 palette rows
  already keep indices). That is a quarter of the texture-cache memory and of the sampling bytes for those textures. It
  needs a new binding in `hw_pixel_uber.frag`. Exact.
- **Texel-buffer reads of the GLM or windows** (2.5).
- **`VK_EXT_host_image_copy`** for texture-replacement uploads (2.4), where `optimalDeviceAccess` is true.
- **Lazy CPU swizzle** (section 6).
- **The tiler fix itself: the D1 attachment path** and framebuffer fetch (`VK_EXT_rasterization_order_attachment_access`
  or dynamic-rendering local read). Not UMA, but the largest bandwidth lever on Adreno and Apple GPUs (ANDROID-PORT-PLAN).

---

## 8. Open questions and what to measure on the phone first

1. **(U) From the logs already exported:**
   - whether IN-481 (colour attachments) is on;
   - the presentation path (own, shared or rendering queue);
   - the swapchain format and pre-transform;
   - the interlock line;
   - the ring plan line;
   - `glm` pages, `window_mb`, `image_mb` and `texture_mb` per VBlank;
   - `uploads_replayed` per VBlank (needs the session's VBlank count).
2. **(U) What memory types 1, 2, 7 and 8 are** (lazily allocated or protected): phase 1's letters show it.
3. **(Hyp) Cached against uncached rings:** `GT4_CORE2_CACHED_UPLOAD=0`. Read the GS thread `draw` and `upload` stages,
   and `gpu_busy`.
4. **(Hyp) The hidden present cost:** P1-E's timer at 1x and 2x, normal and Performance, race and 480i menu; and
   `GT4_HW_PRESENT_QUEUE=0` against the default, for both time and pacing (frametime graph).
5. **(U) DRAM bytes and GPU frequency over a race minute** in Snapdragon Profiler or AGI:
   - whether core 2's passes run binned or direct;
   - whether any core 2 image is UBWC-compressed;
   - how the GPU clock behaves around flare drains.
6. **(U) How far Adreno timestamps written outside render passes distort `gpu_busy`:** compare with the profiler's GPU busy.
7. **(Hyp) The K rule on the phone** (`GT4_CORE2_SUBMIT_AFTER_WAIT=8`): drains per VBlank, GPU idle per drain, read wait,
   wall.
8. **(U) I_cpu on the phone** (the TASK-178 `[hw] flare replies ... i_cpu_ms/read` field): it decides whether 4b is worth
   its risk.
9. **(U) Sustained behaviour:** ten-minute runs with the thermal headroom logged (ANDROID-PERFORMANCE-PLAN 3.3) before and
   after phase 2, because phase 2's main payoff may be heat rather than frame time.
10. **(U) Platform checks with `vulkaninfo`:**
    - `VK_EXT_host_image_copy`, `VK_EXT_external_memory_host` and `VK_EXT_descriptor_buffer` on the Adreno 740;
    - the MoltenVK shared type's `HOST_CACHED` bit;
    - fragment shader interlock on MoltenVK.

---

## Appendix A. Switches named in this study

**Existing:**
- `GT4_CORE2_CACHED_UPLOAD`, `GT4_CORE2_RINGS`, `GT4_CORE2_SLOTS`, `GT4_CORE2_SUBMIT_EVERY`, `GT4_CORE2_SUBMIT_AFTER_WAIT`.
- `GT4_CORE2_ROP`, `GT4_CORE2_SELF_COPY_DRAW`, `GT4_CORE2_DEINT_FIELD_COMPOSE`.
- `GT4_CORE2_BARRIER_TRACE`, `GT4_CORE2_GPU_CATEGORIES`.
- `GT4_HW_PRESENT_QUEUE`, `GT4_HW_PRESENT_SHARED_QUEUE`.
- `GT4_HW_READBACK_SPIN` (parsed, unused by core 2).
- `GT4_HW_FLARE_REPLY_TRACE`, `GT4_HW_DUMP_SOURCE`.

**New, phase 1:**
- `GT4_HW_MEMORY_SHAPE` (diagnostic override).
- `GT4_CORE2_HOST_READ_BARRIER` (A/B, default on).
- `GT4_HW_PRESENT_GPU_TIMING` (diagnostic, default off).

**New, later phases:**
- `GT4_HW_PRESENT_DIRECT` (P2).
- `GT4_CORE2_VERTEX_IN_PLACE`, `GT4_CORE2_ATLAS_SIDE` (P3).
- `GT4_CORE2_PRE_READ_CADENCE`, `GT4_CORE2_ASYNC_FLARE_READ` (P4).
- `GT4_CORE2_SHARED_MIRROR` (P5).

## Appendix B. Sources outside the repository

- **Vulkan specification:**
  - synchronisation: host access, fence signal operation note, `vkQueueSubmit` host-write guarantee;
  - memory properties;
  - `VK_EXT_host_image_copy` and Vulkan 1.4 `hostImageCopy`.
- **MoltenVK `MVKDevice.mm`:** `initMemoryProperties`, `populateHostImageCopyProperties`
  (https://github.com/KhronosGroup/MoltenVK).
- **Qualcomm, "Mobile best practices" (Adreno):** UBWC conditions, `MUTABLE_FORMAT` before Adreno 750, lazily allocated
  memory, timer queries in binned rendering
  (https://docs.qualcomm.com/bundle/publicresource/80-78185-2/topics/mobile_best_practices.md).
- **Apple Metal documentation:** `MTLStorageMode.shared` on Apple silicon
  (https://developer.apple.com/documentation/metal/mtlstoragemode/shared).
