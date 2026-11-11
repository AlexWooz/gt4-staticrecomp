// Tourist Trophy license demos: exact host-side decrypt (tt_license_decrypt.hpp). A C++ port of the CRC-confirmed beam
// search of scripts/license_demo_oracle.py (search_chain, priors measured on GT4's confirmed chains, 2026-10-16).
#include "tt_license_decrypt.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace gt4::tt_license {
namespace {

constexpr std::uint32_t kSign = 0x80000000u;
constexpr std::uint32_t kAbs = 0x7FFFFFFFu;

constexpr std::array<std::uint32_t, 256> make_crc_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256; ++index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) value = (value & 1u) ? (value >> 1) ^ 0xEDB88320u : value >> 1;
    table[index] = value;
  }
  return table;
}
constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();

// CRC register in its running (pre-inverted) form, as zlib's crc32(data, previous) chains it.
inline std::uint32_t crc_step(std::uint32_t reg, std::uint8_t byte) noexcept {
  return kCrcTable[(reg ^ byte) & 0xFFu] ^ (reg >> 8);
}

inline std::uint32_t exponent(std::uint32_t x) noexcept { return (x >> 23) & 0xFFu; }
inline std::uint32_t significand(std::uint32_t x) noexcept { return (x & 0x7FFFFFu) | 0x800000u; }

inline std::uint32_t pack(std::uint32_t sign, std::int32_t biased_exponent, std::uint64_t mantissa) noexcept {
  if (biased_exponent > 255) return sign | kAbs;
  if (biased_exponent <= 0) return sign;
  return sign | (static_cast<std::uint32_t>(biased_exponent) << 23) | (static_cast<std::uint32_t>(mantissa) & 0x7FFFFFu);
}

inline std::uint8_t bit_reverse(std::uint32_t value) noexcept {
  std::uint8_t out = 0;
  for (int bit = 0; bit < 8; ++bit) out = static_cast<std::uint8_t>(out | (((value >> bit) & 1u) << (7 - bit)));
  return out;
}

// Empirical P(hardware div.s = truncated quotient + 1 ulp) by (quotient >= 1, tenth of the truncated remainder).
constexpr double kPlusOnePrior[2][10] = {
    {0.0, 0.0, 0.02, 0.15, 0.43, 0.63, 0.79, 0.93, 0.99, 1.0},  // quotient < 1
    {0.0, 0.0, 0.0, 0.0, 0.0, 0.03, 0.21, 0.57, 0.90, 0.99},    // quotient >= 1
};

double byte_log_prior(std::uint8_t value) noexcept {
  if (value == 0) return std::log(0.25);
  if ((value >= 0x20 && value < 0x7F) || value < 0x10 || value == 0xFF) return std::log(0.004);
  return std::log(0.0018);
}

struct Entry {
  double score;
  std::uint32_t state;
  std::uint32_t crc;     // running CRC register through the plaintext so far
  std::int32_t parent;   // index in the previous layer
};

}  // namespace

std::uint32_t initial_state(std::uint32_t key) noexcept { return (key & 0xFFFFFFu) | 0x3F000000u; }

std::uint8_t keystream_byte(std::uint32_t state) noexcept {
  return static_cast<std::uint8_t>((state ^ (state >> 8) ^ (state >> 16)) & 0xFFu);
}

std::uint32_t divisor_of(std::uint32_t state) noexcept {
  return (static_cast<std::uint32_t>(bit_reverse(state & 0xFFu)) << 16) |
         (static_cast<std::uint32_t>(bit_reverse((state >> 8) & 0xFFu)) << 8) | bit_reverse((state >> 16) & 0xFFu) |
         0x3F000000u;
}

std::uint32_t ft_entry(std::uint8_t index) noexcept {
  return 0x3F800000u + (2u * index + 1u) * 0x4010u + (static_cast<std::uint32_t>(index) >> 5);
}

std::uint32_t mul_chop(std::uint32_t a, std::uint32_t b) noexcept {
  const std::uint32_t sign = (a ^ b) & kSign;
  if (!exponent(a) || !exponent(b)) return sign;
  const std::uint64_t product = static_cast<std::uint64_t>(significand(a)) * significand(b);
  const std::int32_t biased = static_cast<std::int32_t>(exponent(a) + exponent(b)) - 127;
  if (product >> 47) return pack(sign, biased + 1, product >> 24);
  return pack(sign, biased, product >> 23);
}

