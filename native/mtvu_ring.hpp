// TASK-011 step 1: single-producer/single-consumer byte ring for the MTVU worker (docs/MTVU-PLAN.md section 5).
//
// Pure host code: no guest state, no Vulkan, no donor headers. Records are framed as a 16-byte header followed
// by the payload padded to 16 bytes. A record that does not fit before the end of the buffer is preceded by a
// Wrap filler that runs to the end, so every real record is contiguous. Positions are monotonic 64-bit counters
// (index = position % capacity), so no ABA problem exists.
//
// Ordering: the producer writes the record bytes, then publishes the head with a release store; the consumer
// reads the head with an acquire load before reading the bytes. The consumer frees space with a release store
// of the tail that the producer reads with acquire. Waiting uses a shared Doorbell (one counter, bumped with
// release after every publish, waited on with std::atomic::wait), so a waiter that snapshots the doorbell before
// checking its condition can never miss a wakeup.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace gt4::mtvu {

enum class RecordKind : std::uint16_t {
  Wrap = 0,       // filler to the end of the buffer (never delivered)
  VifPacket = 1,  // EE -> worker: raw VIF1 packet bytes for a job
  VifWrite = 2,   // EE -> worker: VIF1 register write for a job
  VuReset = 3,    // EE -> worker: VU1 reset for a job
  Fence = 4,      // EE -> worker: state sync point for a job
  Stop = 5,       // EE -> worker: finish and exit
  VifBatch = 6,   // TASK-084: EE -> worker: u32 count, then per packet a u32 qwc and qwc*16 bytes, one job
  Gif = 16,       // worker -> EE: GIF bytes for path `path`, in emission order
  JobDone = 17,   // worker -> EE: the last output of `job`
};

// Fixed 16-byte header; the layout is part of the on-ring format (checked by static_assert).
struct RecordHeader {
  std::uint16_t kind = 0;
  std::uint8_t path = 0;
  std::uint8_t flags = 0;   // kFlagMore: another chunk of the same logical record follows
  std::uint32_t bytes = 0;  // unpadded payload bytes of this chunk
  std::uint64_t job = 0;
};
static_assert(sizeof(RecordHeader) == 16, "ring record header must be 16 bytes");

constexpr std::uint8_t kFlagMore = 1;
constexpr std::size_t kRecordAlign = 16;
constexpr std::size_t kRecordHeaderBytes = sizeof(RecordHeader);

constexpr std::size_t align_record(std::size_t bytes) { return (bytes + kRecordAlign - 1) & ~(kRecordAlign - 1); }
constexpr std::size_t record_bytes(std::size_t payload) { return kRecordHeaderBytes + align_record(payload); }

// Shared wakeup counter. Every publish or release calls ring(); waiters snapshot first, then check, then wait.
// TASK-084 step 1 (slice261): one doorbell per direction and a sleeper count, so a ring only wakes the peer when the
// peer is actually asleep; the caller can then wait for "my job N is done" on the other direction's bell. The wakeup
// handshake is the standard count-flag / re-check / wait sequence, so a ring between the caller's check and the wait
// cannot be lost. The count (not a flag) matters because two callers can legitimately share one bell: the EE waits for
// worker progress while the worker can wait in emit_job_done for output space.
class Doorbell {
 public:
  struct Stats {
    std::uint64_t rings = 0;    // ring() calls (every publish or release)
    std::uint64_t waits = 0;    // wait_changed() calls
    std::uint64_t wakeups = 0;  // wait_changed() calls that actually slept
    std::uint64_t notifies = 0;  // ring() calls that woke a sleeper (IN-459)
  };

  std::uint32_t snapshot() const { return value_.load(std::memory_order_acquire); }

