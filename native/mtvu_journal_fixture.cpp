// CTest native_mtvu_journal: host-only controls for the TASK-011 step 1 ordered GS journal (mtvu_journal.hpp).
// Random scripts run on the EE side against a real worker thread with random delays; the GS sink log must equal
// the serial program order. Also: tiny-ring deadlock, blocking drain, fast path, shutdown, and worker errors.
// TASK-041: privileged writes queued in the journal keep their place between the GIF outputs.
#include "mtvu_journal.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace gt4::mtvu;

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

struct Event {
  int kind = 0;  // 0 gif, 1 gif reset, 2 vertex reset, 3 vblank flush, 4 privileged write (bytes: address, value)
  std::uint8_t path = 0;
  std::vector<std::uint8_t> bytes;
  bool operator==(const Event &other) const { return kind == other.kind && path == other.path && bytes == other.bytes; }
};

class RecordingSink final : public GsSink {
 public:
  void gif_transfer(std::uint8_t path, const std::uint8_t *data, std::size_t bytes) override {
    Event event;
    event.kind = 0;
    event.path = path;
    event.bytes.assign(data, data + bytes);
    events.push_back(std::move(event));
  }
  void gif_reset() override { events.push_back(Event{1, 0, {}}); }
  void vertex_reset() override { events.push_back(Event{2, 0, {}}); }
  void vblank_flush() override { events.push_back(Event{3, 0, {}}); }
  void privileged_write(std::uint32_t address, std::uint64_t value) override {
    events.push_back(privileged_event(address, value));
  }
  void readback(std::uint32_t qwords) override {
    events.push_back(readback_event(qwords));
  }
  static Event privileged_event(std::uint32_t address, std::uint64_t value) {
    Event event;
    event.kind = 4;
    for (int i = 0; i < 4; ++i) event.bytes.push_back(static_cast<std::uint8_t>(address >> (8 * i)));
    for (int i = 0; i < 8; ++i) event.bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    return event;
  }
  static Event readback_event(std::uint32_t qwords) {
    Event event;
    event.kind = 5;
    for (int i = 0; i < 4; ++i) event.bytes.push_back(static_cast<std::uint8_t>(qwords >> (8 * i)));
    return event;
  }
  std::vector<Event> events;
};

// IN-461 (direct GS intake): a GS consumer that reads the worker's output ring itself, as the hw GS worker does. The EE
// side queues every GS operation and every job marker in stream order; the consumer thread executes them in that order
// into a RecordingSink and takes each job's records from the output ring (reassembling chunked records) up to its
// JobDone, sleeping on the output bell like the GS worker.
class DirectConsumer final : public GsSink {
 public:
  DirectConsumer(SpscByteRing &output, Doorbell &output_bell) : output_(output), output_bell_(output_bell) {
    thread_ = std::thread([this] { run(); });
  }
  ~DirectConsumer() { finish(); }
  void gif_transfer(std::uint8_t path, const std::uint8_t *data, std::size_t bytes) override {
    Item item;
    item.kind = Item::Gif;
    item.path = path;
    item.bytes.assign(data, data + bytes);
    push(std::move(item));
  }
  void gif_reset() override { push(Item{Item::GifReset}); }
  void vertex_reset() override { push(Item{Item::VertexReset}); }
  void vblank_flush() override { push(Item{Item::Vblank}); }
  void privileged_write(std::uint32_t address, std::uint64_t value) override {
    Item item;
    item.kind = Item::Privileged;
    item.address = address;
    item.value = value;
    push(std::move(item));
  }
  void readback(std::uint32_t qwords) override {
    Item item;
    item.kind = Item::Readback;
    item.value = qwords;
    push(std::move(item));
  }
  void direct_job(std::uint64_t job) override {
    Item item;
    item.kind = Item::Job;
    item.value = job;
    push(std::move(item));
  }
  // Ends the consumer after everything queued and returns what it executed (rethrows its failure).
  const std::vector<Event> &finish() {
    if (thread_.joinable()) {
      push(Item{Item::Stop});
      thread_.join();
    }
    if (failure_) std::rethrow_exception(failure_);
    return executed_.events;
  }

