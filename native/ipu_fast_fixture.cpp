// Copyright 2026 GT4Recomp contributors. Licensed under the MIT License (see LICENSE).
// IN-490: native/ipu_fast.hpp against the formulas of the donor's runtime/src/ipu/ipu.cpp, on random and sparse input. A
// difference of one bit fails. Run with --bench for the time of the three parts.
#include "ipu_fast.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

double g_by_x[8][8], g_by_u[8][8];

void build_cosines() {
  for (int x = 0; x < 8; ++x) {
    for (int u = 0; u < 8; ++u) {
      g_by_x[x][u] = (u ? 0.5 : 0.5 / std::sqrt(2.0)) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0);
      g_by_u[u][x] = g_by_x[x][u];
    }
  }
}

// The donor's idct.
void reference_idct(const int *in, std::int16_t *out) {
  double rows[8][8];
  for (int v = 0; v < 8; ++v) {
    for (int x = 0; x < 8; ++x) {
      double s = 0;
      for (int u = 0; u < 8; ++u) s += g_by_x[x][u] * in[v * 8 + u];
      rows[v][x] = s;
    }
  }
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      double s = 0;
      for (int v = 0; v < 8; ++v) s += g_by_x[y][v] * rows[v][x];
      out[y * 8 + x] = static_cast<std::int16_t>(std::clamp(static_cast<int>(std::floor(s + 0.5)), -256, 255));
    }
  }
}

// The donor's bits_at.
std::uint32_t reference_bits(const std::vector<std::uint8_t> &in, std::size_t bit, unsigned n) {
  if (!n) return 0;
  std::uint64_t v = 0;
  const std::size_t byte = bit >> 3;
  for (std::size_t i = 0; i < 8; ++i) v = (v << 8) | (byte + i < in.size() ? in[byte + i] : 0u);
  return static_cast<std::uint32_t>((v << (bit & 7u)) >> (64 - n));
}

// The donor's convert.
std::size_t reference_convert(const std::uint8_t *raw, bool rgb16, int th0, int th1, std::uint8_t *pixels) {
  std::size_t size = 0;
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      const int lum = 149 * std::max(0, raw[y * 16 + x] - 16);
      const int cb = raw[256 + (y / 2) * 8 + x / 2] - 128, cr = raw[320 + (y / 2) * 8 + x / 2] - 128;
      const int r = std::clamp((lum + 204 * cr + 64) >> 7, 0, 255);
      const int g = std::clamp((lum - 50 * cb - 104 * cr + 64) >> 7, 0, 255);
      const int b = std::clamp((lum + 258 * cb + 64) >> 7, 0, 255);
      int a = 0x80;
      if (r < th0 && g < th0 && b < th0) a = 0;
      else if (r < th1 && g < th1 && b < th1) a = 0x40;
      if (rgb16) {
        const std::uint16_t p = static_cast<std::uint16_t>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (a & 0x80 ? 0x8000 : 0));
        std::memcpy(pixels + size, &p, 2);
        size += 2;
      } else {
        const std::uint8_t p[4] = {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b),
                                   static_cast<std::uint8_t>(a)};
        std::memcpy(pixels + size, p, 4);
        size += 4;
      }
    }
  }
  return size;
}

std::uint64_t g_state = 0x1BADB002u;
std::uint32_t next_random() {
  g_state = g_state * 6364136223846793005ull + 1442695040888963407ull;
  return static_cast<std::uint32_t>(g_state >> 32);
}

void fill_block(int *in) {
  std::memset(in, 0, 64 * sizeof(int));
  const std::uint32_t shape = next_random() % 4u;
  const std::uint32_t count = shape == 0 ? 1u : shape == 1 ? 1u + next_random() % 6u : shape == 2 ? 1u + next_random() % 30u : 64u;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t at = count == 64u ? i : next_random() % 64u;
    in[at] = static_cast<int>(next_random() % 4096u) - 2048;
  }
}