namespace {
struct DivParts {
  bool special;
  std::uint32_t value;      // the special result
  std::uint32_t sign;
  std::uint64_t dividend;   // significands
  std::uint64_t divisor;
  std::int32_t biased;
};
DivParts div_parts(std::uint32_t a, std::uint32_t b) noexcept {
  const std::uint32_t sign = (a ^ b) & kSign;
  if (!exponent(b)) return {true, sign | kAbs, sign, 0, 0, 0};
  if (!exponent(a)) return {true, sign, sign, 0, 0, 0};
  return {false, 0, sign, significand(a), significand(b), static_cast<std::int32_t>(exponent(a)) -
                                                              static_cast<std::int32_t>(exponent(b)) + 127};
}
std::uint32_t finish_quotient(std::uint32_t sign, std::uint64_t units, std::int32_t biased) noexcept {
  if (units >= (1ull << 24)) return pack(sign, biased, units >> 1);
  return pack(sign, biased - 1, units);
}
}  // namespace

std::uint32_t div_chop(std::uint32_t a, std::uint32_t b) noexcept {
  const DivParts p = div_parts(a, b);
  if (p.special) return p.value;
  return finish_quotient(p.sign, (p.dividend << 24) / p.divisor, p.biased);
}

std::uint32_t crc32(const std::uint8_t *data, std::size_t size) noexcept {
  std::uint32_t reg = 0xFFFFFFFFu;
  for (std::size_t index = 0; index < size; ++index) reg = crc_step(reg, data[index]);
  return reg ^ 0xFFFFFFFFu;
}

bool game_check(const std::uint8_t *plain, std::size_t size, std::uint32_t key) noexcept {
  if (!size) return false;
  return ((crc32(plain, size) & 0xFFFFFFu) | (static_cast<std::uint32_t>(plain[0]) << 24)) == key;
}

namespace {
inline std::uint32_t step(std::uint8_t cipher, std::uint32_t state, bool plus_one) noexcept {
  const std::uint32_t chop = div_chop(mul_chop(ft_entry(cipher), state), divisor_of(state));
  return plus_one && (chop & kAbs) != 0 && (chop & kAbs) != kAbs ? chop + 1u : chop;
}
}  // namespace

std::vector<std::uint8_t> decrypt_with_choices(const std::uint8_t *cipher, std::size_t size, std::uint32_t key,
                                               const std::vector<bool> &plus_one) {
  std::vector<std::uint8_t> out(size);
  std::uint32_t state = initial_state(key);
  for (std::size_t index = 0; index < size; ++index) {
    out[index] = static_cast<std::uint8_t>(cipher[index] ^ keystream_byte(state));
    state = step(cipher[index], state, index < plus_one.size() && plus_one[index]);
  }
  return out;
}

std::vector<std::uint8_t> encrypt_with_choices(const std::vector<std::uint8_t> &plain, const std::vector<bool> &plus_one,
                                               std::uint32_t &key) {
  key = (crc32(plain.data(), plain.size()) & 0xFFFFFFu) | (plain.empty() ? 0u : static_cast<std::uint32_t>(plain[0]) << 24);
  std::vector<std::uint8_t> out(plain.size());
  std::uint32_t state = initial_state(key);
  for (std::size_t index = 0; index < plain.size(); ++index) {
    const std::uint8_t cipher = static_cast<std::uint8_t>(plain[index] ^ keystream_byte(state));
    out[index] = cipher;
    state = step(cipher, state, index < plus_one.size() && plus_one[index]);
  }
  return out;
}

