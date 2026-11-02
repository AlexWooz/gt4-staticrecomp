// CTest native_mtvu_measure: host-only controls for the TASK-011 step 0 counters
// (GIF scanner, worker overlap model, interval arithmetic, report lines, switch).
#include "mtvu_measure.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace gt4::mtvu;

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

struct Qword {
  std::uint64_t lo = 0, hi = 0;
};

Qword tag(std::uint32_t nloop, std::uint32_t flg, std::uint32_t nreg, std::uint64_t regs, bool eop = true) {
  Qword q;
  q.lo = (nloop & 0x7FFFu) | (eop ? 1ull << 15 : 0) | (static_cast<std::uint64_t>(flg & 3u) << 58) |
         (static_cast<std::uint64_t>(nreg & 0xFu) << 60);
  q.hi = regs;
  return q;
}

Qword address_data(std::uint8_t address, std::uint64_t value) { return Qword{value, address}; }

std::vector<std::uint8_t> bytes(const std::vector<Qword> &qwords) {
  std::vector<std::uint8_t> out(qwords.size() * 16);
  for (std::size_t i = 0; i < qwords.size(); ++i) {
    std::memcpy(out.data() + i * 16, &qwords[i].lo, 8);
    std::memcpy(out.data() + i * 16 + 8, &qwords[i].hi, 8);
  }
  return out;
}

