// TASK-090 CoreGS 3.0: the stored-DEFLATE PNG writer. See coregs3_png.hpp.
#include "coregs3_png.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gt4::coregs3 {
namespace {

std::uint32_t crc_table_entry(std::uint32_t index) {
  std::uint32_t crc = index;
  for (int bit = 0; bit < 8; ++bit) crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
  return crc;
}

const std::array<std::uint32_t, 256> &crc_table() {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> values{};
    for (std::uint32_t index = 0; index < 256; ++index) values[index] = crc_table_entry(index);
    return values;
  }();
  return table;
}

std::uint32_t crc32(const std::uint8_t *data, std::size_t size, std::uint32_t seed = 0xFFFFFFFFu) {
  std::uint32_t crc = seed;
  for (std::size_t index = 0; index < size; ++index) crc = crc_table()[(crc ^ data[index]) & 0xFFu] ^ (crc >> 8);
  return crc;
}

std::uint32_t adler32(const std::uint8_t *data, std::size_t size) {
  std::uint32_t a = 1, b = 0;
  for (std::size_t index = 0; index < size; ++index) {
    a = (a + data[index]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

void put_chunk(std::vector<std::uint8_t> &out, const char type[4], const std::vector<std::uint8_t> &data) {
  put_u32(out, static_cast<std::uint32_t>(data.size()));
  const std::size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  put_u32(out, crc32(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu);
}

}  // namespace

bool write_png_rgba(const std::string &path, std::uint32_t width, std::uint32_t height,
                    const std::uint8_t *rgba) {
  if (width == 0 || height == 0 || rgba == nullptr) return false;

  // Raw scanlines: one filter byte (0 = none) then the row.
  std::vector<std::uint8_t> raw;
  raw.reserve(static_cast<std::size_t>(height) * (1u + width * 4u));
  for (std::uint32_t row = 0; row < height; ++row) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba + static_cast<std::size_t>(row) * width * 4u,
               rgba + static_cast<std::size_t>(row + 1) * width * 4u);
  }

  std::vector<std::uint8_t> zlib;
  zlib.push_back(0x78);  // CMF: deflate, 32 KiB window
  zlib.push_back(0x01);  // FLG: no dictionary, fastest
  std::size_t at = 0;
  while (at < raw.size()) {
    const std::size_t block = std::min<std::size_t>(65535u, raw.size() - at);
    const bool final = at + block == raw.size();
    zlib.push_back(final ? 1u : 0u);
    zlib.push_back(static_cast<std::uint8_t>(block & 0xFFu));
    zlib.push_back(static_cast<std::uint8_t>((block >> 8) & 0xFFu));
    zlib.push_back(static_cast<std::uint8_t>((~block) & 0xFFu));
    zlib.push_back(static_cast<std::uint8_t>(((~block) >> 8) & 0xFFu));
    zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(at),
                raw.begin() + static_cast<std::ptrdiff_t>(at + block));
    at += block;
  }
  put_u32(zlib, adler32(raw.data(), raw.size()));

  std::vector<std::uint8_t> file = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<std::uint8_t> ihdr;
  put_u32(ihdr, width);
  put_u32(ihdr, height);
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(6);  // colour type: RGBA
  ihdr.push_back(0);  // compression
  ihdr.push_back(0);  // filter
  ihdr.push_back(0);  // interlace
  put_chunk(file, "IHDR", ihdr);
  put_chunk(file, "IDAT", zlib);
  put_chunk(file, "IEND", {});

  std::FILE *out = std::fopen(path.c_str(), "wb");
  if (out == nullptr) return false;
  const bool written = std::fwrite(file.data(), 1, file.size(), out) == file.size();
  std::fclose(out);
  return written;
}

}  // namespace gt4::coregs3