 private:
  struct Item {
    enum Kind { Gif, GifReset, VertexReset, Vblank, Privileged, Readback, Job, Stop } kind = Gif;
    std::uint8_t path = 0;
    std::uint32_t address = 0;
    std::uint64_t value = 0;
    std::vector<std::uint8_t> bytes;
  };

  void push(Item item) {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.push_back(std::move(item));
    ready_.notify_one();
  }

  void run() {
    try {
      for (;;) {
        Item item;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          ready_.wait(lock, [this] { return !items_.empty(); });
          item = std::move(items_.front());
          items_.pop_front();
        }
        switch (item.kind) {
          case Item::Gif: executed_.gif_transfer(item.path, item.bytes.data(), item.bytes.size()); break;
          case Item::GifReset: executed_.gif_reset(); break;
          case Item::VertexReset: executed_.vertex_reset(); break;
          case Item::Vblank: executed_.vblank_flush(); break;
          case Item::Privileged: executed_.privileged_write(item.address, item.value); break;
          case Item::Readback: executed_.readback(static_cast<std::uint32_t>(item.value)); break;
          case Item::Job: execute_job(item.value); break;
          case Item::Stop: return;
        }
      }
    } catch (...) {
      failure_ = std::current_exception();
    }
  }

  void execute_job(std::uint64_t job) {
    std::vector<std::uint8_t> record;
    std::uint8_t path = 0;
    for (;;) {
      RecordHeader header;
      const std::uint8_t *payload = nullptr;
      if (!output_.try_peek(header, payload)) {
        const std::uint32_t seen = output_bell_.snapshot();
        if (!output_.try_peek(header, payload)) {
          bool slept = false;
          output_bell_.wait_changed(seen, slept);
          continue;
        }
      }
      require(header.job == job, "direct consumer: output out of job order");
      if (header.kind == static_cast<std::uint16_t>(RecordKind::JobDone)) {
        require(record.empty(), "direct consumer: JobDone inside a chunked record");
        output_.release();
        return;
      }
      require(header.kind == static_cast<std::uint16_t>(RecordKind::Gif), "direct consumer: unexpected record");
      record.insert(record.end(), payload, payload + header.bytes);
      path = header.path;
      const bool more = (header.flags & kFlagMore) != 0;
      output_.release();
      if (!more) {
        executed_.gif_transfer(path, record.data(), record.size());
        record.clear();
      }
    }
  }

  SpscByteRing &output_;
  Doorbell &output_bell_;
  RecordingSink executed_;  // consumer thread only (read by finish after the join)
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Item> items_;
  std::exception_ptr failure_;
  std::thread thread_;
};

struct Output {
  std::uint8_t path = 0;
  std::vector<std::uint8_t> bytes;
};

// The worker's deterministic function of a job: the same function is used for the serial model.
std::vector<Output> outputs_for_job(std::uint64_t job, const std::vector<std::uint8_t> &payload) {
  std::vector<Output> outputs;
  const std::size_t count = static_cast<std::size_t>((job + payload.size()) % 4);
  for (std::size_t k = 0; k < count; ++k) {
    Output out;
    out.path = static_cast<std::uint8_t>(1 + (job + k) % 2);
    const std::size_t size = 16 * ((job + k) % 25 + 1);  // up to 400 bytes: chunked on the tiny rings
    out.bytes.resize(size);
    for (std::size_t i = 0; i < size; ++i) {
      const std::uint8_t seed = payload.empty() ? 0 : payload[i % payload.size()];
      out.bytes[i] = static_cast<std::uint8_t>(seed ^ (job * 17u + k * 5u + i));
    }
    outputs.push_back(std::move(out));
  }
  return outputs;
}

struct WorkerRun {
  // Worker thread body. Exceptions are captured and rethrown on the EE thread at join (section 5.6).
  static void run(SpscByteRing &input, SpscByteRing &output, Doorbell &input_bell, Doorbell &output_bell,
                  std::uint64_t seed, std::size_t wake_bytes,
                  std::exception_ptr &failure) {
    try {
      WorkerEnd end(input, output, input_bell, output_bell, wake_bytes);
      std::mt19937_64 rng(seed);
      RecordHeader header;
      std::vector<std::uint8_t> payload;
      for (;;) {
        end.receive(header, payload);
        if (header.kind == static_cast<std::uint16_t>(RecordKind::Stop)) return;
        if (header.kind != static_cast<std::uint16_t>(RecordKind::VifPacket)) {
          throw std::logic_error("fixture worker received an unexpected record");
        }
        if (rng() % 6 == 0) std::this_thread::sleep_for(std::chrono::microseconds(rng() % 60));
        for (const Output &out : outputs_for_job(header.job, payload)) {
          end.emit_gif(out.path, header.job, out.bytes.data(), out.bytes.size());
        }
        end.emit_job_done(header.job);
      }
    } catch (...) {
      failure = std::current_exception();
    }
  }
};

