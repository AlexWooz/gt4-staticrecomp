// CTest native_coregs3_timeline: the presentation timeline of CoreGS 3.0 (native/coregs3/coregs3_timeline.hpp), host only.
//   - object identity: stable across frames when the model's addresses are unchanged, different for a different model,
//     and separated by instance when the same object is drawn twice in one frame;
//   - the ring: capacity, newest-first access, clear;
//   - pairing: matched, only-older, only-newer counts, and that a pairing never collapses two instances;
//   - interpolation: lerp endpoints and midpoint, alpha clamping and the refusal of a bad frame pair.
#include "coregs3_timeline.hpp"

#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

using namespace gt4::coregs3;

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

CapturedDraw make_draw(std::uint32_t shape, std::uint32_t item_address, float value) {
  CapturedDraw draw;
  draw.shape = shape;
  draw.cmd_value = std::bit_cast<std::uint32_t>(value);
  draw.item_count = 1;
  draw.items[0].address = item_address;
  draw.items[0].qwords = 16;
  draw.items[0].event = 0x201;
  draw.items[0].material_record = 0;
  // A simple invertible matrix so the timeline can read it back.
  draw.vf[0][0] = std::bit_cast<std::uint32_t>(1.0f);
  draw.vf[1][1] = std::bit_cast<std::uint32_t>(1.0f);
  draw.vf[2][2] = std::bit_cast<std::uint32_t>(1.0f);
  draw.vf[3][3] = std::bit_cast<std::uint32_t>(1.0f);
  return draw;
}

FrameSnapshot make_frame(std::uint32_t index, std::uint64_t vblank, const std::vector<CapturedDraw> &draws) {
  FrameSnapshot frame;
  frame.index = index;
  frame.vblank = vblank;
  frame.draws = draws;
  frame.draw_count = static_cast<std::uint32_t>(draws.size());
  return frame;
}

void check_identity() {
  const CapturedDraw a = make_draw(0x00400000u, 0x00100000u, 1.0f);
  CapturedDraw b = a;
  require(object_key(a) == object_key(b), "the same model data gives the same identity");
  b.shape = 0x00401000u;
  require(!(object_key(a) == object_key(b)), "a different shape gives a different identity");
  CapturedDraw c = a;
  c.items[0].address = 0x00101000u;
  require(!(object_key(a) == object_key(c)), "a different item packet gives a different identity");

  // Two instances of the same model in one frame must not collapse into one object.
  FrameSnapshot frame = make_frame(0, 100, {a, a, b});
  const TimelineFrame timeline = timeline_frame(frame, GuestTime{100, 6000});
  require(timeline.objects.size() == 3, "three objects survive");
  require(timeline.objects[0].key.id == timeline.objects[1].key.id, "the same model repeats its id");
  require(timeline.objects[0].key.instance == 0 && timeline.objects[1].key.instance == 1,
          "the second instance is numbered");
  require(timeline.objects[2].key.id != timeline.objects[0].key.id, "the other model keeps its own id");
}

void check_ring() {
  FrameRing ring(4);
  require(ring.size() == 0, "a fresh ring is empty");
  for (std::uint32_t index = 0; index < 6; ++index) {
    TimelineFrame frame;
    frame.index = index;
    frame.time = GuestTime{index, static_cast<std::uint64_t>(index) * 1000};
    ring.push(frame);
  }
  require(ring.size() == 4, "the ring keeps its capacity");
  require(ring.at(0).index == 5, "age 0 is the newest frame");
  require(ring.at(3).index == 2, "age 3 is the oldest kept frame");
  bool threw = false;
  try {
    ring.at(4);
  } catch (const std::out_of_range &) {
    threw = true;
  }
  require(threw, "an age out of range is refused");
  ring.clear();
  require(ring.size() == 0, "clear empties the ring");
}

void check_pairing() {
  const CapturedDraw a = make_draw(0x00400000u, 0x00100000u, 1.0f);
  const CapturedDraw b = make_draw(0x00401000u, 0x00101000u, 1.0f);
  const CapturedDraw c = make_draw(0x00402000u, 0x00102000u, 1.0f);
  const TimelineFrame older = timeline_frame(make_frame(0, 100, {a, b}), GuestTime{100, 6000});
  const TimelineFrame newer = timeline_frame(make_frame(1, 101, {a, c, b}), GuestTime{101, 6167});

  FrameRing ring(4);
  ring.push(older);
  ring.push(newer);
  std::vector<FrameRing::Pair> pairs;
  std::uint32_t only_newer = 0, only_older = 0;
  ring.pair(0, 1, pairs, only_newer, only_older);
  require(pairs.size() == 2, "two objects pair across the two frames");
  require(only_newer == 1 && only_older == 0, "the object only in the newer frame is counted, not paired");
  require(pairs[0].older != nullptr && pairs[0].newer != nullptr, "a pair points at both frames");

  // A repeated model must not be collapsed: older has one `a`, newer has two.
  const TimelineFrame older2 = timeline_frame(make_frame(2, 102, {a}), GuestTime{102, 6334});
  const TimelineFrame newer2 = timeline_frame(make_frame(3, 103, {a, a}), GuestTime{103, 6501});
  FrameRing ring2(4);
  ring2.push(older2);
  ring2.push(newer2);
  ring2.pair(0, 1, pairs, only_newer, only_older);
  require(pairs.size() == 1, "one of the two instances pairs");
  require(only_newer == 1, "the second instance stays unpaired");
}

void check_interpolation() {
  const GuestTime older{100, 6000};
  const GuestTime newer{101, 6167};
  float alpha = -1.0f;
  require(interpolation_alpha(older, newer, 6000, alpha) && alpha == 0.0f, "the older instant is alpha 0");
  require(interpolation_alpha(older, newer, 6167, alpha) && alpha == 1.0f, "the newer instant is alpha 1");
  require(interpolation_alpha(older, newer, 6083, alpha) && alpha > 0.49f && alpha < 0.51f, "the midpoint is ~0.5");
  require(interpolation_alpha(older, newer, 5000, alpha) && alpha == 0.0f, "before the pair clamps to 0");
  require(interpolation_alpha(older, newer, 7000, alpha) && alpha == 1.0f, "after the pair clamps to 1");
  require(!interpolation_alpha(newer, older, 6100, alpha), "an unordered pair is refused");

  const float a[16] = {1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30, 0, 0, 0, 1};
  const float b[16] = {1, 0, 0, 12, 0, 1, 0, 24, 0, 0, 1, 36, 0, 0, 0, 1};
  float out[16] = {};
  lerp_matrix(a, b, 0.5f, out);
  require(out[3] == 11.0f && out[7] == 22.0f && out[11] == 33.0f, "the midpoint halves the translation");
  lerp_matrix(a, b, 0.0f, out);
  require(out[3] == 10.0f, "alpha 0 is the older matrix");
  lerp_matrix(a, b, 1.0f, out);
  require(out[3] == 12.0f, "alpha 1 is the newer matrix");
}

}  // namespace

int main() {
  try {
    check_identity();
    check_ring();
    check_pairing();
    check_interpolation();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "coregs3 timeline fixture: %s\n", error.what());
    return 1;
  }
  std::printf("coregs3 timeline fixture: ok\n");
  return 0;
}
