#pragma once

// TASK-017 license demo guard (slice205). Game-specific, narrow mitigation, not an FPU fix.
//
// GT4 decrypts each replay/license demo in place with an FPU-keyed stream cipher (v2.00 func_00579E00 via
// func_0044D278; Spec II 0x00459E90 via 0x002E3CE8) inside two license demo loaders (v2.00 0x001100D0 and 0x00110230,
// Spec II 0x0010CA18 and 0x0010CBA8; both build "license/%s"). Neither checks the cipher's CRC result. The first loader
// then calls this->vtbl[154] (v2.00 RaceSolitaire::loadDemoInputs 0x003EAD18; Spec II +0x510 -> 0x002012C8); the second
// calls this->vtbl[57] (v2.00 RaceSolitaire::loadReplayInputs 0x003EAD58, RaceNetRallyBattle virtual_56 0x003D5F00,
// ...). Every one of them passes the decrypted body (or a pointer into it) to AutomobileControlRecord::Manager::load,
// which trusts the record count n there (v2.00 body + 0x1AA4; Spec II the virtual adds 0x1A24 to a pointer the loader
// hands it 0x80 into the body). Our mul.s/div.s are not bit-exact with the
// R5900, so about half the US demos decrypt in the wrong key-stream phase, n reads 0x7F7F7F7F and the
// record copy sweeps 64 KiB slots through RAM (crash). Confirmed: docs/journal/2026-10-16.md; the Spec II offsets and
// the shared demo format are in docs/SPEC2-PLAN.md (S8b).
//
// The guard runs at the indirect call from either loader into its demo-input virtual (keyed on the call's
// return address, whatever the target). When the decrypted body
// fails the game's own CRC and is not the single-Recorder layout, it writes n = 0xFFFFFFFF (negative) at the Manager
// address the virtual itself will use:
// Manager::load then takes its own existing path for n < 0 (state 1, the record loop runs zero times),
// so the demo has no inputs and the license itself stays playable. This mimics the owner's PCSX2
// observation (the demo starts loading, then returns to the license menu). Files that decrypt to a
// sane layout, and every file whose CRC passes, are never touched.

#include <cstddef>
#include <cstdint>

#include "game_target.hpp"

namespace gt4::license_guard {

// Guest addresses (Confirmed from the translated units and the gt4-decomp symbols). TASK-100 S3: they come from the
// per-target table; a target without the guard gets zeros, so no loader call site is ever guarded there.
inline constexpr std::uint32_t kLoadDemoInputs = ::gt4::target::active().hooks.license_load_demo_inputs;
inline constexpr std::uint32_t kLoaderEntry = ::gt4::target::active().hooks.license_loader_entry;
inline constexpr std::uint32_t kLoaderReturn = ::gt4::target::active().hooks.license_loader_return;
inline constexpr std::uint32_t kReplayLoaderEntry = ::gt4::target::active().hooks.license_replay_loader_entry;
inline constexpr std::uint32_t kReplayLoaderReturn = ::gt4::target::active().hooks.license_replay_loader_return;
inline constexpr std::uint32_t kCrcTableAddress = ::gt4::target::active().hooks.license_crc_table;

// TASK-100 S8b: the demo-input virtual adds license_manager_add to the pointer it receives to reach the Manager
// stream (v2.00 0x1AA4; Spec II 0x1A24), and the loader hands it a pointer license_body_shift bytes past the header's
// body start (0 in v2.00; 0x80 in Spec II, so 0x1A24 + 0x80 = 0x1AA4 reaches the same single-Recorder stream).
inline constexpr std::uint32_t kManagerAdd = ::gt4::target::active().hooks.license_manager_add;
inline constexpr std::uint32_t kBodyShift = ::gt4::target::active().hooks.license_body_shift;

// The loader (function entry) whose call returns to `return_address`, or 0 when it is not a guarded call site.
constexpr std::uint32_t loader_for_return(std::uint32_t return_address) noexcept {
    if (kLoaderReturn == 0)
        return 0;
    return return_address == kLoaderReturn ? kLoaderEntry
                                           : return_address == kReplayLoaderReturn ? kReplayLoaderEntry : 0u;
}
// Demo file layout: header {u32 body offset, u32 body size, u32 unused, u32 key}, body at the offset.
inline constexpr std::uint32_t kHeaderBytes = 16u;
inline constexpr std::uint32_t kExpectedBodyOffset = 0x40u;
inline constexpr std::uint32_t kMaxBodySize = 0x00100000u;    // license demos are 8-36 KiB; refuse anything absurd
inline constexpr std::uint32_t kManagerOffset = 0x1AA4u;       // record count n (Manager stream) in the decrypted body
inline constexpr std::uint32_t kSkipCount = 0xFFFFFFFFu;       // n < 0: Manager::load copies no records

// The loader computes buffer + header offset; the demo-input virtual may receive that pointer shifted by kBodyShift.
constexpr bool body_pointer_matches(std::uint32_t body_start, std::uint32_t body_pointer) noexcept {
    return body_pointer == body_start || (kBodyShift != 0 && body_pointer == body_start + kBodyShift);
}

// Where the game's Manager::load reads the record count: the virtual's own addition to the pointer it receives.
constexpr std::uint32_t manager_address(std::uint32_t body_pointer) noexcept { return body_pointer + kManagerAdd; }

// Every qualified target must write where its own layout check read: the shift plus the virtual's add reaches the
// same Manager stream at body + kManagerOffset. A target without the guard (zero row) is exempt.
static_assert(kLoaderReturn == 0 || kManagerAdd + kBodyShift == kManagerOffset,
              "license guard: the demo-input add and the loader body shift must reach kManagerOffset");

struct Verdict {
    bool crc_ok = false;      // game rule: (crc32(body) & 0xFFFFFF) | body[0] << 24 == key
    bool layout_ok = false;   // n = 0 and one Recorder {frames > 0, size} that ends at the body end
    std::uint32_t count = 0;  // u32 at body + kManagerOffset (0 when the body is too short)
    bool guard = false;       // !crc_ok && !layout_ok
};

// Standard reflected CRC-32 (polynomial 0xEDB88320), the table at 0x00655358.
std::uint32_t crc32(const std::uint8_t *data, std::size_t size) noexcept;

// The game's own check of a decrypted body against the key word of the demo header.
bool game_crc_ok(const std::uint8_t *body, std::size_t size, std::uint32_t key) noexcept;

// Same rule as scripts/license_demo_oracle.py layout_sane().
bool single_recorder_layout(const std::uint8_t *body, std::size_t size) noexcept;

Verdict evaluate(const std::uint8_t *body, std::size_t size, std::uint32_t key) noexcept;

// Number of loads guarded in this process (for the log line).
std::uint32_t guarded_count() noexcept;
std::uint32_t note_guarded() noexcept;

}  // namespace gt4::license_guard
