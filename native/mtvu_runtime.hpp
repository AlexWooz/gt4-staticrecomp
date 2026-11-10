// TASK-011 step 2: VU1 worker thread (MTVU) runtime, used by the slice170 capsule (docs/MTVU-PLAN.md section 11).
//
// Threads: the EE thread (emulation, shadow VIF1, GS front end, journal) and one VU1 worker. The EE posts each
// VIF1 packet, VIF1 register write and VU1 reset as a job with a journal placeholder; the worker runs the donor
// VIF1 parser and the VU1 programs on its own state and emits PATH1/PATH2 GIF bytes in order; the EE drains the
// journal and executes every GS operation (rendering) in serial order.
//
// Only the EE thread calls the EE entry points; the worker calls worker_gif() and the Donor worker callbacks.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace gt4::mtvu {

class SpscByteRing;
class Doorbell;

struct Settings {
  bool enabled = true;  // GT4_MTVU: worker thread and shadow VIF1 (default on; 0 restores the single-thread path)
  bool verify = false;  // GT4_MTVU_VERIFY: per-frame GS stream hash, shadow vs worker VIF1 state at each VBlank
  bool strict = false;  // GT4_MTVU_STRICT: abort on a violated invariant (PATH1 signal, verify mismatch)
  // GT4_MTVU_REPORT_EVERY (slice171): print the F8-equivalent probe report every N VBlanks (headless; 0 = off).
  std::uint64_t report_every = 0;
  // GT4_MTVU_PRIV_ASYNC (TASK-041, slice198): privileged GS writes do not wait for the worker when an ordered
  // consumer is registered (the GS stream in pcsx2 mode); they are queued in the journal behind the outstanding
  // worker output instead. No effect in donor mode (no consumer registered).
  bool privileged_async = kPrivilegedAsyncDefault;
  // GT4_MTVU_CHAIN (TASK-084 step 1, slice261): coalesce the VIF1 packets of one DMA chain into one worker job
  // (posted at the chain end, at a stall, or before any other journal or sync operation). 1 = on (default), 0 = one
  // job per packet, the pre-TASK-084 behaviour, for A/B measurement.
  bool chain_jobs = true;
  // GT4_GIF_FASTPATH (TASK-084 step 2, slice262): in pcsx2 mode with MTVU on, PATH2/PATH3 signals are applied when the
  // packet is accepted, so the GS stream no longer re-walks every qword to find them again. 1 = on (default), 0 = the
  // pre-TASK-084 walk, for A/B measurement. PATH1 always walks (its signals are never pre-applied).
  bool gif_fast_path = true;
  // GT4_MTVU_VBLANK_JOURNAL (TASK-084 step 3, slice263): in pcsx2 mode the VSYNC becomes a journal entry after the
  // frame's VU1 output, so the EE no longer drains at VBlank. 1 = on (default), 0 = the blocking drain. Donor mode
  // always drains (the CoreGS 1.1 present needs the output).
  bool vblank_journal = true;

  static constexpr bool kPrivilegedAsyncDefault = true;
};

// TASK-092: the one condition for the VBlank journal (the VSYNC is a journal entry, and the GS client holds the field
// until the entry runs). It needs the worker (enabled), the switch on, and no verify (verify keeps the blocking drain
// so both sides are idle). The GS client deferral (mtvu_bridge.cpp) and vblank_sync() both use this function, so the
// two sites cannot drift apart.
inline bool vblank_journal_active(const Settings &settings) {
  return settings.vblank_journal && settings.enabled && !settings.verify;
}

// Each switch accepts unset, "0" or "1"; GT4_MTVU_REPORT_EVERY accepts unset or a VBlank count. Any other value
// throws std::invalid_argument naming the variable.
Settings parse_settings(const char *mtvu, const char *verify, const char *strict, const char *report_every,
                        const char *privileged_async = nullptr, const char *chain_jobs = nullptr,
                        const char *gif_fast_path = nullptr, const char *vblank_journal = nullptr);
Settings settings_from_environment();

