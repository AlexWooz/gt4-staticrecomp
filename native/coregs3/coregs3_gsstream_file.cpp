// TASK-090 CoreGS 3.0, phase S1: the `.gt4gs` reader. See coregs3_gsstream_file.hpp.
#include "coregs3_gsstream_file.hpp"

#include <cstdio>
#include <cstring>

#include "coregs3_gsstream.hpp"

namespace gt4::coregs3 {
namespace {

constexpr std::size_t kRecordHeader = kGsStreamRecordHeaderBytes;

std::uint16_t read_u16(const std::uint8_t *data, std::size_t at) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[at]) |
                                    (static_cast<std::uint16_t>(data[at + 1]) << 8));
}

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

bool read_gs_stream_file(const std::string &path, std::vector<std::uint8_t> &data, GsStreamFileHeader &header,
                         std::vector<GsStreamFileRecord> &records, std::string &error) {
  records.clear();
  header = GsStreamFileHeader{};
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
  if (data.size() < kGsStreamHeaderBytes) {
    error = "GS stream file is shorter than its header";
    return false;
  }
  if (read_u32(data.data(), 0) != kGsStreamMagic) {
    error = "not a .gt4gs file (bad magic)";
    return false;
  }
  header.version = read_u32(data.data(), 4);
  header.target = read_u32(data.data(), 8);
  header.flags = read_u32(data.data(), 12);
  header.first_vblank = read_u64(data.data(), 16);
  if (header.version != kGsStreamVersion) {
    error = "unsupported GS stream version " + std::to_string(header.version);
    return false;
  }
  if (header.target != kCaptureTargetV200) {
    error = "GS stream target " + std::to_string(header.target) + " is not the captured target";
    return false;
  }
  if (header.flags != 0) {
    error = "GS stream version 1 has no flags; got 0x" + std::to_string(header.flags);
    return false;
  }
  if (read_u64(data.data(), 24) != 0) {
    error = "GS stream header reserved word is nonzero";
    return false;
  }
  std::size_t at = kGsStreamHeaderBytes;
  while (at + kRecordHeader <= data.size()) {
    const std::uint32_t bytes = read_u32(data.data(), at);
    GsStreamFileRecord record;
    record.kind = read_u16(data.data(), at + 4);
    record.path = data[at + 6];
    record.flags = data[at + 7];
    record.pass = read_u32(data.data(), at + 8);
    record.sequence = read_u32(data.data(), at + 12);
    record.vblank = read_u64(data.data(), at + 16);
    record.last_job = read_u64(data.data(), at + 24);
    record.source_pc = read_u32(data.data(), at + 32);
    record.source_ra = read_u32(data.data(), at + 36);
    record.qwc = read_u32(data.data(), at + 40);
    record.bytes = bytes;
    record.offset = at + kRecordHeader;
    if (read_u32(data.data(), at + 44) != 0) {
      error = "GS stream record reserved word is nonzero";
      records.clear();
      return false;
    }
    if ((record.flags & ~(kGsStreamFlagSignals | kGsStreamFlagRefused)) != 0) {
      error = "GS stream record flags 0x" + std::to_string(record.flags) + " are not defined in version 1";
      records.clear();
      return false;
    }
    if (bytes % 16u != 0) {
      error = "GS stream record payload is not a multiple of 16 bytes";
      records.clear();
      return false;
    }
    if (bytes > data.size() - (at + kRecordHeader)) {
      error = "GS stream record runs past the end of the file";
      records.clear();
      return false;
    }
    switch (record.kind) {
      case kGsStreamKindGif:
        if (record.path < 1 || record.path > 3) {
          error = "GIF record path " + std::to_string(record.path) + " is outside 1..3";
          records.clear();
          return false;
        }
        if (static_cast<std::uint64_t>(record.qwc) * 16u != bytes) {
          error = "GIF record qwc does not match its payload bytes";
          records.clear();
          return false;
        }
        if (record.pass >= kPassCount) {
          error = "GIF record pass " + std::to_string(record.pass) + " is outside the PassId table";
          records.clear();
          return false;
        }
        break;
      case kGsStreamKindPassBegin:
      case kGsStreamKindPassEnd:
        if (record.path != 0 || bytes != kGsStreamPassPayloadBytes) {
          error = "pass record has the wrong path or payload size";
          records.clear();
          return false;
        }
        if (record.pass >= kPassCount) {
          error = "pass record pass " + std::to_string(record.pass) + " is outside the PassId table";
          records.clear();
          return false;
        }
        break;
      case kGsStreamKindFrameBegin:
      case kGsStreamKindFrameEnd:
        if (record.path != 0 || bytes != 32u || record.pass != 0) {
          error = "frame record has the wrong path, pass or payload size";
          records.clear();
          return false;
        }
        break;
      case kGsStreamKindOverflow:
        if (record.path != 0 || bytes != 32u || record.pass != 0) {
          error = "overflow record has the wrong path, pass or payload size";
          records.clear();
          return false;
        }
        break;
      case kGsStreamKindGsState:
      case kGsStreamKindPrivileged:
        error = "GS stream kind " + std::to_string(record.kind) + " is reserved for a later version";
        records.clear();
        return false;
      default:
        error = "GS stream record kind " + std::to_string(record.kind) + " is not a version 1 kind";
        records.clear();
        return false;
    }
    records.push_back(record);
    at = record.offset + bytes;
  }
  if (at != data.size()) {
    error = "GS stream file has a trailing partial record";
    records.clear();
    return false;
  }
  return true;
}

bool gs_stream_pass_words(const std::vector<std::uint8_t> &data, const GsStreamFileRecord &record,
                          std::uint64_t (&words)[9]) noexcept {
  if ((record.kind != kGsStreamKindPassBegin && record.kind != kGsStreamKindPassEnd) ||
      record.bytes < kGsStreamPassPayloadBytes || record.offset + record.bytes > data.size())
    return false;
  for (std::size_t index = 0; index < 9; ++index) words[index] = read_u64(data.data(), record.offset + index * 8u);
  return true;
}

bool gs_stream_frame_totals(const std::vector<std::uint8_t> &data, const GsStreamFileRecord &record,
                            std::uint32_t &frame_index, std::uint64_t &path2_bytes,
                            std::uint64_t &path3_bytes) noexcept {
  if ((record.kind != kGsStreamKindFrameBegin && record.kind != kGsStreamKindFrameEnd) || record.bytes < 32u ||
      record.offset + record.bytes > data.size())
    return false;
  frame_index = read_u32(data.data(), record.offset);
  path2_bytes = read_u64(data.data(), record.offset + 8u);
  path3_bytes = read_u64(data.data(), record.offset + 16u);
  return true;
}

bool gs_stream_overflow_totals(const std::vector<std::uint8_t> &data, const GsStreamFileRecord &record,
                               std::uint64_t &refused_bytes, std::uint64_t &refused_records,
                               std::uint32_t &cause_bits) noexcept {
  if (record.kind != kGsStreamKindOverflow || record.bytes < 32u || record.offset + record.bytes > data.size())
    return false;
  refused_bytes = read_u64(data.data(), record.offset);
  refused_records = read_u64(data.data(), record.offset + 8u);
  cause_bits = read_u32(data.data(), record.offset + 16u);
  return true;
}

}  // namespace gt4::coregs3
