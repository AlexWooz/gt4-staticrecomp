// TASK-011 step 1: EE-side journal and worker-side end (see mtvu_journal.hpp).
#include "mtvu_journal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdio>
#include <stdexcept>

namespace gt4::mtvu {

WorkerEnd::WorkerEnd(SpscByteRing &input, SpscByteRing &output, Doorbell &input_bell, Doorbell &output_bell,
                     std::size_t wake_output_bytes)
    : input_(input),
      output_(output),
      input_bell_(input_bell),
      output_bell_(output_bell),
      wake_output_bytes_(std::min(wake_output_bytes, output.capacity() / 4)) {}

void WorkerEnd::progress(bool wake) {
  if (wake) {
    output_bell_.ring();
  } else {
    output_bell_.ring_quiet();
  }
  quiet_pending_ = !wake;
}

void WorkerEnd::receive(RecordHeader &header, std::vector<std::uint8_t> &payload) {
  payload.clear();
  std::vector<std::uint8_t> chunk;
  for (;;) {
    RecordHeader part;
    for (;;) {
      const std::uint32_t seen = input_bell_.snapshot();
      if (input_.try_pop(part, chunk)) {
        // TASK-084 step 1: a pop frees input space, which an EE blocked in send_chunks must hear. The EE waits for
        // "worker progress" on the output bell, so one bell covers output data and freed input space.
        // IN-459: only an EE that waits for input space needs to wake for it.
        progress(wake_output_bytes_ == 0 || input_.producer_waiting());
        break;
      }
      // IN-459: the worker goes idle. An EE asleep behind quiet publishes (it may wait for every job) wakes first.
      if (quiet_pending_) progress(true);
      bool slept = false;
      input_bell_.wait_changed(seen, slept);
    }
    payload.insert(payload.end(), chunk.begin(), chunk.end());
    if ((part.flags & kFlagMore) == 0) {
      header = part;
      header.flags = 0;
      header.bytes = static_cast<std::uint32_t>(payload.size());
      // IN-459: the worker stops after this record; the EE wakes for the quiet publishes before it.
      if (header.kind == static_cast<std::uint16_t>(RecordKind::Stop) && quiet_pending_) progress(true);
      return;
    }
  }
}

void WorkerEnd::emit_gif(std::uint8_t path, std::uint64_t job, const std::uint8_t *data, std::size_t bytes) {
  std::size_t offset = 0;
  do {
    const std::size_t chunk = std::min(bytes - offset, output_.max_payload());
    RecordHeader header;
    header.kind = static_cast<std::uint16_t>(RecordKind::Gif);
    header.path = path;
    header.job = job;
    header.bytes = static_cast<std::uint32_t>(chunk);
    header.flags = offset + chunk < bytes ? kFlagMore : 0;
    bool woke = false;
    for (;;) {
      const std::uint32_t seen = output_bell_.snapshot();
      const bool wake = wake_output_bytes_ == 0 || output_.used() + chunk >= wake_output_bytes_;
      if (output_.try_push(header, data + offset, wake)) {
        quiet_pending_ = !wake;
        break;
      }
      if (wake_output_bytes_ != 0 && !woke) {
        // IN-459: the ring is full; an EE asleep behind quiet publishes must wake and drain it. Then look again.
        progress(true);
        woke = true;
        continue;
      }
      bool slept = false;
      output_bell_.wait_changed(seen, slept);
    }
    offset += chunk;
  } while (offset < bytes);
}

void WorkerEnd::emit_job_done(std::uint64_t job) {
  RecordHeader header;
  header.kind = static_cast<std::uint16_t>(RecordKind::JobDone);
  header.job = job;
  bool woke = false;
  for (;;) {
    const std::uint32_t seen = output_bell_.snapshot();
    // IN-459: past the fill threshold only; an EE waiting for this job wakes when the worker goes idle (receive).
    const bool wake = wake_output_bytes_ == 0 || output_.used() >= wake_output_bytes_;
    if (output_.try_push(header, nullptr, wake)) {
      quiet_pending_ = !wake;
      return;
    }
    if (wake_output_bytes_ != 0 && !woke) {
      progress(true);
      woke = true;
      continue;
    }
    bool slept = false;
    output_bell_.wait_changed(seen, slept);
  }
}

EeJournal::EeJournal(SpscByteRing &input, SpscByteRing &output, Doorbell &input_bell, Doorbell &output_bell,
                     GsSink &sink)
    : input_(input), output_(output), input_bell_(input_bell), output_bell_(output_bell), sink_(sink) {}

std::size_t EeJournal::outstanding_jobs() const {
  return static_cast<std::size_t>(std::count_if(journal_.begin(), journal_.end(), [](const JournalEntry &entry) {
    return entry.kind == EntryKind::Placeholder;
  }));
}

void GsSink::direct_job(std::uint64_t) { throw std::logic_error("mtvu GS sink without direct intake"); }

void EeJournal::set_direct(bool direct) {
  if (jobs_posted_ != 0 && direct != direct_) throw std::logic_error("mtvu direct GS intake switched after jobs");
  direct_ = direct;
}

void EeJournal::post_job(RecordKind kind, std::uint64_t job, const std::uint8_t *payload, std::size_t bytes) {
  if (direct_) {
    // IN-461: the queued EE entries (never behind a placeholder here) go first, so the job keeps its stream position.
    execute_ready();
    sink_.direct_job(job);
  } else {
    JournalEntry placeholder;
    placeholder.kind = EntryKind::Placeholder;
    placeholder.job = job;
    journal_.push_back(std::move(placeholder));
  }

  RecordHeader header;
  header.kind = static_cast<std::uint16_t>(kind);
  header.job = job;
  send_chunks(input_, header, payload, bytes);
  ++jobs_posted_;
}

void EeJournal::append_ee_gif(std::uint8_t path, const std::uint8_t *data, std::size_t bytes) {
  JournalEntry entry;
  entry.kind = EntryKind::EeGif;
  entry.path = path;
  if (bytes != 0) entry.bytes.assign(data, data + bytes);
  journal_.push_back(std::move(entry));
}

void EeJournal::append_gif_reset() {
  JournalEntry entry;
  entry.kind = EntryKind::GifReset;
  journal_.push_back(std::move(entry));
}

void EeJournal::append_vertex_reset() {
  JournalEntry entry;
  entry.kind = EntryKind::VertexReset;
  journal_.push_back(std::move(entry));
}

void EeJournal::append_vblank_flush() {
  JournalEntry entry;
  entry.kind = EntryKind::VblankFlush;
  journal_.push_back(std::move(entry));
}

void EeJournal::append_privileged(std::uint32_t address, std::uint64_t value) {
  JournalEntry entry;
  entry.kind = EntryKind::Privileged;
  entry.address = address;
  entry.job = value;
  journal_.push_back(std::move(entry));
}

void EeJournal::append_readback(std::uint32_t qwords) {
  JournalEntry entry;
  entry.kind = EntryKind::Readback;
  entry.job = qwords;
  journal_.push_back(std::move(entry));
}

bool EeJournal::drain(bool blocking) {
  for (;;) {
    const std::uint32_t seen = output_bell_.snapshot();
    // TASK-084 step 2: the normal path executes whole worker records straight from the ring (no EE-side copy);
    // pump_output() stays only on the input-space wait path (send_chunks), where the ring must be freed.
    if (execute_ready()) return true;
    if (!blocking) return false;
    bool slept = false;
    output_bell_.wait_changed(seen, slept);
  }
}

void EeJournal::send_stop() {
  RecordHeader header;
  header.kind = static_cast<std::uint16_t>(RecordKind::Stop);
  send_chunks(input_, header, nullptr, 0);
}

void EeJournal::send_chunks(SpscByteRing &ring, RecordHeader header, const std::uint8_t *payload, std::size_t bytes) {
  std::size_t offset = 0;
  do {
    const std::size_t chunk = std::min(bytes - offset, ring.max_payload());
    header.bytes = static_cast<std::uint32_t>(chunk);
    header.flags = offset + chunk < bytes ? kFlagMore : 0;
    ring.set_producer_waiting(true);  // IN-459: a pop that frees space wakes this thread only while this is set
    for (;;) {
      // TASK-084 step 1: the producer waits for "worker progress" (output data or a freed input slot) on the
      // output bell, because a worker blocked on a full output ring only frees input space after the EE pumps
      // that ring. The input bell belongs to the worker alone. Both waits are event waits, never spins.
      const std::uint32_t seen = output_bell_.snapshot();
      if (ring.try_push(header, payload + offset)) break;
      if (!direct_) {  // IN-461: in direct intake the GS consumer drains the output ring
        pump_output();
        if (ring.try_push(header, payload + offset)) break;
      }
      bool slept = false;
      output_bell_.wait_changed(seen, slept);
    }
    ring.set_producer_waiting(false);
    offset += chunk;
  } while (offset < bytes);
}

void EeJournal::pump_output() {
  RecordHeader header;
  std::vector<std::uint8_t> chunk;
  while (output_.try_pop(header, chunk)) {
    if (header.kind == static_cast<std::uint16_t>(RecordKind::Gif)) {
      if (partial_gif_.empty() && (header.flags & kFlagMore) == 0) {
        // TASK-041: a whole record in one chunk (the usual case) moves into the queue without a second copy.
        CompletedEntry entry;
        entry.job_done = false;
        entry.path = header.path;
        entry.job = header.job;
        entry.bytes = std::move(chunk);
        chunk = {};
        completed_.push_back(std::move(entry));
        continue;
      }
      partial_gif_.insert(partial_gif_.end(), chunk.begin(), chunk.end());
      partial_path_ = header.path;
      partial_job_ = header.job;
      if ((header.flags & kFlagMore) == 0) {
        CompletedEntry entry;
        entry.job_done = false;
        entry.path = partial_path_;
        entry.job = partial_job_;
        entry.bytes = std::move(partial_gif_);
        partial_gif_.clear();
        completed_.push_back(std::move(entry));
      }
    } else if (header.kind == static_cast<std::uint16_t>(RecordKind::JobDone)) {
      if (!partial_gif_.empty()) throw std::logic_error("mtvu JobDone inside an unfinished GIF record");
      CompletedEntry entry;
      entry.job_done = true;
      entry.job = header.job;
      completed_.push_back(std::move(entry));
    } else {
    throw std::logic_error("mtvu worker sent an unexpected output record");
    }
  }
}

bool EeJournal::execute_ready() {
  while (!journal_.empty()) {
    JournalEntry &entry = journal_.front();
    switch (entry.kind) {
      case EntryKind::EeGif:
        sink_.gif_transfer(entry.path, entry.bytes.data(), entry.bytes.size());
        ++outputs_executed_;
        executed_bytes_ += entry.bytes.size();
        break;
      case EntryKind::GifReset:
        sink_.gif_reset();
        break;
      case EntryKind::VertexReset:
        sink_.vertex_reset();
        break;
      case EntryKind::VblankFlush:
        sink_.vblank_flush();
        break;
      case EntryKind::Privileged:
        sink_.privileged_write(entry.address, entry.job);
        break;
      case EntryKind::Readback:
        sink_.readback(static_cast<std::uint32_t>(entry.job));
        break;
      case EntryKind::Placeholder:
        if (!consume_job(entry.job)) return false;
        break;
    }
    journal_.pop_front();
  }
  return true;
}

bool EeJournal::consume_job(std::uint64_t job) {
  for (;;) {
    if (!completed_.empty()) {
      CompletedEntry &front = completed_.front();
      if (front.job != job) throw std::logic_error("mtvu output arrived out of job order");
      if (front.job_done) {
        completed_.pop_front();
        return true;
      }
      sink_.gif_transfer(front.path, front.bytes.data(), front.bytes.size());
      ++outputs_executed_;
      executed_bytes_ += front.bytes.size();
      completed_.pop_front();
      continue;
    }
    // TASK-084 step 2: a whole worker record (the usual case) is forwarded straight from the ring to the sink; the
    // payload is borrowed until release(), so the EE no longer copies every record into a std::vector. The record must
    // belong to this job and no earlier chunk of another record may be buffered.
    RecordHeader header;
    const std::uint8_t *payload = nullptr;
    if (!output_.try_peek(header, payload)) return false;
    if (header.job != job) {
      output_.release();
      throw std::logic_error("mtvu output arrived out of job order");
    }
    if (header.kind == static_cast<std::uint16_t>(RecordKind::Gif) && (header.flags & kFlagMore) == 0 &&
        partial_gif_.empty()) {
      sink_.gif_transfer(header.path, payload, header.bytes);
      ++outputs_executed_;
      executed_bytes_ += header.bytes;
      output_.release();
      continue;
    }
    // A chunked GIF record (only records above the ring's max_payload) or the JobDone marker: copy this chunk; the
    // reassembly keeps the previous buffered form.
    std::vector<std::uint8_t> chunk(payload, payload + header.bytes);
    const bool more = (header.flags & kFlagMore) != 0;
    const std::uint8_t path = header.path;
    const std::uint16_t kind = header.kind;
    output_.release();
    if (kind == static_cast<std::uint16_t>(RecordKind::Gif)) {
      if (partial_gif_.empty()) {
        partial_path_ = path;
        partial_job_ = job;
      }
      partial_gif_.insert(partial_gif_.end(), chunk.begin(), chunk.end());
      if (!more) {
        CompletedEntry entry;
        entry.job_done = false;
        entry.path = partial_path_;
        entry.job = partial_job_;
        entry.bytes = std::move(partial_gif_);
        partial_gif_.clear();
        completed_.push_back(std::move(entry));
      }
      continue;
    }
    if (kind == static_cast<std::uint16_t>(RecordKind::JobDone)) {
      if (!partial_gif_.empty()) throw std::logic_error("mtvu JobDone inside an unfinished GIF record");
      CompletedEntry entry;
      entry.job_done = true;
      entry.job = job;
      completed_.push_back(std::move(entry));
      continue;
    }
    throw std::logic_error("mtvu worker sent an unexpected output record");
  }
}

}  // namespace gt4::mtvu
