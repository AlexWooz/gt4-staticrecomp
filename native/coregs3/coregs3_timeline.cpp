// TASK-090 CoreGS 3.0: the presentation timeline. See coregs3_timeline.hpp for the contract.
#include "coregs3_timeline.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace gt4::coregs3 {
namespace {

constexpr std::uint64_t kFnvOffset = 0xCBF29CE484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001B3ull;

std::uint64_t fnv1a(std::uint64_t hash, std::uint32_t value) noexcept {
  for (int byte = 0; byte < 4; ++byte) {
    hash ^= (value >> (8 * byte)) & 0xFFu;
    hash *= kFnvPrime;
  }
  return hash;
}

bool finite_matrix(const float *matrix) noexcept {
  for (int index = 0; index < 16; ++index)
    if (!(matrix[index] == matrix[index])) return false;  // NaN check without <cmath> in a hot path
  return true;
}

}  // namespace

ObjectKey object_key(const CapturedDraw &draw) noexcept {
  // The identity is the model's own data: the shape (Cmd) address, then every item packet address and the material record
  // address of the item. Two draws of the same model share it; the same model on the next frame shares it again.
  std::uint64_t hash = kFnvOffset;
  hash = fnv1a(hash, draw.shape);
  hash = fnv1a(hash, draw.item_count);
  for (std::uint32_t item = 0; item < draw.item_count; ++item) {
    hash = fnv1a(hash, draw.items[item].address);
    hash = fnv1a(hash, draw.items[item].event);
    hash = fnv1a(hash, draw.items[item].material_record);
  }
  ObjectKey key;
  key.id = hash;
  key.instance = 0;
  return key;
}

TimelineFrame timeline_frame(const FrameSnapshot &frame, GuestTime time) {
  TimelineFrame out;
  out.time = time;
  out.index = frame.index;
  out.objects.reserve(frame.draws.size());
  for (const CapturedDraw &draw : frame.draws) {
    const DrawMatrix matrix = draw_matrix(draw);
    if (!matrix.valid || !finite_matrix(&matrix.row[0][0])) continue;
    ObjectTransform object;
    object.key = object_key(draw);
    object.sequence = draw.sequence;
    std::memcpy(object.matrix, &matrix.row[0][0], sizeof object.matrix);
    // A repeated key (the same model drawn twice in one frame) gets the next instance number so the pairer keeps them
    // apart instead of collapsing two cars into one.
    for (const ObjectTransform &existing : out.objects)
      if (existing.key.id == object.key.id) object.key.instance = existing.key.instance + 1;
    out.objects.push_back(object);
  }
  return out;
}

FrameRing::FrameRing(std::size_t capacity) : capacity_(std::max<std::size_t>(2, capacity)) {}

void FrameRing::push(const TimelineFrame &frame) {
  frames_.push_back(frame);
  while (frames_.size() > capacity_) frames_.erase(frames_.begin());
}

void FrameRing::clear() noexcept { frames_.clear(); }

const TimelineFrame &FrameRing::at(std::size_t age) const {
  if (age >= frames_.size()) throw std::out_of_range("coregs3 timeline: frame age out of range");
  return frames_[frames_.size() - 1 - age];
}

void FrameRing::pair(std::size_t newer_age, std::size_t older_age, std::vector<Pair> &pairs,
                     std::uint32_t &only_newer, std::uint32_t &only_older) const {
  const TimelineFrame &newer = at(newer_age);
  const TimelineFrame &older = at(older_age);
  pairs.clear();
  pairs.reserve(older.objects.size());
  std::vector<bool> used(newer.objects.size(), false);
  only_older = 0;
  for (const ObjectTransform &object : older.objects) {
    bool matched = false;
    for (std::size_t index = 0; index < newer.objects.size(); ++index) {
      if (used[index]) continue;
      const ObjectTransform &candidate = newer.objects[index];
      if (candidate.key.id != object.key.id || candidate.key.instance != object.key.instance) continue;
      used[index] = true;
      pairs.push_back(Pair{&object, &candidate});
      matched = true;
      break;
    }
    if (!matched) ++only_older;
  }
  only_newer = 0;
  for (const bool was_used : used) only_newer += was_used ? 0u : 1u;
}

void lerp_matrix(const float *older, const float *newer, float alpha, float *out) noexcept {
  for (int index = 0; index < 16; ++index) out[index] = older[index] + (newer[index] - older[index]) * alpha;
}

bool interpolation_alpha(const GuestTime &older, const GuestTime &newer, std::uint64_t present_ticks,
                         float &alpha) noexcept {
  if (!(newer.ticks > older.ticks)) return false;
  if (present_ticks <= older.ticks) {
    alpha = 0.0f;
    return true;
  }
  if (present_ticks >= newer.ticks) {
    // Past the newest frame: clamp instead of extrapolating. The caller that wants extrapolation asks for it explicitly
    // (phase 6), which keeps the default behaviour free of overshoot artefacts.
    alpha = 1.0f;
    return true;
  }
  alpha = static_cast<float>(static_cast<double>(present_ticks - older.ticks) /
                             static_cast<double>(newer.ticks - older.ticks));
  return true;
}

}  // namespace gt4::coregs3
