#include "mtvu_measure.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace gt4::mtvu {

namespace {

std::uint64_t load64(const std::uint8_t *at) {
  std::uint64_t value;
  std::memcpy(&value, at, sizeof value);
  return value;
}

double per_frame(double value, std::uint64_t frames) { return frames ? value / static_cast<double>(frames) : 0.0; }

double ms(double ns) { return ns / 1.0e6; }

unsigned long long ull(std::uint64_t value) { return static_cast<unsigned long long>(value); }

PathCounts path_difference(const PathCounts &a, const PathCounts &b) {
  PathCounts r;
  r.transfers = a.transfers - b.transfers;
  r.qwords = a.qwords - b.qwords;
  r.tags = a.tags - b.tags;
  r.signal = a.signal - b.signal;
  r.finish = a.finish - b.finish;
  r.label = a.label - b.label;
  r.trx_upload = a.trx_upload - b.trx_upload;
  r.trx_download = a.trx_download - b.trx_download;
  r.trx_local = a.trx_local - b.trx_local;
  r.front_end_ns = a.front_end_ns - b.front_end_ns;
  return r;
}

}  // namespace

void GifScanner::scan(const std::uint8_t *data, std::uint32_t qwc, PathCounts &counts,
                      std::vector<SignalWrite> *signals) {
  ++counts.transfers;
  counts.qwords += qwc;
  std::uint32_t index = 0;
  while (index < qwc) {
    const std::uint8_t *qword = data + static_cast<std::size_t>(index) * 16u;
    if (!active_) {
      const std::uint64_t tag = load64(qword);
      const std::uint32_t nloop = static_cast<std::uint32_t>(tag) & 0x7FFFu;
      flg_ = static_cast<std::uint32_t>(tag >> 58) & 3u;
      nreg_ = static_cast<std::uint32_t>(tag >> 60) & 0xFu;
      if (!nreg_) nreg_ = 16;
      regs_ = load64(qword + 8);
      packed_reg_ = 0;
      // IN-469: only A+D descriptors are looked at, so the data of a tag without one is skipped in one step.
      packed_has_ad_ = false;
      for (std::uint32_t r = 0; r < nreg_; ++r) packed_has_ad_ = packed_has_ad_ || ((regs_ >> (r * 4u)) & 0xFu) == 0xEu;
      ++index;
      if (!nloop) continue;
      ++counts.tags;
      // PACKED: one qword per register; REGLIST: two registers per qword; IMAGE: NLOOP qwords.
      left_ = flg_ == 0 || flg_ == 1 ? static_cast<std::uint64_t>(nloop) * nreg_ : nloop;
      active_ = true;
      continue;
    }
    if (flg_ == 0 && !packed_has_ad_) {
      const std::uint64_t take = std::min<std::uint64_t>(left_, qwc - index);
      index += static_cast<std::uint32_t>(take);
      left_ -= take;
      packed_reg_ = static_cast<std::uint32_t>((packed_reg_ + take) % nreg_);
      if (!left_) active_ = false;
    } else if (flg_ == 0) {
      const std::uint32_t descriptor = static_cast<std::uint32_t>(regs_ >> (packed_reg_ * 4u)) & 0xFu;
      if (descriptor == 0xEu) {
        const std::uint8_t address = qword[8] & 0x7Fu;
        const std::uint64_t value = load64(qword);
        if (signals && (qword[8] == kGsSignal || qword[8] == kGsFinish || qword[8] == kGsLabel))
          signals->push_back(SignalWrite{qword[8], value});
        if (address == kGsSignal) ++counts.signal;
        else if (address == kGsFinish) ++counts.finish;
        else if (address == kGsLabel) ++counts.label;
        else if (address == kGsTrxDir) {
          const std::uint64_t direction = value & 3u;
          if (direction == 0) ++counts.trx_upload;
          else if (direction == 1) ++counts.trx_download;
          else if (direction == 2) ++counts.trx_local;
        }
      }
      packed_reg_ = (packed_reg_ + 1u) % nreg_;
      ++index;
      if (--left_ == 0) active_ = false;
    } else if (flg_ == 1) {
      // An odd register count is padded to a whole qword.
      left_ = left_ > 2 ? left_ - 2 : 0;
      ++index;
      if (!left_) active_ = false;
    } else {
      const std::uint64_t take = std::min<std::uint64_t>(left_, qwc - index);
      index += static_cast<std::uint32_t>(take);
      left_ -= take;
      if (!left_) active_ = false;
    }
  }
}

