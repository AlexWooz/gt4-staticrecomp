//@section includes
#include "native_settings.hpp"
//@section hook
    if (pc == gt4::native_settings::wide_mode_entry()) [[unlikely]] {
      if (gt4::native_settings::on_wide_mode(c, g_ram, kRamSize, depth)) return;
    }
