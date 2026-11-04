// TASK-117 (rc6): the flare limits of Performance mode, as a guest parameter block our derived ELF reads.
//
// Performance mode draws fewer light flares at night (no halo, distance cull, light caps). The change has to be guest
// code, because the four call sites are direct `jal`s (scripts/flare_limits_elf_patch.py): each one jumps to a small
// routine of ours in the zero padding at 0x00495574..0x00495FFF, which reads one word of the block below and, when its
// flags are 0, behaves exactly like the original instruction. The host fills the block at every guest VBlank:
//
//   +0x00 u32 flags        bit0 no halo, bit1 distance cull, bit2 light caps; 0 = the original behaviour
//   +0x04 f32 track depth  maximum view depth of a track light (60.0 world units)
//   +0x08 f32 car depth    maximum view depth of a car light (40.0)
//   +0x0C u32 reserved     0
//   +0x10 s32 track cap    at most this many track lights queued per frame (12)
//   +0x14 s32 car cap      at most this many car lights queued per frame (12)
//
// GT4_FLARE_LIMITS=1 turns them on (the launcher sets it for GT4 v2.00 when Performance mode is on); unset or 0 writes a
// zero block, so every helper takes the original path. The block sits right after the TASK-080 HUD block in the same
// unused kernel RAM (see native/display_aspect.hpp); the derived ELF reads it as `lw rt, offset(zero)`.
#pragma once

#include <cstdint>

namespace gt4::flare_limits {

inline constexpr std::uint32_t kParameterBlock = 0x00007F10u;
inline constexpr std::uint32_t kParameterWords = 6u;  // flags, track depth, car depth, reserved, track cap, car cap

inline constexpr std::uint32_t kFlagsOffset = 0x00u;
inline constexpr std::uint32_t kTrackDepthOffset = 0x04u;
inline constexpr std::uint32_t kCarDepthOffset = 0x08u;
inline constexpr std::uint32_t kReservedOffset = 0x0Cu;
inline constexpr std::uint32_t kTrackCapOffset = 0x10u;
inline constexpr std::uint32_t kCarCapOffset = 0x14u;

inline constexpr std::uint32_t kNoHalo = 1u;        // every flare draws its glow only
inline constexpr std::uint32_t kDistanceCull = 2u;  // far track and car lights are not queued
inline constexpr std::uint32_t kLightCaps = 4u;     // the queue keeps at most kLightCap lights of each kind
inline constexpr std::uint32_t kAllLimits = kNoHalo | kDistanceCull | kLightCaps;

inline constexpr float kTrackMaxDepth = 60.0f;
inline constexpr float kCarMaxDepth = 40.0f;
inline constexpr std::int32_t kLightCap = 12;
inline constexpr const char *kSwitch = "GT4_FLARE_LIMITS";

struct Parameters {
  std::uint32_t words[kParameterWords] = {};  // raw guest words, f32 values as their bit patterns
  bool operator==(const Parameters &other) const {
    for (std::uint32_t i = 0; i < kParameterWords; ++i)
      if (words[i] != other.words[i])
        return false;
    return true;
  }
};

// The rc6 set: flags 7, track depth 60, car depth 40, track and car caps 12.
Parameters rc6_parameters();
// GT4_FLARE_LIMITS: nullptr, "" or "0" = the original behaviour (a zero block); "1" = rc6_parameters(). Any other value
// throws std::invalid_argument naming the variable.
Parameters parameters_from_switch(const char *value);
// Writes the block when it differs from what guest RAM already holds. Returns true when words were written.
bool write_parameters(std::uint8_t *ram, std::uint32_t ram_size, const Parameters &parameters);
// EE thread, every guest VBlank: reads GT4_FLARE_LIMITS once and re-asserts the block (so a scene change or a guest
// write cannot leave it stale). Cheap: one comparison of six words when nothing changed.
void at_guest_vblank(std::uint8_t *ram, std::uint32_t ram_size);

}  // namespace gt4::flare_limits
