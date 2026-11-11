// TASK-090 CoreGS 3.0 live world: the in-memory per-job texture store. See coregs3_live_world.hpp.
#include "coregs3_live_world.hpp"

#include "coregs3_tap.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>

namespace gt4::coregs3::live_world {
namespace {

struct Store {
  std::mutex mutex;
  bool configured = false;
  bool active = false;   // the store (job sources/records/VRAM base)
  bool world = true;     // the world draw switch (GT4_COREGS3_LIVE_WORLD)
  // Pending job -> the packet addresses it was posted for, in post order (the EE posts before the worker executes).
  std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> job_addresses;
  std::deque<std::uint64_t> pending_order;
  // Pending emitted GIF records, moved into the job record by note_job_textures.
  std::unordered_map<std::uint64_t, std::vector<GifRecord>> pending_streams;
  // Published records, keyed by the full job id. The address table shares them by pointer.
  std::map<std::uint64_t, std::shared_ptr<JobTexture>> records;
  std::unordered_map<std::uint32_t, std::shared_ptr<const JobTexture>> address_textures;
  std::uint64_t newest_vblank = 0;
  std::uint64_t stream_sequence = 0;  // emission order across jobs, for the tap walk
  std::uint64_t bytes_held = 0;
  std::vector<std::uint8_t> base_bytes;
  std::uint32_t base_first = 0;
  std::uint32_t base_blocks = 0;
  std::uint64_t base_vblank = 0;
  bool base_valid = false;
  FrameEnv frame_env;
  Counters counters;
};

Store &store() {
  static Store instance;
  return instance;
}

void forget_oldest_pending(Store &s) noexcept {
  while (!s.pending_order.empty()) {
    const std::uint64_t job = s.pending_order.front();
    s.pending_order.pop_front();
    const auto found = s.job_addresses.find(job);
    if (found == s.job_addresses.end()) continue;
    s.job_addresses.erase(found);
    s.counters.sources_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
}

// Drops records older than the retention window and the address entries that point at them. Called with the mutex held.
void prune(Store &s) noexcept {
  const std::uint64_t oldest = s.newest_vblank > kRetentionVblanks ? s.newest_vblank - kRetentionVblanks : 0;
  for (auto it = s.records.begin(); it != s.records.end();) {
    const JobTexture &record = *it->second;
    if (record.vblank >= oldest || s.newest_vblank <= kRetentionVblanks) {
      ++it;
      continue;
    }
    std::uint64_t record_bytes = record.bytes.size();
    for (const GifRecord &gif : record.gif_records) record_bytes += gif.bytes.size();
    s.bytes_held -= record_bytes;
    s.counters.records_pruned.fetch_add(1, std::memory_order_relaxed);
    it = s.records.erase(it);
  }
  for (auto it = s.address_textures.begin(); it != s.address_textures.end();) {
    const JobTexture &record = *it->second;
    if (record.vblank >= oldest || s.newest_vblank <= kRetentionVblanks) {
      ++it;
      continue;
    }
    it = s.address_textures.erase(it);
  }
  s.counters.bytes_held.store(s.bytes_held, std::memory_order_relaxed);
}

}  // namespace

void configure_from_environment() noexcept {
  Store &s = store();
  const std::lock_guard<std::mutex> lock(s.mutex);
  if (s.configured) return;
  s.configured = true;
  // The store is on whenever the present layer runs: the car pass binds its textures from the same live job records
  // as the world, including in the world-off A/B.
  s.active = true;
  const char *text = std::getenv("GT4_COREGS3_LIVE_WORLD");
  if (text == nullptr || *text == '\0' || std::strcmp(text, "1") == 0) {
    s.world = true;
  } else if (std::strcmp(text, "0") == 0) {
    s.world = false;
  } else {
    s.world = false;
    std::fprintf(stderr, "[coregs3] GT4_COREGS3_LIVE_WORLD=%s is not 0 or 1; the live world stays off\n", text);
  }
}

bool configured() noexcept {
  Store &s = store();
  const std::lock_guard<std::mutex> lock(s.mutex);
  return s.configured;
}

bool active() noexcept {
  Store &s = store();
  const std::lock_guard<std::mutex> lock(s.mutex);
  return s.active;
}

bool world_enabled() noexcept {
  Store &s = store();
  const std::lock_guard<std::mutex> lock(s.mutex);
  return s.world;
}

void set_active(bool active) noexcept {
  Store &s = store();
  const std::lock_guard<std::mutex> lock(s.mutex);
  s.active = active;
  s.world = active;
  s.configured = true;
}

void note_job_source(std::uint64_t job, std::uint32_t guest_address) noexcept {
  Store &s = store();
  const std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.active) return;
  std::vector<std::uint32_t> &addresses = s.job_addresses[job];
  if (std::find(addresses.begin(), addresses.end(), guest_address) == addresses.end())
    addresses.push_back(guest_address);
  s.pending_order.push_back(job);
  s.counters.sources.fetch_add(1, std::memory_order_relaxed);
  while (s.job_addresses.size() > kMaxPendingJobs) forget_oldest_pending(s);
}

void note_job_textures(std::uint64_t job, std::uint64_t vblank, const vram::Window *windows,
                       std::uint32_t window_count, const std::uint8_t *slm) noexcept {
  Store &s = store();
  const std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.active) return;
  // The address list is what makes the record reachable; a job whose source was never seen (or was forgotten) is
  // dropped instead of being kept in an unreachable table.
  const auto linked = s.job_addresses.find(job);
  if (linked == s.job_addresses.end()) {
    s.counters.jobs_unlinked.fetch_add(1, std::memory_order_relaxed);
    s.pending_streams.erase(job);  // a job with no posted source is dropped; never keep its records
    return;
  }
  const auto stream = s.pending_streams.find(job);
  const bool has_stream = stream != s.pending_streams.end() && !stream->second.empty();
  const bool has_windows = windows != nullptr && window_count != 0;
  if ((!has_windows && !has_stream) || window_count > vram::kMaxCaptureWindows ||
      (has_windows && slm == nullptr)) {
    s.counters.jobs_refused.fetch_add(1, std::memory_order_relaxed);
    s.pending_streams.erase(job);
    return;
  }
  if (s.records.size() >= kMaxRecords) {
    s.counters.jobs_refused.fetch_add(1, std::memory_order_relaxed);
    s.pending_streams.erase(job);
    return;
  }
  auto record = std::make_shared<JobTexture>();
  record->job = job;
  record->vblank = vblank;
  record->addresses = linked->second;
  std::uint32_t total_blocks = 0;
  std::uint64_t stream_bytes = 0;
  if (has_windows) {
    for (std::uint32_t index = 0; index < window_count; ++index) {
      vram::Window window = windows[index];
      if (window.block_count == 0 || window.first_block >= vram::kBlocksTotal ||
          window.block_count > vram::kBlocksTotal - window.first_block) {
        s.counters.jobs_refused.fetch_add(1, std::memory_order_relaxed);
        s.pending_streams.erase(job);
        return;
      }
      record->windows.push_back(window);
      total_blocks += window.block_count;
    }
  }
  if (has_stream) {
    record->gif_records = std::move(stream->second);
    s.pending_streams.erase(stream);
    for (const GifRecord &gif : record->gif_records) stream_bytes += gif.bytes.size();
  }
  if (s.bytes_held + static_cast<std::uint64_t>(total_blocks) * vram::kBlockBytes + stream_bytes > kMaxBytes) {
    s.counters.jobs_refused.fetch_add(1, std::memory_order_relaxed);
    s.pending_streams.erase(job);
    return;
  }
  record->bytes.reserve(static_cast<std::size_t>(total_blocks) * vram::kBlockBytes);
  for (const vram::Window &window : record->windows) {
    const std::size_t first = static_cast<std::size_t>(window.first_block) * vram::kBlockBytes;
    const std::size_t bytes = static_cast<std::size_t>(window.block_count) * vram::kBlockBytes;
    record->bytes.insert(record->bytes.end(), slm + first, slm + first + bytes);
  }
  if (vblank > s.newest_vblank) {
    s.newest_vblank = vblank;
    prune(s);
  }
  s.bytes_held += record->bytes.size() + stream_bytes;
  s.counters.bytes_held.store(s.bytes_held, std::memory_order_relaxed);
  for (const std::uint32_t address : linked->second) s.address_textures[address] = record;
  s.records.emplace(job, record);
  s.job_addresses.erase(linked);
  // The pending order is a FIFO of every posted job; executed jobs are dropped from its front so it never accumulates
  // a whole session of postings (a menu frame posts ~2,000 item packets).
  while (!s.pending_order.empty() && s.job_addresses.find(s.pending_order.front()) == s.job_addresses.end())
    s.pending_order.pop_front();
  s.counters.job_records.fetch_add(1, std::memory_order_relaxed);
}