void OverlapModel::vu1_call(std::uint64_t start_ns, std::uint64_t end_ns, std::uint64_t front_end_ns) {
  const double duration = end_ns > start_ns ? static_cast<double>(end_ns - start_ns) : 0.0;
  const double front_end = static_cast<double>(front_end_ns);
  const double program = duration > front_end ? duration - front_end : 0.0;
  const double post = static_cast<double>(start_ns) - shift_;
  worker_free_ = std::max(worker_free_, post) + program;
  shift_ += program;
  if (!started_) return;
  frame_program_ += program;
  if (first_post_ < 0) first_post_ = post - frame_start_;
  last_post_ = post;
}

void OverlapModel::sync(std::uint64_t now_ns) {
  const double compressed = static_cast<double>(now_ns) - shift_;
  const double wait = worker_free_ > compressed ? worker_free_ - compressed : 0.0;
  shift_ -= wait;
  if (started_) {
    ++totals_.frames;
    totals_.program_ns += frame_program_;
    totals_.wait_ns += wait;
    if (wait > 0) ++totals_.frames_waiting;
    if (first_post_ >= 0) {
      ++totals_.frames_with_calls;
      totals_.first_post_ns += first_post_;
      totals_.last_post_to_sync_ns += compressed - last_post_;
    }
  }
  started_ = true;
  frame_start_ = compressed + wait;
  frame_program_ = 0;
  first_post_ = last_post_ = -1;
}

Counters difference(const Counters &a, const Counters &b) {
  Counters r;
  for (int path = 0; path < 4; ++path) r.path[path] = path_difference(a.path[path], b.path[path]);
  r.frames = a.frames - b.frames;
  r.vif1_transfers = a.vif1_transfers - b.vif1_transfers;
  r.vif1_qwords = a.vif1_qwords - b.vif1_qwords;
  r.vu1_calls = a.vu1_calls - b.vu1_calls;
  r.vu0_vif_calls = a.vu0_vif_calls - b.vu0_vif_calls;
  r.vif1_stat_reads = a.vif1_stat_reads - b.vif1_stat_reads;
  r.vif1_other_reads = a.vif1_other_reads - b.vif1_other_reads;
  r.vif1_fbrst_writes = a.vif1_fbrst_writes - b.vif1_fbrst_writes;
  r.vif1_other_writes = a.vif1_other_writes - b.vif1_other_writes;
  r.ee_vu1_data_reads = a.ee_vu1_data_reads - b.ee_vu1_data_reads;
  r.ee_vu1_data_writes = a.ee_vu1_data_writes - b.ee_vu1_data_writes;
  r.ee_vu1_code_reads = a.ee_vu1_code_reads - b.ee_vu1_code_reads;
  r.ee_vu1_code_writes = a.ee_vu1_code_writes - b.ee_vu1_code_writes;
  r.ee_vu0_accesses = a.ee_vu0_accesses - b.ee_vu0_accesses;
  r.vu1_call_ns = a.vu1_call_ns - b.vu1_call_ns;
  r.vu1_front_end_ns = a.vu1_front_end_ns - b.vu1_front_end_ns;
  r.overlap.frames = a.overlap.frames - b.overlap.frames;
  r.overlap.frames_waiting = a.overlap.frames_waiting - b.overlap.frames_waiting;
  r.overlap.frames_with_calls = a.overlap.frames_with_calls - b.overlap.frames_with_calls;
  r.overlap.program_ns = a.overlap.program_ns - b.overlap.program_ns;
  r.overlap.wait_ns = a.overlap.wait_ns - b.overlap.wait_ns;
  r.overlap.first_post_ns = a.overlap.first_post_ns - b.overlap.first_post_ns;
  r.overlap.last_post_to_sync_ns = a.overlap.last_post_to_sync_ns - b.overlap.last_post_to_sync_ns;
  return r;
}