SearchResult exact_decrypt(const std::uint8_t *cipher, std::size_t size, std::uint32_t key, std::vector<std::uint8_t> &plain,
                           const SearchLimits &limits) {
  SearchResult result;
  if (size < 2 || size > limits.max_size || limits.beam == 0) return result;
  const std::uint8_t first = static_cast<std::uint8_t>(cipher[0] ^ keystream_byte(initial_state(key)));
  std::vector<std::vector<Entry>> layers;
  layers.push_back({Entry{0.0, initial_state(key), crc_step(0xFFFFFFFFu, first), -1}});
  // Plaintext tail from a saturated state at `index` (deterministic once the state is 0x7FFFFFFF), cached per index.
  std::unordered_map<std::size_t, std::vector<std::uint8_t>> tails;
  bool have_best = false;
  double best_score = 0.0;
  std::size_t best_layer = 0, best_entry = 0, best_index = 0;
  std::vector<Entry> candidates;
  for (std::size_t index = 0; index + 1 < size; ++index) {
    if (index > limits.max_saturation_step) break;  // no accepted chain saturates later than this
    candidates.clear();
    const std::vector<Entry> &layer = layers.back();
    for (std::size_t entry_index = 0; entry_index < layer.size(); ++entry_index) {
      const Entry &entry = layer[entry_index];
      if (entry.state == kAbs) {
        auto tail = tails.find(index);
        if (tail == tails.end()) {
          std::vector<std::uint8_t> rest;
          rest.reserve(size - index - 1);
          std::uint32_t tail_state = kAbs;
          for (std::size_t position = index; position + 1 < size; ++position) {
            tail_state = div_chop(mul_chop(ft_entry(cipher[position]), tail_state), divisor_of(tail_state));
            rest.push_back(static_cast<std::uint8_t>(cipher[position + 1] ^ keystream_byte(tail_state)));
          }
          tail = tails.emplace(index, std::move(rest)).first;
        }
        std::uint32_t reg = entry.crc;
        for (std::uint8_t byte : tail->second) reg = crc_step(reg, byte);
        const std::uint32_t final_crc = reg ^ 0xFFFFFFFFu;
        if (((final_crc & 0xFFFFFFu) | (static_cast<std::uint32_t>(first) << 24)) == key &&
            (!have_best || entry.score > best_score)) {
          have_best = true;
          best_score = entry.score;
          best_layer = layers.size() - 1;
          best_entry = entry_index;
          best_index = index;
        }
        continue;
      }
      const std::uint8_t c = cipher[index];
      const std::uint32_t product = mul_chop(ft_entry(c), entry.state);
      const std::uint32_t divisor = divisor_of(entry.state);
      const std::uint32_t chop = div_chop(product, divisor);
      const std::uint64_t dividend_bits = significand(product), divisor_bits = significand(divisor);
      const std::uint64_t numerator = dividend_bits << (dividend_bits >= divisor_bits ? 23 : 24);
      const std::size_t tenth = static_cast<std::size_t>(std::min<std::uint64_t>(9, (numerator % divisor_bits) * 10 / divisor_bits));
      double plus = kPlusOnePrior[dividend_bits >= divisor_bits ? 1 : 0][tenth];
      plus = std::min(std::max(plus, 0.004), 0.996);
      const auto push = [&](std::uint32_t next, double probability) {
        const std::uint8_t byte = static_cast<std::uint8_t>(cipher[index + 1] ^ keystream_byte(next));
        candidates.push_back(Entry{entry.score + std::log(probability) + byte_log_prior(byte), next,
                                   crc_step(entry.crc, byte), static_cast<std::int32_t>(entry_index)});
      };
      push(chop, 1.0 - plus);
      if ((chop & kAbs) != 0 && (chop & kAbs) != kAbs) push(chop + 1u, plus);
    }
    if (candidates.empty()) break;
    if (candidates.size() > limits.beam) {
      std::nth_element(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(limits.beam), candidates.end(),
                       [](const Entry &a, const Entry &b) { return a.score > b.score; });
      candidates.resize(limits.beam);
    }
    layers.push_back(candidates);
  }
  if (!have_best || best_index > limits.max_saturation_step) return result;
  // Walk the accepted chain back to its states, then rebuild the plaintext the way the game would on hardware.
  std::vector<std::uint32_t> states;
  std::size_t layer_index = best_layer, entry_index = best_entry;
  while (layer_index > 0) {
    const Entry &entry = layers[layer_index][entry_index];
    states.push_back(entry.state);
    entry_index = static_cast<std::size_t>(entry.parent);
    --layer_index;
  }
  std::reverse(states.begin(), states.end());
  plain.assign(size, 0);
  std::uint32_t state = initial_state(key);
  for (std::size_t index = 0; index < size; ++index) {
    plain[index] = static_cast<std::uint8_t>(cipher[index] ^ keystream_byte(state));
    if (index < states.size()) {
      const std::uint32_t chop = div_chop(mul_chop(ft_entry(cipher[index]), state), divisor_of(state));
      if (states[index] != chop) ++result.plus_one_steps;
      state = states[index];
    } else {
      state = div_chop(mul_chop(ft_entry(cipher[index]), state), divisor_of(state));
    }
  }
  if (!game_check(plain.data(), size, key)) return result;  // never hand back anything the game's own check rejects
  result.found = true;
  result.saturation_step = best_index;
  return result;
}

}  // namespace gt4::tt_license