// One random script. Returns the number of operations, and checks the GS log against the serial model.
void run_script(std::uint64_t seed, std::size_t input_bytes, std::size_t output_bytes, int operations,
                std::uint64_t &jobs_out, std::uint64_t &fast_path_out, std::size_t wake_bytes = 0, bool direct = false) {
  Doorbell input_bell, output_bell;
  SpscByteRing input(input_bytes, input_bell);
  SpscByteRing output(output_bytes, output_bell);
  RecordingSink recording;
  std::unique_ptr<DirectConsumer> consumer;
  if (direct) consumer = std::make_unique<DirectConsumer>(output, output_bell);  // IN-461
  GsSink &sink = direct ? static_cast<GsSink &>(*consumer) : static_cast<GsSink &>(recording);
  EeJournal journal(input, output, input_bell, output_bell, sink);
  journal.set_direct(direct);

  std::exception_ptr failure;
  std::thread worker(WorkerRun::run, std::ref(input), std::ref(output), std::ref(input_bell), std::ref(output_bell), seed * 7919u + 1u,
                     wake_bytes, std::ref(failure));
  // MTVU_FIXTURE_TRACE=1 (diagnostics of a hang): every 2 s of a long script, the step (step * 100 + operation), the
  // ring fill and the bell counters, so a stuck run shows which side sleeps.
  std::atomic<bool> done{false};
  std::atomic<int> op{0};
  std::thread monitor;
  if (std::getenv("MTVU_FIXTURE_TRACE") && operations > 1000) {
    monitor = std::thread([&] {
      using Count = unsigned long long;
      while (!done.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const Doorbell::Stats in = input_bell.stats(), out = output_bell.stats();
        std::fprintf(stderr,
                     "op %d input used %zu output used %zu | in rings %llu waits %llu wakeups %llu | out rings %llu "
                     "waits %llu wakeups %llu notifies %llu\n",
                     op.load(), input.used(), output.used(), Count{in.rings}, Count{in.waits}, Count{in.wakeups},
                     Count{out.rings}, Count{out.waits}, Count{out.wakeups}, Count{out.notifies});
      }
    });
  }

  std::mt19937_64 rng(seed);
  std::vector<Event> expected;
  std::uint64_t next_job = 1;
  std::uint64_t fast_path = 0;
  for (int step = 0; step < operations; ++step) {
    const unsigned choice = static_cast<unsigned>(rng() % 100);
    op.store(step * 100 + static_cast<int>(choice));
    if (choice < 35) {
      const std::size_t bytes = rng() % 300;
      std::vector<std::uint8_t> payload(bytes);
      for (std::size_t i = 0; i < bytes; ++i) payload[i] = static_cast<std::uint8_t>(rng());
      const std::uint64_t job = next_job++;
      journal.post_job(RecordKind::VifPacket, job, payload.data(), payload.size());
      for (const Output &out : outputs_for_job(job, payload)) {
        expected.push_back(Event{0, out.path, out.bytes});
      }
    } else if (choice < 60) {
      const std::size_t bytes = rng() % 200;
      std::vector<std::uint8_t> data(bytes);
      for (std::size_t i = 0; i < bytes; ++i) data[i] = static_cast<std::uint8_t>(rng());
      expected.push_back(Event{0, 3, data});
      if (journal.can_execute_now()) {
        // Fast path: nothing is queued, so the EE executes the operation directly without a journal copy.
        sink.gif_transfer(3, data.data(), data.size());
        ++fast_path;
      } else {
        journal.append_ee_gif(3, data.data(), data.size());
      }
    } else if (choice < 65) {
      expected.push_back(Event{1, 0, {}});
      if (journal.can_execute_now()) {
        sink.gif_reset();
        ++fast_path;
      } else {
        journal.append_gif_reset();
      }
    } else if (choice < 70) {
      expected.push_back(Event{2, 0, {}});
      if (journal.can_execute_now()) {
        sink.vertex_reset();
        ++fast_path;
      } else {
        journal.append_vertex_reset();
      }
    } else if (choice < 75) {
      expected.push_back(Event{3, 0, {}});
      if (journal.can_execute_now()) {
        sink.vblank_flush();
        ++fast_path;
      } else {
        journal.append_vblank_flush();
      }
    } else if (choice < 80) {
      // TASK-041: a privileged GS write goes behind the queued entries, or straight to the sink when none is queued.
      const std::uint32_t address = (0x12000000u + static_cast<std::uint32_t>(rng() % 0x2000u)) & ~7u;
      const std::uint64_t value = rng();
      expected.push_back(RecordingSink::privileged_event(address, value));
      if (journal.can_execute_now()) {
        sink.privileged_write(address, value);
        ++fast_path;
      } else {
        journal.append_privileged(address, value);
      }
    } else if (choice < 85) {
      // TASK-116 step 1: a READ_FIFO queued behind the journal reaches the sink at its program position, in order.
      const std::uint32_t qwords = static_cast<std::uint32_t>(1 + rng() % 512);
      expected.push_back(RecordingSink::readback_event(qwords));
      journal.append_readback(qwords);
    } else if (choice < 95) {
      journal.drain(false);
    } else {
      require(journal.drain(true) && journal.can_execute_now(), "blocking drain empties the journal");
    }
  }
  op.store(-1);
  require(journal.drain(true), "final blocking drain");
  done.store(true);
  if (monitor.joinable()) monitor.join();
  require(journal.can_execute_now() && journal.outstanding_jobs() == 0, "journal idle at the end");
  journal.send_stop();
  worker.join();
  if (failure) std::rethrow_exception(failure);
  const std::vector<Event> &events = direct ? consumer->finish() : recording.events;
  require(events.size() == expected.size(), "GS event count matches the serial model");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (!(events[i] == expected[i])) {
      std::fprintf(stderr, "script seed %llu: first mismatch at event %zu\n", static_cast<unsigned long long>(seed), i);
      throw std::runtime_error("GS order differs from the serial program order");
    }
  }
  require(journal.jobs_posted() == next_job - 1, "jobs posted match");
  jobs_out += journal.jobs_posted();
  fast_path_out += fast_path;
}

