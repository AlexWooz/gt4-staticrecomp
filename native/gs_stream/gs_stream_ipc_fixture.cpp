// SPDX-License-Identifier: MIT
// CTest native_gs_stream_ipc: controls for the GS stream shared-memory transport (gs_stream_ipc.hpp).
//
// Same process, two threads: record layout, FIFO order and byte identity of 10^6 random records on a small ring
// (wrap fillers and back-pressure on every lap), split records (kRecordFlagMore), readback round trips, the
// frames-in-flight limit, spurious wake-ups, and the fatal flag.
// Child process (this executable started again with --child): handshake (READY), ordering and readbacks across
// processes, a handshake timeout against a silent child, the producer seeing the consumer die (PeerExited) and the
// consumer seeing its peer die.
#include "gs_stream_ipc.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace gs_stream;
using namespace gs_stream::ipc;

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

// Deterministic payload byte, so the consumer recomputes what the producer wrote.
std::uint8_t pattern(std::uint64_t record, std::uint64_t index) {
  return static_cast<std::uint8_t>(record * 131u + index * 7u + 3u);
}

std::vector<std::uint8_t> make_payload(std::size_t bytes, std::uint64_t record) {
  std::vector<std::uint8_t> data(bytes);
  for (std::size_t i = 0; i < bytes; ++i) data[i] = pattern(record, i);
  return data;
}

SessionOptions small_options(std::uint64_t ring_bytes) {
  SessionOptions options;
  options.prefix = "gt4gs-test";
  options.command_capacity = ring_bytes;
  options.event_capacity = 4096;
  options.response_capacity = 4096;
  options.frames_in_flight = 2;
  options.serial = "TEST-00001";
  return options;
}

void check_layout() {
  require(sizeof(RecordHeader) == 16 && sizeof(ControlBlock) == 4096 && sizeof(FileHeader) == 64, "struct sizes");
  require(record_size(0) == 16 && record_size(1) == 32 && record_size(16) == 32 && record_size(17) == 48, "padding");
  auto session = Session::create_producer(small_options(8192));
  const ControlBlock &c = session->control();
  require(c.magic == kMagic && c.version_major == kVersionMajor && c.state == kStateProducerReady, "control block");
  require(c.command_offset == 4096 && c.command_capacity == 8192 && c.response_offset == 4096 + 8192, "regions");
  require(std::string(c.serial) == "TEST-00001", "serial");
  require(session->commands_out().max_payload() == 4096 - 16, "max payload is capacity/2 minus the header");
  require(session->name().rfind("Local\\gt4gs-test-", 0) == 0, "session name prefix");
  bool rejected = false;
  try {
    Session::create_producer(small_options(1000));
  } catch (const IpcError &) {
    rejected = true;
  }
  require(rejected, "capacity not a multiple of 4096 is refused");
  // Oversized records are refused before anything is written.
  std::vector<std::uint8_t> big(8192);
  rejected = false;
  try {
    session->commands_out().try_write(RecordType::gif, 0, big.data(), big.size(), nullptr, 0);
  } catch (const IpcError &) {
    rejected = true;
  }
  require(rejected && session->commands_out().used_bytes() == 0, "oversized record refused");
  // A consumer opened on the same name sees the same block; a wrong name fails cleanly.
  auto consumer = Session::open_consumer(session->name());
  require(consumer->control().producer_pid == current_process_id(), "consumer sees the producer pid");
  rejected = false;
  try {
    Session::open_consumer("Local\\gt4gs-test-missing");
  } catch (const IpcError &) {
    rejected = true;
  }
  require(rejected, "missing mapping refused");
  std::puts("[gs-stream-ipc] layout ok");
}