void note_job_stream(std::uint64_t job, std::uint64_t vblank, const std::uint8_t *data,
                     const std::uint32_t *offsets, const std::uint32_t *sizes, std::uint32_t count) noexcept {
  Store &s = store();
  const std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.active || data == nullptr || offsets == nullptr || sizes == nullptr || count == 0) return;
  while (s.pending_streams.size() > kMaxPendingJobs) s.pending_streams.erase(s.pending_streams.begin());
  std::vector<GifRecord> &records = s.pending_streams[job];
  for (std::uint32_t index = 0; index < count; ++index) {
    if (sizes[index] == 0 || sizes[index] % 16u != 0) continue;
    const std::size_t offset = offsets[index];
    GifRecord gif;
    gif.sequence = s.stream_sequence++;
    gif.job = job;
    gif.vblank = vblank;
    gif.bytes.assign(data + offset, data + offset + sizes[index]);
    records.push_back(std::move(gif));
    s.counters.stream_records.fetch_add(1, std::memory_order_relaxed);
  }
}

void note_vram_base(std::uint64_t vblank, std::uint32_t first_block, std::uint32_t block_count,
                    const std::uint8_t *slm) noexcept {
  Store &s = store();
  const std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.active || slm == nullptr || block_count == 0 || first_block >= vram::kBlocksTotal ||
      block_count > vram::kBlocksTotal - first_block) {
    return;
  }
  s.base_bytes.assign(slm + static_cast<std::size_t>(first_block) * vram::kBlockBytes,
                      slm + static_cast<std::size_t>(first_block + block_count) * vram::kBlockBytes);
  s.base_first = first_block;
  s.base_blocks = block_count;
  s.base_vblank = vblank;
  s.base_valid = true;
}

