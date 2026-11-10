# Can GT4's GS readbacks become asynchronous *and* exact?

Read-only study for the owner's question of 2026-11-05. Date: 2026-11-05. Branch read: `task/182-barrier-scope`
(worktree `C:\Antigravity\gt4-wt-178-download-wait`, ahead 4 / behind 8 of `release/1.3.0-rc15`, with another agent's
uncommitted edits in five `native/hw_gs/` files — I read the working tree as it was and changed nothing). **No build, no
run, no file written.** Agent status: **not PCSX2-tainted** — no PCSX2 or GSdx source, no `gt4recomp-gs`, no
`private/tooling/pcsx2-*`, no `private/slice189-pcsx2-ref`. The only renderer knowledge used is this repository's own
code and `docs/hw-renderer-spec/`.

Labels: **C** Confirmed (code, data or a named log read), **H** High confidence, **Hyp** Hypothesis, **U** Unknown.
Numbers are per VBlank unless stated.

---

## 1. The answer in one page

1. **The wait the owner sees is not where it looks.** In the normal CoreGS 2.0 path the **EE no longer waits for the
   flare table at all** (IN-522): the 128-qword read is posted and answered later, and the VU1 waits for the words
   through a VIF-packet patch (C: `hw_gs_stream_sink.cpp:995-1000`, `mtvu_runtime.cpp:226-240`, `hw_flare_table.hpp:63-70`).
   What still waits is the **GS worker thread**, inside `Renderer::ReadFifo` → `Readback` → `Gpu::Reduce` →
   `submit(true)` → `wait_all` (C: `core2_renderer.cpp:5525-5570`, `2847-2935`, `core2_gpu.cpp:4774-4805`, `2581-2586`).
   Because that is the thread that feeds the GPU, the GPU goes idle behind it — the "full queue drain that serialises
   CPU and GPU" is real, but it is a *feed-thread* stall, not an EE stall.
2. **Guest time cannot be changed by removing a host wait.** `now()` is
   `g_base + (g_given − g_cpu.budget)` (C: donor `runtime/src/system.cpp:181`): guest time is retired EE cycles. A
   blocking host call inside a guest instruction consumes no budget, so timers, VBlanks, the DMA IRQ and the interrupt
   points are unmoved. "Asynchronous" here means *less host serialisation*, not a different guest timeline. This is the
   strongest exactness argument in the study, and it kills a whole class of worries: there is no race with the guest
   clock to reason about.
3. **Therefore exactness reduces to three orderings**: the destination bytes at each guest read, the GS register state
   at each guest read, and the renderer's per-target bookkeeping in stream order. The GPU *recording* of a readback is
   already at the right stream position (`Reduce` records at the read point); only the CPU **wait and copy** are the
   optional part (C: `core2_gpu.cpp:4789-4802`).
4. **Lazy completion (block at first touch) is exact by construction only if every reader of the destination is
   hooked.** For an EE-RAM destination the readers are the inline `ld*/st*` (donor `runtime/include/ps2/memory.hpp:28-93`),
   the DMA source path (`dmac.cpp:37-40`), VIF/GIF uploads, SIF/SPU2/IPU and the recompiled code's `ld128`. Hooking the
   inline path means rebuilding the **pinned** `ps2game.lib` (C: `docs/BUILD.md:22`, `scripts/ee_producer.py` pins the
   object inventory); a **guard page** catches all of them with no rebuild, at the price of Windows VEH vs Android
   SIGSEGV. **How far the EE can run ahead: zero guest cycles — but in host time, the whole gap between the request and
   the first read, which for GT4's EE-visible downloads is short (the EEG reads them at once); the long gaps are all
   *renderer-internal* readbacks whose consumer is GS memory, not the EE.**
5. **Early completion is already done.** The reduce reads only `drawn ∩ read rect` at the read's stream position
   (C: `core2_renderer.cpp:2859-2871`), one texel per native pixel, no resolve; the rest of the 2 KiB comes from GS
   memory. The remaining lever is *when the CPU waits*, plus the K rule (O1) which was measured exact and halved the
   post-drain GPU idle (C: TASK-178 §9.3). O3 (read blocks only) was measured **inexact** and is dropped (C: §9.8).
6. **Prediction and rollback are impossible in the normal mode.** There is no per-instruction checkpoint, the guest is
   not re-runnable (it reads time, cards and input), the speculative value would already have produced GIF output sent
   to the GPU, and "verify later, roll back" would change the *picture* the owner already rejected once (one-frame-late
   flare, decision 0073). Prediction survives only as Performance mode, which is not exact by design.
7. **The best exact, implementable candidate today is a one-slot deferred flare readback** — the reduce recorded at the
   read's stream position, submitted without waiting, and completed (fence wait + 2 KiB into GS memory + FIFO walk into
   guest RAM + `finish(table)`) at the next stream point that needs the words or touches those GS blocks. It is O4 with
   the smallest possible deferred-write surface (one span, one record), ~4-6 days, and it derisks UMA phase 4b. The K
   rule (already built, default 0) should be enabled with it or right after.
8. **The phone's read wait will not be fixed by asynchrony.** On the S23 the GPU is the slower consumer (11.5 ms GPU
   against 8.7 ms GS CPU, C from the UMA study's readings of the brief), so the backlog at the read is real work, not a
   bubble. Only less GPU work (TASK-182) or not waiting for it shortens that; asynchrony buys the *bubble* and the feed
   thread's CPU gap (I_cpu 0.3-0.75 ms on the desktop, C).

---

## 2. How the wait is built today

Three threads matter (C, code):