void format(std::FILE *out, const char *reason, const Counters &c) {
  const std::uint64_t f = c.frames;
  std::fprintf(out, "[mtvu-measure] %s: frames=%llu vif1_transfers/frame=%.1f vif1_qwords/frame=%.1f vu1_calls/frame=%.1f vu0_vif_calls=%llu\n",
               reason, ull(f), per_frame(double(c.vif1_transfers), f), per_frame(double(c.vif1_qwords), f),
               per_frame(double(c.vu1_calls), f), ull(c.vu0_vif_calls));
  std::fprintf(out, "[mtvu-measure] vif1_ee_access: stat_reads=%llu other_reads=%llu fbrst_writes=%llu other_writes=%llu\n",
               ull(c.vif1_stat_reads), ull(c.vif1_other_reads), ull(c.vif1_fbrst_writes), ull(c.vif1_other_writes));
  for (int path = 1; path <= 3; ++path) {
    const PathCounts &p = c.path[path];
    std::fprintf(out, "[mtvu-measure] gif_path%d: transfers/frame=%.1f qwords/frame=%.1f tags/frame=%.1f signal=%llu finish=%llu label=%llu "
                      "trx_upload=%llu trx_download=%llu trx_local=%llu front_end_ms/frame=%.3f\n",
                 path, per_frame(double(p.transfers), f), per_frame(double(p.qwords), f), per_frame(double(p.tags), f),
                 ull(p.signal), ull(p.finish), ull(p.label), ull(p.trx_upload), ull(p.trx_download), ull(p.trx_local),
                 ms(per_frame(double(p.front_end_ns), f)));
  }
  std::fprintf(out, "[mtvu-measure] ee_vu_memory: vu1_data_reads=%llu vu1_data_writes=%llu vu1_code_reads=%llu vu1_code_writes=%llu vu0_accesses=%llu\n",
               ull(c.ee_vu1_data_reads), ull(c.ee_vu1_data_writes), ull(c.ee_vu1_code_reads), ull(c.ee_vu1_code_writes),
               ull(c.ee_vu0_accesses));
  const double call = per_frame(double(c.vu1_call_ns), f), front = per_frame(double(c.vu1_front_end_ns), f);
  std::fprintf(out, "[mtvu-measure] vu1_time ms/frame: call=%.3f path1_front_end_inside=%.3f program=%.3f (host timing)\n",
               ms(call), ms(front), ms(call > front ? call - front : 0.0));
  const OverlapTotals &o = c.overlap;
  const double program = per_frame(o.program_ns, o.frames), wait = per_frame(o.wait_ns, o.frames);
  std::fprintf(out, "[mtvu-measure] overlap_model ms/frame: program_moved=%.3f vblank_wait=%.3f saving=%.3f frames=%llu "
                    "frames_waiting=%.1f%% first_post_after_vblank=%.3f last_post_before_vblank=%.3f (host timing)\n",
               ms(program), ms(wait), ms(program - wait), ull(o.frames),
               o.frames ? 100.0 * double(o.frames_waiting) / double(o.frames) : 0.0,
               ms(per_frame(o.first_post_ns, o.frames_with_calls)), ms(per_frame(o.last_post_to_sync_ns, o.frames_with_calls)));
  const PathCounts &path1 = c.path[1];
  std::fprintf(out, "[mtvu-measure] invariant: path1_signal_writes=%llu (0 required for exact MTVU) hard_syncs: downloads=%llu ee_vu1_memory=%llu\n",
               ull(path1.signal + path1.finish + path1.label),
               ull(c.path[1].trx_download + c.path[2].trx_download + c.path[3].trx_download),
               ull(c.ee_vu1_data_reads + c.ee_vu1_data_writes + c.ee_vu1_code_reads + c.ee_vu1_code_writes));
}

bool parse_switch(const char *value, bool &enabled) {
  if (!value || std::strcmp(value, "1") == 0) {
    enabled = true;
    return true;
  }
  enabled = false;
  return std::strcmp(value, "0") == 0;
}

Measure::Measure(bool enabled, Clock clock) : enabled_(enabled), clock_(clock) {}

void Measure::gif_transfer(int path, const std::uint8_t *data, std::uint32_t qwc) {
  if (!enabled_ || path < 0 || path > 3) return;
  scanners_[path].scan(data, qwc, total_.path[path]);
}

void Measure::gif_done(int path, std::uint64_t start_ns) {
  if (!enabled_ || path < 0 || path > 3) return;
  const std::uint64_t now_ns = clock_();
  const std::uint64_t elapsed = now_ns > start_ns ? now_ns - start_ns : 0;
  total_.path[path].front_end_ns += elapsed;
  if (path == 1 && in_vu1_call_) call_front_end_ns_ += elapsed;
}

void Measure::vu_call_begin(int unit) {
  if (!enabled_) return;
  if (unit != 1) {
    ++total_.vu0_vif_calls;
    return;
  }
  in_vu1_call_ = true;
  call_front_end_ns_ = 0;
}

void Measure::vu_call_end(int unit, std::uint64_t start_ns) {
  if (!enabled_ || unit != 1) return;
  const std::uint64_t end_ns = clock_();
  in_vu1_call_ = false;
  ++total_.vu1_calls;
  total_.vu1_call_ns += end_ns > start_ns ? end_ns - start_ns : 0;
  total_.vu1_front_end_ns += call_front_end_ns_;
  model_.vu1_call(start_ns, end_ns, call_front_end_ns_);
  total_.overlap = model_.totals();
}