bool copy_vram_base(std::vector<std::uint8_t> &bytes, std::uint32_t &first_block, std::uint32_t &block_count,
                    std::uint64_t &vblank) noexcept {
  Store &s = store();
  const std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.base_valid) return false;
  bytes = s.base_bytes;
  first_block = s.base_first;
  block_count = s.base_blocks;
  vblank = s.base_vblank;
  return true;
}

bool build_tap_snapshot(std::uint64_t frame_vblank, const std::vector<std::uint32_t> &addresses,
                        std::vector<std::uint8_t> &data, std::vector<TapFileRecord> &records) noexcept {
  Store &s = store();
  const std::lock_guard<std::mutex> lock(s.mutex);
  data.clear();
  records.clear();
  if (!s.active || addresses.empty()) return false;
  // One source record per requested packet address, stamped with the captured frame so the table's pairing keeps it;
  // the GIF records of the jobs those addresses resolved to.
  std::map<std::uint64_t, std::shared_ptr<const JobTexture>> jobs;  // ordered: deterministic snapshots
  for (const std::uint32_t address : addresses) {
    const auto found = s.address_textures.find(address);
    if (found == s.address_textures.end()) continue;
    jobs.emplace(found->second->job, found->second);
  }
  if (jobs.empty()) return false;
  for (const std::uint32_t address : addresses) {
    const auto found = s.address_textures.find(address);
    if (found == s.address_textures.end()) continue;
    TapFileRecord record;
    record.kind = kTapKindSource;
    record.job = found->second->job;
    record.vblank = frame_vblank;
    record.address = address;
    records.push_back(record);
  }
  // The state the VU1 emits before an item's vertices can come from records another job emitted (the vertex-less
  // TEX0/TEX1/CLAMP records), so the snapshot keeps every stored record in emission order, exactly the offline tap
  // file's walk; the source records above stay filtered to the requested addresses.
  std::vector<std::pair<std::uint64_t, const GifRecord *>> ordered;  // (sequence, record)
  for (const auto &job : s.records)
    for (const GifRecord &gif : job.second->gif_records) ordered.emplace_back(gif.sequence, &gif);
  std::sort(ordered.begin(), ordered.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });
  std::uint64_t snapshot_bytes = 0;
  std::uint64_t gif_records = 0;
  for (const auto &entry : ordered) {
    const GifRecord &gif = *entry.second;
    if (snapshot_bytes + gif.bytes.size() > kMaxSnapshotBytes) {
      s.counters.snapshot_refused.fetch_add(1, std::memory_order_relaxed);
      data.clear();
      records.clear();
      return false;
    }
    TapFileRecord record;
    record.kind = kTapKindGif;
    record.job = gif.job;
    record.vblank = gif.vblank;
    record.bytes = static_cast<std::uint32_t>(gif.bytes.size());
    record.offset = data.size();
    data.insert(data.end(), gif.bytes.begin(), gif.bytes.end());
    snapshot_bytes += gif.bytes.size();
    records.push_back(record);
    ++gif_records;
  }
  s.counters.snapshots.fetch_add(1, std::memory_order_relaxed);
  s.counters.snapshot_jobs.store(jobs.size(), std::memory_order_relaxed);
  s.counters.snapshot_vertices.store(gif_records, std::memory_order_relaxed);
  return !data.empty();
}

