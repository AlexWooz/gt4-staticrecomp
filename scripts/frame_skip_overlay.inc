//@section includes
#include "frame_skip.hpp"
//@section helpers
// TASK-082: frame skipping (native/frame_skip.hpp). Off unless GT4_FRAME_SKIP or GT4_FRAME_SKIP_FORCE is set; off, one branch.
//@section hook
    if (gt4::frame_skip::g_enabled) [[unlikely]] {
      if (gt4::frame_skip::on_dispatch(c, pc, g_ram, kRamSize)) {
        c.pc = c.r[31].w[0];
        return;
      }
    }