| Thread | Owns | Blocks on |
|---|---|---|
| EE (main) | recompiled EE, IOP interpreter, DMA, scheduler (`system.cpp:329-336`) | the MTVU journal drain at `sync("download")` (`mtvu_runtime.cpp:790-835`), ring back-pressure, pacing sleep |
| VU1 worker (MTVU) | VIF1/VU1 micro, emits GIF records into the output ring (`mtvu_runtime.cpp:242-268`) | the flare `TablePatch` (`mtvu_runtime.cpp:237`), input space, its own output ring |
| GS worker (`gs-worker`) | the GS stream: GIF parse, translate, record, **submit**, present (`hw_gs_stream_sink.cpp:744-827`) | `vkWaitForFences` inside `Gpu::submit(true)` (`core2_gpu.cpp:2581-2586`), slot reuse (`begin_recording` → `wait_slot`, `2588-2591`), the VU1 output ring (`execute_mtvu_job`, `833-868`), `present` (backend) |

**The double drain, in order, for one EE-visible download** (C):

1. The guest handler writes DMA ch1 CHCR; the donor's `run_normal(1)` calls `mtvu::sync("download")` then
   `gs::read_image(...)` (`scripts/prepare_perf_runtime.py:267`, donor `dmac.cpp:118-136`).
2. `sync("download")` does `journal->drain(true)`. With direct GS intake the journal holds only EE entries, so this is
   cheap (C: `mtvu_runtime.cpp:812-815`; the study's "the EE does not wait", TASK-178).
3. `read_fifo` (`hw_gs_stream_sink.cpp:984-1028`): 128 qwords → IN-522 post, return (**no wait**, C: `995-1000`);
   anything else → `g_worker->drain()` — **wait until the whole GS ring is empty** — then `read_fifo_now`.
4. `read_fifo_now` (`205-215`) → `backend().read_transfer_fifo` → `Renderer::ReadFifo` → `Readback` →
   `Gpu::Reduce`/`ReduceMany` → `submit(true)` → `wait_all` → **wait for every in-flight slot** (up to 16).
5. Then `ReadLocalToHost` walks the transfer into `destination`, and `reg::kTrxdir = 3` is written (`5549-5569`).

**The GS worker's stall is the expensive part**, because nothing else can record or submit while it is inside step 4,
and the GPU has no queued work after the drained submission until the worker records the next 64 batches
(`submitEvery`, `core2_gpu.cpp:292-295`) or the VBlank flush. That is the 1.36-3.48 ms of GPU idle per drain measured on
the desktop (C: TASK-178 §9.6) and the bubble K was made to fix (C: §9.3).

**The EE's own blocking paths** (C): a non-128-qword download (`drain()`), ring back-pressure (16 MiB ring, 2 VBlanks
of slack), `sync("vblank")` only when the VBlank journal is off (`mtvu_runtime.cpp:902-923`; it is on in core 2), and
`wait_for_jobs` for state/reset/`vu_memory` syncs (`mtvu_runtime.cpp:815-830`). Store-before-write readbacks do **not**
block the EE directly: they run on the GS worker and only reach the EE as back-pressure.

---

## 3. The timing model: why "asynchronous" is a host-side word

`now()` is `g_base + (g_given − g_cpu.budget)` (C: donor `runtime/src/system.cpp:181`). `g_base` only moves in `idle()`
(`144-155`), which the EE reaches when every thread sleeps. So:

- Guest time advances **only** with retired EE cycles. A host `vkWaitForFences` inside a translated instruction retires
  no cycles; the next slice starts at the same `now()`, the same timer deadlines fire, the same VBlank lands at the same
  guest time, and the same interrupt bits are pending. **C (code), and by construction.**
- The DMA completion IRQ is raised synchronously at the guest's CHCR write today (`dmac.cpp:80-86`, `finish` →
  `hw::dmac_raise`), not after a modelled transfer time. A lazy **data** model that still calls `finish(ch)` at the same
  instruction leaves that IRQ exactly where it is (C by construction).
- Two host-time policies exist and must not be consulted on a normal path: the pacer (wall clock → 60 Hz grid) and
  IN-514's `FlareDeadline` (`hw_flare_table.hpp:20-38`), which can answer a **late** table when a window is attached
  and `GT4_HW_FLARE_DEFER=0`. IN-522 takes precedence in normal mode (C: `hw_gs_stream_sink.cpp:995` before `1002`), so
  the normal path is wall-clock-free.

**Consequence.** Every argument about "the EE runs ahead of the GPU" is mis-framed for this runtime. The EE cannot run
ahead in guest time. What changes when a wait is removed is only (a) how much host work overlaps, and (b) the *order* in
which host-side data/state mutations happen. Exactness is entirely (b).

---

## 4. Question 1 — inventory of every guest-visible GS readback

### 4.1 What counts as one

Three families, and only the first is guest-visible:

| Family | Mechanism | Guest-visible? |
|---|---|---|
| **A. LOCAL→HOST download** | GIF `BITBLTBUF`/`TRXPOS`/`TRXREG`/`FINISH`/`TRXDIR=1`, then DMA ch1 reverse reads the FIFO into EE RAM | **Yes** (the EE gets bytes) |
| **B. Renderer-internal store** | the renderer must put drawn pixels into GS memory before something reads/overwrites them (store-before-write, keep, clut, texture-over-target, move) | No (only GS memory / the GPU) |
| **C. Sync-register reads** | EE reads CSR/IMR/BUSDIR/SIGLBLID | Yes, but **never waits** |

**C is not a wait at all.** The donor keeps CSR, IMR, BUSDIR and SIGLBLID as CPU-side shadows
(C: donor `gs.cpp:38`, `755-762`), updated when the worker *parses* SIGNAL/FINISH (`gs.cpp:615-632`,
`gif.cpp:113`); `prepare_external_vulkan.py:34` states it explicitly ("the donor keeps CSR/IMR/SIGLBLID for EE reads").
`Backend::read_privileged` exists but has **no caller** (C: `hw_gs_backend.cpp:1888-1891`, grep). So no CSR/FINISH poll
in GT4 touches the GPU, and none can be made faster or more exact. The halfword `read_image(...,1)` FIFO read in
`prepare_perf_runtime.py:263` also goes through the same `read_fifo` and waits like any other download (C).

### 4.2 Family A, by consumer

