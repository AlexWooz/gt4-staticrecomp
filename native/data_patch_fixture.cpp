// TASK-212: the data-patch byte substitution on a synthetic range.
//
// The container loader is exercised against a real disc image by the slice421 headless run; this fixture checks the
// hot path: a partial overlap, a read that spans the range edges, and a read outside it.
#include "data_patch.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "data_patch fixture failed: %s\n", message);
    std::exit(1);
  }
}

} // namespace

int main() {
  gt4::data_patch::reset_for_test();
  require(!gt4::data_patch::active(), "inactive by default");

  std::vector<std::uint8_t> original(16, 0x11);
  std::vector<std::uint8_t> replacement(16, 0xAA);
  require(gt4::data_patch::install_for_test(100, original.data(), replacement.data(), replacement.size()),
          "install a range");
  require(gt4::data_patch::active(), "active after install");

  std::vector<std::uint8_t> block(64, 0);
  gt4::data_patch::apply(80, block.data(), block.size());
  for (int i = 0; i < 64; ++i) {
    const int at = 80 + i;
    const std::uint8_t expected = (at >= 100 && at < 116) ? 0xAA : 0x00;
    require(block[static_cast<std::size_t>(i)] == expected, "the range and only the range is replaced");
  }

  std::vector<std::uint8_t> tail(32, 0);
  gt4::data_patch::apply(104, tail.data(), tail.size());
  require(tail[0] == 0xAA && tail[11] == 0xAA && tail[12] == 0x00, "a read starting inside the range is clipped");

  std::vector<std::uint8_t> before(4, 0);
  gt4::data_patch::apply(0, before.data(), before.size());
  require(before[0] == 0 && before[3] == 0, "a read before the range is untouched");

  gt4::data_patch::reset_for_test();
  require(!gt4::data_patch::active(), "reset disables the patch");
  std::printf("data_patch fixture: ok\n");
  return 0;
}
