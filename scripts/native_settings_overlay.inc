//@section includes
#include "native_settings.hpp"
#include "screen_rows.hpp"
//@section hook
    if (pc == gt4::native_settings::wide_mode_entry()) [[unlikely]] {
      if (gt4::native_settings::on_wide_mode(c, g_ram, kRamSize, depth)) return;
    }
    if (pc == gt4::screen_rows::adjust_v_entry()) [[unlikely]] {
      if (gt4::screen_rows::on_adjust(c, g_ram, kRamSize, depth, 0)) return;
    }
    if (pc == gt4::screen_rows::adjust_h_entry()) [[unlikely]] {
      if (gt4::screen_rows::on_adjust(c, g_ram, kRamSize, depth, 1)) return;
    }
    if (pc == gt4::screen_rows::render_entry()) [[unlikely]]
      gt4::screen_rows::on_render_frame(c, g_ram, kRamSize, depth);