int compare() {
  int failures = 0;
  int in[64];
  std::int16_t fast[64], reference[64];
  for (int round = 0; round < 200000; ++round) {
    fill_block(in);
    gt4::ipu_fast::idct(g_by_x, g_by_u, in, fast);
    reference_idct(in, reference);
    if (std::memcmp(fast, reference, sizeof fast) != 0 && ++failures <= 5) std::printf("idct differs at round %d\n", round);
  }
  // Every value of a block with only its first coefficient, zero included.
  std::memset(in, 0, sizeof in);
  for (int value = -2048; value < 2048; ++value) {
    in[0] = value;
    gt4::ipu_fast::idct(g_by_x, g_by_u, in, fast);
    reference_idct(in, reference);
    if (std::memcmp(fast, reference, sizeof fast) != 0 && ++failures <= 5) std::printf("idct differs for first coefficient %d\n", value);
  }

  std::vector<std::uint8_t> stream(4099);
  for (std::uint8_t &byte : stream) byte = static_cast<std::uint8_t>(next_random());
  for (int round = 0; round < 2000000; ++round) {
    const std::size_t bit = next_random() % (round % 8 == 0 ? 400u : stream.size() * 8u) + (round % 8 == 0 ? (stream.size() * 8u - 200u) : 0u);
    const unsigned n = next_random() % 33u;
    if (gt4::ipu_fast::read_bits(stream.data(), stream.size(), bit, n) != reference_bits(stream, bit, n) && ++failures <= 5) {
      std::printf("bits differ at %zu, %u\n", bit, n);
    }
  }

  std::uint8_t raw[384], pixels_fast[1024], pixels_reference[1024];
  for (int round = 0; round < 40000; ++round) {
    const std::uint32_t shape = next_random() % 3u;
    for (std::uint8_t &byte : raw) {
      const std::uint32_t r = next_random();
      byte = shape == 0 ? static_cast<std::uint8_t>(r) : shape == 1 ? static_cast<std::uint8_t>(r % 32u) : static_cast<std::uint8_t>(224u + r % 32u);
    }
    const int th0 = round % 4 == 0 ? 0 : static_cast<int>(next_random() % 512u), th1 = round % 4 == 0 ? 0 : static_cast<int>(next_random() % 512u);
    const bool rgb16 = (round & 1) != 0;
    const std::size_t a = gt4::ipu_fast::convert(raw, rgb16, th0, th1, pixels_fast);
    const std::size_t b = reference_convert(raw, rgb16, th0, th1, pixels_reference);
    if ((a != b || std::memcmp(pixels_fast, pixels_reference, a) != 0) && ++failures <= 5) std::printf("convert differs at round %d\n", round);
  }
  std::printf("ipu_fast: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}

int bench() {
  using clock = std::chrono::steady_clock;
  std::vector<std::array<int, 64>> blocks(4096);
  for (auto &block : blocks) {
    block.fill(0);
    const std::uint32_t count = 1u + next_random() % 10u;
    for (std::uint32_t i = 0; i < count; ++i) block[next_random() % 24u] = static_cast<int>(next_random() % 512u) - 256;
  }
  std::int16_t out[64];
  long long sink = 0;
  const auto t0 = clock::now();
  for (const auto &block : blocks) {
    reference_idct(block.data(), out);
    sink += out[5];
  }
  const auto t1 = clock::now();
  for (const auto &block : blocks) {
    gt4::ipu_fast::idct(g_by_x, g_by_u, block.data(), out);
    sink += out[5];
  }
  const auto t2 = clock::now();
  std::uint8_t raw[384], pixels[1024];
  for (std::uint8_t &byte : raw) byte = static_cast<std::uint8_t>(next_random());
  const auto t3 = clock::now();
  for (int i = 0; i < 4096; ++i) sink += reference_convert(raw, false, 0, 0, pixels);
  const auto t4 = clock::now();
  for (int i = 0; i < 4096; ++i) sink += gt4::ipu_fast::convert(raw, false, 0, 0, pixels);
  const auto t5 = clock::now();
  const auto us = [](auto d) { return std::chrono::duration<double, std::micro>(d).count() / 4096.0; };
  std::printf("us per block: idct donor %.3f fast %.3f; us per macroblock: convert donor %.3f fast %.3f (%lld)\n", us(t1 - t0), us(t2 - t1),
              us(t4 - t3), us(t5 - t4), sink);
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  build_cosines();
  if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) return bench();
  return compare();
}