void check_scanner() {
  // PACKED A+D packet: SIGNAL, FINISH, LABEL, three TRXDIR directions, one other register.
  const std::vector<Qword> packet = {tag(7, 0, 1, 0xE),
                                     address_data(kGsSignal, 0x1),
                                     address_data(kGsFinish, 0),
                                     address_data(kGsLabel, 0x2),
                                     address_data(kGsTrxDir, 0),
                                     address_data(kGsTrxDir, 1),
                                     address_data(kGsTrxDir, 2),
                                     address_data(0x00, 0x3)};
  const auto data = bytes(packet);
  GifScanner scanner;
  PathCounts counts;
  scanner.scan(data.data(), static_cast<std::uint32_t>(packet.size()), counts);
  require(counts.transfers == 1 && counts.qwords == 8 && counts.tags == 1, "packet totals");
  require(counts.signal == 1 && counts.finish == 1 && counts.label == 1, "signal registers");
  require(counts.trx_upload == 1 && counts.trx_download == 1 && counts.trx_local == 1, "TRXDIR directions");
  require(!scanner.inside_packet(), "packet closed");

  // The same packet split across three transfers counts the same.
  GifScanner split;
  PathCounts split_counts;
  split.scan(data.data(), 2, split_counts);
  require(split.inside_packet(), "open after first part");
  split.scan(data.data() + 32, 5, split_counts);
  split.scan(data.data() + 112, 1, split_counts);
  require(split_counts.signal == 1 && split_counts.finish == 1 && split_counts.label == 1 &&
              split_counts.trx_download == 1 && split_counts.transfers == 3 && split_counts.tags == 1,
          "split packet");

  // Two descriptors per loop (0x0 then A+D): only the second qword of each loop is A+D.
  const std::vector<Qword> mixed = {tag(2, 0, 2, 0xE0), address_data(kGsSignal, 0), address_data(kGsFinish, 0),
                                    address_data(kGsSignal, 0), address_data(kGsLabel, 0)};
  const auto mixed_data = bytes(mixed);
  PathCounts mixed_counts;
  GifScanner{}.scan(mixed_data.data(), 5, mixed_counts);
  require(mixed_counts.signal == 0 && mixed_counts.finish == 1 && mixed_counts.label == 1, "descriptor order");

  // REGLIST (3 registers -> 2 qwords) and IMAGE (2 qwords) payloads that look like A+D are skipped,
  // NREG 0 means 16, and an NLOOP 0 tag carries no data.
  const std::vector<Qword> skipped = {tag(1, 1, 3, 0xEEE), address_data(kGsSignal, 0), address_data(kGsFinish, 0),
                                      tag(2, 2, 0, 0), address_data(kGsSignal, 0), address_data(kGsSignal, 0),
                                      tag(0, 0, 1, 0xE), tag(1, 0, 0, 0xEEEEEEEEEEEEEEEEull)};
  std::vector<Qword> with_sixteen = skipped;
  for (int i = 0; i < 16; ++i) with_sixteen.push_back(address_data(i == 15 ? kGsLabel : 0x00, 0));
  const auto skipped_data = bytes(with_sixteen);
  PathCounts skipped_counts;
  GifScanner sixteen;
  sixteen.scan(skipped_data.data(), static_cast<std::uint32_t>(with_sixteen.size()), skipped_counts);
  require(skipped_counts.signal == 0 && skipped_counts.finish == 0, "REGLIST and IMAGE skipped");
  require(skipped_counts.label == 1 && skipped_counts.tags == 3 && !sixteen.inside_packet(), "NREG 0 means 16");

  // IN-469: a PACKED tag without A+D (STQ, RGBA, XYZF2) is skipped whole, also across transfers, even when its data
  // looks like A+D, and the next tag is read where it ends. A descriptor past NREG does not count as A+D.
  const std::vector<Qword> strip = {tag(2, 0, 3, 0xE412), address_data(kGsSignal, 0), address_data(kGsSignal, 0),
                                    address_data(kGsSignal, 0), address_data(kGsFinish, 0), address_data(kGsFinish, 0),
                                    address_data(kGsFinish, 0), tag(1, 0, 1, 0xE), address_data(kGsLabel, 0)};
  const auto strip_data = bytes(strip);
  for (std::uint32_t cut = 1; cut < strip.size(); ++cut) {
    GifScanner parts;
    PathCounts strip_counts;
    parts.scan(strip_data.data(), cut, strip_counts);
    parts.scan(strip_data.data() + static_cast<std::size_t>(cut) * 16, static_cast<std::uint32_t>(strip.size()) - cut,
               strip_counts);
    require(strip_counts.signal == 0 && strip_counts.finish == 0 && strip_counts.label == 1 &&
                strip_counts.tags == 2 && strip_counts.qwords == strip.size() && !parts.inside_packet(),
            "PACKED tag without A+D skipped");
  }
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

void check_overlap_model() {
  OverlapModel model;
  model.sync(0);  // first sync starts the model
  // Frame 1: one call at 1000-4000 with 1000 of PATH1 GS work: 2000 moves to the worker, no wait.
  model.vu1_call(1000, 4000, 1000);
  model.sync(10000);
  require(model.totals().frames == 1 && near(model.totals().program_ns, 2000) && near(model.totals().wait_ns, 0),
          "early call overlaps fully");
  require(near(model.totals().first_post_ns, 1000) && near(model.totals().last_post_to_sync_ns, 7000),
          "post positions in the compressed timeline");
  // Frame 2: a late 8000 call posted at compressed 16000; sync at compressed 17000 waits 7000.
  model.vu1_call(18000, 26000, 0);
  model.sync(27000);
  require(model.totals().frames == 2 && model.totals().frames_waiting == 1, "late call waits");
  require(near(model.totals().program_ns, 10000) && near(model.totals().wait_ns, 7000), "wait amount");
  // Frame 3: no calls; the delayed EE timeline continues without waiting.
  model.sync(30000);
  require(model.totals().frames == 3 && model.totals().frames_with_calls == 2 && near(model.totals().wait_ns, 7000),
          "empty frame");
  // Two back-to-back calls queue on the worker: posts at 0 and 1000 (compressed), 5000 each.
  OverlapModel queue;
  queue.sync(0);
  queue.vu1_call(0, 5000, 0);
  queue.vu1_call(5000, 10000, 0);
  queue.sync(11000);  // compressed 1000; worker free at 10000
  require(near(queue.totals().wait_ns, 9000) && near(queue.totals().program_ns, 10000), "worker queue order");
}

std::uint64_t fake_now = 0;
std::uint64_t fake_clock() { return fake_now; }

std::string report_text(const Counters &counters) {
  std::FILE *stream = nullptr;
  require(tmpfile_s(&stream) == 0 && stream != nullptr, "tmpfile");
  format(stream, "test", counters);
  std::fflush(stream);
  std::rewind(stream);
  std::string text;
  char buffer[1024];
  while (std::fgets(buffer, sizeof buffer, stream)) text += buffer;
  std::fclose(stream);
  return text;
}

void check_measure() {
  Measure measure(true, fake_clock);
  fake_now = 0;
  measure.vblank();
  measure.vif_transfer(1, 10);
  measure.vif_transfer(0, 99);  // VIF0 is not counted
  measure.vif_read(1, 0x00);
  measure.vif_read(1, 0x30);
  measure.vif_write(1, 0x10);
  measure.vif_write(1, 0x20);
  measure.ee_vu_memory(0x1100C010u, false);
  measure.ee_vu_memory(0x1100C010u, true);
  measure.ee_vu_memory(0x11008000u, true);
  measure.ee_vu_memory(0x11004000u, false);
  // A VU1 call from 100 to 1100 with one PATH1 transfer of 300 inside it.
  const std::vector<Qword> kick = {tag(1, 0, 1, 0xE), address_data(kGsFinish, 0)};
  const auto kick_data = bytes(kick);
  fake_now = 100;
  measure.vu_call_begin(1);
  fake_now = 400;
  measure.gif_transfer(1, kick_data.data(), 2);
  fake_now = 700;
  measure.gif_done(1, 400);
  fake_now = 1100;
  measure.vu_call_end(1, 100);
  // PATH3 outside the call is not part of the VU1 front end.
  measure.gif_transfer(3, kick_data.data(), 2);
  fake_now = 1500;
  measure.gif_done(3, 1100);
  measure.vu_call_begin(0);
  fake_now = 2000;
  measure.vblank();
  const Counters total = measure.total();  // copy: later VBlanks change the live totals
  require(total.frames == 2 && total.vif1_transfers == 1 && total.vif1_qwords == 10, "VIF1 counters");
  require(total.vif1_stat_reads == 1 && total.vif1_other_reads == 1 && total.vif1_fbrst_writes == 1 &&
              total.vif1_other_writes == 1, "VIF1 register access");
  require(total.ee_vu1_data_reads == 1 && total.ee_vu1_data_writes == 1 && total.ee_vu1_code_writes == 1 &&
              total.ee_vu1_code_reads == 0 && total.ee_vu0_accesses == 1, "EE VU memory");
  require(total.vu1_calls == 1 && total.vu0_vif_calls == 1 && total.vu1_call_ns == 1000 && total.vu1_front_end_ns == 300,
          "VU1 call split");
  require(total.path[1].finish == 1 && total.path[1].front_end_ns == 300 && total.path[3].front_end_ns == 400,
          "per-path front end");
  require(total.overlap.frames == 1 && near(total.overlap.program_ns, 700), "model fed from calls");

  // Interval since reset.
  measure.reset_interval();
  measure.vif_transfer(1, 4);
  fake_now = 3000;
  measure.vblank();
  const Counters interval = measure.interval();
  require(interval.frames == 1 && interval.vif1_transfers == 1 && interval.vif1_qwords == 4 && interval.vu1_calls == 0,
          "interval difference");

  const std::string text = report_text(total);
  for (const char *line : {"[mtvu-measure] test: frames=2", "[mtvu-measure] gif_path1:", "[mtvu-measure] gif_path3:",
                           "[mtvu-measure] ee_vu_memory: vu1_data_reads=1", "[mtvu-measure] vu1_time ms/frame:",
                           "[mtvu-measure] overlap_model ms/frame:", "path1_signal_writes=1"})
    require(text.find(line) != std::string::npos, line);

  // Disabled: nothing counts.
  Measure off(false, fake_clock);
  off.vblank();
  off.vif_transfer(1, 1);
  off.gif_transfer(1, kick_data.data(), 2);
  off.vu_call_begin(1);
  off.vu_call_end(1, 0);
  require(off.total().frames == 0 && off.total().vif1_transfers == 0 && off.total().path[1].transfers == 0 &&
              off.total().vu1_calls == 0, "disabled measure counts nothing");
}

void check_switch() {
  bool enabled = false;
  require(parse_switch(nullptr, enabled) && enabled, "unset is on");
  require(parse_switch("1", enabled) && enabled, "1 is on");
  require(parse_switch("0", enabled) && !enabled, "0 is off");
  require(!parse_switch("yes", enabled) && !enabled, "other values are refused and off");
  require(!parse_switch("", enabled) && !enabled, "empty is refused");
}

Qword signal_entry(std::uint64_t value, std::uint8_t address) {
  Qword q;
  q.lo = value;                     // A+D data: the value
  q.hi = static_cast<std::uint64_t>(address);  // A+D address in byte 8
  return q;
}

// Section 12 ordering rule: SIGNAL/FINISH/LABEL of PATH2/PATH3 are applied at packet acceptance, in stream order.
// The scanner must report every such write in order, across chunk boundaries, with the donor's address byte.
void check_signal_order() {
  GifScanner scanner;
  PathCounts counts;
  std::vector<SignalWrite> writes;
  // PACKED tag, 3 entries, each an A+D register (0xE): SIGNAL 0x60 with 0x11, FINISH 0x61 with 0x33, then the byte
  // 0xE0 (not a signal address for the donor: write() dispatches on the full byte).
  Qword tag_word = tag(3, 0, 3, 0xEull | (0xEull << 4) | (0xEull << 8));
  std::vector<Qword> first{tag_word, signal_entry(0x11, 0x60)};
  scanner.scan(reinterpret_cast<const std::uint8_t *>(first.data()), 2, counts, &writes);
  require(writes.size() == 1 && writes[0].reg == 0x60 && writes[0].value == 0x11,
          "first chunk reports SIGNAL 0x11 at 0x60");
  // The packet state carries across the chunk: FINISH 0x33, then address byte 0xE0 (not a signal).
  std::vector<Qword> second{signal_entry(0x33, 0x61), signal_entry(0x44, 0xE0)};
  scanner.scan(reinterpret_cast<const std::uint8_t *>(second.data()), 2, counts, &writes);
  require(writes.size() == 2 && writes[1].reg == 0x61 && writes[1].value == 0x33,
          "second chunk reports FINISH 0x33 at 0x61 in order");
  require(writes.size() == 2, "the 0xE0 address byte is not a signal (donor address is the full byte)");
  require(counts.finish == 1 && counts.label == 0, "counters still count FINISH with the list in use");

  // Without the list the counters still count (no behavior change for the measure build).
  GifScanner plain;
  PathCounts plain_counts;
  plain.scan(reinterpret_cast<const std::uint8_t *>(first.data()), 2, plain_counts);
  require(plain_counts.signal == 1, "scan without a list counts SIGNAL");

  // Predicate: only PATH2/PATH3 pre-apply, only addresses 0x60-0x62.
  require(pre_applies_signal(2, 0x60) && pre_applies_signal(2, 0x62) && pre_applies_signal(3, 0x61),
          "PATH2 and PATH3 signals are pre-applied");
  require(!pre_applies_signal(1, 0x60), "PATH1 signals are not pre-applied");
  require(!pre_applies_signal(2, 0x5F) && !pre_applies_signal(3, 0x63), "other addresses are not pre-applied");
}

}  // namespace

int main() {
  try {
    check_scanner();
    check_overlap_model();
    check_measure();
    check_switch();
    check_signal_order();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "native_mtvu_measure FAILED: %s\n", error.what());
    return 1;
  }
  std::puts("native_mtvu_measure: scanner, overlap model, counters, report and switch OK");
  return 0;
}
