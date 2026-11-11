// Tourist Trophy: the host call that scripts/tt_units.py adds at the entry of TT's license cipher (0x004A4228,
// v2.00 func_00579E00): decrypt(a0 = destination, a1 = source, a2 = size, a3 = key), returns v0 = game check passed.
//
// The hook tries the exact, CRC-confirmed decrypt (tt_license_decrypt.hpp) on the cipher text the game passes. When it
// finds one, it writes the plaintext to the destination, sets v0 = 1 and returns to the caller the way the translated
// function returns (s0..s2 and sp untouched: the hook runs before the prologue). Otherwise the translated cipher runs as
// before and the TASK-017 guard protects the record copy. GT4_TT_LICENSE_EXACT=0 turns the hook off.
#include "tt_license_decrypt.hpp"

#include <ps2/cpu.hpp>
#include <ps2/memory.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gt4::tt_license {

namespace {
bool enabled() {
  static const bool value = [] {
    const char *setting = std::getenv("GT4_TT_LICENSE_EXACT");
    const bool on = setting == nullptr || std::strcmp(setting, "0") != 0;
    std::fprintf(stderr, "[tt-license] exact license demo decrypt %s (GT4_TT_LICENSE_EXACT)\n", on ? "on" : "off");
    return on;
  }();
  return value;
}

bool in_ram(std::uint32_t address, std::uint32_t size) {
  return address < ps2::kRamSize && size <= ps2::kRamSize - address;
}
}  // namespace

// Returns true when the decrypt was done here (the caller then returns from the translated function).
bool before_license_decrypt(ps2::Cpu &c) {
  if (!enabled()) return false;
  const std::uint32_t destination = c.r[4].w[0] & 0x1FFFFFFFu;
  const std::uint32_t source = c.r[5].w[0] & 0x1FFFFFFFu;
  const std::uint32_t size = c.r[6].w[0];
  const std::uint32_t key = c.r[7].w[0];
  if (size < 2 || size > SearchLimits{}.max_size || !in_ram(source, size) || !in_ram(destination, size)) {
    std::fprintf(stderr, "[tt-license] decrypt dst 0x%08X src 0x%08X size 0x%X outside the limits: game cipher\n",
                 destination, source, size);
    return false;
  }
  const std::vector<std::uint8_t> cipher(ps2::g_ram + source, ps2::g_ram + source + size);
  std::vector<std::uint8_t> plain;
  const auto start = std::chrono::steady_clock::now();
  const SearchResult result = exact_decrypt(cipher.data(), cipher.size(), key, plain);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (!result.found) {
    std::fprintf(stderr, "[tt-license] decrypt size 0x%X key 0x%08X: no CRC-confirmed chain (%.0f ms); game cipher\n", size,
                 key, ms);
    return false;
  }
  std::memcpy(ps2::g_ram + destination, plain.data(), size);
  c.r[2].d[0] = 1;  // the game's check passed
  c.pc = c.r[31].w[0];
  std::fprintf(stderr,
               "[tt-license] decrypt size 0x%X key 0x%08X: exact (CRC confirmed, saturation step %zu, %zu div.s one ulp up, "
               "%.0f ms)\n",
               size, key, result.saturation_step, result.plus_one_steps, ms);
  return true;
}

}  // namespace gt4::tt_license
