# Save state feasibility for long races

Study date: 2026-11-04. Source baseline: `task/134-game-code-optimization-study` at
`232b1437`. This is a documentation-only study, not an implementation or an
owner-approved feature schedule. The branch and its runtime may change.

## Conclusion and scope

**High confidence:** a suspend-to-disk feature is technically plausible, including
in a recompiled game. AOT code does not prevent saving the guest machine's data
and restoring execution at a supported continuation point. The MIT donor already
has a state mechanism. **Unknown:** whether a complete, safe round trip works in
any current shipping configuration; no build, capture, restore or gameplay test
was performed for this study. A proof of concept must establish that first.

A memory card save is game-managed progress. A save state is a snapshot of the
running machine, including an ongoing race. Input recordings, GS screenshots and
memory-card backups are not substitutes. This study targets closing the game and
resuming a long endurance race on the same compatible build; cross-version,
cross-machine, renderer switching and arbitrary old-state support are separate,
unproven goals.

## Evidence and gaps

| Area | Evidence | Implication |
|---|---|---|
| Donor machine | Frozen MIT donor `private/references/ps2-web-recomp/runtime/src/system.cpp`, `machine_state`, `save_state`, `load_state`, `preempt` and `vblank`: serializes CPU, RAM, scratchpad, timing/events, kernel, hardware, DMAC, VIF/VU, IPU, GS, GIF, IOP and SIF; events are restored by ID; VU worker is fenced. | **Confirmed:** a starting mechanism exists. **Unknown:** completeness for the current overlays and generated AOT runtime. Private source path is local evidence, not a shipped artifact. |
| Historical experiment | `docs/reverse-engineering/slice113-native-video.md` limits private donor states to the pinned slice 113 producer. | **Confirmed:** historical experiments are not a product compatibility guarantee. |
| Current renderer selection | `scripts/prepare_external_vulkan.py` injects a throw into `gs::state` when `stream_renderer()` is true; `native/gs_stream/gs_stream_client.cpp` defines that as every renderer except donor. | **Confirmed:** this path rejects both CoreGS 2.0 and external GSdx, despite the error text naming only pcsx2. The donor path still needs testing. |
| CoreGS 2.0 | `native/hw_gs/hw_gs_stream_sink.cpp` queues GS jobs; `drain`, synchronized `read_fifo`, and shutdown `wait_idle` are existing primitives. `native/hw_gs/frontend/hw_front_end.hpp` maintains GS state. | **High confidence:** a coordinated snapshot needs GS register, VRAM and pending-work ownership inventory, GPU completion/readback where needed, and reconstruction of host resources. A drain alone is not a proven whole-machine barrier. |
| External GS | `native/gs_stream/gs_stream_protocol.h` defines image snapshot operations, not a demonstrated full state contract. | **Unknown:** a complete external-renderer export/import protocol and its feasibility require a separate investigation. Respect the process and licensing boundary; no GPL code in the game executable. |
| IOP and card | Donor `runtime/src/iop/io.cpp`, `cdvd.cpp`, `sio2.cpp` and `spu2.cpp` contain state handling; SIO2 captures protocol state, not the backing card file. | **High confidence:** audit open files, disc positions, audio, and card consistency. Restoring an old race must not silently overwrite newer progress or another card. |
| Failure handling | Donor `load_state` invokes `machine_state` on live state and catches exceptions afterwards; `save_state` writes the file directly. | **Confirmed:** that code alone does not establish atomic restore or crash-safe save. A product needs validation, safe failure, and atomic replacement. |
| Host services | `native/host_io.cpp` uses asynchronous host audio; `native/achievements.cpp` configures RetroAchievements hardcore. | **High confidence:** drain/reset host audio separately from guest SPU2 and define achievement policy with the integration's requirements. Recreate handles rather than persisting process pointers. |

## Proposed investigation and implementation gates

1. **Inventory and compatibility.** Trace the exact generated runtime, EE AOT
   continuation/unwind behavior, VU fallback and AOT state, IOP services, DMA,
   timers, asynchronous operations, mods and configuration. Identify every
   mutable state owner and each external side effect. Pin executable, game target
   (v2.00 versus Spec II), disc identity, renderer, format and relevant settings;
   reject mismatches until compatibility is actually proven.
2. **Disposable donor proof of concept.** On a test card and isolated files, test
   save, normal process exit, fresh process load and continued execution in a
   race. Compare against an uninterrupted run at the same guest boundary; include
   EE/IOP/VU state, race clock, lap, position, opponents, audio and frame output.
   Determine whether compiled EE execution can always unwind to a restorable
   continuation. Historical frame captures alone do not pass this gate.
3. **Consistent coordinated boundary.** Request capture at a safe point, stop new
   guest work, fence VU and all GS workers, finish GPU work and resolve relevant
   GPU-owned data. Audit in-flight I/O and card writes. Capture a mutually
   consistent state; restore with a defined ordering, rebuild transient caches,
   reset host timing/audio and resume. Verify the same boundary for load.
4. **Renderer paths.** Implement and test a logical CoreGS 2.0 state independent
   of Vulkan handles, including all authoritative 1x GS memory and registers,
   deferred work and necessary rendering history. Determine whether higher-scale
   presentation data can be recreated without visible or gameplay errors.
   Separately design a versioned request/acknowledgement and renderer state
   transfer for the external GS process, or explicitly restrict the feature to a
   supported renderer until it exists. Fail safely on renderer failure.
5. **Persistence and data safety.** Version and bound the format, validate all
   sections, lengths, integrity and identity before changing live state; save to a
   temporary file and atomically replace after successful flush. Restore in a
   disposable fresh process or other transactionally safe design. Specify card
   snapshot/conflict behavior and recovery before enabling writes; never silently
   roll back or overwrite the player's current card. Avoid embedding game assets.
6. **Product and verification.** Define slots, user controls, progress/errors,
   disk-space limits, backup/recovery and achievement policy. Exercise corrupt and
   truncated files, incompatible builds, process interruption, card divergence,
   audio devices, both game targets and supported renderers. Compare repeated
   save/close/reopen/resume against uninterrupted runs, including pit stops,
   penalties, race end and a representative endurance session; owner gameplay
   approval remains necessary.

## Cost and uncertainty

**Estimate, not a measurement:** this is a large, multi-subsystem feature, not a
small switch. A narrow donor-only feasibility spike could take several engineer
days to a few weeks; a reliable player-facing implementation could take multiple
weeks to months, particularly with CoreGS 2.0, external GS, card safety and long
race validation. The range is deliberately broad: the EE continuation contract,
renderer state coverage and external-process protocol are unproven. The spike may
reveal a blocker or a lower cost. No save size, pause duration or performance
impact has been measured. The work should begin with the donor round trip and
state inventory, then be re-estimated from observed gaps.
