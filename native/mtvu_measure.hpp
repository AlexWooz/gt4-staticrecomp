#pragma once

// TASK-011 step 0 (docs/MTVU-PLAN.md): measurement only, no behavior change.
// Counts the points where the EE observes VU1 work or depends on its order
// (GIF packets per path and the GS SIGNAL/FINISH/LABEL/TRXDIR writes they
// carry, VIF1 register access, EE access to VU memory), times VU1 calls and
// the PATH1 GS front-end work inside them, and replays a model of a VU1
// worker thread to estimate how much VU1 time a worker could overlap before
// the VBlank sync. Everything here runs on the EE thread.

#include <cstdint>
#include <cstdio>
#include <vector>

namespace gt4::mtvu {

// GS register addresses written through GIF A+D (PACKED descriptor 0xE).
constexpr std::uint8_t kGsTrxDir = 0x53;
constexpr std::uint8_t kGsSignal = 0x60;
constexpr std::uint8_t kGsFinish = 0x61;
constexpr std::uint8_t kGsLabel = 0x62;

// True when a PATH2/PATH3 SIGNAL/FINISH/LABEL write (address 0x60-0x62) is applied on the EE at packet acceptance
// (section 12). PATH1 is never pre-applied: its signals stay at render time, under the PATH1 invariant.
inline bool pre_applies_signal(int path, std::uint32_t address) {
  return (path == 2 || path == 3) && address >= 0x60u && address <= 0x62u;
}

// One signal write as the donor GS applies it: gs::signal(reg, value), reg = the A+D address byte.
struct SignalWrite {
  std::uint32_t reg;
  std::uint64_t value;
};

struct PathCounts {
  std::uint64_t transfers = 0;  // gif::transfer calls
  std::uint64_t qwords = 0;
  std::uint64_t tags = 0;       // GIF tags with NLOOP != 0
  std::uint64_t signal = 0, finish = 0, label = 0;
  // TRXDIR writes by direction: 0 host->local, 1 local->host (download), 2 local->local.
  std::uint64_t trx_upload = 0, trx_download = 0, trx_local = 0;
  std::uint64_t front_end_ns = 0;  // host time inside gif::transfer (GS front end)
};

// Follows the GIF tags of one path across transfers. Only PACKED A+D entries
// can address SIGNAL/FINISH/LABEL/TRXDIR; REGLIST and IMAGE data are skipped.
class GifScanner {
public:
  // signals (optional): every SIGNAL/FINISH/LABEL write in stream order, with the donor register and value.
  void scan(const std::uint8_t *data, std::uint32_t qwc, PathCounts &counts,
            std::vector<SignalWrite> *signals = nullptr);
  bool inside_packet() const { return active_; }

private:
  bool active_ = false;
  std::uint32_t flg_ = 0, nreg_ = 0;
  std::uint64_t regs_ = 0;
  std::uint64_t left_ = 0;      // qwords (PACKED, IMAGE) or registers (REGLIST) still to skip
  std::uint32_t packed_reg_ = 0;  // next PACKED descriptor index
  bool packed_has_ad_ = false;    // IN-469: the current tag has an A+D descriptor
};

struct OverlapTotals {
  std::uint64_t frames = 0;          // modeled VBlank-to-VBlank frames
  std::uint64_t frames_waiting = 0;  // frames where the EE would wait at the sync
  std::uint64_t frames_with_calls = 0;
  double program_ns = 0;             // VU1 time moved to the worker (call time minus PATH1 GS)
  double wait_ns = 0;                // EE wait at the VBlank sync
  double first_post_ns = 0;          // sum over frames with calls: first post after the frame start
  double last_post_to_sync_ns = 0;   // sum over frames with calls: last post to the sync
};

// One VU1 worker in a compressed EE timeline: each VU1 call is posted when the
// EE reached it, its program time leaves the EE timeline, its PATH1 GS front-end
// time stays on the EE, and the worker runs calls in order. At the VBlank sync
// the EE waits until the worker is idle (MTVU-PLAN.md section 5.3).
class OverlapModel {
public:
  void vu1_call(std::uint64_t start_ns, std::uint64_t end_ns, std::uint64_t front_end_ns);
  void sync(std::uint64_t now_ns);
  const OverlapTotals &totals() const { return totals_; }

private:
  bool started_ = false;
  double shift_ = 0;        // host time removed from the EE timeline so far
  double worker_free_ = 0;  // compressed time when the worker queue is empty
  double frame_start_ = 0;  // compressed time of the last sync
  double frame_program_ = 0, first_post_ = -1, last_post_ = -1;
  OverlapTotals totals_;
};

struct Counters {
  PathCounts path[4];
  std::uint64_t frames = 0;
  std::uint64_t vif1_transfers = 0, vif1_qwords = 0;
  std::uint64_t vu1_calls = 0, vu0_vif_calls = 0;
  std::uint64_t vif1_stat_reads = 0, vif1_other_reads = 0;
  std::uint64_t vif1_fbrst_writes = 0, vif1_other_writes = 0;
  std::uint64_t ee_vu1_data_reads = 0, ee_vu1_data_writes = 0;
  std::uint64_t ee_vu1_code_reads = 0, ee_vu1_code_writes = 0, ee_vu0_accesses = 0;
  std::uint64_t vu1_call_ns = 0, vu1_front_end_ns = 0;
  OverlapTotals overlap;
};

Counters difference(const Counters &now, const Counters &before);

// Prints the interval lines (prefix "[mtvu-measure]").
void format(std::FILE *output, const char *reason, const Counters &interval);

// GT4_MTVU_MEASURE: unset or "1" = on, "0" = off. Returns false for any other value.
bool parse_switch(const char *value, bool &enabled);

class Measure {
public:
  using Clock = std::uint64_t (*)();  // monotonic nanoseconds
  Measure(bool enabled, Clock clock);
  bool enabled() const { return enabled_; }
  std::uint64_t now() const { return clock_(); }