All of them go through `func_004A5178(fbp, psm, fbw, x, y, w, h, dst)`; there are **four** static call sites (C:
`docs/GS-READBACK-STUDY.md:56-73`).

| Readback | Trigger / guest path | Data | Frequency | Who reads the bytes | Wait today |
|---|---|---|---|---|---|
| **Flare visibility table** | end of `func_0033EAB8` → `func_003B8848` → `func_00497EF8` → `func_004A5178` | 64×8 CT32, 2 KiB at (0,0) | **1.00/VBlank** in a night race; only when a light is sampled in daylight (C: study §2, `hidden_flare_tables`/`read_fifos`=1.00) | **VU1**, via a VIF REF later in the stream (C, IN-522) | **GS worker** fence, 2.0-5.5 ms (C: TASK-178 9.3) |
| **Car icons** | `CarIconMaker::render` (0x393B70) → `func_001059F8` → `func_004A5178` | 96×64 CT16 + 64×32 CT16 per car (≈16 KiB) | once per race load | EE (later uploaded as a HUD texture) | EE: ring drain + GPU drain |
| **Loading-screen picture** | `func_00104B68` (0x104BEC) | the non-shown display buffer, two halves ≈960 KiB | once per loading screen | EE (shown while loading) | EE: ring drain + GPU drain |
| **Screen thumbnail** | `func_00330AE8`, end of `render_world` | 64×64 CT16, 8 KiB | rare (0 in the measured races) | EE | EE: ring drain + GPU drain |
| **Photo autofocus** | `func_003C7A50` → `func_003C7AE0`, `func_003C5430` → `func_003308E8` | 8×4 Z32, 128 B | photo mode, ≈1/game frame | **EE, immediately** (`z = word & 0xFFFFFF`, then `focus = m[14]/(1−z/2²³+m[10])`) | EE: ring drain + GPU drain |
| **Photo capture tiles** | `func_00333A00`, `func_00333E50` → `func_001059F8` | tiles of the high-res photo, blended back from EE each sub-frame | photo only | EE | EE: ring drain + GPU drain |

All of this is C from `docs/GS-READBACK-STUDY.md` §1.1 (its own labels: C for the call paths and sizes, H for the
photo read being "right after the request"). What matters for this study:

- **Only the flare repeats per frame.** It is also the only one the EE never reads (C).
- The EE-visible ones are load-time or photo-mode: they cost a *hitch*, not a per-frame rate.
- **Photo autofocus is a special case**: GT4 reads the word right after queueing the transfer, so on hardware the value
  is the *previous* transfer's (H, study §1.1). Our synchronous DMA gives it the *current* one. Lazy completion would
  make this case **more** faithful, not less — but it is out of the normal path and must not be changed without the
  owner's word.

### 4.3 Family B, the renderer-internal stores

| Store | When | Per VBlank | Who reads the GS memory next | Wait |
|---|---|---|---|---|
| **store-before-write** (`StoreDrawnBeforeWrite`, `core2_renderer.cpp:3030-3111`) | an EE upload or a local move writes pages a drawn target covers | **0.07-0.12** (HK/Motegi, O3 off), **0.28** in one Motegi window (C: TASK-178 9.5, frame-cost §6) | the GPU, through the GLM/decode of the upload; later the CPU CLUT/move/display | GS worker fence, **2.2-3.7 ms per store on the Avell**, 0.25 ms/VBlank desktop (C) |
| **keep** (`KeepGpuReadContent`, `1938-1955`) | a target read on the GPU is dropped by ageing or a width change | rare (0-6 per event) | GS memory | GS worker fence |
| **clut** (`OnClutLoaded`, `969-991`) | a CLUT load over a drawn target with the GPU CLUT path off | ≈0 with IN-408 on; countable via `clut_readbacks` | CPU CLUT unit | GS worker fence |
| **texture over target** (`fromGlm`, `4251-4280`) | a decode over a target the GLM cannot materialise | rare now | GS memory | GS worker fence |
| **move source** (`LocalToLocalMove`, `3394-3413`) | TRXDIR=2 | U | GS memory | GS worker fence |
| **`ReadClut`** (`core2_gpu.cpp:4678-4693`) | the tone hook needs the GPU CLUT | U (`clut_reads`) | EE / CPU CLUT | full drain + `submit(true)` |
| **`ResetGs` → `ReadbackAll`** (`5496`) | a GS reset | 0 in steady state | — | full drain |

**The counter to watch is already printed**: `[core2] gpu_wait ms/vblank by readback reason (readbacks/vblank): download=…
before write=… keep=… clut=… other=…` plus `stores_skipped`, `clut_readbacks`, `clut_reads`, `keep` traces
(C: `core2_renderer.cpp:5766-5785`, `5326`). The TASK-178 interval line adds `downloads/vblank`, `with_readback`,
`targets/readback_download`, `px/readback_download`, `wait_ms/readback_download`, `drains/vblank`,
`gpu_idle_ms/drain`, `cpu_gap_ms/drain` (`5729-5755`).

### 4.4 Races against menus

- **Race**: 1.00 download per VBlank, 0.63-1.00 of them with a GPU readback, 0.69-1.07 drains per VBlank (C: TASK-178
  9.6). Motegi reduces the table alone in 88% of reads; Hong Kong adds a 128×128 Z24 and often a 640×224 CT24, 99.9%
  of the pixels for 4 pixels of table (C: 9.5).
- **Menu**: no flare (the flare chain is `render_world` only). The waits are store-before-write at transitions and the
  occasional keep/clut. The only hard number I have is historical: "periodic menu VBlanks (every 6 around 950-1045,
  every 2 around 2609-2649) with an IN-411 store before an upload waiting 15-17 ms for the GPU"
  (C for that log, `docs/journal/2026-10-29.md:303-306`, **old build**). Current menu counts per VBlank: **U** — the
  counters above answer it in one run.
- **Phone**: the owner's observation that normal mode halves the frame rate is H (his words, no log in this worktree).

---

## 5. Question 2 — lazy completion (block at first touch)

