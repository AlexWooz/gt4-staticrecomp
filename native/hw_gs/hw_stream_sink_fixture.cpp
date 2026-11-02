// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// CTest hw_stream_sink (task/027-coregs2): with GT4_RENDERER=hw the GS stream seam of main (gs_stream_client.hpp)
// sends its hooks to the in-process HWR backend. This drives the hooks in the order the spliced donor sources call
// them (reset, SetGsCrt, one GIF packet, a FIFO read, three VSYNCs, finish) and lets CTest check the session line.
// Headless: no window is attached, so each VSYNC closes one HWR frame. Written from docs/hw-renderer-spec (clean room).
// `--deferred-vsync` (TASK-084 merge): the MTVU VBlank journal holds the VSYNC at the guest VBlank and posts it from the
// journal drain after the frame's output (set_vsync_deferral / post_pending_vsync). The hw sink must receive exactly
// one VSYNC per post, none at the hold, and a post with nothing held must report false.
// `--display-aspect` (owner test 6): the TASK-080 display-aspect fill requests reach the in-process presenter.

#include "gs_stream/gs_stream_client.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
  namespace stream = gt4::gs_stream;
  const bool deferred = argc > 1 && std::strcmp(argv[1], "--deferred-vsync") == 0;
  const bool display_aspect = argc > 1 && std::strcmp(argv[1], "--display-aspect") == 0;
  if (!stream::stream_renderer()) {
    std::printf("FAIL: GT4_RENDERER=hw did not select a non-donor renderer\n");
    return 1;
  }
  stream::on_gs_reset();
  stream::on_gif_reset();
  stream::on_set_gs_crt(1, 0x02, 0);  // NTSC interlaced, field mode: SMODE1, SYNCV and SMODE2 reach the backend
  stream::on_privileged_write(0x12000000u, 0x66u);  // PMODE

  // One PACKED GIF packet: tag NLOOP=1, EOP=1, NREG=1, REGS=A+D (0xE), then FRAME_1 (0x4C) = 0.
  alignas(16) std::uint8_t packet[32] = {};
  const std::uint64_t tag_low = 1u | (1ull << 15) | (1ull << 60);
  const std::uint64_t tag_high = 0xEu;
  const std::uint64_t ad_value = 0, ad_register = 0x4C;
  std::memcpy(packet + 0, &tag_low, 8);
  std::memcpy(packet + 8, &tag_high, 8);
  std::memcpy(packet + 16, &ad_value, 8);
  std::memcpy(packet + 24, &ad_register, 8);
  if (!stream::on_gif_render(1, packet, 2)) {
    std::printf("FAIL: the hw sink did not take the GIF chunk (the donor front end would run)\n");
    return 1;
  }

  std::uint8_t fifo[32];
  std::memset(fifo, 0xA5, sizeof fifo);
  if (!stream::read_fifo(fifo, 2)) {
    std::printf("FAIL: the hw sink did not answer the FIFO read\n");
    return 1;
  }
  for (std::uint8_t byte : fifo) {
    if (byte != 0) {  // M0 backend: no local-to-host data yet, so the reply is zero-filled
      std::printf("FAIL: FIFO reply not zero-filled at M0\n");
      return 1;
    }
  }

  if (deferred) {
    stream::set_vsync_deferral(true);
    if (stream::post_pending_vsync()) {
      std::puts("FAIL: a VSYNC was posted with nothing held");
      return 1;
    }
    for (int frame = 0; frame < 3; ++frame) {
      stream::on_vsync(frame & 1);           // held: the sink must not see it yet
      if (!stream::on_gif_render(1, packet, 2)) {  // the frame's late output still reaches the sink first
        std::puts("FAIL: GIF chunk refused while a VSYNC was held");
        return 1;
      }
      if (!stream::post_pending_vsync()) {
        std::puts("FAIL: the held VSYNC was not posted");
        return 1;
      }
      if (stream::post_pending_vsync()) {
        std::puts("FAIL: one held VSYNC was posted twice");
        return 1;
      }
    }
    stream::set_vsync_deferral(false);
  } else {
    for (int frame = 0; frame < 3; ++frame) stream::on_vsync(frame & 1);
  }
  if (display_aspect) {
    // TASK-080 through the seam: the window's presentation sink calls set_display_aspect_mode; with GT4_RENDERER=hw
    // there is no GS process, so the request must reach the in-process presenter (owner test 6). 21:9 and 32:9 fill,
    // then back to the 16:9 pillarbox (the session line reports the last state).
    stream::set_display_aspect_mode(stream::DisplayAspectMode::fill, stream::aspect_millionths(21.0 / 9.0));
    stream::set_display_aspect_mode(stream::DisplayAspectMode::fill, stream::aspect_millionths(32.0 / 9.0));
    stream::set_display_aspect_mode(stream::DisplayAspectMode::game_aspect, 0);
  }
  stream::finish(stdout);
  std::printf("PASS: hw stream sink\n");
  return 0;
}