void Measure::vif_transfer(int unit, std::uint32_t qwc) {
  if (!enabled_ || unit != 1) return;
  ++total_.vif1_transfers;
  total_.vif1_qwords += qwc;
}

void Measure::vif_read(int unit, std::uint32_t reg) {
  if (!enabled_ || unit != 1) return;
  if (reg == 0x00) ++total_.vif1_stat_reads;
  else ++total_.vif1_other_reads;
}

void Measure::vif_write(int unit, std::uint32_t reg) {
  if (!enabled_ || unit != 1) return;
  if (reg == 0x10) ++total_.vif1_fbrst_writes;
  else ++total_.vif1_other_writes;
}

void Measure::ee_vu_memory(std::uint32_t physical, bool write) {
  if (!enabled_) return;
  // VU memory window 0x11000000-0x1100FFFF: VU0 code/data below 0x11008000, VU1 code, then VU1 data at 0x1100C000.
  const std::uint32_t offset = physical & 0xFFFFu;
  if (offset < 0x8000u) ++total_.ee_vu0_accesses;
  else if (offset < 0xC000u) ++(write ? total_.ee_vu1_code_writes : total_.ee_vu1_code_reads);
  else ++(write ? total_.ee_vu1_data_writes : total_.ee_vu1_data_reads);
}

void Measure::vblank() {
  if (!enabled_) return;
  ++total_.frames;
  model_.sync(clock_());
  total_.overlap = model_.totals();
}

namespace {

std::uint64_t steady_ns() {
  static const auto base = std::chrono::steady_clock::now();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - base).count());
}

struct Shared {
  Measure measure;
  std::uint64_t report_every = 0;  // GT4_MTVU_MEASURE_EVERY: headless interval lines every N VBlanks
  Counters every_start;
  bool reset_requested = false;
};

Shared &shared_state() {
  static Shared state = [] {
    bool enabled = true;
    const char *value = std::getenv("GT4_MTVU_MEASURE");
    if (!parse_switch(value, enabled))
      std::fprintf(stderr, "[mtvu-measure] GT4_MTVU_MEASURE must be 0 or 1; counters disabled\n");
    std::uint64_t every = 0;
    if (const char *period = std::getenv("GT4_MTVU_MEASURE_EVERY")) every = std::strtoull(period, nullptr, 10);
    std::fprintf(stderr, "[mtvu-measure] %s (TASK-011 step 0, counters only)\n", enabled ? "enabled" : "disabled");
    return Shared{Measure(enabled, steady_ns), every, Counters{}, false};
  }();
  return state;
}

}  // namespace

Measure &shared() { return shared_state().measure; }

GifScope::GifScope(int path, const std::uint8_t *data, std::uint32_t qwc) : path_(path) {
  Measure &measure = shared();
  if (!measure.enabled()) return;
  measure.gif_transfer(path, data, qwc);
  start_ = measure.now();
}

GifScope::~GifScope() {
  Measure &measure = shared();
  if (measure.enabled()) measure.gif_done(path_, start_);
}

VuCallScope::VuCallScope(int unit) : unit_(unit) {
  Measure &measure = shared();
  if (!measure.enabled()) return;
  measure.vu_call_begin(unit);
  start_ = measure.now();
}

VuCallScope::~VuCallScope() {
  Measure &measure = shared();
  if (measure.enabled()) measure.vu_call_end(unit_, start_);
}

void on_vif_transfer(int unit, std::uint32_t qwc) { shared().vif_transfer(unit, qwc); }
void on_vif_read(int unit, std::uint32_t reg) { shared().vif_read(unit, reg); }
void on_vif_write(int unit, std::uint32_t reg) { shared().vif_write(unit, reg); }
void on_ee_vu_memory(std::uint32_t physical, bool write) { shared().ee_vu_memory(physical, write); }

void on_vblank() {
  Shared &state = shared_state();
  if (!state.measure.enabled()) return;
  state.measure.vblank();
  if (state.reset_requested) {
    state.reset_requested = false;
    state.measure.reset_interval();
  }
  if (state.report_every && state.measure.total().frames % state.report_every == 0) {
    format(stderr, "frames", difference(state.measure.total(), state.every_start));
    state.every_start = state.measure.total();
  }
}

void report(std::FILE *output, const char *reason) {
  Measure &measure = shared();
  if (measure.enabled()) format(output, reason, measure.interval());
}

void reset_interval() { shared().reset_interval(); }

void request_interval_reset() { shared_state().reset_requested = true; }

void final_report(std::FILE *output) {
  Measure &measure = shared();
  if (measure.enabled()) format(output, "session total", measure.total());
}

}  // namespace gt4::mtvu
