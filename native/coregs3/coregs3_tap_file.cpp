// TASK-090 CoreGS 3.0: the `.gt4tap` reader. See coregs3_tap_file.hpp.
#include "coregs3_tap_file.hpp"

#include <cstdio>
#include <cstring>

#include "coregs3_tap.hpp"

namespace gt4::coregs3 {
namespace {

std::uint32_t read_u32(const std::uint8_t *data, std::size_t at) {
  return static_cast<std::uint32_t>(data[at]) | (static_cast<std::uint32_t>(data[at + 1]) << 8) |
         (static_cast<std::uint32_t>(data[at + 2]) << 16) | (static_cast<std::uint32_t>(data[at + 3]) << 24);
}

std::uint64_t read_u64(const std::uint8_t *data, std::size_t at) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) value |= static_cast<std::uint64_t>(data[at + index]) << (8 * index);
  return value;
}

}  // namespace

bool read_tap_file(const std::string &path, std::vector<std::uint8_t> &data, std::vector<TapFileRecord> &records,
                   std::string &error) {
  records.clear();
  std::FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    error = "cannot open " + path;
    return false;
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    error = "cannot seek " + path;
    return false;
  }
  const long size = std::ftell(file);
  if (size < 0) {
    std::fclose(file);
    error = "cannot size " + path;
    return false;
  }
  if (std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    error = "cannot rewind " + path;
    return false;
  }
  data.resize(static_cast<std::size_t>(size));
  const bool read_ok = data.empty() || std::fread(data.data(), 1, data.size(), file) == data.size();
  std::fclose(file);
  if (!read_ok) {
    error = "cannot read " + path;
    return false;
  }
  if (data.size() < kTapFileHeaderBytes) {
    error = "tap file is shorter than its header";
    return false;
  }
  if (read_u32(data.data(), 0) != kTapMagic) {
    error = "not a .gt4tap file (bad magic)";
    return false;
  }
  if (read_u32(data.data(), 4) != kTapVersion) {
    error = "unsupported tap version " + std::to_string(read_u32(data.data(), 4));
    return false;
  }
  std::size_t at = kTapFileHeaderBytes;
  while (at + 24u <= data.size()) {
    const std::uint32_t bytes = read_u32(data.data(), at);
    if (bytes > data.size() - (at + 24u)) {
      error = "tap record runs past the end of the file";
      records.clear();
      return false;
    }
    TapFileRecord record;
    record.bytes = bytes;
    record.kind = read_u32(data.data(), at + 4);
    record.job = read_u64(data.data(), at + 8);
    record.vblank = read_u64(data.data(), at + 16);
    record.offset = at + 24u;
    if (record.kind == kTapKindSource && bytes >= 8u) {
      record.address = read_u32(data.data(), record.offset);
      record.qwc = read_u32(data.data(), record.offset + 4u);
    }
    records.push_back(record);
    at = record.offset + bytes;
  }
  if (at != data.size()) {
    error = "tap file has a trailing partial record";
    records.clear();
    return false;
  }
  return true;
}

}  // namespace gt4::coregs3
