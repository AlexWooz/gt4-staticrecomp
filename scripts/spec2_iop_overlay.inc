//@section includes
#include "hw.hpp"
#include "iop/io.hpp"
#include "iop/iop.hpp"
#include "iop/kernel.hpp"
#include "iop/sif.hpp"
#include "spec2_iop.hpp"
//@section reboot
    if (const auto space = path.find(' '); space != std::string::npos) read_loadfile_version(path.substr(space + 1));
    if (gt4::spec2_iop::async_fileio_boot_image(path, g_loadfile_version)) g_fileio_async = true;