// 10^6 random records through a 64 KiB ring: every record arrives once, in order, byte-identical, with wrap fillers
// and back-pressure on every lap. Random sleeps on both sides exercise both sleep paths.
void check_threaded_random(std::uint64_t records) {
  auto producer = Session::create_producer(small_options(64 * 1024));
  auto consumer = Session::open_consumer(producer->name());
  std::atomic<bool> failed{false};
  std::string failure;
  std::thread reader([&] {
    try {
      std::mt19937_64 rng(7);
      RecordView view;
      for (std::uint64_t n = 1; n <= records; ++n) {
        consumer->next_command(view);
        const RecordHeader &h = *view.header;
        if (h.sequence != n) throw std::runtime_error("sequence out of order");
        if (h.type != static_cast<std::uint16_t>(RecordType::gif)) throw std::runtime_error("unexpected type");
        for (std::uint32_t i = 0; i < h.payload_bytes; ++i)
          if (view.payload[i] != pattern(n, i)) throw std::runtime_error("payload byte differs");
        for (std::uint64_t i = h.payload_bytes; i < align_record(h.payload_bytes); ++i)
          if (view.payload[i] != 0) throw std::runtime_error("padding not zero");
        consumer->release_command(view);
        if ((rng() & 0x3FFFFu) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    } catch (const std::exception &error) {
      failure = error.what();
      failed = true;
    }
  });
  std::mt19937_64 rng(11);
  std::vector<std::uint8_t> payload;
  for (std::uint64_t n = 1; n <= records && !failed; ++n) {
    // Mostly small records, some near the 32 KiB limit.
    const std::size_t bytes = (rng() % 16 == 0) ? static_cast<std::size_t>(rng() % (32 * 1024 - 16))
                                                : static_cast<std::size_t>(rng() % 300);
    payload.resize(bytes);
    for (std::size_t i = 0; i < bytes; ++i) payload[i] = pattern(n, i);
    producer->post(RecordType::gif, 0, payload.data(), bytes);
    if ((rng() & 0x3FFFFu) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  reader.join();
  if (failed) throw std::runtime_error("threaded random: " + failure);
  require(producer->commands_out().records_written() == records, "records written");
  require(producer->wait_totals().space_waits > 0, "back-pressure was exercised");
  std::printf("[gs-stream-ipc] threaded random: %llu records, %llu bytes, %llu space waits, ok\n",
              static_cast<unsigned long long>(records),
              static_cast<unsigned long long>(producer->commands_out().bytes_written()),
              static_cast<unsigned long long>(producer->wait_totals().space_waits));
}

// Split records: a producer-side chunker marks all but the last part with kRecordFlagMore; the reader joins them.
void check_split() {
  auto producer = Session::create_producer(small_options(16384));
  auto consumer = Session::open_consumer(producer->name());
  const std::vector<std::uint8_t> data = make_payload(50000, 5);
  std::vector<std::uint8_t> joined;
  std::uint64_t parts = 0;
  std::thread reader([&] {
    RecordView view;
    for (;;) {
      consumer->next_command(view);
      ++parts;
      joined.insert(joined.end(), view.payload, view.payload + view.header->payload_bytes);
      const bool more = view.header->flags & kRecordFlagMore;
      consumer->release_command(view);
      if (!more) return;
    }
  });
  const std::size_t chunk = static_cast<std::size_t>(producer->commands_out().max_payload()) & ~std::size_t(15);
  for (std::size_t at = 0; at < data.size(); at += chunk) {
    const std::size_t bytes = std::min(chunk, data.size() - at);
    const std::uint16_t flags = at + bytes < data.size() ? kRecordFlagMore : 0;
    producer->post(RecordType::gif, flags, data.data() + at, bytes);
  }
  reader.join();
  require(joined == data && parts == (data.size() + chunk - 1) / chunk, "split record joined");
  std::printf("[gs-stream-ipc] split: %llu parts joined, ok\n", static_cast<unsigned long long>(parts));
}

// Consumer side of the readback and VSYNC protocol, shared by the thread tests and the child process.
// Answers READ_FIFO with pattern(request_id, i); counts VSYNCs; stops at CONTROL shutdown. Returns records seen.
std::uint64_t serve(Session &consumer, std::uint64_t die_after = 0, std::uint32_t vsync_delay_ms = 0) {
  RecordView view;
  std::uint64_t seen = 0;
  for (;;) {
    consumer.next_command(view);
    ++seen;
    if (die_after && seen == die_after) ExitProcess(9);  // abrupt death: no EXITING, no release
    const auto type = static_cast<RecordType>(view.header->type);
    if (type == RecordType::read_fifo) {
      ReadFifoPayload request;
      std::memcpy(&request, view.payload, sizeof request);
      const std::uint64_t bytes = std::uint64_t(request.qwords) * 16;
      for (std::uint64_t i = 0; i < bytes; ++i) consumer.response_area()[i] = pattern(request.request_id, i);
      consumer.release_command(view);
      consumer.complete_readback(request.request_id, bytes, kReadbackOk);
      continue;
    }
    const bool shutdown = type == RecordType::control &&
                          reinterpret_cast<const ControlPayload *>(view.payload)->kind ==
                              static_cast<std::uint32_t>(ControlKind::shutdown);
    consumer.release_command(view);
    if (type == RecordType::vsync) {
      if (vsync_delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(vsync_delay_ms));
      consumer.complete_vsync();
    }
    if (shutdown) {
      const ExitingPayload exiting{0, 0, 0};
      consumer.post_event(RecordType::exiting, &exiting, sizeof exiting);
      return seen;
    }
  }
}

void post_shutdown(Session &producer) {
  const ControlPayload shutdown{static_cast<std::uint32_t>(ControlKind::shutdown), 0, {0, 0, 0}};
  producer.post(RecordType::control, 0, &shutdown, sizeof shutdown);
}

void check_readback_and_frames() {
  auto producer = Session::create_producer(small_options(16384));
  auto consumer = Session::open_consumer(producer->name());
  std::thread server([&] { serve(*consumer, 0, 3); });
  std::vector<std::uint8_t> reply(4096);
  std::uint64_t max_in_flight = 0;
  for (std::uint32_t frame = 0; frame < 40; ++frame) {
    const std::uint8_t filler[64] = {};
    producer->post(RecordType::gif, 0, filler, sizeof filler);
    const VsyncPayload vsync{frame, static_cast<std::uint8_t>(frame & 1), {}, kVsyncFlagFieldValid};
    producer->post_vsync(vsync);
    const ControlBlock &c = producer->control();
    max_in_flight = std::max<std::uint64_t>(max_in_flight, c.vsyncs_posted - c.vsyncs_done);
    if (frame % 4 == 0) {
      const std::uint32_t qwords = 1 + frame * 6;
      const std::uint32_t status = producer->read_fifo(qwords, reply.data());
      const std::uint64_t id = producer->control().readback_request;
      require(status == kReadbackOk, "readback status");
      for (std::uint64_t i = 0; i < std::uint64_t(qwords) * 16; ++i)
        require(reply[i] == pattern(id, i), "readback bytes");
    }
  }
  post_shutdown(*producer);
  server.join();
  RecordView event;
  require(producer->wait_event(event, 1000) && event.header->type == static_cast<std::uint16_t>(RecordType::exiting),
          "EXITING event");
  producer->events_in().release(event);
  require(max_in_flight <= 2, "frames in flight bounded by the limit");
  require(producer->wait_totals().frames_waits > 0, "frames-in-flight wait exercised");
  std::printf("[gs-stream-ipc] readback/frames: max in flight %llu, frame waits %llu, ok\n",
              static_cast<unsigned long long>(max_in_flight),
              static_cast<unsigned long long>(producer->wait_totals().frames_waits));
}

// TASK-116 step 1: a READ_FIFO posted without waiting, then collected by request id. The record is the one read_fifo
// posts (same payload, same id sequence), so the stream bytes and the hash do not change; only the wait moves.
void check_readback_nowait() {
  auto producer = Session::create_producer(small_options(16384));
  auto consumer = Session::open_consumer(producer->name());
  std::thread server([&] { serve(*consumer, 0, 3); });
  std::vector<std::uint8_t> reply(16384);
  for (std::uint32_t n = 0; n < 6; ++n) {
    const std::uint32_t qwords = 4 + n * 11;
    const std::uint64_t id = producer->post_read_fifo_nowait(qwords);
    require(id == producer->control().readback_request, "a no-wait post advances the request id");
    for (int spin = 0; spin < 5000 && producer->control().readback_done < id; ++spin) Sleep(1);
    require(producer->readback_reply(id, reply.data(), qwords), "the reply of the no-wait request is collected");
    require(producer->readback_status() == kReadbackOk, "no-wait readback status");
    for (std::uint64_t i = 0; i < std::uint64_t(qwords) * 16; ++i)
      require(reply[i] == pattern(id, i), "no-wait readback bytes");
    // Only the reply that is still in the response area is returned.
    require(!producer->readback_reply(id - 1, reply.data(), qwords), "an older reply id is not returned");
    require(!producer->readback_reply(id + 1, reply.data(), qwords), "a not-yet-posted reply id is not returned");
  }
  post_shutdown(*producer);
  server.join();
  RecordView event;
  require(producer->wait_event(event, 1000) && event.header->type == static_cast<std::uint16_t>(RecordType::exiting),
          "EXITING event after no-wait readbacks");
  producer->events_in().release(event);
  std::printf("[gs-stream-ipc] no-wait readbacks: 6 posted and collected by id, ok\n");
}

// Auto-reset events left signalled from earlier waits wake a sleeper with nothing to do; it must sleep again.
void check_spurious_wakes() {
  auto producer = Session::create_producer(small_options(8192));
  auto consumer = Session::open_consumer(producer->name());
  producer->set_spin_us(0);
  consumer->set_spin_us(0);
  producer->set_data_wake_bytes(0);  // one wake-up per record: the sleeper must see the record at once
  for (Wake wake : {Wake::data, Wake::space, Wake::response, Wake::event}) producer->signal(wake);
  std::atomic<bool> got{false};
  std::thread reader([&] {
    RecordView view;
    consumer->next_command(view);  // wakes spuriously first, then for the real record
    got = view.header->sequence == 1;
    consumer->release_command(view);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  require(!got, "no record delivered before it was posted");
  const std::uint8_t data[16] = {1};
  producer->post(RecordType::gif, 0, data, sizeof data);
  reader.join();
  require(got, "record delivered after spurious wake");
  RecordView event;
  require(!producer->wait_event(event, 120), "spurious event wake does not invent an event");
  std::puts("[gs-stream-ipc] spurious wakes ok");
}

// TASK-041: wake batching. A sleeping consumer is signalled after data_wake_bytes of GIF/PRIV records, at once for
// any other record type, and before the producer waits for ring space (else a full ring would stall until the
// consumer's 100 ms sleep slice ends).
std::uint64_t post_gif_records(Session &producer, int count, bool pause) {
  std::vector<std::uint8_t> data(1024, 7);
  for (int i = 0; i < count; ++i) {
    producer.post(RecordType::gif, 0, data.data(), data.size());
    if (pause) std::this_thread::sleep_for(std::chrono::milliseconds(2));  // the consumer drains and sleeps again
  }
  return producer.wait_totals().wakes_signalled[static_cast<int>(Wake::data)];
}

void check_wake_batching() {
  for (const std::uint64_t threshold : {std::uint64_t(0), std::uint64_t(16) << 10}) {
    auto producer = Session::create_producer(small_options(1 << 20));
    auto consumer = Session::open_consumer(producer->name());
    producer->set_spin_us(0);
    consumer->set_spin_us(0);
    producer->set_data_wake_bytes(threshold);
    std::atomic<int> received{0};
    std::thread reader([&] {
      RecordView view;
      for (;;) {
        consumer->next_command(view);
        const bool last = view.header->type == static_cast<std::uint16_t>(RecordType::control);
        consumer->release_command(view);
        ++received;
        if (last) return;
      }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::uint64_t wakes = post_gif_records(*producer, 64, true);
    const ControlPayload shutdown{static_cast<std::uint32_t>(ControlKind::shutdown), 0, {}};
    const auto start = std::chrono::steady_clock::now();
    producer->post(RecordType::control, 0, &shutdown, sizeof shutdown);  // not batched: wakes the consumer at once
    reader.join();
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    require(received == 65, "every record delivered");
    require(waited.count() < 90, "a CONTROL record wakes the sleeping consumer at once");
    if (threshold == 0)
      require(wakes >= 60, "without batching every record wakes the sleeping consumer");
    else
      require(wakes >= 2 && wakes <= 6, "64 records of 1,056 bytes with a 16 KiB threshold wake it about 4 times");
    std::printf("[gs-stream-ipc] wake batching threshold %llu: %llu data wake-ups for 64 GIF records\n",
                static_cast<unsigned long long>(threshold), static_cast<unsigned long long>(wakes));
  }
  // Back-pressure with a threshold larger than the ring: the producer must wake the consumer before it waits.
  auto producer = Session::create_producer(small_options(8192));
  auto consumer = Session::open_consumer(producer->name());
  producer->set_spin_us(0);
  consumer->set_spin_us(0);
  producer->set_data_wake_bytes(1u << 20);
  std::atomic<int> received{0};
  std::thread reader([&] {
    RecordView view;
    for (int i = 0; i < 101; ++i) {
      consumer->next_command(view);
      consumer->release_command(view);
      ++received;
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto start = std::chrono::steady_clock::now();
  post_gif_records(*producer, 100, false);
  const ControlPayload shutdown{static_cast<std::uint32_t>(ControlKind::shutdown), 0, {}};
  producer->post(RecordType::control, 0, &shutdown, sizeof shutdown);
  reader.join();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  require(received == 101 && producer->wait_totals().space_waits > 0, "ring space waits exercised, every record delivered");
  require(elapsed.count() < 1000, "a full ring wakes the batched consumer (no 100 ms stall per space wait)");
  std::printf("[gs-stream-ipc] wake batching back-pressure: %llu space waits, %lld ms\n",
              static_cast<unsigned long long>(producer->wait_totals().space_waits), static_cast<long long>(elapsed.count()));
}

void check_fatal_flag() {
  auto producer = Session::create_producer(small_options(8192));
  auto consumer = Session::open_consumer(producer->name());
  std::thread setter([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::atomic_ref<std::uint32_t>(producer->control().state).fetch_or(kStateFatal);
  });
  bool stopped = false;
  try {
    RecordView view;
    consumer->next_command(view);
  } catch (const PeerExited &) {
    stopped = true;
  }
  setter.join();
  require(stopped, "fatal flag ends a wait");
  std::puts("[gs-stream-ipc] fatal flag ok");
}

std::wstring this_executable() {
  wchar_t path[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::wstring(path, length);
}

ChildProcess spawn_child(const Session &producer_session, const char *mode) {
  const std::wstring arguments = L"--child " + widen(producer_session.name()) + L" " + widen(mode);
  return ChildProcess::spawn(this_executable(), arguments);
}

// Child: opens the session, waits on the parent process, posts READY (unless silent), then serves.
int child_main(const std::string &name, const std::string &mode) {
  auto consumer = Session::open_consumer(name);
  NativeHandle parent = open_process_for_wait(consumer->control().producer_pid);
  consumer->set_peer(parent);
  consumer->control().consumer_pid = current_process_id();
  if (mode == "silent") {
    Sleep(30000);
    return 0;
  }
  ReadyPayload ready{};
  ready.version_major = kVersionMajor;
  ready.version_minor = kVersionMinor;
  ready.consumer_pid = current_process_id();
  std::memcpy(ready.consumer, "gs_stream_ipc_fixture", 21);
  consumer->post_event(RecordType::ready, &ready, sizeof ready);
  try {
    serve(*consumer, mode == "die" ? 500 : 0);
  } catch (const PeerExited &) {
    return 3;
  }
  close_handle(parent);
  return 0;
}

void wait_ready(Session &producer, const ChildProcess &child) {
  RecordView event;
  require(producer.wait_event(event, 20000), "child READY within 20 s");
  require(event.header->type == static_cast<std::uint16_t>(RecordType::ready), "first event is READY");
  ReadyPayload ready;
  std::memcpy(&ready, event.payload, sizeof ready);
  require(ready.version_major == kVersionMajor && ready.consumer_pid == child.pid(), "READY payload");
  producer.events_in().release(event);
}

void check_child_process() {
  // Normal session: ordering, wrap and back-pressure across processes, readbacks, clean shutdown.
  {
    auto producer = Session::create_producer(small_options(32768));
    ChildProcess child = spawn_child(*producer, "serve");
    producer->set_peer(child.process());
    wait_ready(*producer, child);
    std::mt19937_64 rng(3);
    std::vector<std::uint8_t> reply(4096);
    for (std::uint64_t n = 0; n < 20000; ++n) {
      const std::vector<std::uint8_t> data = make_payload(static_cast<std::size_t>(rng() % 2000) & ~std::size_t(15), n);
      producer->post(RecordType::gif, 0, data.data(), data.size());
      if (n % 1000 == 999) {
        const std::uint32_t status = producer->read_fifo(200, reply.data());
        const std::uint64_t id = producer->control().readback_request;
        require(status == kReadbackOk, "cross-process readback status");
        for (std::uint64_t i = 0; i < 3200; ++i) require(reply[i] == pattern(id, i), "cross-process readback bytes");
      }
    }
    post_shutdown(*producer);
    std::uint32_t code = 99;
    require(child.wait_exit(10000, &code) && code == 0, "child exits 0 after shutdown");
    std::puts("[gs-stream-ipc] child process session ok");
  }
  // Handshake timeout: a child that never answers is reported, then killed with the job.
  {
    auto producer = Session::create_producer(small_options(8192));
    ChildProcess child = spawn_child(*producer, "silent");
    producer->set_peer(child.process());
    RecordView event;
    const auto start = std::chrono::steady_clock::now();
    require(!producer->wait_event(event, 700), "silent child: no READY");
    require(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(650), "timeout honoured");
    child.kill();
    require(!child.running(), "job kill ends the child");
    std::puts("[gs-stream-ipc] handshake timeout ok");
  }
  // Consumer death: the producer is blocked on ring space or a readback when the child exits; the wait ends with
  // PeerExited instead of hanging.
  {
    auto producer = Session::create_producer(small_options(8192));
    ChildProcess child = spawn_child(*producer, "die");
    producer->set_peer(child.process());
    wait_ready(*producer, child);
    bool peer_exit = false;
    try {
      std::vector<std::uint8_t> reply(4096);
      for (std::uint64_t n = 0; n < 100000; ++n) {
        const std::vector<std::uint8_t> data = make_payload(1024, n);
        producer->post(RecordType::gif, 0, data.data(), data.size());
        if (n % 100 == 99) producer->read_fifo(4, reply.data());
      }
    } catch (const PeerExited &) {
      peer_exit = true;
    }
    std::uint32_t code = 0;
    require(peer_exit && child.wait_exit(5000, &code) && code == 9, "producer sees consumer death");
    std::puts("[gs-stream-ipc] consumer death seen by the producer ok");
  }
  // Producer death as seen by a consumer: the consumer waits on a peer process handle that ends.
  {
    auto producer = Session::create_producer(small_options(8192));
    ChildProcess peer = spawn_child(*producer, "silent");  // stands in for a producer process
    auto consumer = Session::open_consumer(producer->name());
    consumer->set_peer(peer.process());
    bool peer_exit = false;
    std::thread killer([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      peer.kill();
    });
    try {
      RecordView view;
      consumer->next_command(view);
    } catch (const PeerExited &) {
      peer_exit = true;
    }
    killer.join();
    require(peer_exit, "consumer sees peer death");
    std::puts("[gs-stream-ipc] peer death seen by the consumer ok");
  }
}

}  // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 4 && std::string(argv[1]) == "--child") return child_main(argv[2], argv[3]);
    const std::uint64_t records = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000;
    check_layout();
    check_split();
    check_readback_and_frames();
    check_readback_nowait();
    check_spurious_wakes();
    check_wake_batching();
    check_fatal_flag();
    check_threaded_random(records);
    check_child_process();
    std::puts("[gs-stream-ipc] all checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "[gs-stream-ipc] FAIL: %s\n", error.what());
    return 1;
  }
}
