// TASK-100 S7b fixture: the Spec II IOP reboot image rule (native/spec2_iop.hpp) in isolation, no guest code.
//
// The rule must answer for exactly the two Sony IOP reboot images the game asks for, so the donor behaviour for
// v2.00 (IOPRP300.IMG) and the Spec II behaviour (DNAS310.IMG) cannot drift, and no other path may switch the
// asynchronous FILEIO server on.
#include <cstdio>
#include <string>

#include "spec2_iop.hpp"

namespace {

int g_failures = 0;

void expect(bool condition, const char *what) {
    if (condition) return;
    std::fprintf(stderr, "spec2_iop: FAILED %s\n", what);
    ++g_failures;
}

}  // namespace

int main() {
    using gt4::spec2_iop::async_fileio_boot_image;
    // The reboot argument is the whole string the guest passes to the reboot RPC (rom argument, space, image).
    expect(async_fileio_boot_image("rom0:UDNL cdrom0:\\IOPRP300.IMG;1", "3000"), "retail IOPRP300 reboot");
    expect(async_fileio_boot_image("rom0:UDNL cdrom0:\\DNAS310.IMG;1", "3100"), "Spec II DNAS310 reboot");
    // The image name is matched without regard to case.
    expect(async_fileio_boot_image("rom0:UDNL cdrom0:\\dnas310.img;1", "3100"), "lower-case DNAS310 path");
    // A name that merely contains IOPRP keeps the donor's substring behaviour.
    expect(async_fileio_boot_image("rom0:UDNL cdrom0:\\IOPRP300.IMG2;1", "3000"), "IOPRP substring rule");
    // Any other reboot image keeps the donor's synchronous FILEIO server.

    expect(!async_fileio_boot_image("rom0:UDNL cdrom0:\\OTHER.IMG;1", "3000"), "unrelated reboot image");
    expect(!async_fileio_boot_image("rom0:UDNL cdrom0:\\DNAS311.IMG;1", "3100"), "another DNAS image");
    expect(!async_fileio_boot_image("", "3000"), "empty path");
    if (g_failures) {
        std::fprintf(stderr, "spec2_iop: %d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "spec2_iop: all checks passed\n");
    return 0;
}
