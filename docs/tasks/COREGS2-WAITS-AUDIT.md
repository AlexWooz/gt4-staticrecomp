# CoreGS 2.0 waits and async audit (TASK-215c, 2026-11-10)

Read-only audit of **every wait in the CoreGS 2.0 in-process path**, with the async/overlap design that removes or hides
it. Companion to `docs/tasks/COREGS2-PERF-AUDIT.md` (TASK-215b): that page maps CPU/GPU cost; this page maps **time the
pipeline spends not working**. The owner's direction (2026-11-10, voice): the bottleneck is **waits**, not
rasterisation; the target is weak/mobile hardware (Galaxy S23 Ultra class, tile GPU); the synchronous mode must become
asynchronous; the GPU should start the next task/frame while the EE waits for a readback, finish the waiting task when the
answer arrives, present, and resume; never let a wait block the pipeline. Frame skipping is only for occasional 59/58
dips; 30 fps is the last resort. Normal mode keeps the **same-frame** flare; a one-frame-late readback only in
Performance Mode or with the owner's explicit acceptance.

- Worktree: `C:\Antigravity\gt4-wt-215-coregs2-audit`, branch `docs/coregs2-perf-audit`, from `origin/main` `59f27aa1`
  (the branch was at `37f0c9b6` when this audit started).
- No build, benchmark or game run. Every number is quoted from the existing ledger, CPU survey, game-code study or the
  previous audit, with its source. New ideas carry the label of their evidence.
- Not PCSX2-tainted: no PCSX2, GSdx, `gt4recomp-gs`, `private/tooling/pcsx2-*`, `private/slice189-pcsx2-ref` or ARMSX2
  source was opened. GSdx is a black box. The mobile notes use architecture facts and prose only.
- Labels: **C** = confirmed (measured, or read directly in the code cited); **H** = high confidence; **Hyp** =
  hypothesis; **Unknown** = needs a measurement or a design decision.

## 0. The short answer

The current frame pipeline is **half asynchronous**: the EE can run up to two VBlanks ahead of the GS worker, the GS worker
records up to 8/16 Vulkan submissions ahead of the GPU, and the second presentation queue decouples the display on a
device with two queues. But it has **one hard synchronous point per race frame**: the flare visibility table. The GS
worker reaches the 2 KiB download, submits a full reduction and calls `vkWaitForFences` on **every** in-flight slot; the
VU1 worker cannot produce the flare draws until the bytes arrive; the EE can continue for a while but the VU1 input/output
rings fill; the GPU finishes the work before the read and then **idles**; the next frame's stream cannot pass the read.
The measured cost is 4.5-5.5 ms per VBlank at 6x on the Avell laptop, 1.3 ms at 1x (C, TASK-134 section 9); the same
wait, on a much slower tile GPU, is likely the frame (Hyp for the target; no mobile measurement). The exact way out is **GPU-side flare modulation** (GS-READBACK-STUDY S4):
the VU1 no longer needs the table, the readback/`vkWaitForFences` disappears, and the stream never stops. The other waits
are smaller but matter on mobile: the **one-queue present** (`acquire_image(UINT64_MAX)` on the GS worker; the second
queue exists only when the queue family has two queues), the **submission-slot fence** (8 slots / 32 batches on an
integrated GPU; the code comment itself records the GS thread waiting "on every heavy stretch"), the **tone-hook CLUT
readback** (`Gpu::ReadClut` -> full GPU wait), and the **readback-before-write/keep** that remains after IN-508. There is
**no separate submission thread** today; the GS worker records and submits on one thread. The async priorities are: kill
the flare readback (C), decouple the one-queue present, deepen/adapt the submission ring, move the tone CLUT read to the
shader, move the remaining before-write/keep stores to the GPU, then a submission thread and the mobile shader warm-up.

## 1. The current pipeline: what overlaps and what does not

```
EE thread     guest -> VU1 jobs -> input ring -> [2 VBlanks ahead] -> VBlank: push Vsync, pacer, present request
VU1 worker    VU1 programs -> PATH1 output ring -> (GS worker consumes, direct intake IN-461/IN-462)
GS worker     stream in order -> batches -> record -> submit every 64/32 batches -> 8/16 slots in flight
              at a readback: Reduce -> vkWaitForFences on all slots -> copy bytes -> resume
              at VBlank: front-end flush, compose, present (second queue if available)
GPU           1 graphics/compute queue (core2) + optional second present queue; the draw/compute passes
Present       2 presentations in flight, 3 frame slots, 3 swapchain images (IN-511/IN-516)
```

**Frames and buffers in flight (C, code):**

| Item | Value | Where |
|---|---|---|
| Guest VBlanks the EE can queue ahead of the GS worker | 2 (`kFramesAhead`) | `hw_gs_stream_sink.cpp:508` |
| GS stream ring | 16 MiB | `hw_gs_stream_sink.cpp:510` |
| CoreGS 2.0 submissions in flight | 8 integrated / 16 discrete (`GT4_CORE2_SLOTS` 2-24) | `core2_gpu.cpp:205-218`, `2471` |
| Batches per submission | 32 integrated / 64 discrete (`GT4_CORE2_SUBMIT_EVERY`) | `core2_gpu.cpp:269-277`, `2472` |
| Presentations in flight | 2, 3 frame slots, 3 swapchain images | `hw_gs_backend.cpp:44-45`, `IN-511/IN-516` |
| Second presentation queue | only when the graphics family has `queueCount >= 2` | `hw_gs_backend.cpp:686-688`, `774-775` |
| Separate submission thread | **none** | `Gpu::submit` runs on the GS worker (`core2_gpu.cpp:2188-2212`); the `pacer_thread` exists only for frame skipping (`hw_gs_backend.cpp:1254-1276`); the `frametime` thread only reads present times |
| Next frame's CPU recording while the GPU works | **yes, until the slots fill or a readback blocks** | `begin_recording` waits only the oldest slot (`core2_gpu.cpp:2150-2157`); `submitEvery` lets the GPU start early (`3574-3577`) |

**Where the pipeline actually stops (C/H):**

1. **The flare readback, once per race frame.** `read_fifo` posts the read; the GS worker executes it at its stream
   position (`hw_gs_stream_sink.cpp:682-688`), calls `Renderer::ReadFifo` (`core2_renderer.cpp:5162-5204`), which calls
   `Readback` (`2643-2731`) -> `Gpu::Reduce` (`core2_gpu.cpp:4089-4119`) -> `submit(true)` (`4114`) -> `wait_all`
   (`2143-2148`) -> `vkWaitForFences(UINT64_MAX)` on every slot (`2103`). The VU1 worker's `wait_copy`
   (`hw_flare_table.hpp:65-69`) cannot finish the VIF packet that uses the table until the GS worker's `finish`
   (`85-92`); the VU1 output after that packet does not exist; the GS worker has nothing after the read to process; the
   GPU finishes the work before the read and then has no queued work. **The whole chain drains once per frame.** The EE
   itself no longer waits (IN-522), but the EE's next frame fills the VU1 input ring and the GS output ring; the rings
   only move the stall, they do not remove it. Measured: 4.5-5.5 ms at 6x, 1.3 ms at 1x (C, TASK-134 section 9); the
   ledger's 373 build had 3.80 ms of "flare and other reads" and 3.42 ms of GPU waits; the final 374 table shows EE at
   the read 0.00 (C/H, ledger 373/374).