### 5.1 The mechanism, in this codebase

A download's guest-visible side effects are (C):

1. the destination bytes, written by `ReadLocalToHost` (`core2_renderer.cpp:5559`) into `destination`;
2. the DMA channel registers and the IRQ — already synchronous at the CHCR write (`dmac.cpp:80-86`, `118-136`);
3. `reg::kTrxdir = 3` and `zeroRead_` cleared when the transfer drains (`core2_renderer.cpp:5566-5569`).

Lazy completion would keep (2) exactly where it is, move (1) and (3) to the first observer, and leave everything else
alone.

### 5.2 Destinations and readers — the completeness problem

A `ReadFifo` destination is written through `target(c.madr + i*16)` (donor `dmac.cpp:42-45`), i.e. **EE RAM or the
scratchpad** (`g_ram` / `g_spr`, `memory.hpp:14-15`). Every reader of those bytes that a lazy model must catch:

| Reader | Where | How to hook |
|---|---|---|
| **Every EE load** | inline `ld8/16/32/64/128` → `fast<T>()` → `*p` (donor `memory.hpp:28-93`), used by all 326 recompiled units (`codegen.cpp:508-520`) | a range check in `ld*` **or** a guard page |
| EE stores | same | same |
| DMA source reads (VIF0/VIF1/GIF/SPR/IPU) | `dmac.cpp:37-40` `source()`, `deliver()` `48-72` | range check in `source()` |
| VIF1 → VU1 data memory | `deliver` case 1 → `vif::transfer` | covered by `source()` |
| SIF (IOP ↔ EE) | `sif::pump()` from `sync_iop` (`system.cpp:329-336`) | range check in the SIF copy |
| SPU2 / IPU | through the IOP / `ipu_pull` | range check |
| The renderer's own `memcpy` (IN-522 writes the table into `dest`) | `hw_gs_stream_sink.cpp:805` | unprotect before writing (see 5.5) |
| The GS worker's `memcpy` of the FIFO reply | `hw_gs_stream_sink.cpp:206-207` into the caller's buffer | same |

**Is it exact by construction?** Yes, *iff* the completion happens before the first of these touches the destination,
and before any guest read of TRXDIR. Otherwise it is silently inexact, and there is no gate that would catch a rare
miss (the bytes are plausible). That "iff" is the whole risk.

### 5.3 Two implementation routes

**Route A — checked load path.** Add a one-page/multi-range watch to `ld*/st*`. **Cost:** `ps2game.lib` is the pinned
prebuilt library (`docs/BUILD.md:22`, pin `c837d13d…`), and every unit inlines `ld32`; changing the header changes every
object and the `QUALIFIED_OBJECT_INVENTORY` (`scripts/ee_producer.py:19`). It is a legitimate but heavy producer
re-run, and it charges one extra compare/branch to *every* EE memory access. **Verdict: not for a first step.**

**Route B — guard page.** `g_ram` is a plain 32 MiB global (`memory.cpp:11`), `g_spr` 16 KiB; both are exclusively used
by the guest. Protect the destination's pages; the first touch faults; the handler completes the pending download,
unprotects, and resumes.

- **Windows:** `AddVectoredExceptionHandler`, `EXCEPTION_CONTINUE_EXECUTION` after `VirtualProtect`. A read is restarted
  by the CPU; a write too, which is what a real DMA race would do.
- **Android:** `sigaction(SIGSEGV)` + `sigaltstack`; returning from the handler re-executes the faulting instruction.
  Both are established techniques (Hyp: no prototype here), and the handler must be async-signal-safe: no allocation,
  no locks — preallocate the completion context and use an atomic "pending" flag.
- **Cost:** one fault in practice. Protect *every* page of the destination (otherwise a read that starts at a later page
  misses the completion), but the first fault completes the whole transfer and unprotects the range, so the rest are
  free.
- **No rebuild of the pinned library.** That is the decisive advantage.

**Scratchpad and SPR destinations** work the same way (`g_spr` pages), with the caveat that `g_spr` shares its pages
with neighbouring globals; over-protecting is harmless (an early completion), under-protecting is not.

### 5.4 The cycle model and the DMA IRQ

As §3: no guest-visible change, because `finish(ch)` stays at the CHCR write and guest time is cycles. The one thing
lazy completion *does* change is that the IRQ can now precede the data, which is exactly what hardware does; GT4's
handler only restores the bus (H, study §1), so no reader runs there.

### 5.5 The VU1/JIF and the MTVU journal

- **The VU1 already has its own deferred path** (IN-522): `FlareTableReads::wait_copy` blocks the VU1 worker until
  `done_ >= ticket` (`hw_flare_table.hpp:63-70`), and `apply_table_patches` calls it before the job runs
  (`mtvu_runtime.cpp:226-240`). A lazy EE download for the flare is therefore **already implemented end to end**; the
  only difference from "lazy completion" is that the GS worker, not the EE, does the completion — and it does it
  *synchronously inside `read_fifo_now`*, which is the stall.
- **Ordering rule that must not break**: `wait_copy` copies from the single `table_` buffer, and its correctness relies
  on "a VU1 job that uses a read comes after it in the stream, so a later read cannot finish first" (C:
  `hw_flare_table.hpp:63-66`). Any deferred completion must stay in stream order or become per-ticket.
- **Journal ordering**: `sync("download")` → `journal->drain(true)` must still be able to post the READ_FIFO at its
  stream position. With direct intake this is an EE-only drain (C), so a lazy completion does not change it.

### 5.6 How far the EE could run ahead, in practice

- **Guest cycles: 0.** By §3.
- **Host time, flare**: the EE already runs to the next frame's work while the table is produced (C, IN-522). The gap
  between the request and the VU1's use is I_cpu, **0.3-0.75 ms on the desktop** (C: TASK-178 9.3), likely larger on
  the phone (Hyp).
