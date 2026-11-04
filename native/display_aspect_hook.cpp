// TASK-095: the calls that scripts/camera360_units.py adds before the 2D overlay ortho calls and after the strobe renderer's
// draw (native/display_aspect.hpp, docs/ULTRAWIDE-OVERLAYS.md).
//
// The ortho hook runs between the delay slot of the call and the call itself, where the four edges of the canvas are already
// in the argument registers (f12 left, f13 right, f14 bottom, f15 top) and nothing has used them yet. Part of the game
// executable because it reads the guest registers; the rules it applies are the pure ones of display_aspect.hpp, through
// on_overlay_ortho(), which knows what the presentation shows and opens the strobe text window.
#include "display_aspect.hpp"

#include <ps2/cpu.hpp>
#include <ps2/memory.hpp>

#include <cstdio>

namespace gt4::display_aspect {

namespace {

// The strobe renderer's caller: the return address it saved in its own frame (low word of the 64-bit slot).
std::uint32_t overlay_caller(const ps2::Cpu &c) {
  const std::uint32_t slot = c.r[29].w[0] + kOverlayCallerSlot;
  if ((slot & 3u) != 0 || static_cast<std::uint64_t>(slot) + 4u > ps2::kRamSize)
    return 0;
  return static_cast<std::uint32_t>(ps2::g_ram[slot]) | (static_cast<std::uint32_t>(ps2::g_ram[slot + 1]) << 8) |
         (static_cast<std::uint32_t>(ps2::g_ram[slot + 2]) << 16) | (static_cast<std::uint32_t>(ps2::g_ram[slot + 3]) << 24);
}

}  // namespace

void before_overlay_ortho_call(ps2::Cpu &c) {
  const std::uint32_t return_label = c.r[31].w[0];
  if (return_label != kMenuOrthoReturn && return_label != kOverlayOrthoReturn) {
    // Only the call sites of this target's own units may change a canvas; anything else leaves the guest alone.
    static bool logged = false;
    if (!logged) {
      std::fprintf(stderr, "[aspect] overlay hook reached with ra 0x%08X, expected 0x%08X or 0x%08X: canvas unchanged\n",
                   return_label, kMenuOrthoReturn, kOverlayOrthoReturn);
      logged = true;
    }
    return;
  }
  if (return_label == kOverlayOrthoReturn) {
    end_overlay_text();  // a window an unfinished draw left open never carries over to the next one
    if (overlay_caller(c) == kMonitorOverlayCallReturn)
      return;  // the race monitor stays as the game draws it (display_aspect.hpp)
  }
  const OrthoCanvas canvas = on_overlay_ortho({c.f[12], c.f[13], c.f[14], c.f[15]}, return_label);
  c.f[12] = canvas.left;
  c.f[13] = canvas.right;
  c.f[14] = canvas.bottom;
  c.f[15] = canvas.top;
}

void after_overlay_draw(ps2::Cpu &c) {
  const std::uint32_t return_label = c.r[31].w[0];
  if (return_label != kOverlayDrawEndReturn) {
    static bool logged = false;
    if (!logged) {
      std::fprintf(stderr, "[aspect] overlay end hook reached with ra 0x%08X, expected 0x%08X: text window left as is\n",
                   return_label, kOverlayDrawEndReturn);
      logged = true;
    }
    return;
  }
  end_overlay_text();
}

}  // namespace gt4::display_aspect
