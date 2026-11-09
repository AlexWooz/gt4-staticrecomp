// TASK-212 (slice421): same-length disc data substitution. See native/data_patch.hpp for the contract.
#include "data_patch.hpp"

#include "ps2/disc.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

namespace gt4::data_patch {
namespace {

constexpr char kMagic[8] = {'G', 'T', '4', 'D', 'P', 'A', 'T', '1'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeaderBytes = 24;  // magic, version, count, payload bytes, reserved
constexpr std::size_t kRecordBytes = 80;  // offset, length, payload offset, original digest, replacement digest
constexpr std::size_t kFileCap = 64u << 20;
constexpr std::size_t kRangeCap = 32u << 20;

using Digest = std::array<std::uint8_t, 32>;

struct Record {
  std::uint64_t offset = 0;
  std::uint32_t length = 0;
  std::uint32_t payload_offset = 0;
  Digest original{};
};

struct Loaded {
  std::vector<Record> records;
  std::vector<std::uint8_t> payload;
};

std::vector<Record> g_records;
std::vector<std::uint8_t> g_payload;
bool g_active = false;
bool g_installed = false;

bool sha256(const std::uint8_t *data, std::size_t size, std::uint8_t out[32]) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD object_bytes = 0, written = 0;
  bool ok = false;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
  if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_bytes), sizeof object_bytes,
                        &written, 0) >= 0) {
    std::vector<std::uint8_t> object(object_bytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_bytes, nullptr, 0, 0) >= 0) {
      ok = BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) >= 0 &&
           BCryptFinishHash(hash, out, 32, 0) >= 0;
    }
  }
  if (hash) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return ok;
}