- **Host time, EE-read ones**: the EE reads them almost immediately (icons are stored and uploaded later; loading is
  passed to the loading object; autofocus is read in the next instructions). A lazy model would gain the *GPU* time only
  where the reader is far away (the icons' upload), which is a load-time hitch, not a per-frame rate. **Small.**

---

## 6. Question 3 — early completion

**What is already early** (C):

- The reduce is recorded at the read's stream position: `flush_draws`, end pass, `full_barrier`, dispatch, `full_barrier`,
  `submit(true)` (`core2_gpu.cpp:4789-4800`). Nothing after the read can change its content.
- Only `drawn ∩ read rect` is reduced, one invocation per native pixel, nearest sample at the native centre, no resolve
  (C: `core2_renderer.cpp:2861-2866`, `core2_reduce.comp`). The rest of the 2 KiB comes from GS memory at post time.
- Only waiting submissions use the `readback` buffer (`core2_gpu.cpp:2617`); `ReduceLater` has its own 64×4 KiB buffer
  (`333-341`).

**What early completion cannot fix**: the table depends on the depth of the whole scene, on one in-order queue
(C: frame-cost §1.6). A fence over "only the read's dependencies" is the same fence.

**Relation to TASK-178**:

- **O1/K** (`GT4_CORE2_SUBMIT_AFTER_WAIT`): after a waited submission, submit once after K batches. Measured exact at
  K = 8/16/32 (C: flare hashes identical; `gates6 wrong=0`), and it halved the GPU idle per drain (C: -0.96/-0.91/-1.75
  ms). It is *early completion of the post-read work*, not of the read.
- **O3** (read blocks only): measured inexact and dropped (C: Motegi reply 6086 differs; +1.0/+1.3 ms wall at 6x/10x).
  The cause is the state change, not the bytes: the old whole write-back clears the target's drawn area, and the
  overlapping 64×192 scratch windows depend on it. **This is the exact hazard any deferred store must respect.**
- **UMA phase 4b** (`STUDY-UNIFIED-MEMORY.md` 5.3): coherent sequence word + VU1 polling + no copy step; its stated
  blocker is exactly the deferred write-back layer. Nothing in this study contradicts it; the phase-1 plan below is its
  smallest instance.

---

## 7. Question 4 — prediction and verification

**Can a speculative result be rolled back? No.** Reasons, in order of hardness:

1. **No checkpoint.** The EE state is `Cpu` + 32 MiB `g_ram` + 16 KiB `g_spr` + kernel threads + IOP + SPU2 + DMA +
   timers + the GS/GPU; the only serializer is the frame-boundary `ps2::state::Stream`, and the GS-stream renderer
   refuses save states outright (`prepare_external_vulkan.py`, the `state` hook). Restoring mid-frame is not supported
   by any code path (C).
2. **The guest is not re-runnable.** Same frame ≠ same result: GT4 reads RTC/card/input and advances its own animation
   state; re-running would diverge in places no gate covers.
3. **The speculative value has already left.** The flare table's consumer is the VU1, whose GIF output is already in
   the stream when verification lands. Rolling back means rolling back the GPU queue and the presented picture.
4. **The owner already rejected the honest version of this**: one-frame-late flares (decision 0073, 2026-11-04). A
   speculative-then-corrected flare is the same class of visual error.

The only exact "no wait, no rollback" form is to compute the table **without the GPU**: a CPU model of the sampling
chain, bit-exact in PS2 float (O6 / GS-READBACK-STUDY R3). Weeks, high risk, and it must reproduce the GS's 1.0-alpha
write, the box-filter halving and the strength multiply exactly. Not a first step.

---

## 8. Question 5 — the other waits

| Wait | Where | Exact to overlap? | Evidence |
|---|---|---|---|
| **VBlank flush** | `sync("vblank")` when the journal is off; with core 2 it is a journal entry after the frame's VU1 output | **Yes**, and already done | C: `mtvu_runtime.cpp:902-923`; IN-524 |
| **Present** | `hw.present()` on the GS worker at the VBlank (`hw_gs_stream_sink.cpp:500-510`); acquire/present can block while holding the queue lock | **Yes** (presentation is not guest state); partly done (second queue, pacer thread, TASK-174 shared-queue path) | C: `hw_gs_backend.cpp:1400-1408`, `230-270`; IN-516/536 |
| **Slot reuse** | `begin_recording` → `wait_slot` (`core2_gpu.cpp:2588-2591`), 16 slots | **Yes** to overlap (more slots / earlier submits), **no** to remove | C: IN-517 |
| **Post-drain GPU idle** | worker records after the drain; GPU has nothing queued | **Yes** — this is the K rule | C: TASK-178 9.3 |
| **MTVU joins** (`state`, `hw_reset`, `gif_reset`, `vu_memory`) | `wait_for_jobs` (`mtvu_runtime.cpp:779-788`) | **No**: the EE is about to read/write VU memory or reset the units; the join *is* the semantics | C |
| **IOP / SIF** | `sync_iop()` runs the IOP on the EE thread up to `now()` (`system.cpp:329-336`) | **Yes in principle, no in practice**: an IOP thread means locking the shared RAM and the kernel; exactness is provable but the change is large and the IOP is 0.4-0.5 ms (C, frame-cost table) | C for the numbers |
| **Audio** | paced mode writes into a ring (`host_io.cpp:536-548`); unpaced `submit()` waits for a device buffer (`473-489`) | **Yes**: the host buffer is not guest state; paced mode already does it | C |
| **Flare deadline (IN-514)** | wall clock, only when IN-522 is off and a window is attached | **Not exact** by design; keep it an A/B only | C: `hw_flare_table.hpp:20-38` |

---

## 9. Question 6 — ranking and the phase-1 plan

### 9.1 Ranking

Gains are per VBlank unless noted; "desktop" = RX 9060 XT, "S23" = Adreno 740. Every gain not marked C is a hypothesis.

| # | Candidate | Desktop | S23 | Exactness | Risk | Effort |
|---|---|---|---|---|---|---|
| **1** | **O4-mini: deferred flare readback (one pending span)** — §9.2 | -I_cpu 0.3-0.75 (C component) and part of the 1.36-3.48 GPU idle per drain (Hyp sum) | I_cpu likely larger (Hyp); removes the feed-thread stall | **Exact** if the completion rule below holds; gated by the reply hash | Medium | 4-6 d |
| 2 | **K rule on by default for tilers** (`GT4_CORE2_SUBMIT_AFTER_WAIT`) | -0.9…-2.4 ms wall medians, -0.9…-1.8 idle/drain (C) | Hyp; `submitEvery` is 32 there, try K=8 | **Exact** (C: hashes at 8/16/32, `wrong=0`) | Low | 1 d (A/B only) |
| 3 | **Pre-read cadence** (submit every 4-8 batches for the last ~64 batches before the frame's read) | -0.2…-1 ms of read wait (Hyp, UMA 5.2) | Hyp; GPU-bound, so little | **Exact** (submission boundaries) | Low-medium | 2-3 d |
| 4 | **Store-before-write on the GPU for depth-32 / IN-539 for depth targets** | 0.25 ms/VBlank of store wait (C) | **Largest measured per-store cost on a slow machine**: 2.2-3.7 ms/store on the Avell (C) | **Exact** (a block merge; IN-512/538 already proven) | Medium (Z32 block order) | 3-5 d |
| 5 | **O2: newest-fence wait + bounded spin** | -0.05…-0.3 per drain (Hyp) | Hyp; power cost | **Exact** | Low | hours |
| 6 | **Lazy EE download completion (guard page)** | 0 per VBlank; removes load-time hitches | same | **Exact** iff every reader is covered (§5.2) | Medium-high (fault handler) | 1-2 wk |
| 7 | **Present fully off the GS thread on one-queue GPUs** | 0 (two queues here) | Hyp: the acquire/present stall | **Exact** (presentation only) | Medium | 3-5 d (TASK-174 continues) |
| 8 | **Bit-exact CPU flare model (O6)** | the whole read: 1.3-2.4 ms George V, ~4 HK (C) | same mechanism | **Exact only if bit-exact** | Very high | weeks |
| — | **Prediction + rollback** | — | — | **Impossible** (§7) | — | — |
| — | **O3 (read blocks only)** | no consistent gain (C) | — | **Inexact** (C) | — | dropped |

Order of attack: **1 with 2** (they compose: 2 shortens the next read's backlog, 1 removes the current stall), then 4
(the phone's per-store cost), then 3, then 5. 6 only if a load-time hitch is reported.

### 9.2 Phase 1 — "O4-mini": the flare download's fence wait leaves the GS worker

**Behaviour.** With `GT4_CORE2_DEFER_FLARE_READ=1` (default **0**, A/B): when the GS worker reaches a flare download
whose readback is a single same-view target with no pending dirty rectangles and at most 1024 words, it records the
reduction with `Gpu::ReduceLater` (which already does `flush_draws` → end pass → `full_barrier` → dispatch →
`full_barrier` at the read's stream position), calls `Gpu::Submit(false)` so the GPU starts it immediately, remembers
one pending record, and **returns without waiting**. The pending record is completed — fence wait, 2 KiB into GS memory,
`MarkWritten`/page version/`drawn` clearing, the FIFO walk into the guest destination, `reg::kTrxdir = 3`, then
`FlareTableReads::finish(table)` — at the **first** of:

1. the next `SinkRecord::MtvuJob` for a job carrying that read's `TablePatch` (before `execute_mtvu_job`);
2. the next `SinkRecord::FlareRead`, `SkippedRead`, `Vsync`, `GsReset` or `GifReset` (stream/FIFO order);
3. any renderer operation whose GS-memory span overlaps the pending span: `CommitUpload`/`OnMemoryWritten`,
   `StoreDrawnBeforeWrite`, `LocalToLocalMove`, `MaterializeTargets`, `OnClutLoaded`, `KeepGpuReadContent`,
   `TextureFromGlm`, `ReadFifo`/`ReadbackAll`, or the display-from-memory path;
4. the VBlank (a backstop, so nothing is ever left open across frames).

Falls back to today's synchronous path when: more than one target, a dirty rectangle, a different view, >1024 words
(the 640×224 other-view targets), `GT4_CORE2_PERFORMANCE` (IN-441/477 own that case), or the switch is 0.

**Exactness argument (write it in the code comment).**

- The reduce is recorded at the read's stream position, so its content and its ordering against earlier draws are
  unchanged (C: `ReduceLater` records exactly what `Reduce` records).
- The GS-memory write and the `drawn` clearing move later, but any operation that could observe them **on those blocks**
  completes the pending record first (rule 3), so the order of mutations on those blocks is preserved. Operations on
  other blocks are independent (the region-hazard argument of TASK-182 R2, in its simplest form).
- The guest bytes are written before the VU1's `TablePatch` can read them, because `finish(table)` — which advances
  `done_` — happens only after the FIFO walk (rules 1 and 2), and `wait_copy` gates on `done_`.
- The DMA registers and IRQ are untouched (they happened at the CHCR write).
- Guest time is cycles (§3), so no timer, VBlank or interrupt point moves.

**Files and functions.**

| File | Change |
|---|---|
| `native/hw_gs/core2/core2_gpu.hpp/.cpp` | `bool WaitReduced(std::uint32_t ticket, std::vector<std::uint32_t>&, std::string&)`: like `TakeReduced` but blocks on the carrying slot with `wait_slot`. Reuse `ReduceLater` (`4865-4905`) and `Submit(false, ...)` (`5312`). |
| `native/hw_gs/core2/core2_renderer.hpp/.cpp` | A `PendingDownload` member (ticket, count, `BufferView`, area, drawn, `BlockSpan`, `std::function<void()> onDone`); `bool BeginDeferredDownload(view, rect)`; `bool FinishDeferredDownload(std::uint8_t* dest, size_t bytes)`; `void FlushPendingOverlapping(const BlockSpan&)` called from the sites in rule 3; `bool HasPendingDownload() const`; counters `deferred_reads`, `deferred_open_ms`, `deferred_by` (mtvu / fifo / overlap / vblank). |
| `native/hw_gs/core2/core2_renderer.cpp` | `ReadFifo` (`5525`) gains the deferred branch and finishes any pending read first (FIFO order); `Readback` (`2847`) is split into record/finish internally so both paths share the state writes (`2924-2931`). |
| `native/hw_gs/hw_gs_backend.hpp/.cpp` | `bool read_transfer_fifo_deferred(void* dest, size_t bytes)` and `bool finish_transfer_fifo()`, forwarding to the renderer; `read_transfer_fifo` unchanged. |
| `native/hw_gs/hw_gs_stream_sink.cpp` | `SinkRecord::FlareRead` (`802-808`) takes the deferred path when the switch is on; the finish points of rules 1-2; `flare_gap_read/finish` unchanged. |
| `native/hw_gs/hw_flare_table.hpp` | Comment only: `finish` may be called later, in stream order. |
| `native/hw_gs/core2/core2_renderer.cpp` (stats) | The new counters in the `[core2]` interval line next to `downloads/vblank` (`5729`). |

**A/B switch.** `GT4_CORE2_DEFER_FLARE_READ` — `0` off (today), `1` on. One variable. Log the setting in the `[core2]`
init line next to `submissions:` (`core2_gpu.cpp:3434`).

**Gates (all must pass with the switch on *and* off, and the on/off values must be equal).**

1. **Flare reply hashes** (`[hw] flare replies vsync=… replies=… hash=…`, `hw_gs_stream_sink.cpp:256-278`), Spec II
   press scenes: Hong Kong 2x `2f4c1cc9f174d00e` (6,835), Motegi 2x `7bf932fbb8799965` (6,588), Hong Kong 10x
   `61109e8f3143264a`, Motegi 10x `9ab7085aa7c6d425`, George V 1x `1e108f7eb7013b1c` (5,746). The 16 per-run lines must
   match, not only the final hash.
2. **`gates6.sh wrong=0`** (`private/task178-tools/gates6.sh`): Spec II George V/El Capitan/Tokyo R246 at 1x (race clock,
   `state.bin`, frames every 250 VBlanks), v2.00 night 3x (state + 10 frames), Arcade 3x (14 frames), George V 6x
   (12 frames), plus `GT4_VU1_VERIFY`/`GT4_VU0_VERIFY` 0 mismatches and `GT4_MTVU_VERIFY` 0 mismatches.
3. **Frame identity**: `frame_diff.py` on the same dumps, 0 differing pixels (the gate above already runs it).
4. **CTest**: full suite (320 at this base, 1 skip `hw_replace_q07_pcsx2_dumps`), plus a new
   `hw_core2_deferred_read`:
   - a stream with two overlapping 64×192 scratch windows (the O3 trap) and an upload over the pending blocks;
   - the pending read is forced by (a) an overlapping upload, (b) the next download, (c) the VBlank;
   - images and GS memory bit-identical with the switch off and on; the counters show the path taken (deferred 0 → 3,
     completed by overlap 1 / fifo 1 / vblank 1).
5. **Python suite** against the build under test (`GT4_NATIVE_BUILD=…`).
6. **Pins**: no new manifest (no pinned input changes); the executable pin is regenerated by the usual flow.

**Measurement.** Avell (and the S23 if the owner runs it): Hong Kong, Motegi and the George V lap, 6x, headless, order
A B B A A B, the switch checked in every log. Report wall, GS worker, `wait_ms/readback_download`, `gpu_idle_ms/drain`,
`cpu_gap_ms/drain`, I_cpu and the new `deferred_open_ms`. **Keep it only if the wall falls ≥0.3 ms/VBlank on two
scenes with no loss on the third** (the TASK-178 rule). If `deferred_open_ms` is ≈0 (the completion is forced
immediately), the change is worthless and must be reported as such, not tuned around.

**Then** (same task, no new code): run the K sweep (`GT4_CORE2_SUBMIT_AFTER_WAIT=8/16/32/0`) on the Avell and the S23
with the same scenes, since K and O4-mini attack the same bubble from both sides.

---

## 10. Question 7 — unknowns, and what to measure first

| Unknown | Why it matters | How to resolve it |
|---|---|---|
| **I_cpu on the S23** | O4-mini's whole gain is bounded by it | `[hw] flare replies … i_cpu_ms/read=` on the phone, normal mode, Nordschleife |
| **How much of the S23's normal-mode loss is the read wait vs the post-drain bubble vs store-before-write** | decides whether 1 or 4 is first | `[core2] gpu_wait … by readback reason`, `drains/vblank`, `gpu_idle_ms/drain`, `stores_skipped` on the phone |
| **Does GT4 read TRXDIR or any GS register between a download and its data?** | a lazy model must keep TRXDIR at the same guest point | scan the translated units for reads of `0x12001040`/`0x12001000` (grep of the private units; **not done here**) |
| **Which readbacks run in menus, and how many** | the menu hitch is unexplained on the phone | one 600-VBlank run with the counters above |
| **The exact span of the 640×224 other-view reductions in Hong Kong** | they are 99.9% of the download pixels and cannot be deferred with a 1024-word slot | `GT4_CORE2_READBACK_TRACE` (already exists) |
| **Whether a guard-page completion is async-signal-safe on Android with the engine's threads** | route B's viability | a prototype fixture: protect a page, read it from the EE thread, complete a fake download, resume |
| **The cost of one `vkWaitForFences` wake-up vs a spin on the phone** | O2 | `GT4_HW_READBACK_SPIN` A/B (already parsed) |
| **The phone's `submitEvery`/slots** | K and the pre-read cadence are tuned by them | `[core2] submissions:` line on the device |
| **Whether the cost of `ReduceLater`'s extra submission outweighs the wait saved** | the one submission per deferred read | the A/B itself: `submissions/vblank` and wall |

**Measure first, in this order (1 day, no code):** the S23 in normal mode, one 600-VBlank run of the Nordschleife race
at 2x, with `GT4_HW_PROFILE_EVERY=600`, and read `downloads/vblank`, `with_readback`, `wait_ms/readback_download`,
`drains/vblank`, `gpu_idle_ms/drain`, `cpu_gap_ms/drain`, the `by readback reason` line, `stores_skipped`, and
`i_cpu_ms/read`. That single log decides whether phase 1 is O4-mini (the stall dominates) or the store-before-write merge
(the per-store cost dominates).

---

## Appendix A — claims and labels

| # | Claim | Label | Evidence |
|---|---|---|---|
| 1 | Guest time is `g_base + (g_given − g_cpu.budget)`, i.e. retired EE cycles | **C** | donor `runtime/src/system.cpp:181` |
| 2 | A host block inside a guest instruction retires no cycles | **C** | same, and `refill`/`set_budget` (`78-83`) |
| 3 | The EE does not wait for the 128-qword flare read in normal mode | **C** | `hw_gs_stream_sink.cpp:995-1000`; `mtvu_bridge.cpp:84-89` |
| 4 | The GS worker waits for the whole queue at the flare read | **C** | `hw_gs_stream_sink.cpp:802-808` → `core2_renderer.cpp:5546` → `core2_gpu.cpp:4800` → `2581-2586` |
| 5 | Every other EE FIFO read drains the GS ring and then the GPU | **C** | `hw_gs_stream_sink.cpp:1021-1025` |
| 6 | DMA ch1 runs synchronously at the guest CHCR write and raises the IRQ there | **C** | donor `dmac.cpp:118-136`, `80-86` |
| 7 | CSR/IMR/BUSDIR/SIGLBLID are CPU shadows; no GPU query | **C** | donor `gs.cpp:755-762`; `prepare_external_vulkan.py:34`; `Backend::read_privileged` has no caller |
| 8 | The reduce is recorded at the read's stream position and reads only `drawn ∩ rect` | **C** | `core2_gpu.cpp:4789-4800`; `core2_renderer.cpp:2859-2871` |
| 9 | K is exact and halves the post-drain GPU idle | **C** | TASK-178 §9.2-9.3 |
| 10 | O3 is inexact and has no consistent gain | **C** | TASK-178 §9.8 |
| 11 | Every EE load/store is inline `ld*`/`st*` over a raw `g_ram`/`g_spr` pointer | **C** | donor `memory.hpp:28-93`; `codegen.cpp:508-520` |
| 12 | Rebuilding `ps2game.lib` is a pin-affecting producer run | **C** | `docs/BUILD.md:22`; `scripts/ee_producer.py:19` |
| 13 | A guard page catches every reader without a rebuild | **C** for Windows VEH; **Hyp** for Android SIGSEGV here | platform APIs |
| 14 | The flare reply hash is computed over the reply bytes at the FIFO read | **C** | `hw_gs_stream_sink.cpp:205-215` |
| 15 | `wait_copy` relies on stream order of reads | **C** | `hw_flare_table.hpp:63-66` |
| 16 | The phone's normal mode halves the frame rate on readbacks | **H** | the owner's words; no log in this worktree |
| 17 | I_cpu on the phone is larger than 0.3-0.75 ms | **Hyp** | extrapolation from the desktop |
| 18 | Pre-read cadence helps a GPU-bound phone | **Hyp / probably little** | UMA 5.2; the 2026-10-29 measurement (0.6 → 0.08 ms) was on an unsatiated GPU |
| 19 | Photo autofocus reads the previous transfer's value on hardware | **H** | `GS-READBACK-STUDY.md:127-129` |
| 20 | Rollback of EE state is impossible | **C** | no mid-frame state path; `ps2::state` is frame-boundary; the GS-stream renderer refuses states |

## Appendix B — switches and counters used

- `GT4_CORE2_DEFER_FLARE_READ` (new), `GT4_CORE2_SUBMIT_AFTER_WAIT` (K), `GT4_CORE2_SUBMIT_EVERY`,
  `GT4_CORE2_BATCH_READBACK`, `GT4_CORE2_DEFER_MODE`, `GT4_HW_FLARE_DEFER`, `GT4_GS_HIDE_FLARES`,
  `GT4_CORE2_PERFORMANCE`, `GT4_CORE2_STORE_BEFORE_WRITE`, `GT4_CORE2_GPU_CLUT`.
- `GT4_CORE2_READBACK_TRACE`, `GT4_CORE2_STORE_TRACE`, `GT4_CORE2_BARRIER_TRACE`, `GT4_CORE2_GPU_CATEGORIES`,
  `GT4_CORE2_GPU_PROFILE_AT`, `GT4_HW_PROFILE_EVERY`, `GT4_HW_FLARE_DEADLINE_MS`.
- Log lines: `[core2] downloads/vblank=… with_readback=… wait_ms/readback_download=… | drains/vblank=…
  gpu_idle_ms/drain=… cpu_gap_ms/drain=…`; `[core2] gpu_wait ms/vblank by readback reason: …`; `[hw] flare replies
  vsync=… replies=… hash=… | i_cpu_ms/read=…`; `[hw] transport …`; `[core2] submissions: …`; `[hw] flare table: …`.

---

### Bottom line

**Yes for the flare path, no for the EE-read one-shots, and the phone's biggest wait is not the readback but the GPU
backlog behind it.** The exact, general form of "asynchronous readback" in this runtime is: *record the GPU work at the
read's stream position (already done), defer only the wait and the memory write, and materialise them before the first
observer of those blocks.* The flare is the one place where the observer is the VU1 and the mechanism already exists
(IN-522); the missing piece is that the GS worker still pays the fence itself. Making that one record deferred is a
contained 4-6 day change with a proven gate (the reply hash) and it is the smallest honest instance of UMA phase 4b.
Everything larger — a general deferred write-back layer in front of GS memory — is real but is a 1-2 week,
high-risk change that should follow, not lead.
