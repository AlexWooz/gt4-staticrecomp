//@section includes
#include "data_patch.hpp"
//@section system_includes
#include "data_patch.hpp"
//@section read_tail
    gt4::data_patch::apply(static_cast<std::uint64_t>(sector) * kSector, static_cast<std::uint8_t *>(out), want);
//@section install
    gt4::data_patch::install_after_disc_open();