void check_random_scripts() {
  std::uint64_t jobs = 0, fast = 0;
  // 2,000 scripts of 60 operations on mixed ring sizes, then one long script on the default-sized rings.
  for (std::uint64_t seed = 1; seed <= 2000; ++seed) {
    const std::size_t input = seed % 3 == 0 ? 128 : (seed % 3 == 1 ? 256 : 4096);
    const std::size_t output = seed % 2 == 0 ? 128 : 512;
    // IN-459: half of them with quiet worker publishes (wake past 96 bytes, a quarter of the ring at most).
    if (std::getenv("MTVU_FIXTURE_TRACE")) std::fprintf(stderr, "script %llu\n", static_cast<unsigned long long>(seed));
    run_script(seed, input, output, 60, jobs, fast, seed % 4 < 2 ? 0 : 96);
  }
  if (std::getenv("MTVU_FIXTURE_TRACE")) std::fprintf(stderr, "long scripts\n");
  run_script(90001, 32 * 1024 * 1024 / 512, 8 * 1024 * 1024 / 512, 100000, jobs, fast);
  if (std::getenv("MTVU_FIXTURE_TRACE")) std::fprintf(stderr, "long script with quiet wakes\n");
  run_script(90002, 32 * 1024 * 1024 / 512, 8 * 1024 * 1024 / 512, 100000, jobs, fast, 64 * 1024);
  // IN-461: 1,000 scripts and a long one with direct GS intake (a consumer thread reads the output ring).
  if (std::getenv("MTVU_FIXTURE_TRACE")) std::fprintf(stderr, "direct scripts\n");
  for (std::uint64_t seed = 3001; seed <= 4000; ++seed) {
    const std::size_t input = seed % 3 == 0 ? 128 : (seed % 3 == 1 ? 256 : 4096);
    const std::size_t output = seed % 2 == 0 ? 128 : 512;
    run_script(seed, input, output, 60, jobs, fast, seed % 4 < 2 ? 0 : 96, true);
  }
  run_script(90003, 32 * 1024 * 1024 / 512, 8 * 1024 * 1024 / 512, 100000, jobs, fast, 64 * 1024, true);
  require(jobs > 20000 && fast > 0, "scripts posted jobs and used the fast path");
}

