#pragma once

// Tourist Trophy license demos: exact host-side decrypt (docs/TT-STUDY.md, "License demos").
//
// TT keeps GT4's FPU-keyed stream cipher (TT 0x004A4228 = v2.00 func_00579E00; docs/LICENSE-DEMO-CIPHER.md). The key
// stream is the bit pattern of a chain of mul.s / div.s results, and our div.s is not bit-exact with the R5900 (TASK-063),
// so the game decrypts its license demos in the wrong key-stream phase. In TT the demo file carries the license setup too
// (the rider spawns off the course, as under PCSX2), so guarding the record count (TASK-017) is not enough.
//
// Confirmed on 52,469 CRC-confirmed GT4 steps (docs/journal/2026-10-16.md): every hardware div.s result here is the
// truncated quotient or one ulp above it, and mul.s equals the truncated product. This search walks that tree with a
// beam ordered by the measured priors (scripts/license_demo_oracle.py search_chain), and accepts a chain only when the
// game's own check passes: (crc32(plain) & 0xFFFFFF) | plain[0] << 24 == key. A file without such a chain inside the
// limits is left to the game (the TASK-017 guard still protects the record copy).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gt4::tt_license {

struct SearchLimits {
  std::size_t beam = 2000;              // candidates kept per step (the GT4 C prototype's width)
  std::size_t max_saturation_step = 400;  // longer chains were likely false CRC matches on GT4 (journal 2026-10-16)
  std::size_t max_size = 1u << 20;      // license demos are a few KiB
};

struct SearchResult {
  bool found = false;
  std::size_t saturation_step = 0;  // index of the first saturated state of the accepted chain
  std::size_t plus_one_steps = 0;   // div.s results taken one ulp above the truncated quotient
};

// The cipher's primitives (pure; scripts/license_demo_oracle.py names).
std::uint32_t initial_state(std::uint32_t key) noexcept;
std::uint8_t keystream_byte(std::uint32_t state) noexcept;
std::uint32_t divisor_of(std::uint32_t state) noexcept;
std::uint32_t ft_entry(std::uint8_t index) noexcept;
std::uint32_t mul_chop(std::uint32_t a, std::uint32_t b) noexcept;
std::uint32_t div_chop(std::uint32_t a, std::uint32_t b) noexcept;
std::uint32_t crc32(const std::uint8_t *data, std::size_t size) noexcept;
bool game_check(const std::uint8_t *plain, std::size_t size, std::uint32_t key) noexcept;

// Decrypt with an explicit per-step choice (plus_one[i] = div result of step i taken one ulp up); for tests and tools.
std::vector<std::uint8_t> decrypt_with_choices(const std::uint8_t *cipher, std::size_t size, std::uint32_t key,
                                               const std::vector<bool> &plus_one);
// Encrypt (the game's save-side counterpart) with the same choices; returns the key through `key`.
std::vector<std::uint8_t> encrypt_with_choices(const std::vector<std::uint8_t> &plain, const std::vector<bool> &plus_one,
                                               std::uint32_t &key);

// The CRC-confirmed search. On success `plain` holds the exact plaintext (same size as the cipher text).
SearchResult exact_decrypt(const std::uint8_t *cipher, std::size_t size, std::uint32_t key, std::vector<std::uint8_t> &plain,
                           const SearchLimits &limits = {});

}  // namespace gt4::tt_license
