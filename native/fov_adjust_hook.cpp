// TASK-160: the call that scripts/camera360_units.py adds to the translated projection function (native/fov_adjust.hpp).
//
// It runs between the delay slot of the projection store call and the call (v2.00 jal 0x00378F18 at 0x0037A1BC, Spec II
// jal 0x0023D558 at 0x0023E82C). There the game has put 1.0 (its `lui at,0x3F80` immediate) in f12 and, in the delay slot,
// f13; f14 holds 1.0 / t and f0 still holds t = tan(fov / 2). With the zoom factor k at 1 nothing is touched; otherwise
// f12 = f13 = k and f14 = k / t with the guest's own divide, which is what the patched immediate would give.
#include "fov_adjust.hpp"

#include <ps2/generated.hpp>

#include <cstdio>

namespace gt4::fov_adjust {

void before_projection_scale_call(ps2::Cpu &c) {
  if (!enabled()) return;
  const std::int32_t steps = current_steps();
  if (steps == 0) return;
  static const Addresses addresses = active_addresses();
  if (c.r[31].w[0] != addresses.projection_call_return) {
    static bool logged = false;
    if (!logged) {
      std::fprintf(stderr, "[fov] hook reached with ra 0x%08X, expected 0x%08X: projection unchanged\n", c.r[31].w[0],
                   addresses.projection_call_return);
      logged = true;
    }
    return;
  }
  const std::uint32_t k = factor_bits(steps);
  c.f[12] = k;
  c.f[13] = k;
  ps2::fpu_div(c, 14, k, c.f[0]);  // the translated div.s: the same value and FPU flags
}

}  // namespace gt4::fov_adjust
