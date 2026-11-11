// TASK-090 CoreGS 3.0 live world: the in-memory per-job texture store. See coregs3_live_world.hpp.
#include "coregs3_live_world.hpp"

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
  bool active = false;
  // Pending job -> the packet addresses it was posted for, in post order (the EE posts before the worker executes).
  std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> job_addresses;
  std::deque<std::uint64_t> pending_order;
  // Published records, keyed by the full job id. The address table shares them by pointer.
  std::map<std::uint64_t, std::shared_ptr<JobTexture>> records;
  std::unordered_map<std::uint32_t, std::shared_ptr<const JobTexture>> address_textures;
  std::uint64_t newest_vblank = 0;
  std::uint64_t bytes_held = 0;
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
    s.bytes_held -= record.bytes.size();
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
  const char *text = std::getenv("GT4_COREGS3_LIVE_WORLD");
  if (text == nullptr || *text == '\0') {
    s.active = true;
  } else if (std::strcmp(text, "0") == 0) {
    s.active = false;
  } else if (std::strcmp(text, "1") == 0) {
    s.active = true;
  } else {
    s.active = false;
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

void set_active(bool active) noexcept {
  Store &s = store();
  const std::lock_guard<std::mutex> lock(s.mutex);
  s.active = active;
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
  if (slm == nullptr || windows == nullptr || window_count == 0 || window_count > vram::kMaxCaptureWindows) {
    s.counters.jobs_refused.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // The address list is what makes the record reachable; a job whose source was never seen (or was forgotten) is
  // dropped instead of being kept in an unreachable table.
  const auto linked = s.job_addresses.find(job);
  if (linked == s.job_addresses.end()) {
    s.counters.jobs_unlinked.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (s.records.size() >= kMaxRecords) {
    s.counters.jobs_refused.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  auto record = std::make_shared<JobTexture>();
  record->job = job;
  record->vblank = vblank;
  std::uint32_t total_blocks = 0;
  for (std::uint32_t index = 0; index < window_count; ++index) {
    vram::Window window = windows[index];
    if (window.block_count == 0 || window.first_block >= vram::kBlocksTotal ||
        window.block_count > vram::kBlocksTotal - window.first_block) {
      s.counters.jobs_refused.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    record->windows.push_back(window);
    total_blocks += window.block_count;
  }
  if (s.bytes_held + static_cast<std::uint64_t>(total_blocks) * vram::kBlockBytes > kMaxBytes) {
    s.counters.jobs_refused.fetch_add(1, std::memory_order_relaxed);
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
  s.bytes_held += record->bytes.size();
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

const Counters &counters() noexcept { return store().counters; }

void reset() noexcept {
  Store &s = store();
  const std::lock_guard<std::mutex> lock(s.mutex);
  s.job_addresses.clear();
  s.pending_order.clear();
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
}

}  // namespace gt4::coregs3::live_world
