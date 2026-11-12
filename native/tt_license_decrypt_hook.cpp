// Tourist Trophy: the host call that scripts/tt_units.py adds at the entry of TT's license cipher (0x004A4228,
// v2.00 func_00579E00): decrypt(a0 = destination, a1 = source, a2 = size, a3 = key), returns v0 = game check passed.
//
// The hook tries the exact, CRC-confirmed decrypt (tt_license_decrypt.hpp) on the cipher text the game passes. When it
// finds one, it writes the plaintext to the destination, sets v0 = 1 and returns to the caller the way the translated
// function returns (s0..s2 and sp untouched: the hook runs before the prologue). Otherwise the translated cipher runs as
// before and the TASK-017 guard protects the record copy. GT4_TT_LICENSE_EXACT=0 turns the hook off.
//
// The first search uses the default limits (beam 2000, saturation by step 400). A file without a chain there is searched
// again ten times wider (beam 20,000, saturation by step 600: GT4's second pass recovered more files that way, journal
// 2026-10-16). Because a later saturation step leaves more room for a false 24-bit CRC match, a chain from the wide pass is
// only used when the TASK-017 guard's own layout rule holds too (n = 0 and one Recorder ending at the body end, at TT's
// Manager offset). Every result, a failure included, is kept for the session: the game loads the same file several times.
// GT4_TT_LICENSE_DUMP=<folder> writes the cipher text of every file searched (solved or not), for offline study of the search.
#include "tt_license_decrypt.hpp"

#include "license_guard.hpp"

#include <ps2/cpu.hpp>
#include <ps2/memory.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace gt4::tt_license {

namespace {
using CacheKey = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;  // key, size, CRC-32 of the cipher text

std::mutex g_cache_mutex;
std::map<CacheKey, std::optional<std::vector<std::uint8_t>>> g_cache;

// GT4_TT_LICENSE_DUMP=<folder>: the cipher text of every file searched this session, named by key and size and marked
// solved or unsolved (offline study of the search; game data, never committed).
void dump_cipher(const std::vector<std::uint8_t> &cipher, std::uint32_t key, bool solved) {
  const char *folder = std::getenv("GT4_TT_LICENSE_DUMP");
  if (folder == nullptr || *folder == '\0') return;
  char name[96];
  std::snprintf(name, sizeof name, "\\tt-license-%s-%08X-%X.bin", solved ? "solved" : "unsolved", key,
                static_cast<unsigned>(cipher.size()));
  const std::string path = std::string(folder) + name;
  if (std::FILE *file = std::fopen(path.c_str(), "wb")) {
    std::fwrite(cipher.data(), 1, cipher.size(), file);
    std::fclose(file);
    std::fprintf(stderr, "[tt-license] cipher text written to %s\n", path.c_str());
  }
}

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
  const CacheKey cache_key{key, size, crc32(cipher.data(), cipher.size())};
  std::optional<std::vector<std::uint8_t>> plain;
  bool cached = false;
  {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    if (const auto found = g_cache.find(cache_key); found != g_cache.end()) {
      plain = found->second;
      cached = true;
    }
  }
  if (!cached) {
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> candidate;
    SearchResult result = exact_decrypt(cipher.data(), cipher.size(), key, candidate);
    const char *pass = "default";
    if (!result.found) {
      SearchLimits wide;
      wide.beam = 20000;
      wide.max_saturation_step = 600;
      result = exact_decrypt(cipher.data(), cipher.size(), key, candidate, wide);
      pass = "wide";
      // A late chain must also satisfy the guard's independent layout rule before the game sees it.
      if (result.found && !::gt4::license_guard::evaluate(candidate.data(), candidate.size(), key).layout_ok) {
        std::fprintf(stderr, "[tt-license] decrypt size 0x%X key 0x%08X: wide-pass chain fails the layout check; refused\n",
                     size, key);
        result.found = false;
      }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (result.found) {
      plain = std::move(candidate);
      std::fprintf(stderr,
                   "[tt-license] decrypt size 0x%X key 0x%08X: exact (%s pass, CRC confirmed, saturation step %zu, %zu div.s "
                   "one ulp up, %.0f ms)\n",
                   size, key, pass, result.saturation_step, result.plus_one_steps, ms);
    } else {
      std::fprintf(stderr, "[tt-license] decrypt size 0x%X key 0x%08X: no CRC-confirmed chain (%.0f ms); game cipher\n",
                   size, key, ms);
    }
    dump_cipher(cipher, key, result.found);
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_cache[cache_key] = plain;
  }
  if (!plain) return false;
  std::memcpy(ps2::g_ram + destination, plain->data(), size);
  c.r[2].d[0] = 1;  // the game's check passed
  c.pc = c.r[31].w[0];
  if (cached) std::fprintf(stderr, "[tt-license] decrypt size 0x%X key 0x%08X: exact (from this session's cache)\n", size, key);
  return true;
}

}  // namespace gt4::tt_license