void check_tiny_ring_deadlock(std::size_t wake_bytes, bool direct = false) {
  // 500 jobs with 200-byte inputs and up to 400-byte outputs through 128-byte rings: both sides must keep
  // draining each other or this never finishes (CTest TIMEOUT catches a hang). IN-461: with direct intake the EE never
  // drains the output ring; the consumer thread does.
  Doorbell input_bell, output_bell;
  SpscByteRing input(128, input_bell);
  SpscByteRing output(128, output_bell);
  RecordingSink recording;
  std::unique_ptr<DirectConsumer> consumer;
  if (direct) consumer = std::make_unique<DirectConsumer>(output, output_bell);
  GsSink &sink = direct ? static_cast<GsSink &>(*consumer) : static_cast<GsSink &>(recording);
  EeJournal journal(input, output, input_bell, output_bell, sink);
  journal.set_direct(direct);
  std::exception_ptr failure;
  std::thread worker(WorkerRun::run, std::ref(input), std::ref(output), std::ref(input_bell), std::ref(output_bell), std::uint64_t{99},
                     wake_bytes, std::ref(failure));
  std::vector<Event> expected;
  std::mt19937_64 rng(5);
  for (std::uint64_t job = 1; job <= 500; ++job) {
    std::vector<std::uint8_t> payload(200);
    for (auto &byte : payload) byte = static_cast<std::uint8_t>(rng());
    journal.post_job(RecordKind::VifPacket, job, payload.data(), payload.size());
    for (const Output &out : outputs_for_job(job, payload)) expected.push_back(Event{0, out.path, out.bytes});
  }
  require(journal.drain(true), "tiny ring drain");
  journal.send_stop();
  worker.join();
  if (failure) std::rethrow_exception(failure);
  const std::vector<Event> &events = direct ? consumer->finish() : recording.events;
  require(events.size() == expected.size(), "tiny ring event count");
  for (std::size_t i = 0; i < expected.size(); ++i) require(events[i] == expected[i], "tiny ring order");
}