  // TASK-092: ring() (store value, then load sleepers_) and wait_changed() (store sleepers_, then load value) form a
  // store-then-load (Dekker) pattern. Only sequential consistency rules out a lost wakeup on weakly ordered CPUs
  // (ARM, Android); x86 locked RMWs already order it. Keep every operation of the handshake seq_cst.
  void ring() {
    rings_.fetch_add(1, std::memory_order_relaxed);
    value_.fetch_add(1, std::memory_order_seq_cst);
    // Only wake a peer that is asleep; while it runs, the next snapshot/re-check sees the new value anyway.
    if (sleepers_.load(std::memory_order_seq_cst) > 0) {
      notifies_.fetch_add(1, std::memory_order_relaxed);
      value_.notify_all();
    }
  }

  // IN-459: advances the counter without waking a sleeper. A peer that is awake sees the change at its next snapshot or
  // re-check; one that sleeps stays asleep until a ring() it needs (the caller decides which events those are).
  void ring_quiet() {
    rings_.fetch_add(1, std::memory_order_relaxed);
    value_.fetch_add(1, std::memory_order_seq_cst);
  }

  // Returns once the counter differs from `seen`, or at once when it already changed. `slept` reports whether the
  // call really blocked. Callers re-check their condition in a loop.
  bool wait_changed(std::uint32_t seen, bool &slept) {
    waits_.fetch_add(1, std::memory_order_relaxed);
    sleepers_.fetch_add(1, std::memory_order_seq_cst);
    if (value_.load(std::memory_order_seq_cst) != seen) {
      sleepers_.fetch_sub(1, std::memory_order_acq_rel);
      slept = false;
      return false;
    }
    value_.wait(seen, std::memory_order_acquire);
    sleepers_.fetch_sub(1, std::memory_order_acq_rel);
    wakeups_.fetch_add(1, std::memory_order_relaxed);
    slept = true;
    return true;
  }

  Stats stats() const {
    return Stats{rings_.load(std::memory_order_relaxed), waits_.load(std::memory_order_relaxed),
                 wakeups_.load(std::memory_order_relaxed), notifies_.load(std::memory_order_relaxed)};
  }

 private:
  std::atomic<std::uint32_t> value_{0};
  std::atomic<int> sleepers_{0};  // callers currently inside wait_changed (a count: two callers may share one bell)
  std::atomic<std::uint64_t> rings_{0}, waits_{0}, wakeups_{0}, notifies_{0};
};

class SpscByteRing {
 public:
  // capacity_bytes must be a multiple of 32 and at least 128. The largest record is capacity/2 bytes, which
  // guarantees that a wrap filler plus the record always fit into an empty buffer.
  SpscByteRing(std::size_t capacity_bytes, Doorbell &bell) : capacity_(capacity_bytes), bell_(bell), storage_(capacity_bytes) {
    if (capacity_bytes < 128 || capacity_bytes % 32 != 0) {
      throw std::invalid_argument("mtvu ring capacity must be a multiple of 32 and at least 128 bytes");
    }
  }

  SpscByteRing(const SpscByteRing &) = delete;
  SpscByteRing &operator=(const SpscByteRing &) = delete;

  std::size_t capacity() const { return capacity_; }

  // Largest payload of one record (chunks above this are split by the callers).
  std::size_t max_payload() const { return capacity_ / 2 - kRecordHeaderBytes; }

  // Bytes currently held (published but not yet released). Safe from either side as an estimate.
  std::size_t used() const {
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(head - tail);
  }

  bool empty() const { return used() == 0; }

  // IN-459: the producer sets this while it waits for space, so the consumer knows a freed slot must wake it. Set it
  // before the snapshot of the bell it waits on (seq_cst on both sides: a consumer that reads false published its
  // release before the producer's snapshot, so the producer sees the space or the changed bell).
  void set_producer_waiting(bool waiting) { producer_waiting_.store(waiting, std::memory_order_seq_cst); }
  bool producer_waiting() const { return producer_waiting_.load(std::memory_order_seq_cst); }

