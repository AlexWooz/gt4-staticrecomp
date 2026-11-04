// SPDX-License-Identifier: MIT
// TASK-116 step 0 (P0 gate): the READ_FIFO probe; see gs_readback_probe.hpp.
#include "gs_readback_probe.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

namespace gt4::gs_stream {
namespace {

constexpr int kMaxDownloadMode = 3;  // GSHardwareDownloadMode: 0 Enabled, 1/3 older values, 2 NoReadbacks

std::string trim(const std::string &text) {
  const char *space = " \t\r\n\f\v";
  const std::size_t first = text.find_first_not_of(space);
  if (first == std::string::npos) return {};
  const std::size_t last = text.find_last_not_of(space);
  return text.substr(first, last - first + 1);
}

bool equals_ignore_case(const std::string &left, const char *right) {
  const std::size_t length = std::strlen(right);
  if (left.size() != length) return false;
  for (std::size_t i = 0; i < length; ++i) {
    char a = left[i], b = right[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

const char *kSection = "EmuCore/GS";
const char *kKey = "HWDownloadMode";

}  // namespace

int download_mode_from_text(const std::string &text) {
  bool in_section = false;
  std::size_t at = 0;
  while (at <= text.size()) {
    std::size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    const std::string line = trim(text.substr(at, end - at));
    at = end + 1;
    if (line.empty() || line[0] == ';' || line[0] == '#') continue;
    if (line[0] == '[') {
      const std::size_t close = line.find(']');
      const std::string name = trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1));
      in_section = equals_ignore_case(name, kSection);
      continue;
    }
    if (!in_section) continue;
    const std::size_t split = line.find('=');
    if (split == std::string::npos) continue;
    if (!equals_ignore_case(trim(line.substr(0, split)), kKey)) continue;
    const std::string value = trim(line.substr(split + 1));
    if (value.empty() || value.size() > 2) return -1;
    int parsed = 0;
    for (const char digit : value) {
      if (digit < '0' || digit > '9') return -1;
      parsed = parsed * 10 + (digit - '0');
    }
    return parsed <= kMaxDownloadMode ? parsed : -1;
  }
  return -1;
}

int download_mode_from_ini(const char *path) {
  if (!path || !*path) return -1;
  std::FILE *file = std::fopen(path, "rb");
  if (!file) return -1;
  std::string text;
  char buffer[4096];
  for (;;) {
    const std::size_t got = std::fread(buffer, 1, sizeof buffer, file);
    if (got == 0) break;
    text.append(buffer, got);
    if (text.size() > (1u << 20)) {  // a gt4-gs.ini is a few KB; refuse to read an unrelated large file
      std::fclose(file);
      return -1;
    }
  }
  std::fclose(file);
  return download_mode_from_text(text);
}

std::uint64_t fnv1a64(const void *data, std::size_t size) {
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  std::uint64_t value = 0xCBF29CE484222325ull;
  for (std::size_t i = 0; i < size; ++i) {
    value ^= bytes[i];
    value *= 0x100000001B3ull;
  }
  return value;
}

bool ReadbackProbe::enabled_from_environment() {
  const char *value = std::getenv("GT4_GS_READBACK_LOG");
  if (!value || !*value) return false;
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  return end && !*end && parsed != 0;
}

void ReadbackProbe::record(const ReadbackObservation &observation, std::FILE *output) {
  if (!enabled_) return;
  const std::size_t bytes = static_cast<std::size_t>(observation.qwords) * 16;
  const std::uint8_t *data = observation.bytes;
  std::uint32_t nonzero = 0;
  for (std::size_t i = 0; data && i < bytes; ++i) nonzero += data[i] != 0;
  const std::uint64_t hash = data ? fnv1a64(data, bytes) : 0;

  Destination &destination = by_destination_[{observation.destination, observation.qwords}];
  ++calls_;
  ++destination.calls;
  destination.wait_us_total += observation.wait_us;
  destination.wait_us_max = std::max(destination.wait_us_max, observation.wait_us);
  destination.nonzero_min = std::min(destination.nonzero_min, nonzero);
  destination.nonzero_max = std::max(destination.nonzero_max, nonzero);
  if (observation.status < 32) destination.statuses |= 1u << observation.status;
  if (nonzero == 0) ++destination.zero_replies;
  const bool repeated = destination.has_previous && destination.previous_hash == hash;
  if (repeated) ++destination.same_as_previous;
  if (destination.calls == 1) destination.first_hash = hash;
  destination.last_hash = hash;
  destination.previous_hash = hash;
  destination.has_previous = true;
  destination.distinct.insert(hash);

  std::uint32_t head[4] = {};
  for (std::size_t word = 0; data && word < 4 && (word + 1) * 4 <= bytes; ++word)
    for (std::size_t byte = 0; byte < 4; ++byte) head[word] |= static_cast<std::uint32_t>(data[word * 4 + byte]) << (8 * byte);
  std::fprintf(output,
               "[gs-readback] f=%llu dst=0x%08x qw=%u status=%u wait_us=%llu nz=%u head=%08x%08x%08x%08x hash=%016llx "
               "same=%d\n",
               static_cast<unsigned long long>(observation.frame), observation.destination, observation.qwords,
               observation.status, static_cast<unsigned long long>(observation.wait_us), nonzero, head[0], head[1],
               head[2], head[3], static_cast<unsigned long long>(hash), repeated ? 1 : 0);
}

void ReadbackProbe::print_summary(std::FILE *output) const {
  if (!enabled_ || calls_ == 0) return;
  std::fprintf(output, "[gs-readback] summary: readbacks=%llu destinations=%zu (GT4_GS_READBACK_LOG)\n",
               static_cast<unsigned long long>(calls_), by_destination_.size());
  for (const auto &[key, destination] : by_destination_) {
    std::string statuses;
    for (unsigned status = 0; status < 32; ++status) {
      if ((destination.statuses & (1u << status)) == 0) continue;
      if (!statuses.empty()) statuses += ',';
      statuses += std::to_string(status);
    }
    std::fprintf(output,
                 "[gs-readback] dst=0x%08x qw=%u calls=%llu distinct=%zu repeated=%llu zero=%llu nz=%u..%u statuses=%s "
                 "mean_wait_us=%llu max_wait_us=%llu first=%016llx last=%016llx\n",
                 key.first, key.second, static_cast<unsigned long long>(destination.calls), destination.distinct.size(),
                 static_cast<unsigned long long>(destination.same_as_previous),
                 static_cast<unsigned long long>(destination.zero_replies), destination.nonzero_min,
                 destination.nonzero_max, statuses.c_str(),
                 static_cast<unsigned long long>(destination.wait_us_total / destination.calls),
                 static_cast<unsigned long long>(destination.wait_us_max),
                 static_cast<unsigned long long>(destination.first_hash),
                 static_cast<unsigned long long>(destination.last_hash));
  }
}

}  // namespace gt4::gs_stream