2. **The submission-slot fence.** `begin_recording` waits for the slot before the last submitted (`core2_gpu.cpp:2150-2157`).
   With 8 slots / 32 batches on an integrated GPU and 25-45 submissions per 6x VBlank, the comment at `205-208` records
   the GS thread waiting "within a third of a frame of the GPU, waiting for it on every heavy stretch". This is the
   back-pressure wait that is hidden on the fast PC and likely large on the S23 Ultra (Unknown; no mobile measurement).
3. **The one-queue present.** Without a second queue, the GS worker calls `vkWaitForFences` for the present slots
   (`hw_gs_backend.cpp:1414-1416`) and `acquire_image(UINT64_MAX)` (`1419`); with FIFO the acquire waits for the display.
   The measured 60 Hz virtual-display showroom wait was 11.42 ms (8 slots) and the races had 17.7/33 ms waits; the second
   queue brought the showroom to 0.01 ms (C, IN-516). On a mobile with one queue family the GS worker has exactly this
   wait, and it is the **present** blocking the next frame's stream.
4. **The tone-hook CLUT readback.** `ToneHook` (`core2_renderer.cpp:3278-3337`) calls `SyncClutToCpu` (`1075-1089`),
   which calls `Gpu::ReadClut` (`core2_gpu.cpp:3994-4008`): a `submit(true)` and a full `vkWaitForFences` to copy 512
   halfwords to the CPU. It happens only when a GPU-written palette is used by the colour filter, but that is the race
   colour filter; the wait is not attributed by the `gpu_wait by reason` line (the `ReadClut` path does not set
   `readbackReason_`), so its cost is **Unknown** in the current reports.
5. **The readback-before-write / keep.** `StoreDrawnBeforeWrite` (`core2_renderer.cpp:2826-2909`) and `KeepGpuReadContent`
   (`1734-1765`, `Age` `4949+`) call `Readback` and wait. IN-508 removed the "write touches no drawn page" case (3.00 ->
   0.00 per VBlank, readback phase 6.91 -> 2.76 ms at 6x; C, ledger IN-508); the remaining stores are the showroom
   backdrop (IN-538), photo mode (IN-537/539) and the keep of a GPU-read target before ageing.
6. **One-shot downloads.** A non-flare `read_fifo` (`hw_gs_stream_sink.cpp:895-901`) calls `GsWorker::drain`
   (`591-599`): the EE waits for every queued record, then the GS worker on the same thread does the GPU readback. The
   car icons at load, the loading picture, the screen thumbnail and photo autofocus/capture are one-shots (GS-READBACK-
   STUDY section 1.1); the race-start camera cut (27-33 readbacks, about 42 ms once) is the measured stutter (IN-429/430;
   C/H, STATUS 2026-11-01).
7. **The EE-side pacing and audio waits.** The pacer sleeps/spins on the wall-clock grid (`window.cpp:618-657`,
   `frame_clock.cpp:49-77`); the audio mode waits on `MsgWaitForMultipleObjects(..., 50)` (`host_io.cpp:471-494`). These
   are intentional cadence, not renderer stalls, but they are part of the frame budget and the phone's timer/spin cost.
8. **The locks and small waits.** `backendMutex_` per stream record (`hw_gs_stream_sink.cpp:735-740`), the flare
   condition variable (`hw_flare_table.hpp:103-104`), the texture-replacement worker (`core2_replace.cpp:36-75`), the
   pacer/frametime condition variables (`hw_gs_backend.cpp:162,225,251,1248,1262,1282`), the shader worker
   (`core2_gpu.cpp:390,1378`), the `core2` slot tests, and the resize/shutdown `vkDeviceWaitIdle`
   (`hw_gs_backend.cpp:368,957,1650,1727`). The first four are small; the last three are not per-frame.

## 2. Every wait class and call site

The table is the complete inventory for the CoreGS 2.0 in-process path. "Blocks" is what the waiter prevents. The
"overlap?" column says whether the work can continue elsewhere (see section 3 for the design).