// IN-522: a worker about to wait for something other than input wakes a reader asleep behind its quiet publishes. Without the
// wake this never finishes (CTest TIMEOUT): the reader may be what that wait depends on.
void check_wake_before_waiting() {
  Doorbell input_bell, output_bell;
  SpscByteRing input(4096, input_bell);
  SpscByteRing output(4096, output_bell);
  WorkerEnd worker(input, output, input_bell, output_bell, 1024);
  std::thread reader([&] {
    RecordHeader record;
    const std::uint8_t *bytes = nullptr;
    for (;;) {
      const std::uint32_t seen = output_bell.snapshot();
      if (output.try_peek(record, bytes)) return;
      bool slept = false;
      output_bell.wait_changed(seen, slept);
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const std::uint8_t data[16] = {};
  worker.emit_gif(0, 1, data, sizeof data);
  worker.wake_before_waiting();
  reader.join();
}

void check_blocking_drain_waits_for_worker() {
  Doorbell input_bell, output_bell;
  SpscByteRing input(4096, input_bell);
  SpscByteRing output(4096, output_bell);
  RecordingSink sink;
  EeJournal journal(input, output, input_bell, output_bell, sink);
  std::exception_ptr failure;
  std::thread worker(WorkerRun::run, std::ref(input), std::ref(output), std::ref(input_bell), std::ref(output_bell), std::uint64_t{7},
                     std::size_t{0}, std::ref(failure));
  std::vector<std::uint8_t> payload(64, 0x5A);
  journal.post_job(RecordKind::VifPacket, 1, payload.data(), payload.size());
  journal.append_vblank_flush();
  // The job may or may not be finished yet; a non-blocking drain must never run the flush before the job.
  journal.drain(false);
  require(sink.events.size() <= outputs_for_job(1, payload).size(), "flush ran before its job's outputs");
  require(journal.drain(true), "blocking drain completes");
  require(sink.events.size() == outputs_for_job(1, payload).size() + 1 && sink.events.back().kind == 3,
          "blocking drain executes outputs then the flush");
  journal.send_stop();
  worker.join();
  if (failure) std::rethrow_exception(failure);
}

void check_shutdown_after_jobs() {
  Doorbell input_bell, output_bell;
  SpscByteRing input(256, input_bell);
  SpscByteRing output(256, output_bell);
  RecordingSink sink;
  EeJournal journal(input, output, input_bell, output_bell, sink);
  std::exception_ptr failure;
  std::thread worker(WorkerRun::run, std::ref(input), std::ref(output), std::ref(input_bell), std::ref(output_bell), std::uint64_t{3},
                     std::size_t{64}, std::ref(failure));
  std::vector<Event> expected;
  for (std::uint64_t job = 1; job <= 10; ++job) {
    std::vector<std::uint8_t> payload(job * 9, static_cast<std::uint8_t>(job));
    journal.post_job(RecordKind::VifPacket, job, payload.data(), payload.size());
    for (const Output &out : outputs_for_job(job, payload)) expected.push_back(Event{0, out.path, out.bytes});
  }
  // Shutdown without an explicit drain: Stop follows the jobs, so the worker finishes them first.
  journal.send_stop();
  require(journal.drain(true), "shutdown drain");
  worker.join();
  if (failure) std::rethrow_exception(failure);
  require(sink.events.size() == expected.size(), "shutdown delivers every job output");
  for (std::size_t i = 0; i < expected.size(); ++i) require(sink.events[i] == expected[i], "shutdown order");
  require(input.empty() && output.empty(), "rings empty after shutdown");
}

void check_worker_protocol_error_is_reported() {
  // A worker that sends an unexpected record kind must be caught on the EE side, not silently accepted.
  Doorbell input_bell, output_bell;
  SpscByteRing input(256, input_bell);
  SpscByteRing output(256, output_bell);
  RecordingSink sink;
  EeJournal journal(input, output, input_bell, output_bell, sink);
  RecordHeader bogus;
  bogus.kind = static_cast<std::uint16_t>(RecordKind::VifWrite);
  WorkerEnd end(input, output, input_bell, output_bell);
  std::vector<std::uint8_t> payload(8, 1);
  journal.post_job(RecordKind::VifPacket, 1, payload.data(), payload.size());
  // Write a wrong-kind output record directly into the output ring, as a broken worker would.
  require(output.try_push(bogus, nullptr), "bogus output record pushed");
  bool caught = false;
  try {
    journal.drain(false);
  } catch (const std::logic_error &) {
    caught = true;
  }
  require(caught, "unexpected worker record is an error");
  // Consume the job input so the fixture does not leave a record behind.
  RecordHeader header;
  std::vector<std::uint8_t> received;
  end.receive(header, received);
  require(header.job == 1, "input job delivered");
}

}  // namespace

int main() {
  try {
    const bool trace = std::getenv("MTVU_FIXTURE_TRACE") != nullptr;
    check_random_scripts();
    if (trace) std::fprintf(stderr, "tiny ring, loud\n");
    check_tiny_ring_deadlock(0);
    if (trace) std::fprintf(stderr, "tiny ring, quiet\n");
    check_tiny_ring_deadlock(64);  // IN-459: quiet publishes; the full ring must still wake the EE
    if (trace) std::fprintf(stderr, "tiny ring, direct\n");
    check_tiny_ring_deadlock(0, true);   // IN-461: direct GS intake, both wake modes
    check_tiny_ring_deadlock(64, true);
    check_wake_before_waiting();
    if (trace) std::fprintf(stderr, "blocking drain\n");
    check_blocking_drain_waits_for_worker();
    if (trace) std::fprintf(stderr, "shutdown\n");
    check_shutdown_after_jobs();
    if (trace) std::fprintf(stderr, "protocol error\n");
    check_worker_protocol_error_is_reported();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "native_mtvu_journal failed: %s\n", error.what());
    return 1;
  }
  std::printf("native_mtvu_journal ok: 2000 random scripts + three 100000-step scripts vs serial order (half with quiet worker wakes; 1000 scripts and one long one with direct GS intake), tiny-ring deadlock in both wake modes and with direct intake, blocking drain, shutdown, protocol error\n");
  return 0;
}