  // Producer only. Returns false when the record does not fit right now. Throws when the payload can never fit.
  // `wake` false publishes with ring_quiet() (IN-459: the producer decides when a sleeping consumer must wake).
  bool try_push(RecordHeader header, const std::uint8_t *payload, bool wake = true) {
    if (header.bytes > max_payload()) throw std::logic_error("mtvu ring record larger than max_payload");
    const std::size_t total = record_bytes(header.bytes);
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    const std::size_t offset = static_cast<std::size_t>(head % capacity_);
    const std::size_t filler = offset + total > capacity_ ? capacity_ - offset : 0;
    if (capacity_ - static_cast<std::size_t>(head - tail) < filler + total) return false;

    std::uint64_t position = head;
    std::size_t write_offset = offset;
    if (filler != 0) {
      RecordHeader wrap;
      wrap.kind = static_cast<std::uint16_t>(RecordKind::Wrap);
      wrap.bytes = static_cast<std::uint32_t>(filler - kRecordHeaderBytes);
      std::memcpy(storage_.data() + offset, &wrap, kRecordHeaderBytes);
      position += filler;
      write_offset = 0;
    }
    std::memcpy(storage_.data() + write_offset, &header, kRecordHeaderBytes);
    if (header.bytes != 0) std::memcpy(storage_.data() + write_offset + kRecordHeaderBytes, payload, header.bytes);
    head_.store(position + total, std::memory_order_release);
    if (wake) {
      bell_.ring();
    } else {
      bell_.ring_quiet();
    }
    return true;
  }

  // Consumer only. Skips wrap fillers. Returns false when the ring is empty.
  bool try_pop(RecordHeader &header, std::vector<std::uint8_t> &payload) {
    std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    bool released = false;
    while (tail != head) {
      const std::size_t offset = static_cast<std::size_t>(tail % capacity_);
      std::memcpy(&header, storage_.data() + offset, kRecordHeaderBytes);
      const std::size_t total = record_bytes(header.bytes);
      if (header.kind == static_cast<std::uint16_t>(RecordKind::Wrap)) {
        tail += total;
        tail_.store(tail, std::memory_order_release);
        released = true;
        continue;
      }
      payload.assign(storage_.data() + offset + kRecordHeaderBytes, storage_.data() + offset + kRecordHeaderBytes + header.bytes);
      tail += total;
      tail_.store(tail, std::memory_order_release);
      bell_.ring();
      return true;
    }
    if (released) bell_.ring();
    return false;
  }

  // Consumer only, TASK-084 step 2: borrows the next record without copying its payload. The bytes stay valid until
  // release(); the consumer holds the tail, so no other record moves. Skips wrap fillers. Returns false when empty.
  bool try_peek(RecordHeader &header, const std::uint8_t *&payload) {
    for (;;) {
      const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
      const std::uint64_t head = head_.load(std::memory_order_acquire);
      if (tail == head) return false;
      const std::size_t offset = static_cast<std::size_t>(tail % capacity_);
      std::memcpy(&header, storage_.data() + offset, kRecordHeaderBytes);
      const std::size_t total = record_bytes(header.bytes);
      if (header.kind == static_cast<std::uint16_t>(RecordKind::Wrap)) {
        tail_.store(tail + total, std::memory_order_release);
        bell_.ring();
        continue;
      }
      payload = storage_.data() + offset + kRecordHeaderBytes;
      peeked_bytes_ = total;
      return true;
    }
  }

  // Consumer only: releases the record returned by the last successful try_peek.
  void release() {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    tail_.store(tail + peeked_bytes_, std::memory_order_release);
    peeked_bytes_ = 0;
    bell_.ring();
  }

 private:
  std::size_t capacity_;
  Doorbell &bell_;
  std::vector<std::uint8_t> storage_;
  alignas(64) std::atomic<std::uint64_t> head_{0};
  alignas(64) std::atomic<std::uint64_t> tail_{0};
  std::atomic<bool> producer_waiting_{false};
  std::size_t peeked_bytes_ = 0;  // consumer only: bytes of the record lent by try_peek
};

}  // namespace gt4::mtvu