  void gif_transfer(int path, const std::uint8_t *data, std::uint32_t qwc);
  void gif_done(int path, std::uint64_t start_ns);
  void vu_call_begin(int unit);
  void vu_call_end(int unit, std::uint64_t start_ns);
  void vif_transfer(int unit, std::uint32_t qwc);
  void vif_read(int unit, std::uint32_t reg);
  void vif_write(int unit, std::uint32_t reg);
  void ee_vu_memory(std::uint32_t physical, bool write);
  void vblank();

  const Counters &total() const { return total_; }
  // Interval since the last reset_interval (F7, F8, periodic perf reports).
  Counters interval() const { return difference(total_, interval_start_); }
  void reset_interval() { interval_start_ = total_; }

private:
  bool enabled_;
  Clock clock_;
  Counters total_, interval_start_;
  GifScanner scanners_[4];
  OverlapModel model_;
  bool in_vu1_call_ = false;
  std::uint64_t call_front_end_ns_ = 0;
};

// Process-wide instance for the runtime hooks; reads GT4_MTVU_MEASURE once.
// An invalid value disables the counters and prints one warning.
Measure &shared();

// Hooks used by the slice169 runtime capsule (scripts/prepare_perf_runtime.py --mtvu-measure).
class GifScope {
public:
  GifScope(int path, const std::uint8_t *data, std::uint32_t qwc);
  ~GifScope();
  GifScope(const GifScope &) = delete;
  GifScope &operator=(const GifScope &) = delete;

private:
  int path_;
  std::uint64_t start_ = 0;
};

class VuCallScope {
public:
  explicit VuCallScope(int unit);
  ~VuCallScope();
  VuCallScope(const VuCallScope &) = delete;
  VuCallScope &operator=(const VuCallScope &) = delete;

private:
  int unit_;
  std::uint64_t start_ = 0;
};

void on_vif_transfer(int unit, std::uint32_t qwc);
void on_vif_read(int unit, std::uint32_t reg);
void on_vif_write(int unit, std::uint32_t reg);
void on_ee_vu_memory(std::uint32_t physical, bool write);
// Guest VBlank (system.cpp vblank, before the host on_vblank callback).
void on_vblank();
// F8/periodic perf report section (native/window.cpp); prints the interval and starts a new one.
void report(std::FILE *output, const char *reason);
void reset_interval();
// F7: the interval restarts at the next guest VBlank, the same boundary as the perf probe.
void request_interval_reset();
// End of run: session totals (headless runs have no F8).
void final_report(std::FILE *output);

}  // namespace gt4::mtvu