// TASK-041: ordered privileged GS writes. The GS stream (GT4_RENDERER=pcsx2) needs privileged writes only in stream
// order with the GIF output; the EE keeps its own CSR/IMR/SIGLBLID copy at the program point. The consumer registers
// the function that posts a write; with GT4_MTVU_PRIV_ASYNC=1, sync("gs_privileged") then returns without waiting and
// the consumer calls queue_privileged for each write.
void set_ordered_privileged_consumer(void (*post)(std::uint32_t address, std::uint64_t value));
// True when GT4_MTVU_PRIV_ASYNC is on, the worker runs and a consumer is registered.
bool privileged_async();
// EE thread: true when the write was queued behind journal entries (the drain posts it in order through the
// consumer); false when the journal is empty or the mode is off, and the caller posts it now.
bool queue_privileged(std::uint32_t address, std::uint64_t value);
// EE wait of the last blocking gs_privileged sync in microseconds, then 0 (per-register accounting).
std::uint64_t take_privileged_wait_us();

// TASK-116 step 1 (rc6): the download sync without the VU1 drain. In pcsx2 mode with Performance mode on, the per-frame
// flare readback is answered locally, so sync("download") only closes the VIF1 batch and returns; the READ_FIFO that
// follows is queued in the journal (queue_readback) and posted at its stream position without waiting, and the blocking
// drain runs only for the readbacks that still need accurate data (drain_download) or at the VBlank. Every pointer
// starts null: with nothing registered sync("download") drains exactly as before (donor mode, CoreGS 1.1).
struct DownloadOrdering {
  // EE thread: true when the readback that follows is answered locally by the GS stream.
  bool (*defer)() = nullptr;
  // EE thread: posts the queued READ_FIFO at its journal position (no wait for the reply).
  void (*post_readback)(std::uint32_t qwords) = nullptr;
};
void set_download_ordering(const DownloadOrdering &ordering);
// EE thread: queues a READ_FIFO behind the journal entries already queued. False when the worker does not run or the
// consumer is not registered: the caller then takes the accurate path.
bool queue_readback(std::uint32_t qwords);
// EE thread: the blocking drain a readback that needs accurate data performs before its READ_FIFO. No-op when the
// journal is empty (nothing deferred); counted as "download" for the F8 report.
void drain_download();

// Donor operations. The worker callbacks run on the worker thread; render_gs runs on the EE thread.
// Every callback starts null (1.2.2 bug, TASK-092): donor mode never sets post_vsync, and an uninitialised pointer
// sent vblank_sync into the TASK-084 deferred path and a call through stack garbage at the first VBlank.
struct Donor {
  void (*worker_vif_packet)(const std::uint8_t *data, std::uint32_t qwc) = nullptr;
  void (*worker_vif_write)(std::uint32_t reg, std::uint32_t value) = nullptr;
  void (*worker_vu_reset)() = nullptr;
  void (*render_gs)(int path, const std::uint8_t *data, std::uint32_t qwc) = nullptr;
  // Hash of the VIF1 state: the worker side (true) or the EE shadow (false). Called on the EE after a sync.
  std::uint64_t (*vif1_state_hash)(bool worker_side) = nullptr;
  // gs::signal (SIGNAL/FINISH/LABEL), applied on the EE when a PATH2/PATH3 packet is accepted (docs section 12).
  void (*apply_signal)(std::uint32_t reg, std::uint64_t value) = nullptr;
  // TASK-084 step 3 (pcsx2 mode only; null in donor mode): posts the VSYNC the client held at the guest VBlank.
  // Returns false when no VSYNC was held. Called from the journal drain, so the record lands after the frame's GIF
  // output in stream order.
  bool (*post_vsync)() = nullptr;
  // IN-461 (CPU survey T3, direct GS intake; hw renderer only): true when a GS consumer in this process can read the
  // worker's output ring itself. Asked once, at start.
  bool (*direct_gs_available)() = nullptr;
  // IN-461: puts job `job` at this point of the GS stream; the consumer then reads that job's records from `output`
  // (waiting on `bell`) up to its JobDone. EE thread, at the job's program position.
  void (*post_gs_job)(std::uint64_t job, SpscByteRing &output, Doorbell &bell) = nullptr;
};

// Starts the worker when settings.enabled (EE thread, before emulation). Verify hashing works either way.
void start(const Settings &settings, const Donor &donor);

