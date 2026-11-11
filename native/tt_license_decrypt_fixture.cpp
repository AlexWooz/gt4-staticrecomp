// CTest native_tt_license_decrypt: the exact license demo decrypt (tt_license_decrypt.hpp) on synthetic files encrypted the
// way the R5900 would: each div.s result is the truncated quotient or one ulp above it, chosen here by the remainder of the
// truncated division (the kind of rule the hardware follows; docs/journal/2026-10-16.md). The search must recover the
// plaintext byte for byte, report the game check as passed, and refuse a wrong key.
#include "tt_license_decrypt.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

using namespace gt4::tt_license;

std::uint32_t significand(std::uint32_t x) { return (x & 0x7FFFFFu) | 0x800000u; }

// "Hardware" choice: one ulp up when the truncated remainder is in its top 30% (quotient < 1) or top 20% (quotient >= 1).
bool hardware_plus_one(std::uint8_t cipher, std::uint32_t state) {
  const std::uint32_t product = mul_chop(ft_entry(cipher), state);
  const std::uint32_t divisor = divisor_of(state);
  const std::uint64_t a = significand(product), b = significand(divisor);
  const std::uint64_t numerator = a << (a >= b ? 23 : 24);
  const std::uint64_t tenth = (numerator % b) * 10 / b;
  return a >= b ? tenth >= 8 : tenth >= 7;
}

// Encrypts like the game's save side, with the hardware choices above; returns the cipher text and the key.
std::vector<std::uint8_t> hardware_encrypt(const std::vector<std::uint8_t> &plain, std::uint32_t &key, std::size_t &plus) {
  key = (crc32(plain.data(), plain.size()) & 0xFFFFFFu) | (static_cast<std::uint32_t>(plain[0]) << 24);
  std::vector<std::uint8_t> out(plain.size());
  std::uint32_t state = initial_state(key);
  plus = 0;
  for (std::size_t index = 0; index < plain.size(); ++index) {
    const std::uint8_t cipher = static_cast<std::uint8_t>(plain[index] ^ keystream_byte(state));
    out[index] = cipher;
    const bool up = hardware_plus_one(cipher, state);
    const std::uint32_t chop = div_chop(mul_chop(ft_entry(cipher), state), divisor_of(state));
    const bool can = (chop & 0x7FFFFFFFu) != 0 && (chop & 0x7FFFFFFFu) != 0x7FFFFFFFu;
    if (up && can) ++plus;
    state = up && can ? chop + 1u : chop;
  }
  return out;
}

// A demo-shaped body: size word, a magic and a name, zeros, a few records and one Recorder at 0x1A24.
std::vector<std::uint8_t> demo_body(std::uint32_t seed) {
  std::vector<std::uint8_t> body(0x1A24u + 4u + 0x110u, 0u);
  const std::uint32_t size = static_cast<std::uint32_t>(body.size());
  for (int byte = 0; byte < 4; ++byte) body[byte] = static_cast<std::uint8_t>(size >> (8 * byte));
  const char text[] = "TTMRaceLicense";
  for (std::size_t index = 0; index + 1 < sizeof text; ++index) body[4 + index] = static_cast<std::uint8_t>(text[index]);
  std::uint32_t value = seed;
  for (std::size_t index = 0x100; index < 0x1A24u; index += 4) {
    value = value * 1664525u + 1013904223u;
    if ((value >> 28) == 0) body[index] = static_cast<std::uint8_t>(value >> 8);
  }
  body[0x1A28] = 0xBF;
  body[0x1A29] = 0x05;
  body[0x1A2C] = 0x0E;
  body[0x1A2D] = 0x01;
  return body;
}

}  // namespace

int main() {
  // Primitives against the oracle's tables (scripts/license_demo_oracle.py) and known values.
  check(ft_entry(0) == 0x3F804010u && ft_entry(255) == 0x3F800000u + 511u * 0x4010u + 7u, "FT formula");
  // T8[s & 255] << 16 | T8[(s >> 8) & 255] << 8 | T8[(s >> 16) & 255] | 0x3F000000, T8 = 8-bit bit reversal.
  check(divisor_of(0x3F000001u) == 0x3F800000u, "divisor from bit-reversed state bytes (low byte)");
  check(divisor_of(0x00C00300u) == (0x3F000000u | (0xC0u << 8) | 0x03u), "divisor from bit-reversed state bytes (mid, high)");
  const char text[] = "123456789";
  check(crc32(reinterpret_cast<const std::uint8_t *>(text), 9) == 0xCBF43926u, "CRC-32 check value");
  check(mul_chop(0x3F800000u, 0x3FFFFFFFu) == 0x3FFFFFFFu, "mul.s 1.0 * x = x");
  check(div_chop(0x3F800000u, 0x3FFFFFFFu) == 0x3F000000u, "div.s chop of 1.0 / 0x3FFFFFFF");

  for (std::uint32_t seed = 1; seed <= 6; ++seed) {
    const std::vector<std::uint8_t> plain = demo_body(seed);
    std::uint32_t key = 0;
    std::size_t plus = 0;
    const std::vector<std::uint8_t> cipher = hardware_encrypt(plain, key, plus);
    // What our build's chop-only decrypt makes of it: wrong, and the game's check fails.
    const std::vector<std::uint8_t> chop_plain = decrypt_with_choices(cipher.data(), cipher.size(), key, {});
    if (plus) check(!game_check(chop_plain.data(), chop_plain.size(), key), "chop-only decrypt fails the game check");
    std::vector<std::uint8_t> recovered;
    const SearchResult result = exact_decrypt(cipher.data(), cipher.size(), key, recovered);
    check(result.found, "exact decrypt finds a chain");
    check(recovered == plain, "exact decrypt recovers the plaintext byte for byte");
    check(game_check(recovered.data(), recovered.size(), key), "recovered plaintext passes the game check");
    std::vector<std::uint8_t> refused;
    check(!exact_decrypt(cipher.data(), cipher.size(), key ^ 0x00000100u, refused).found, "wrong key refused");
    std::printf("seed %u: %zu div.s one ulp up in the hardware chain, found=%d saturation step %zu\n", seed, plus,
                result.found ? 1 : 0, result.saturation_step);
  }

  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("native_tt_license_decrypt: ok");
  return 0;
}