| ID | Layer / wait | Call site | Who waits / blocks | Known cost (evidence) | Label | Overlap? |
|---|---|---|---|---|---|---|
| W1 | Pacer sleep/spin on the wall-clock grid | `window.cpp:618-657`; `frame_clock.cpp:49-77`; `pacing.cpp:84-102` | EE; the next guest frame | 16.7 ms cadence; 0.5 ms spin; `GT4_PACING_MIN_INTERVAL_US=15700` (IN-519) | C | no (cadence) |
| W2 | Audio device wait (`GT4_PACING=audio` only) | `host_io.cpp:471-494`, `500`, `596` | EE; the guest push | 50 ms `MsgWaitForMultipleObjects`; 5 s stall guard; default `paced` avoids | C | yes: use the paced ring |
| W3 | EE -> GS VBlank back-pressure | `hw_gs_stream_sink.cpp:522-535` | EE; the next VBlank | 0 at 3x in the CPU survey (`[hw] transport vsync_wait_ms`); mobile Unknown | C/H | yes: deeper queue/GS speed |
| W4 | EE -> GS ring full | `hw_gs_stream_sink.cpp:536-552` | EE; the GS stream | `max_depth` counter; CPU survey 5.9 MB/VBlank, 16 MiB ring | C/H | yes: ring/GS speed |
| W5 | EE non-flare `drain` + GPU readback | `hw_gs_stream_sink.cpp:858-901`, `591-599`; `core2_renderer.cpp:5162-5204` | EE; the one-shot read | load/camera cut 27-33 reads, ~42 ms once (IN-429/430); `drain_wait_ms`/`read_fifo_ms` | C/H | no for the bytes; async ticket possible |
| W6 | Flare table EE wait (fallback) | `hw_gs_stream_sink.cpp:876-894`; `hw_flare_table.hpp:72-82` | EE; the read | pre-IN-522 8.5 ms at 6x; default now deferred (IN-522) | C | yes: keep deferred / GPU flare |
| W7 | MTVU VBlank drain | `mtvu_runtime.cpp:762-769`, `801-803`; `mtvu_journal.cpp` | EE; the VU1 tail | 1.08 ms vblank at 1x day (FRAME-COST); the 3.17 ms download at 3x is W8; `[mtvu] EE wait` | C | partially: VBlank journal entry, worker speed |
| W8 | MTVU download sync before a GS read | `mtvu_runtime.cpp:565`; `mtvu_wait_download` | EE; the read | 3.34 ms at 1x night before IN-461; the read moved to the GS worker (IN-461) / removed for the flare (IN-522); current split Unknown | C/H | yes: direct intake + deferred table |
| W9 | MTVU input/output ring back-pressure | `mtvu_journal.cpp:194-204`, `212-235`; `mtvu_runtime.cpp:769` | EE, VU1 worker | `mtvu_wait_ms`, `mtvu_sleeps`, `input_bell`/`output_bell` stats; the VU1 worker 12.6 ms on the notebook (TASK-134) | C/H | partially: ring sizes/GS speed |
| W10 | VU1 worker waits for the flare table | `hw_flare_table.hpp:65-69`; `hw_gs_stream_sink.cpp:845-856` | VU1 worker; its output | the GS worker's GPU wait propagated; C removes | C/H | yes: GPU flare |
| W11 | GS worker waits for VU1 output | `hw_gs_stream_sink.cpp:707-742` (`mtvuBell_->wait_changed`, `717`) | GS worker; the stream | `[hw] transport mtvu_wait_ms`; license VU1 15 ms (TASK-171) | C | partially: VU1 speed/ring |
| W12 | GS worker submission-slot fence | `core2_gpu.cpp:2099-2140`, `2150-2157`, `2188-2212` | GS worker; the next recording | 8/16 slots; 25-45 submits per 6x VBlank; comment 205-208 "waiting on every heavy stretch"; `stats.waitNs` | C/H | yes: more slots / adaptive interval |
| W13 | GS worker readback (flare) | `core2_renderer.cpp:2643-2731`; `core2_gpu.cpp:4089-4119`, `4114` | GS worker; the stream | 4.5-5.5 ms at 6x, 1.3 ms at 1x (TASK-134 section 9); 3.80 ms reads / 3.42 ms waits at 373 | C | yes: GPU flare (C) |
| W14 | GS worker readback (before write) | `core2_renderer.cpp:2826-2909` | GS worker; the write | IN-508: 3.00 -> 0.00 per VBlank at 6x, readback phase 6.91 -> 2.76; remaining whole-page writes | C | yes: block patch/GPU store |
| W15 | GS worker readback (keep) | `core2_renderer.cpp:1734-1765`, `4949+` | GS worker; target ageing | the remaining 2.76 ms phase at 6x includes keep; exact share Unknown | H | yes: GLM newest copy (TASK-162) |
| W16 | GS worker tone CLUT readback | `core2_renderer.cpp:1075-1089`, `3278-3337`; `core2_gpu.cpp:3994-4008` | GS worker; the tone pass | `ReadClut` `submit(true)`; `clut_reads`; per-reason `clut` not attributed; cost Unknown | H | yes: tone shader reads the GPU CLUT |
| W17 | GS worker present (one queue) | `hw_gs_backend.cpp:1410-1467` | GS worker; the next frame | 11.42 ms showroom, 17.7/33 ms races at 60 Hz (IN-516); the second queue 0.01 ms | C | yes: second queue/present family/thread |
| W18 | GS worker present (second queue) | `hw_gs_backend.cpp:1155-1237` | GS worker; the copy/present fences | `fences[slot]`/`draw_fences`; the display acquire is `timeout 0` and drops; the ledger 0.01 ms showroom | C/H | partially: frames/slots/fences |
| W34 | GPU semaphore waits in the present | `hw_gs_backend.cpp:1211-1230`, `1313-1326` | GPU, not CPU | the second queue waits `acquire_semaphores`/`picture_ready`; the present waits `render_finished`; no CPU block | C | no: the GPU order |
| W19 | Paced present / 30 fps | `hw_gs_backend.cpp:1254-1332`, `1297-1301` (`acquire_image(100000000)`), `1335-1380` | pacer thread/GS worker | `IN-525`; only frame skip; 30 fps last resort | C | no (the owner's choice) |
| W20 | Frame-skip transition drain | `hw_gs_backend.cpp:1279-1283`, `1405` | GS worker; the pacer queue | only after frame skipping; not the strategy | C | no |
| W21 | Resize/orientation `vkDeviceWaitIdle` | `hw_gs_backend.cpp:955-960`, `1727`; `1650` | GS worker/EE; the swapchain | one-time; not steady | C | no |
| W35 | Window attach/resize drain | `hw_gs_stream_sink.cpp:937-939`; `hw_gs_backend.cpp:1727` | GS worker; the swapchain | one-time; the `drain` then `vkDeviceWaitIdle` | C | no (not steady) |
| W22 | Pipeline/shader warm | `core2_gpu.cpp:2320-2392`; `1378`; `IN-437/489/507` | GS worker/driver; the first variants | 97 variants, 128 ms each on average, max 250 ms (IN-437); the prepare/cache removes from play | C/H | yes: prepare/cache |
| W23 | Allocation (`vkAllocateMemory`, image growth) | `core2_gpu.cpp:512`, `546-599`, `616-699`, `728-733` | GS worker/driver | IN-424: race start 40-160 new images/VBlank, 60 VBlanks over 20 ms -> 2; IN-418 churn | C | yes: blocks/recycle (done) |
| W24 | Texture replacement wait | `core2_replace.cpp:36-75`; `core2_renderer.cpp:3798` | GS worker; the batch | `GT4_HW_TEXTURES_WAIT_US` default 6000; 0.67 s over the Tokyo recording (IN-533) | C | yes: wait 0/late texture |
| W25 | Backend mutex per record | `hw_gs_stream_sink.cpp:735-740` | GS worker; the record | uncontended mostly; `execute_mtvu_job` locks per VU1 record | H | yes: one lock per job/lock-free |
| W26 | Flare condition variable | `hw_flare_table.hpp:65-69`, `103-104`; `hw_gs_stream_sink.cpp:680-688` | VU1/GS worker | the same GPU wait; C removes | C/H | yes: GPU flare |
| W27 | Pacer/frametime condition variables | `hw_gs_backend.cpp:162,225,251,1248,1262,1282` | pacer/frametime threads | the pacer is frame skip/30 only; the frametime thread (latency control) reads present times with a timeout-0 poll; neither is the GS worker | C | no (not steady) |
| W28 | Full/global GPU barriers | `core2_gpu.cpp:1552`, `1724-1802`, `IN-419/464` | GPU; the next command | 579 -> 276 barriers at 3x (IN-464); each barrier can stall a tile GPU | C | partially: page barriers/attachments |
| W29 | Render-pass boundaries | `core2_gpu.cpp:1878+`, `IN-420` | GPU; the next pass | 106-312 -> 4-8 passes (IN-420) | C | no: already reduced |
| W30 | GPU idle gaps | no call site; the GPU has no queued work | GPU; the next work | IN-425: 5.9 ms isolated GPU against 7-19 ms/VBlank; the flare drain; the slot wait | C/H | yes: the async items |
| W31 | `vkQueueWaitIdle`/`vkDeviceWaitIdle` (CoreGS 1.1) | `native_vulkan.cpp:183-197`, `849`, `885-889`; `hw_gs_writeback_gpu.cpp:1367/1377/2405-2413/2563/3372-3380` | CoreGS 1.1 thread | **not the CoreGS 2.0 path**; listed for completeness | C | n/a |
| W32 | `vkWaitSemaphores` timeline (CoreGS 1.1) | `native_vulkan.cpp:712`, `722`, `735` | CoreGS 1.1 thread | **not CoreGS 2.0** | C | n/a |
| W33 | External GS process waits | `gs_stream_ipc.cpp:328`, `478` | PCSX2 GS path | **not CoreGS 2.0** | C | n/a |

**Not waited on / not a renderer wait:** `achievements::on_vblank` (`window.cpp:716`), the performance HUD thread
(`hw_hud_system`), the input poll, the message pump, the frame-skip decision, the perf probe. They are outside this map.

## 3. Async/overlap design, wait by wait

The goal is not "make every wait shorter": it is to **never let a wait stop the producer**. The producer is the EE/VU1/GS
stream; the consumer is the GPU; the present is a third stage. The design below says, for each wait, who can keep working
and how.

### 3.1 The flare readback (W6/W10/W13/W26): remove the CPU read, not just shorten the wait

**Current:** the GS worker submits the reduction and waits; the VU1 waits for the bytes; the next frame's stream cannot
pass the read. **Design (C, GS-READBACK-STUDY S4):** replace the REF'd qword with 0xFF, mark the VU1 job that uses the
table, and modulate the flare draws in the shader from the 64x8 target. The EE/VU1/GS pipeline never stops; the GPU keeps
the table on-chip; the same-frame result is exact (the VU1 program must use the value linearly: P1). **Overlap:** the
next frame's recording proceeds while the GPU finishes the current frame; the GPU never idles at the read; the VU1 worker
never waits. **Risk:** the marker must cover exactly the flare primitives; ±1 LSB on flare pixels; the table target must
be read after the sample draws (the existing `touch`/barrier tracking). **Test:** P1 harness; night race 0 table
readbacks; flare pixels within ±1; all other pixels identical; 3x/6x/12x; F8 on the target. **Label:** cost C, gain H.
**Smallest proof:** `GT4_CORE2_DELAYED_DOWNLOADS=1` or `GT4_GS_HIDE_FLARES=1` on the target removes the same wait (with
the owner-rejected lateness / the missing effect); the delta is the upper bound; P1 is the offline gate.

### 3.2 The readback before a write and the keep (W14/W15): keep the target on the GPU

**Current:** a CPU write over a drawn target stores it first; a GPU-read target dropped by ageing is stored. **Design:** the
GLM holds the pages as the newest copy of memory (TASK-162's design), so a target needs one store per round of reads, not
one per write; whole-block writes go through the GPU (IN-539, currently off because it repeats the store), and the small
writes use the block merge (IN-512/IN-538). The CPU never waits for the store; the next draw/write uses the GPU copy.
**Overlap:** the GS worker records the write and the target's patch; the GPU does the store/patch behind the stream; the
EE/GS continue. **Risk:** memory is older than the targets until a read; a target that is later read back must see the
newest copy (the GLM); the `IN-539` measurement showed the GPU store can cost more GPU time if repeated. **Test:**
`[core2] gpu_wait ... before write/keep`, `GT4_CORE2_PATCH_BLOCKS=0/1`, `GT4_CORE2_WHOLE_BLOCK_WRITES=0/1`, frames
identical. **Label:** H for the mechanism, the gain Unknown until the target scene (showroom/photo/6x race). **Smallest
proof:** one showroom/photo run with the `before write`/`keep` reasons and the existing A/B.

### 3.3 The one-shot readbacks (W5): a ticket, not a stop-the-world

**Current:** a non-flare read drains the GS worker and reads the GPU on the EE/GS thread; the EE needs the bytes. **Design:**
at the request, record the copy to a host-visible buffer and return a ticket; the GS worker continues with later stream
records; the EE copies the bytes when it actually consumes them (or at the next VBlank). The `ReadFifo` API is synchronous
in the guest; a one-frame-late answer is only acceptable for the loading screen/icon if the owner says so (GS-READBACK-
STUDY R0/R1); the photo autofocus is read immediately and must stay exact. **Overlap:** the copy overlaps the next
frame's GPU work; the EE's wait moves to the last moment. **Risk:** the game may consume the bytes immediately; the
one-shot inventory is C (GS-READBACK-STUDY section 1.1); no change to the flare. **Test:** the icon/loading/photo scenes,
the `read_fifo_ms`/`drain_wait_ms`, frames. **Label:** H for the overlap, the acceptance is the owner's. **Note:** this is
**not** the flare; the owner's rejection of the late flare in normal mode does not automatically cover the one-shots, but
each one needs its own acceptance.

### 3.4 The tone-hook CLUT readback (W16): read the GPU CLUT in the tone shader

**Current:** `SyncClutToCpu` copies 512 halfwords from the GPU CLUT to the CPU because the tone pass takes a CPU palette;
the GS worker waits. **Design:** bind the GPU CLUT (`s.clut`, already a storage buffer bound in the decode/CLUT sets,
`core2_gpu.cpp:3631-3651`) to the tone compute shader and decode the palette entries there, exactly as
`hw_glm_decode.comp`'s `gpu_palette_entry` does; `Gpu::Tone` no longer takes `palette[256]`, and `ToneHook` no longer
calls `SyncClutToCpu`/`ReadClut`. **Overlap:** the GS worker records the tone pass; no CPU read; the GPU keeps the
palette. **Risk:** reproduce the CPSM/CSA/TEXA expansion exactly; the `IN-408` GPU CLUT is the authoritative buffer; the
tone frames must be identical. **Test:** `GT4_CORE2_TONE_TRACE=1`, `clut_reads` 0, `[core2] gpu_wait ... clut` 0, the
colour-filter frames identical. **Label:** H; the wait's cost is Unknown (not attributed today), the removal is exact.
**Smallest proof:** the `clut_reads` counter and the `[core2] gpu_wait` line on the night race with the colour filter.

### 3.5 The submission-slot fence (W12): deeper, adaptive, or a submission thread

**Current:** the GS worker waits for the oldest of 8/16 slots. **Design:** on the target, measure `GT4_CORE2_SLOTS` 8/16/24
and `GT4_CORE2_SUBMIT_EVERY` 32/16/8; the code already exposes both. The integrated default (8/32) was chosen for the HD
620; the S23 Ultra's tile GPU may need 16/24 and a smaller interval to keep the GPU fed. A **timeline semaphore** or a
per-submission fence would let the GS worker reuse a slot as soon as its fence signals, instead of the ring order; the
current fence is per slot, so a slot is freed in order; a timeline would allow out-of-order reuse only if the command
buffers are independent, which they are not (one queue). **Overlap:** more slots let the GS worker record longer while the
GPU works; the next frame's stream proceeds. **Risk:** more slots increase latency and memory; a smaller interval starts
the GPU earlier but costs submissions. **Test:** `[core2] gpu_wait`, wall/frame, the same scene; the frames identical (no
rendering change). **Label:** H/Hyp; the PC hides it, the mobile Unknown. **Smallest proof:** one run per value on the
target, no code change.

### 3.6 The one-queue present (W17): a second queue, a present family, or a present thread

**Current:** `own_present_queue` is true only if `families[queue_family].queueCount >= 2` (`hw_gs_backend.cpp:686-688`);
otherwise the GS worker uses `present_frame` (`1410-1467`) and waits at `acquire_image(UINT64_MAX)`. **Design:** (a) search
the physical device for a **present-capable family different from the graphics family** (or the same family with two
queues) and create the device with it; the `IN-516` second queue is the existing design, but the selection is narrower
than the hardware; (b) on a device with truly one queue, run the acquire/present on a **presentation thread** with a
semaphore from the render submission, so the GS worker never calls `acquire_image`; the swapchain images are
double-buffered and the render thread does not wait for the display; (c) `VK_PRESENT_MODE`/`VK_KHR_present_wait` for
latency, not throughput. **Overlap:** the GS worker records the next frame while the presentation thread waits for the
display. **Risk:** the one-queue present thread still shares the queue; the present must be ordered after the render
semaphore; the swapchain image lifetime; the Android `SurfaceFlinger`/buffer count. **Test:** the `[hw] presentation
queue` line, `present_us` in `[hw] profile`, `presents_dropped`, wall/frame on the target; the `GT4_HW_PRESENT_QUEUE=0/1`
A/B where two queues exist. **Label:** C for the desktop wait, H/Hyp for the mobile; the S23 Ultra's queue families are
Unknown. **Smallest proof:** one 60 Hz scene on the target with the `[hw] presentation queue`/`present_us` lines; if it
prints "the rendering queue" and `present_us` is large, the wait exists.

### 3.7 The second-queue present fences (W18): they are copy fences, not display fences

**Current:** `present_on_own_queue` waits `fences[slot]` (`1164`), `draw_fences[slot]` (`1175`) and the previous
`draw_fences` (`1192`); the acquire is `timeout 0` and drops. **Design:** keep the display decoupled; the waits are for
the copy of the picture, not for the display. **Overlap:** the next frame records after the copy; if the copy is the
limit, use a **third picture slot** or a smaller copy; the `frame_slots`/`presents_in_flight` are 3/2. **Risk:** more
pictures add latency; the ledger's 4 swapchain images were worse and 24 slots had nothing over 16 (do not repeat).
**Test:** `[hw] profile present_us`, `presents_dropped`, the frame; the second queue A/B. **Label:** C/H; not a primary
on the desktop, possibly on mobile.

### 3.8 The EE-side pacing/audio and the frame-skip transition (W1/W2/W19/W20)

**Current:** the pacer sleeps/spins; the audio mode waits; the frame-skip path has a pacer thread and a `pacer_drain`.
**Design:** the pacer is the cadence, not a renderer wait; on the phone, use the high-resolution timer or a sleep that
does not spin the CPU (the 0.5 ms spin is `frame_clock.cpp:69-77`); the audio mode is not the default; the frame-skip
path is for occasional dips only (owner). **Overlap:** none needed for the cadence; the pacer must not add a second wait
on top of the renderer. **Risk:** changing the pacer changes the game cadence; the identity gates are the race clock and
the presentation intervals. **Test:** the `[pacing]` interval histogram, the frame sheet, the owner's 60 fps. **Label:**
C; not the bottleneck.

### 3.9 The producer/consumer chain (W3/W4/W7/W8/W9/W11): the rings move the stall, the flare is the knot

**Current:** the EE/VU1/GS rings are already lock-free SPSC (IN-446), the EE is two VBlanks ahead, the GS worker reads
the VU1 output directly (IN-461/IN-462), the VU1 wakes the EE quietly (IN-459), and the flare EE wait is gone (IN-522).
The remaining chain: the VU1 worker waits for the flare table; the GS worker waits for the GPU at the read; the VU1 output
ring fills; the EE input ring fills; the EE VBlank back-pressure. **Design:** the GPU flare (3.1) removes the knot; the
ring sizes/slot depth (3.5) absorb the jitter; the VU1 worker speed is TASK-134's (the license 15 ms; the mobile
Unknown); a VBlank-as-a-journal-entry (FRAME-COST item 3) is still not done and would remove the EE's VBlank drain, but
the CoreGS 2.0 present needs the frame's output; the direct intake is the current best. **Overlap:** with the flare gone,
the EE/VU1/GS can run ahead; the rings are the buffers. **Risk:** the ring sizes/CPU/GPU balance; the VBlank ordering.
**Test:** `[mtvu] EE wait`, `[hw] transport`, `[core2] gpu_wait`, frames. **Label:** C for the existing; H for the
remaining.

### 3.10 The small waits: locks, allocations, pipeline warm (W21-W25)

- **Locks:** `backendMutex_` per record; the GS worker is the only backend user during a job; take it once per job and
  release before sleeping on the VU1 ring, or use the `drain` message; the risk is the EE `drain`/present; test the
  stream and the frames. H, small.
- **Allocations:** the readback path allocates `std::vector`s per read in `Readback`/`Reduce`/`ReadFifo`; the
  `PaletteRow` creates a 256-word vector per new row; reuse the containers/counters; the risk is low; the gain Unknown
  (the CPU sampler's 0.3 ms copies are the copies, not the allocations). H, small.
- **Pipeline warm:** `PrepareShaders`/`IN-507` and the cache `IN-489` already remove the first-play compile on the
  desktop; on Android the prepare/cache must be wired; the first play without a cache compiles 97+ variants at 128 ms
  each (IN-437), a one-time stall; the frames are correct with the uber meanwhile. H, medium on mobile.
- **Resize/shutdown `vkDeviceWaitIdle`:** not per-frame; only a window resize/orientation/app-lifecycle stall; do not
  optimize unless measured. C.

### 3.11 The frame ring (2-3) and the present ring: already there, not the lever by itself

The current ring is **2 guest VBlanks EE→GS** (`kFramesAhead=2`), **8/16 core2 submissions** (32/64 batches), and **2
presentations in flight on 3 frame slots/3 swapchain images**. The 2-3 frame ring is therefore already in place; the
owner's "start the next task/frame" is not blocked by the ring size. Going to **4 swapchain images was worse** (race over
17 ms 633 against 182), **24 slots had nothing over 16**, and **latest-ready** had no gain (IN-514..IN-519); do not repeat
those. A deeper ring only delays a stall that remains at the read: the next frame's **records** cannot pass the flare read
because the VU1 output after it does not exist (3.1). The ring changes that are worth a target measurement are the
**submission slots** (8/16/24) and the **submit interval** (3.5), not more pictures. On mobile the present ring and the
swapchain image count are a display/SurfaceFlinger question (3.6), not a renderer one.

### 3.12 Recording while waiting: where the GS worker could keep producing

- **At the flare read:** there is no later record to produce; the VU1 is blocked on the table, and the rest of the frame
  is behind that VU1 job. Only removing the read (3.1) or accepting a one-frame-late table (Performance Mode/owner) lets
  the VU1/GS continue. A deeper ring does not help.
- **At a one-shot read (W5):** the EE drains the GS worker and then performs the GPU read itself; a ticket/host-visible
  copy posted in stream order could let the GS worker continue with later records while the copy completes, and the EE
  copy the bytes at its next use. This is the async-ticket design (3.3); it needs the per-site policy and the owner's
  acceptance for a late one-shot.
- **At the tone CLUT read (W16):** the next batches after the tone do not depend on the CPU palette, but the GS worker
  calls `ReadClut` synchronously. The clean overlap is the shader read (3.4), which removes the read rather than hiding it.
- **At a before-write/keep store (W14/W15):** the GS worker can record the write and let the GPU do the store/patch behind
  the stream (3.2); the CPU must not wait for the store.
- **At the slot fence (W12):** no useful work can be recorded without a free slot; deeper slots/adaptive interval (3.5) or
  a submission thread (3.13) are the only overlaps.
- **At the present (W17/W18):** the second queue already lets the GS worker record the next frame while the display waits;
  the one-queue path needs the present thread/family (3.6). The compose itself is on the GS worker; a separate compose
  thread would need a display-target double buffer, which is a larger design (TASK-082's aux-frame direction).

### 3.13 Async compute/queues and lock-free queues: limited, not the first lever

- **Async compute:** the core2 compute passes (decode, CLUT/palette, patch/GLM, fills, tone, compose) are on the same
  queue and mostly feed the next draw; the order is a dependency, not an overlap opportunity. A separate compute queue is
  order-safe only for work that does not feed the current batch (for example a **next-batch** texture decode), and on a
  tile GPU it may share the shader cores/tile memory and not overlap. The `timelineSemaphore` feature is enabled in the
  device (`hw_gs_backend.cpp:694-695`) but core2 uses fences; a timeline would not remove the GPU work before a read.
  Not a primary lever; do not build it before 3.1-3.6.
- **Lock-free queues:** the GS stream and MTVU rings are already lock-free SPSC (IN-446); the remaining backend mutex is
  per record (3.10) and small; the flare condition variable is the wait itself (3.1). The lock-free direction is
  complete enough; the `IN-448` slot rings were reverted (no gain).
- **The separate presentation queue** is the only multi-queue overlap in use; the one-queue present thread is the mobile
  extension (3.6).

### 3.14 Risk and test for the waits outside the async top

For the waits that are not in the section 4 list, the risk and the exact check are:

| Waits | Risk of the change | Exact test / identity gate |
|---|---|---|
| W1/W2 pacer/audio | cadence/audio regression; the pacer is not a renderer wait | `[pacing]` interval histogram, audio underruns/queue, the frame sheet; the race clock |
| W3/W4 EE→GS back-pressure | a deeper queue adds latency; a smaller ring stalls the EE | `[hw] transport` `vsync_wait_ms`/`drain_wait_ms`/`max_depth`; frames; the input latency |
| W5 one-shot readback | the game may use the bytes immediately; a late icon/photo is visible | the icon/loading/photo scenes, `read_fifo_ms`/`drain_wait_ms`, frames |
| W7/W8/W9/W10/W11/W26 MTVU/VU1 | the VBlank ordering and the VU1 state; a lost wake hangs | `[mtvu] EE wait`, `[hw] transport mtvu_wait_ms`, the MTVU/VU1 verify, frames |
| W14/W15 before-write/keep | memory older than the targets; a later read sees stale pixels | `[core2] gpu_wait` `before write`/`keep`, `GT4_CORE2_PATCH_BLOCKS=0/1`, `GT4_CORE2_WHOLE_BLOCK_WRITES=0/1`, frames |
| W16 tone CLUT | a palette decode mismatch; the colour filter changes | colour-filter frames identical, `clut_reads` 0, `GT4_CORE2_TONE_TRACE=1` |
| W17/W18/W34 present | swapchain lifetime/queue-family misuse; a dropped frame | `[hw] presentation queue`, `[hw] profile present_us`, `presents_dropped`, frames; the validation layer |
| W19/W20 frame skip/30 | a transition hitch; the owner's cadence | `[pacing]`, the frame sheet, the presentation intervals; not the strategy |
| W21 resize `vkDeviceWaitIdle` | a one-time stall on resize/orientation | a resize/orientation log; not per-frame |
| W22 pipeline warm | a first-play stall; a quarantined variant draws with the uber | `shaders.log`, first-run vs warm-cache frames; the picture is unchanged |
| W23 allocation | a driver stall/memory pressure; the image blocks | `[core2] image_mb`/`texture_mb`, `GT4_CORE2_SPIKE_MS`, the race-start frames |
| W24 replacement wait | a late texture for a frame; the `late` counter | `GT4_HW_TEXTURES_WAIT_US=0` A/B, `[core2] texture replacement late`, frames |
| W25/W27 locks | contention/wake cost; no picture change | the CPU sampler, `[hw] transport` wakeups, frames |
| W28/W29 barriers/passes | a tile flush; a picture change if moved | the GPU timestamps, `[core2] full_barriers`/`render_passes`; frames; IN-464/IN-420 already measured |

## 4. Prioritized async list (weak/mobile hardware)

Ordered by expected gain on a tile GPU per unit of effort, then by risk. The first is the only one that removes a
per-frame hard stop; the rest hide or shorten waits that remain after it. "Proof" is the smallest measurement or A/B
that can confirm the wait and the gain, without building the final feature.

| # | Async item | Expected (label) | Effort / risk | Smallest proof |
|---|---|---|---|---|
| 1 | **GPU-side flare modulation** (3.1): no CPU read, no GS/GPU/VU1 wait, same-frame exact | **H**: removes 4.5-5.5 ms at 6x / 1.3 ms at 1x (measured on the Avell), and the whole per-frame drain; the tile GPU's readback is even more expensive | 3-5 d; medium (marker, shader, ±1 LSB) | P1 harness + `GT4_CORE2_DELAYED_DOWNLOADS=1`/`GT4_GS_HIDE_FLARES=1` on the target: the wait disappears; then the exact shader path |
| 2 | **One-queue present decoupling** (3.6): present-capable family or a presentation thread | **H** on a one-queue device: removes the 11.42 ms showroom / 17-33 ms race display wait (measured at 60 Hz on the desktop); the S23 Ultra is Unknown | 2-4 d; medium (queue family/thread, swapchain lifetime) | `[hw] presentation queue` + `[hw] profile present_us` + `presents_dropped` on a 60 Hz target scene; `GT4_HW_PRESENT_QUEUE=0/1` where two queues exist |
| 3 | **Submission-ring depth / submit interval on the target** (3.5): `GT4_CORE2_SLOTS` 8/16/24, `SUBMIT_EVERY` 32/16/8 | **H/Hyp**: the GS worker's slot wait; the code comment records waiting on every heavy stretch with 8/32; the PC hides it | 0.5 d measurement; 1 d to choose/adapt; low | one run per value on the target, `[core2] gpu_wait` and wall; no code |
| 4 | **Tone-hook CLUT in the shader** (3.4): no `ReadClut`, no full wait | **H**: removes the `ReadClut` wait; the cost is Unknown (not attributed); exact | 1-2 d; low-medium (shader palette decode) | `clut_reads`, `[core2] gpu_wait ... clut`, the colour-filter frames; `GT4_CORE2_TONE_TRACE=1` |
| 5 | **Before-write / keep on the GPU** (3.2): GLM newest copy, block patches, no CPU store wait | **H** for the mechanism; the gain is the remaining 2.76 ms readback phase at 6x (IN-508) minus the flare; TASK-162 | 3-5 d; medium-high (GLM pages, ordering) | `[core2] gpu_wait ... before write/keep`; `GT4_CORE2_PATCH_BLOCKS=0/1`, `GT4_CORE2_WHOLE_BLOCK_WRITES=0/1`; showroom/photo frames |
| 6 | **Independent submission thread** (3.5/3.9): the GS worker records while the driver submits | **H** (known 0.3 ms from TASK-134); more on a phone where `vkQueueSubmit`/driver time is larger | 2-3 d; medium (one ordered queue, slot/semaphore) | `[hw] profile`/`[core2] gpu_wait`, the `submit` time, wall; the command-buffer ring |
| 7 | **Mobile shader prepare/cache** (3.10): the first-play compile | **H** for the one-time stall (97 variants, 128 ms each on the desktop; mobile Unknown) | 1-2 d to wire on Android; low | `shaders.log`, first-run vs warm-cache frame times |

Not in the list: the **pacer/audio** (cadence, not the bottleneck), the **frame-skip/30 fps** path (owner: occasional
dips only, 30 last resort), the **CoreGS 1.1 `native_vulkan`** waits (not this path), and the **resize/shutdown**
`vkDeviceWaitIdle` (not per-frame). The CPU/GPU cost items from TASK-215b (scissor/XYOFFSET batching, CLUT scope,
effect-target cap, self-copy) are complementary: they shorten the work between waits; this list removes the waits
themselves.

## 5. Mobile / tile-GPU notes (architecture facts, no ARMSX2/GSdx source)

The S23 Ultra class is a **tile-based GPU** with a shared memory hierarchy and a mobile driver. The following are the
architectural reasons the same waits are worse there; they are **Hyp/Unknown for this game** until measured, not claims
from another renderer's source.

1. **Readbacks are resolves, not copies.** A storage image written by draws lives in tile memory until it is flushed; a
   readback (`Reduce`, `CopyImageToBuffer`, `ReadClut`) forces the tile buffer to be resolved/written to memory, then
   copied to a host-visible buffer. The `vkWaitForFences` after the copy waits for all tiles. The flare table (2 KiB) is
   tiny, but the **resolve/flush** of the targets that feed it is the cost. The GPU-side flare (C) avoids the read; a
   one-frame-late read still resolves, just later.
2. **Barriers flush tiles.** `vkCmdPipelineBarrier` full/global barriers (276/VBlank at 3x after IN-464) can force a tile
   flush/resolve; the `core2` path's storage images and `VK_IMAGE_LAYOUT_GENERAL` do not use the tile-local fast path or
   framebuffer compression. The `IN-464` page barriers reduce the desktop cost; on a tile GPU the same barrier may still
   flush. The attachment/ROP path (`IN-481`, `IN-523`, `G5`) was measured worse on the desktop and not built; on a tile
   GPU it may be different, but that is a **design study**, not a TASK-215c item.
3. **Present and queue stalls.** The `acquire_image(UINT64_MAX)` one-queue path waits for the display; a tile GPU's
   display controller/SurfaceFlinger may queue 2-3 buffers; the second queue (`IN-516`) is the desktop answer but the
   code selects it only when the **same family** has two queues. A separate present family or a presentation thread is
   the mobile-safe overlap. `VK_PRESENT_MODE_FIFO` at 120 Hz on the phone vs the game's 60 Hz is a **pacing** question,
   not a renderer one.
4. **Async compute is not free.** A separate compute queue can overlap with graphics only if the tile scheduler can run
   both; mobile GPUs often share the shader cores/tile memory, and a compute pass that reads/writes the same targets
   serializes at the barrier. Use async compute only for independent work (e.g. the next texture decode) with
   semaphores; the `core2` decode/patch/CLUT already form a dependency chain before the draw, so it is not a primary
   lever. The `VK_EXT_fragment_shader_interlock` support on the target is **Unknown**; the no-interlock path (`IN-471`,
   `GT4_HW_INTERLOCK=0`) is the mobile fallback: before IN-471/IN-496 it was 49 ms at 3x against 3.9 ms with the
   interlock on the desktop; IN-471 brought it to 6.85 ms. The interlock itself can be a tile-GPU hazard.
5. **Memory/allocation.** Mobile is UMA with tighter budgets; the 64 MiB image blocks / 192 MiB texture budget / 3x
   pictures can pressure; `vkAllocateMemory`/`vkFreeMemory` can stall the driver; the block/recycle path (IN-424) is the
   right base, the mobile budget must be measured. A new target/texture during play can allocate; the prepare/prewarm
   of the target set and textures is safer than on-demand.
6. **Shader compile.** The mobile driver compiles pipelines on `vkCreateGraphicsPipelines`; 97+ variants at 128 ms each
   (IN-437) is a first-play stall; the `IN-507` prepare and `IN-489` cache must be wired on Android; the `TASK-170` guard
   is for a crash, not for speed.
7. **Thermal/clock.** The waits let the CPU/GPU idle; a phone governor may keep the clock low and then boost/burst; the
   owner's "sometimes 50, then throttles" is consistent with a clock/thermal state, but that is **Unknown** without a
   target log. Removing the waits raises utilization and can raise power; the `IN-425` "gaps, not power" is a desktop
   fact, not a phone fact.
8. **The embedded GSdx/ARMSX2 comparison is black-box.** The owner's observation (GSdx with readbacks off reaches 3x/Full
   HD; CoreGS 2.0 2x is 35-40 fps) is consistent with the per-frame readback/present/slot waits being a larger fraction
   on a tile GPU; no source was read. The audit treats it as a **design target**, not a code reference.

**Safe vs unsafe overlaps on a tile GPU:**

| Overlap | Safe? | Why |
|---|---|---|
| EE/VU1/GS run ahead into rings/slots | yes | producer/consumer buffers; the frames/identity gates unchanged |
| GPU-side flare (no CPU read) | yes | the table stays on the GPU; no resolve; same-frame exact |
| Second-queue present | yes, if the family/queue exists | the display is not in the render queue |
| Presentation thread on one queue | conditional | the same queue still orders; the GS worker must not acquire; the swapchain lifetime |
| Async compute with semaphores | conditional | only independent work; tile GPUs may not overlap; barriers |
| One-frame-late flare readback | **Performance Mode only** | the owner rejected it in normal mode |
| Reading a tile-local target without a resolve | **unsafe** | undefined/stale tile memory |
| More swapchain images / 24 slots | measured worse / no gain on the desktop | do not repeat |

## 6. Do-not-repeat and owner decisions

- **The one-frame-late flare answer in normal mode is rejected** (owner, decision 0073). It may be used only in
  Performance Mode (`GT4_CORE2_PERFORMANCE`) or with the owner's explicit acceptance for a named scene. The same applies
  to `GT4_CORE2_DELAYED_DOWNLOADS`/`GT4_HW_FLARE_DEADLINE_MS` in normal mode.
- **Frame skipping is not the performance strategy**: it is for occasional 59/58 dips only; 30 fps is the last resort.
  Do not propose frame skipping/30 fps as the fix for a 35-40 fps scene.
- **No GPL source, no ARMSX2/GSdx/PCSX2 code**, no GSdx logs; GSdx is a black box. **No Avell**: the Avell measurements
  in the ledger are evidence of the wait classes, not the target; the target tests are on the S23 Ultra class.
- Already measured, do not repeat:
  - IN-448 (MTVU journal slot rings: no measurable gain);
  - IN-514..IN-519 (latest-ready present: no gain; 4 swapchain images worse; 24 slots nothing over 16; the second queue
    and 3 images are the kept default);
  - the TASK-134 submission batch sweep (64 best on the Avell; the **mobile** 8/16/24 and 32/16 measurement is new and
    target-specific, not a repeat);
  - IN-439/IN-441 (the performance-mode late/hidden flare; the owner's decision above);
  - IN-508's fixed before-write case (the remaining before-write/keep is TASK-162, not the old experiment);
  - IN-537/IN-539 (Z16 and whole-block GPU stores: measured off; do not re-enable without the GLM-pages design);
  - IN-477/IN-478 (the HD 620 laptop, no Avell);
  - `vkDeviceWaitIdle`/`vkQueueWaitIdle` at resize/shutdown (not per-frame; do not optimize unless a resize stall is
    measured);
  - the CoreGS 1.1 `native_vulkan.cpp`/`hw_gs_writeback_gpu.cpp` waits (not the CoreGS 2.0 path);
  - the `gs_stream_ipc` external-process waits (not the in-process path);
  - a renderer rewrite/JIT (out of scope, no GPL).
- The 4-swapchain-image and 24-slot experiments were worse/nothing on the desktop; a **3-image/2-present/3-slot** ring is
  the current default (IN-511/IN-516); do not re-run the variants without the mobile question.

## 7. Open questions (measurement or decision, not code reading)

1. **The target's queue families**: how many queues in the graphics family, is there a separate present-capable family,
   and does the current `queueCount >= 2` selection ever choose the second queue on the S23 Ultra? The `[hw] presentation
   queue` line answers it at init.
2. **The target's wait split**: `[core2] gpu_wait ms/vblank by readback reason`, `[hw] transport` (`vsync_wait_ms`,
   `drain_wait_ms`, `read_fifo_ms`, `mtvu_wait_ms`), `[hw] profile` (`present_us`, `submit`), `[mtvu] EE wait`,
   `[core2] exclusive`, on one 60 Hz race scene at 2x. No such log exists in the tree.
3. **The flare wait at 2x on the target**: how much of the 35-40 fps is the 2 KiB read/resolve + the VU1 stall; the
   delayed/hide-flare A/B is the upper bound.
4. **The slot wait at 8/16/24 and `SUBMIT_EVERY` 32/16/8** on the target; the PC hides it; the code comment's "waiting on
   every heavy stretch" is the desktop 8-slot case.
5. **The tone CLUT readback**: how often `SyncClutToCpu`/`ReadClut` runs on the target (the colour filter), and its
   `clut_reads`/`gpu_wait` cost; the per-reason line does not attribute it today.
6. **The before-write/keep waits at 2x** on the showroom/photo/race and how much the GLM-newest-copy design would remove.
7. **The one-shot readbacks at load**: the icons/loading picture/thumbnail/photo at the target; the race-start camera cut
   (42 ms once) and whether the owner sees it on the phone.
8. **The mobile shader prepare/cache**: is the `IN-507` prepare/`IN-489` cache wired on Android; the first-play compile
   time; the `shaders.log`.
9. **The tile-GPU cost of the storage-image/`GENERAL` path, the 276 barriers and the readbacks**: a profile on the target;
   the attachment/ROP path (`IN-523`) is a study, not a TASK-215c item.
10. **Thermal/clock**: the "sometimes 50, then throttles" is a clock/thermal state (Hyp); a target log of GPU/CPU clocks
    and temperature during the scene would separate it from the waits.
11. **The pacer/audio on Android**: the high-resolution timer/spin, the 120 Hz display vs the 60 Hz game, the audio mode;
    the `[pacing]` histogram.
12. **The frame-skip transition** (`pacer_drain`): whether an occasional dip causes a visible hitch when the skip starts/
    ends; the owner says frame skipping is only for dips, not the strategy.
13. **The `vkDeviceWaitIdle` on resize/orientation/app lifecycle**: is there a stall when the phone rotates or resumes?
    Not per-frame, but a user-visible one.
14. **The embedded GSdx/ARMSX2 comparison is black-box**: no source/logs; the only use is the design target (GSdx with
    readbacks off has no per-frame GS->CPU read; the CoreGS 2.0 exact normal mode does).

## 8. Evidence, limits and what was not done

- Read: `docs/tasks/COREGS2-PERF-AUDIT.md` (TASK-215b), `docs/hw-renderer-spec/CORE2-PERF-LEDGER.md`,
  `docs/FRAME-COST-STUDY.md`, `docs/GS-READBACK-STUDY.md`, `docs/hw-renderer-spec/CORE2-CPU-SURVEY.md`,
  `docs/GAME-CODE-OPTIMIZATION-STUDY.md` sections 8-9, `docs/STATUS.md` (current sections), `TASKS.md`
  (TASK-134/162/168/171/215), `docs/hw-renderer-spec/CORE2-GPU-PLAN.md`.
- Read in the code: `native/hw_gs/core2/core2_gpu.cpp`, `core2_renderer.cpp`, `hw_gs_stream_sink.cpp`,
  `hw_gs_backend.cpp`, `hw_flare_table.hpp`, `core2_replace.cpp`, `native/window.cpp`, `native/frame_clock.cpp`,
  `native/pacing.cpp`, `native/host_io.cpp`, `native/mtvu_journal.cpp`, `native/mtvu_runtime.cpp`,
  `native/mtvu_ring.hpp`, `native/native_vulkan.cpp` (CoreGS 1.1, to mark it out of scope),
  `native/hw_gs/writeback/hw_gs_writeback_gpu.cpp` (CoreGS 1.1, out of scope), `native/gs_stream/gs_stream_ipc.cpp`
  (external process, out of scope).
- Not done: no build, benchmark, game run, code/data/pin/test change; no PCSX2/GSdx/fork/ARMSX2 source; no owner data;
  no Telegram/PR/release/tag/draft.
- Limits: every cost is a desktop measurement from the existing ledger/audit unless it says otherwise; **no mobile
  measurement exists**. The S23 Ultra queue families, interlock/async-compute support, the actual wait split, the thermal
  state and the GSdx comparison are **Unknown**. The wait table is a code map, not a profile: the next step for TASK-215
  is one instrumented run on the target with the existing lines, then the async list above.
- This page is TASK-215c, the waits/async half; TASK-215b is the static CPU/GPU audit; TASK-215 owns the runs and the
  implementation. A measurement wins over a hypothesis here.