std::shared_ptr<const JobTexture> find(std::uint32_t guest_address) noexcept {
  Store &s = store();
  const std::lock_guard<std::mutex> lock(s.mutex);
  s.counters.lookups.fetch_add(1, std::memory_order_relaxed);
  const auto found = s.address_textures.find(guest_address);
  if (found == s.address_textures.end()) {
    s.counters.lookup_missing.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  return found->second;
}

void note_frame_env(const FrameEnv &env) noexcept {
  Store &s = store();
  const std::lock_guard<std::mutex> lock(s.mutex);
  s.frame_env = env;
}

FrameEnv frame_env() noexcept {
  Store &s = store();
  const std::lock_guard<std::mutex> lock(s.mutex);
  return s.frame_env;
}

const Counters &counters() noexcept { return store().counters; }

void reset() noexcept {
  Store &s = store();
  const std::lock_guard<std::mutex> lock(s.mutex);
  s.job_addresses.clear();
  s.pending_order.clear();
  s.pending_streams.clear();
  s.records.clear();
  s.address_textures.clear();
  s.newest_vblank = 0;
  s.bytes_held = 0;
  s.counters.sources.store(0, std::memory_order_relaxed);
  s.counters.sources_dropped.store(0, std::memory_order_relaxed);
  s.counters.job_records.store(0, std::memory_order_relaxed);
  s.counters.jobs_unlinked.store(0, std::memory_order_relaxed);
  s.counters.jobs_refused.store(0, std::memory_order_relaxed);
  s.counters.records_pruned.store(0, std::memory_order_relaxed);
  s.counters.bytes_held.store(0, std::memory_order_relaxed);
  s.counters.lookups.store(0, std::memory_order_relaxed);
  s.counters.lookup_missing.store(0, std::memory_order_relaxed);
  s.counters.stream_records.store(0, std::memory_order_relaxed);
  s.counters.snapshots.store(0, std::memory_order_relaxed);
  s.counters.snapshot_refused.store(0, std::memory_order_relaxed);
  s.counters.snapshot_jobs.store(0, std::memory_order_relaxed);
  s.counters.snapshot_vertices.store(0, std::memory_order_relaxed);
  s.base_bytes.clear();
  s.base_valid = false;
}

}  // namespace gt4::coregs3::live_world