// Starts the worker with the donor callbacks of mtvu_bridge.cpp (slice170 capsule builds only).
void start_with_donor(const Settings &settings);

// True while the worker runs and GT4_MTVU is on: VIF1 is then shadowed on the EE thread.
bool enabled();
// True on the VU1 worker thread.
bool on_worker();
// TASK-062: cumulative busy time of the VU1 worker (VU1 programs plus its PATH1/PATH2 GIF output) in microseconds, as
// published after each job; 0 when the worker never ran or the performance accounting is off. Any thread.
std::uint64_t worker_busy_us();

// EE entry points (no-ops when not enabled, except gif_transfer which renders directly).
void post_vif_packet(const std::uint8_t *data, std::uint32_t qwc);
// IN-522: the guest memory of a table the GS worker will write (`ticket` in the GS stream). A VIF packet copied from it gets the
// fresh words on the VU1 worker, before it runs. `wait_copy` waits for that read and copies part of its table.
void note_deferred_table(const std::uint8_t *dest, std::size_t bytes, std::uint64_t ticket);
using DeferredTableCopy = void (*)(std::uint64_t ticket, std::size_t offset, std::size_t bytes, std::uint8_t *out);
void set_deferred_table_copy(DeferredTableCopy copy);
std::uint64_t deferred_table_patches();  // VIF packets given fresh words so far
// TASK-178 diagnostics (any thread): the first job given words of read `ticket`, 0 while none is known (the last 16 reads
// with patches are kept).
std::uint64_t deferred_table_first_job(std::uint64_t ticket);
bool direct_gs_intake();  // VU1 jobs run on the worker and the GS worker reads their output itself
// TASK-084 step 1: closes the pending VIF1 batch and posts it as one job. Called by the DMA chain end, a VIF1 stall,
// and before any other journal or sync operation; a no-op when nothing is batched or GT4_MTVU_CHAIN=0.
void flush_vif_chain();
// Counts one VIF1 DMA chain start (the denominator of "jobs per DMA chain").
void note_vif_chain();
void post_vif_write(std::uint32_t reg, std::uint32_t value);
// EE GIF transfer (PATH3 from DMA, or any path on the EE). Scans signals; a signal forces a synchronous drain.
void gif_transfer(int path, const std::uint8_t *data, std::uint32_t qwc);
// Shadow VIF1 DIRECT/DIRECTHL data (the worker emits it): scans signals; a signal forces a synchronous drain.
void shadow_direct(int path, const std::uint8_t *data, std::uint32_t qwc);
// True when a PATH2/PATH3 packet's SIGNAL/FINISH/LABEL writes were applied when the packet was accepted, so the GS
// stream must not walk the packet again to find them (TASK-084 step 2). False for PATH1 and with MTVU off.
bool stream_signals_pre_applied(int path);
// GIF reset: synchronous drain, then the EE scanners restart.
void gif_reset();
// True when a PATH2/PATH3 PACKED A+D write to `address` (0x60-0x62) was already applied on the EE by gif_transfer or
// shadow_direct, so the drained render must skip it. PATH1 is never pre-applied (its signals stay at render time).
bool signal_pre_applied(int path, std::uint32_t address);
// Every GS render (EE thread, journal or direct) passes here for the verify stream hash.
void note_render(int path, const std::uint8_t *data, std::uint32_t qwc);

// Blocking drain: the EE waits until the worker is idle and every journal entry is executed. No-op when disabled
// or on the worker. `reason` is counted for the summary.
void sync(const char *reason);
// Non-blocking drain: executes the journal entries that are complete. Cheap when nothing is queued.
void pump();
// Guest VBlank, before the host presents: synchronous drain, then the verify report.
void vblank_sync();

// Worker side, called by the donor GIF transfer when the worker produces GIF bytes (PATH1 XGKICK, PATH2 DIRECT).
void worker_gif(int path, const std::uint8_t *data, std::uint32_t qwc);

// Joins the worker after a final drain. Safe to call when not started.
void stop();
// Prints the counters (one "[mtvu]" line each) to `output`.
void report(std::FILE *output);

}  // namespace gt4::mtvu
