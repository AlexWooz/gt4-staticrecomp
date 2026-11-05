// SPDX-License-Identifier: MIT
// TASK-180: the donor's GPU interface (port gpu.hpp, ps2::gs::gpu) for builds without CoreGS 1.1 (the donor Vulkan
// compute renderer, Windows-only: native_vulkan.cpp presents through Win32). Outside Windows CoreGS 2.0 renders through
// the GS stream seam (GT4_RENDERER=hw); gs.cpp then never initialises the donor GPU, so mode() stays off and none of
// the drawing entries below is reached. Every entry that would draw stops the game with a clear message instead of
// silently doing nothing.
#include "gpu.hpp"

#include <cstdio>
#include <cstdlib>

namespace ps2::gs::gpu {
namespace {

[[noreturn]] void unavailable(const char *entry) {
  std::fprintf(stderr, "[gs] the donor GPU renderer (CoreGS 1.1) is not part of this build: %s called; use GT4_RENDERER=hw\n",
               entry);
  std::fflush(stderr);
  std::abort();
}

}  // namespace

Mode mode() { return Mode::off; }
bool init(Mode m) {
  if (m != Mode::off)
    std::fprintf(stderr, "[gs] the donor GPU renderer (CoreGS 1.1) is not part of this build; set GT4_RENDERER=hw\n");
  return false;
}
void draw(const State &, const Vertex *, int) { unavailable("draw"); }
void image_begin(const State &) { unavailable("image_begin"); }
void image_data(const std::uint8_t *, std::size_t) { unavailable("image_data"); }
void local_copy(const State &) { unavailable("local_copy"); }
void load_clut(std::uint64_t, std::uint64_t) { unavailable("load_clut"); }
void flush() {}
void texflush() {}
void wait_idle() {}
void download(std::uint8_t *, std::uint32_t, std::uint32_t) { unavailable("download"); }
bool download_async(std::uint8_t *, std::uint32_t, std::uint32_t, std::uint32_t) { unavailable("download_async"); }
bool finish_download(std::uint32_t) { return true; }
void read_state(std::vector<std::uint32_t> &, std::uint32_t &, std::vector<std::uint32_t> &) { unavailable("read_state"); }
void write_state(const std::vector<std::uint32_t> &, std::uint32_t, const std::vector<std::uint32_t> &) {
  unavailable("write_state");
}
bool display(const DisplaySource &, std::vector<std::uint32_t> &, std::uint32_t &, std::uint32_t &) { return false; }
bool display_async(const DisplaySource &, std::vector<std::uint32_t> &, std::uint32_t &, std::uint32_t &) { return false; }
bool attach_surface(const SurfaceTarget &) { return false; }
void resize_surface(std::uint32_t, std::uint32_t) {}
bool has_surface() { return false; }
void set_widescreen(bool) {}
bool present(const DisplaySource &) { return false; }
double busy_ms() { return 0.0; }
void stats() {}

}  // namespace ps2::gs::gpu