std::uint32_t le32(const std::uint8_t *p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t le64(const std::uint8_t *p) {
  return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

bool read_image(std::uint64_t offset, std::uint8_t *out, std::size_t size) {
  // The runtime reads whole sectors; a range can start and end inside one.
  const std::uint64_t first = offset / ps2::disc::kSector;
  const std::uint64_t end = offset + size;
  const std::uint64_t last = (end + ps2::disc::kSector - 1) / ps2::disc::kSector;
  if (last <= first || last - first > (kRangeCap / ps2::disc::kSector) + 1) return false;
  std::vector<std::uint8_t> sectors(static_cast<std::size_t>(last - first) * ps2::disc::kSector);
  if (!ps2::disc::read(static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(last - first), sectors.data()))
    return false;
  std::memcpy(out, sectors.data() + (offset - first * ps2::disc::kSector), size);
  return true;
}

bool load_container(const char *path, Loaded &loaded) {
  std::FILE *file = std::fopen(path, "rb");
  if (!file) {
    std::fprintf(stderr, "[data-patch] %s: cannot open; unpatched\n", path);
    return false;
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::rewind(file);
  if (size <= 0 || static_cast<std::size_t>(size) > kFileCap) {
    std::fclose(file);
    std::fprintf(stderr, "[data-patch] %s: size %ld outside the cap; unpatched\n", path, size);
    return false;
  }
  std::vector<std::uint8_t> blob(static_cast<std::size_t>(size));
  const std::size_t got = std::fread(blob.data(), 1, blob.size(), file);
  std::fclose(file);
  if (got != blob.size() || blob.size() < kHeaderBytes || std::memcmp(blob.data(), kMagic, sizeof kMagic) != 0) {
    std::fprintf(stderr, "[data-patch] %s: not a GT4DPAT1 container; unpatched\n", path);
    return false;
  }
  const std::uint32_t version = le32(blob.data() + 8);
  const std::uint32_t count = le32(blob.data() + 12);
  const std::uint32_t payload_bytes = le32(blob.data() + 16);
  const std::size_t table_end = kHeaderBytes + static_cast<std::size_t>(count) * kRecordBytes;
  if (version != kVersion || count == 0 || count > 64 || table_end > blob.size() ||
      blob.size() - table_end != payload_bytes) {
    std::fprintf(stderr, "[data-patch] %s: unsupported header or record table; unpatched\n", path);
    return false;
  }
  loaded.payload.assign(blob.begin() + static_cast<std::ptrdiff_t>(table_end), blob.end());
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint8_t *record = blob.data() + kHeaderBytes + static_cast<std::size_t>(i) * kRecordBytes;
    Record item;
    item.offset = le64(record);
    item.length = le32(record + 8);
    item.payload_offset = le32(record + 12);
    std::memcpy(item.original.data(), record + 16, 32);
    if (item.length == 0 || item.length > kRangeCap || item.payload_offset > loaded.payload.size() ||
        item.length > loaded.payload.size() - item.payload_offset) {
      std::fprintf(stderr, "[data-patch] %s: record %u outside its payload; unpatched\n", path, i);
      return false;
    }
    std::uint8_t block_sha[32];
    if (!sha256(loaded.payload.data() + item.payload_offset, item.length, block_sha) ||
        std::memcmp(block_sha, record + 48, 32) != 0) {
      std::fprintf(stderr, "[data-patch] %s: record %u replacement does not match its digest; unpatched\n", path, i);
      return false;
    }
    loaded.records.push_back(item);
  }
  std::sort(loaded.records.begin(), loaded.records.end(),
            [](const Record &a, const Record &b) { return a.offset < b.offset; });
  for (std::size_t i = 1; i < loaded.records.size(); ++i) {
    if (loaded.records[i - 1].offset + loaded.records[i - 1].length > loaded.records[i].offset) {
      std::fprintf(stderr, "[data-patch] %s: overlapping ranges; unpatched\n", path);
      return false;
    }
  }
  return true;
}

} // namespace

bool install_after_disc_open() {
  if (g_installed) return g_active;
  g_installed = true;
  const char *path = std::getenv("GT4_DATA_PATCH");
  if (!path || !*path) return false;
  if (!ps2::disc::is_open()) {
    std::fprintf(stderr, "[data-patch] %s: the disc image is not open; unpatched\n", path);
    return false;
  }
  Loaded loaded;
  if (!load_container(path, loaded)) return false;
  // Every original range must match the user's image before a single byte is replaced.
  for (const Record &record : loaded.records) {
    std::vector<std::uint8_t> original(record.length);
    if (!read_image(record.offset, original.data(), original.size())) {
      std::fprintf(stderr, "[data-patch] %s: cannot read 0x%llX from the image; unpatched\n", path,
                   static_cast<unsigned long long>(record.offset));
      return false;
    }
    std::uint8_t actual[32];
    if (!sha256(original.data(), original.size(), actual) ||
        std::memcmp(actual, record.original.data(), 32) != 0) {
      std::fprintf(stderr, "[data-patch] %s: image bytes at 0x%llX differ from the recipe; unpatched\n", path,
                   static_cast<unsigned long long>(record.offset));
      return false;
    }
  }
  g_records = std::move(loaded.records);
  g_payload = std::move(loaded.payload);
  g_active = true;
  std::fprintf(stderr, "[data-patch] %s: %zu range(s), %zu payload bytes, verified against the image\n", path,
               g_records.size(), g_payload.size());
  return true;
}

bool active() { return g_active; }

void apply(std::uint64_t offset, std::uint8_t *bytes, std::size_t size) {
  if (!g_active || size == 0) return;
  const std::uint64_t end = offset + size;
  for (const Record &record : g_records) {
    const std::uint64_t record_end = record.offset + record.length;
    if (offset >= record_end || record.offset >= end) continue;
    const std::uint64_t from = std::max(offset, record.offset);
    const std::uint64_t to = std::min(end, record_end);
    std::memcpy(bytes + (from - offset), g_payload.data() + record.payload_offset + (from - record.offset),
                static_cast<std::size_t>(to - from));
  }
}

bool install_for_test(std::uint64_t offset, const std::uint8_t *original, const std::uint8_t *replacement,
                      std::size_t size) {
  if (size == 0 || size > kRangeCap) return false;
  (void)original;
  g_payload.assign(replacement, replacement + size);
  g_records.assign(1, Record{offset, static_cast<std::uint32_t>(size), 0, {}});
  g_active = true;
  g_installed = true;
  return true;
}

void reset_for_test() {
  g_records.clear();
  g_payload.clear();
  g_active = false;
  g_installed = false;
}

} // namespace gt4::data_patch
