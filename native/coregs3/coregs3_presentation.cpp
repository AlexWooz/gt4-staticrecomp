// TASK-090 CoreGS 3.0: the live presentation timeline. See coregs3_presentation.hpp for the contract; the rigid rules
// are a port of the measured prediction study (build/analysis-prediction in the coregs3 worktree,
// docs/journal/2026-11-10.md).
#include "coregs3_presentation.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace gt4::coregs3 {
namespace {

constexpr float kFiniteEpsilon = 1e-12f;

bool finite(float value) noexcept {
  return value == value && value != std::numeric_limits<float>::infinity() &&
         value != -std::numeric_limits<float>::infinity();
}

bool finite_matrix(const float matrix[16]) noexcept {
  for (int index = 0; index < 16; ++index)
    if (!finite(matrix[index])) return false;
  return true;
}

float bits_to_float(std::uint32_t bits) noexcept {
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

// Row-major 4x4 helpers: `out = a * b`, matching the analysis' A.matmul.
void multiply(const float a[16], const float b[16], float out[16]) noexcept {
  float result[16] = {};
  for (int row = 0; row < 4; ++row)
    for (int column = 0; column < 4; ++column) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) sum += a[row * 4 + k] * b[k * 4 + column];
      result[row * 4 + column] = sum;
    }
  std::memcpy(out, result, sizeof result);
}

// Gauss-Jordan inverse with partial pivoting (the analysis' inverse4). False on a singular matrix.
bool inverse(const float matrix[16], float out[16]) noexcept {
  double a[4][8] = {};
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) a[row][column] = matrix[row * 4 + column];
    a[row][4 + row] = 1.0;
  }
  for (int column = 0; column < 4; ++column) {
    int pivot = column;
    for (int row = column + 1; row < 4; ++row)
      if (std::fabs(a[row][column]) > std::fabs(a[pivot][column])) pivot = row;
    if (std::fabs(a[pivot][column]) < 1e-18) return false;
    if (pivot != column)
      for (int k = 0; k < 8; ++k) {
        const double swap = a[column][k];
        a[column][k] = a[pivot][k];
        a[pivot][k] = swap;
      }
    const double scale = 1.0 / a[column][column];
    for (int k = 0; k < 8; ++k) a[column][k] *= scale;
    for (int row = 0; row < 4; ++row) {
      if (row == column) continue;
      const double factor = a[row][column];
      if (factor == 0.0) continue;
      for (int k = 0; k < 8; ++k) a[row][k] -= factor * a[column][k];
    }
  }
  for (int row = 0; row < 4; ++row)
    for (int column = 0; column < 4; ++column) out[row * 4 + column] = static_cast<float>(a[row][4 + column]);
  return finite_matrix(out);
}

void identity(float out[16]) noexcept {
  std::memset(out, 0, 16 * sizeof(float));
  out[0] = out[5] = out[10] = out[15] = 1.0f;
}

// [w, x, y, z] from a row-major rotation (the analysis' mat_to_quat).
void quat_multiply(const float a[4], const float b[4], float out[4]) noexcept {
  const float aw = a[0], ax = a[1], ay = a[2], az = a[3];
  const float bw = b[0], bx = b[1], by = b[2], bz = b[3];
  float result[4] = {aw * bw - ax * bx - ay * by - az * bz, aw * bx + ax * bw + ay * bz - az * by,
                     aw * by - ax * bz + ay * bw + az * bx, aw * bz + ax * by - ay * bx + az * bw};
  std::memcpy(out, result, sizeof result);
}

void quat_conjugate(const float q[4], float out[4]) noexcept {
  out[0] = q[0];
  out[1] = -q[1];
  out[2] = -q[2];
  out[3] = -q[3];
}

float quat_norm(const float q[4]) noexcept { return std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]); }

void quat_normalize(float q[4]) noexcept {
  const float norm = quat_norm(q);
  if (!(norm > kFiniteEpsilon)) {
    q[0] = 1.0f;
    q[1] = q[2] = q[3] = 0.0f;
    return;
  }
  for (int index = 0; index < 4; ++index) q[index] /= norm;
}

// q^alpha along the shortest arc (constant angular velocity), the analysis' quat_power.
void quat_power(float q[4], float alpha, float out[4]) noexcept {
  quat_normalize(q);
  if (q[0] < 0.0f)
    for (int index = 0; index < 4; ++index) q[index] = -q[index];
  const float w = std::fmax(-1.0f, std::fmin(1.0f, q[0]));
  const float angle = 2.0f * std::acos(w);
  const float sine = std::sin(angle * 0.5f);
  if (sine < 1e-12f) {
    out[0] = 1.0f;
    out[1] = out[2] = out[3] = 0.0f;
    return;
  }
  const float axis[3] = {q[1] / sine, q[2] / sine, q[3] / sine};
  const float half = angle * 0.5f * alpha;
  out[0] = std::cos(half);
  out[1] = axis[0] * std::sin(half);
  out[2] = axis[1] * std::sin(half);
  out[3] = axis[2] * std::sin(half);
}

void rotation_angle(const float a[9], const float b[9], float &radians) noexcept {
  float qa[4], qb[4], relative[4];
  mat_to_quat(a, qa);
  mat_to_quat(b, qb);
  float conjugate[4];
  quat_conjugate(qa, conjugate);
  quat_multiply(conjugate, qb, relative);
  radians = 2.0f * std::acos(std::fmin(1.0f, std::fabs(relative[0])));
}

// The camera world position from a view matrix's rotation and translation: -R^T t.
void camera_position(const float rotation[9], const float translation[3], float out[3]) noexcept {
  for (int column = 0; column < 3; ++column) {
    float sum = 0.0f;
    for (int row = 0; row < 3; ++row) sum += rotation[row * 3 + column] * translation[row];
    out[column] = -sum;
  }
}

bool view_pose(const CameraFrame &camera, float rotation[9], float position[3]) noexcept {
  float scale = 0.0f, translation[3] = {};
  if (!decompose_rigid(&camera.view[0][0], scale, rotation, translation)) return false;
  camera_position(rotation, translation, position);
  return true;
}

// The projection*view of one captured camera, inverted; false when the camera is missing or P*V is singular.
bool camera_pv_inverse(const CameraFrame &camera, float inverse_pv[16]) noexcept {
  if (!camera.valid) return false;
  float pv[16];
  multiply(&camera.projection[0][0], &camera.view[0][0], pv);
  return inverse(pv, inverse_pv);
}

// The world matrix of one captured model->clip matrix: inv(P*V) * M, normalized by its homogeneous w (the analysis'
// world_full). False when the division is degenerate or the result is not affine.
bool world_from_capture(const float inverse_pv[16], const float matrix[16], float world[16]) noexcept {
  multiply(inverse_pv, matrix, world);
  const float w = world[15];
  if (!(std::fabs(w) > 1e-9f)) return false;
  for (int index = 0; index < 16; ++index) world[index] /= w;
  return finite_matrix(world);
}

// Interpolates two world matrices: translation lerp, rotation slerp, scale lerp (the study's slerp_rigid_world).
bool slerp_rigid_world(const float older[16], const float newer[16], float alpha, float out[16]) noexcept {
  float s0 = 0.0f, s1 = 0.0f, r0[9] = {}, r1[9] = {}, t0[3] = {}, t1[3] = {};
  if (!decompose_rigid(older, s0, r0, t0) || !decompose_rigid(newer, s1, r1, t1)) return false;
  float q0[4], q1[4], q[4];
  mat_to_quat(r0, q0);
  mat_to_quat(r1, q1);
  quat_slerp(q0, q1, alpha, q);
  float rotation[9];
  quat_to_mat(q, rotation);
  const float scale = s0 + (s1 - s0) * alpha;
  identity(out);
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) out[row * 4 + column] = scale * rotation[row * 3 + column];
    out[row * 4 + 3] = t0[row] + (t1[row] - t0[row]) * alpha;
  }
  return finite_matrix(out);
}

bool camera_pair_cut(const CameraFrame &older, const CameraFrame &newer, const InterpolationLimits &limits,
                     bool &cut) noexcept {
  cut = false;
  float r0[9] = {}, r1[9] = {}, p0[3] = {}, p1[3] = {};
  if (!view_pose(older, r0, p0) || !view_pose(newer, r1, p1)) return false;
  float delta[3];
  float distance = 0.0f;
  for (int axis = 0; axis < 3; ++axis) {
    delta[axis] = p1[axis] - p0[axis];
    distance += delta[axis] * delta[axis];
  }
  distance = std::sqrt(distance);
  float angle = 0.0f;
  rotation_angle(r0, r1, angle);
  cut = distance > limits.max_camera_translation_step_m || angle > limits.max_camera_rotation_step_rad;
  return true;
}

}  // namespace

std::uint64_t guest_ticks(std::uint64_t vblank) noexcept {
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  if (vblank > (kMax - (kPresentationTicksPerGuestFrame - 1)) / kPresentationTicksPerGuestFrame) return kMax;
  return vblank * kPresentationTicksPerGuestFrame;
}

bool presentation_rate_supported(std::uint32_t hz) noexcept {
  return hz == 20 || hz == 30 || hz == 45 || hz == 60 || hz == 120 || hz == 144;
}

const char *presentation_rate_list() noexcept { return "20, 30, 45, 60, 120, 144"; }

std::uint32_t parse_presentation_hz(const char *text) noexcept {
  if (text == nullptr || *text == '\0') return 0;
  char *end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  if (end == nullptr || *end != '\0' || value == 0 || value > 100000ul) return 0;
  const std::uint32_t hz = static_cast<std::uint32_t>(value);
  return presentation_rate_supported(hz) ? hz : 0;
}

std::uint64_t presentation_step_ticks(std::uint32_t hz) noexcept {
  if (!presentation_rate_supported(hz)) return 0;
  return kPresentationTicksPerSecond / hz;
}

const char *presentation_mode_name(PresentationMode mode) noexcept {
  switch (mode) {
    case PresentationMode::interpolate: return "interpolate";
    case PresentationMode::nearest: return "nearest";
  }
  return "?";
}

bool parse_presentation_mode(const char *text, PresentationMode &mode) noexcept {
  if (text == nullptr || std::strcmp(text, "interpolate") == 0) {
    mode = PresentationMode::interpolate;
    return true;
  }
  if (std::strcmp(text, "nearest") == 0) {
    mode = PresentationMode::nearest;
    return true;
  }
  return false;
}

CameraFrame camera_frame(const CameraSnapshot &camera) noexcept {
  CameraFrame out;
  // The K4 point copies vf24..vf31: the projection's four columns then the view's four (the analysis' camera_pose).
  if ((camera.valid & 2u) == 0) return out;
  bool usable = true;
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row) {
      out.projection[row][column] = bits_to_float(camera.vf_k4[column][row]);
      out.view[row][column] = bits_to_float(camera.vf_k4[4 + column][row]);
      usable = usable && finite(out.projection[row][column]) && finite(out.view[row][column]);
    }
  out.valid = usable;
  return out;
}

PresentationClock::PresentationClock(std::uint32_t rate_hz) noexcept
    : rate_hz_(rate_hz), step_(presentation_step_ticks(rate_hz)) {}

void PresentationClock::reset() noexcept {
  origin_ = 0;
  next_ = 0;
  started_ = false;
}

PresentationClock::Due PresentationClock::observe(std::uint64_t newest_ticks) noexcept {
  Due due;
  due.step = step_;
  if (step_ == 0) return due;
  if (!started_) {
    started_ = true;
    origin_ = newest_ticks;
    next_ = newest_ticks;
  }
  if (newest_ticks < next_) return due;  // an out-of-order frame: nothing new is due, never step backwards
  std::uint64_t count = (newest_ticks - next_) / step_ + 1;
  if (count > kMaxDuePerFrame) {
    const std::uint64_t skipped = count - kMaxDuePerFrame;
    next_ += skipped * step_;
    count = kMaxDuePerFrame;
    due.skipped = skipped;
  }
  due.first = next_;
  due.count = static_cast<std::uint32_t>(count);
  next_ += count * step_;
  return due;
}

void InterpolationCounters::add(const InterpolationCounters &other) noexcept {
  instants += other.instants;
  pairs += other.pairs;
  pairs_refused += other.pairs_refused;
  paired += other.paired;
  only_newer += other.only_newer;
  only_older += other.only_older;
  interpolated += other.interpolated;
  entrywise += other.entrywise;
  nearest += other.nearest;
  predicted += other.predicted;
  dropped_discontinuity += other.dropped_discontinuity;
}

bool decompose_rigid(const float matrix[16], float &scale, float rotation[9], float translation[3]) noexcept {
  if (std::fmax(std::fmax(std::fabs(matrix[12]), std::fabs(matrix[13])),
                std::fmax(std::fabs(matrix[14]), std::fabs(matrix[15] - 1.0f))) > 1e-3f)
    return false;
  float norms[3] = {};
  for (int column = 0; column < 3; ++column) {
    float sum = 0.0f;
    for (int row = 0; row < 3; ++row) sum += matrix[row * 4 + column] * matrix[row * 4 + column];
    norms[column] = std::sqrt(sum);
  }
  scale = (norms[0] + norms[1] + norms[2]) / 3.0f;
  if (!(scale > 0.0f) || !finite(scale)) return false;
  for (int row = 0; row < 3; ++row)
    for (int column = 0; column < 3; ++column) rotation[row * 3 + column] = matrix[row * 4 + column] / scale;
  // Gram-Schmidt on the columns, so rounding and slight non-uniformity cannot leak into the quaternion.
  for (int column = 0; column < 3; ++column) {
    for (int k = 0; k < column; ++k) {
      float dot = 0.0f;
      for (int row = 0; row < 3; ++row) dot += rotation[row * 3 + column] * rotation[row * 3 + k];
      for (int row = 0; row < 3; ++row) rotation[row * 3 + column] -= dot * rotation[row * 3 + k];
    }
    float norm = 0.0f;
    for (int row = 0; row < 3; ++row) norm += rotation[row * 3 + column] * rotation[row * 3 + column];
    norm = std::sqrt(norm);
    if (!(norm > 1e-9f)) return false;
    for (int row = 0; row < 3; ++row) rotation[row * 3 + column] /= norm;
  }
  translation[0] = matrix[3];
  translation[1] = matrix[7];
  translation[2] = matrix[11];
  return finite_matrix(matrix);
}

bool rigid_rank_deficient(const float matrix[16]) noexcept {
  float scale = 0.0f, rotation[9] = {}, translation[3] = {};
  if (!decompose_rigid(matrix, scale, rotation, translation)) return true;
  const float m00 = matrix[0], m01 = matrix[1], m02 = matrix[2];
  const float m10 = matrix[4], m11 = matrix[5], m12 = matrix[6];
  const float m20 = matrix[8], m21 = matrix[9], m22 = matrix[10];
  const float determinant = m00 * (m11 * m22 - m12 * m21) - m01 * (m10 * m22 - m12 * m20) +
                            m02 * (m10 * m21 - m11 * m20);
  const float scale3 = scale * scale * scale;
  if (!(scale3 > 0.0f) || std::fabs(determinant) / scale3 < 0.05f) return true;
  const float block[3][3] = {{m00, m01, m02}, {m10, m11, m12}, {m20, m21, m22}};
  float residual = 0.0f;
  for (int row = 0; row < 3; ++row)
    for (int column = 0; column < 3; ++column) {
      float gram = 0.0f;
      for (int k = 0; k < 3; ++k) gram += block[k][row] * block[k][column];
      const float expected = row == column ? scale * scale : 0.0f;
      residual = std::fmax(residual, std::fabs(gram - expected));
    }
  return residual / (scale * scale) > 0.01f;
}

void mat_to_quat(const float rotation[9], float quat[4]) noexcept {
  const float r00 = rotation[0], r01 = rotation[1], r02 = rotation[2];
  const float r10 = rotation[3], r11 = rotation[4], r12 = rotation[5];
  const float r20 = rotation[6], r21 = rotation[7], r22 = rotation[8];
  const float trace = r00 + r11 + r22;
  float q[4];
  if (trace > 0.0f) {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    q[0] = 0.25f * s;
    q[1] = (r21 - r12) / s;
    q[2] = (r02 - r20) / s;
    q[3] = (r10 - r01) / s;
  } else if (r00 > r11 && r00 > r22) {
    const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
    q[0] = (r21 - r12) / s;
    q[1] = 0.25f * s;
    q[2] = (r01 + r10) / s;
    q[3] = (r02 + r20) / s;
  } else if (r11 > r22) {
    const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
    q[0] = (r02 - r20) / s;
    q[1] = (r01 + r10) / s;
    q[2] = 0.25f * s;
    q[3] = (r12 + r21) / s;
  } else {
    const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
    q[0] = (r10 - r01) / s;
    q[1] = (r02 + r20) / s;
    q[2] = (r12 + r21) / s;
    q[3] = 0.25f * s;
  }
  quat_normalize(q);
  std::memcpy(quat, q, sizeof q);
}

void quat_to_mat(const float quat[4], float rotation[9]) noexcept {
  float q[4];
  std::memcpy(q, quat, sizeof q);
  quat_normalize(q);
  const float w = q[0], x = q[1], y = q[2], z = q[3];
  rotation[0] = 1.0f - 2.0f * (y * y + z * z);
  rotation[1] = 2.0f * (x * y - z * w);
  rotation[2] = 2.0f * (x * z + y * w);
  rotation[3] = 2.0f * (x * y + z * w);
  rotation[4] = 1.0f - 2.0f * (x * x + z * z);
  rotation[5] = 2.0f * (y * z - x * w);
  rotation[6] = 2.0f * (x * z - y * w);
  rotation[7] = 2.0f * (y * z + x * w);
  rotation[8] = 1.0f - 2.0f * (x * x + y * y);
}

void quat_slerp(const float a[4], const float b[4], float alpha, float out[4]) noexcept {
  float q0[4], q1[4], conjugate[4], increment[4], powered[4], result[4];
  std::memcpy(q0, a, sizeof q0);
  std::memcpy(q1, b, sizeof q1);
  quat_normalize(q0);
  quat_normalize(q1);
  quat_conjugate(q0, conjugate);
  quat_multiply(conjugate, q1, increment);
  quat_power(increment, alpha, powered);
  quat_multiply(q0, powered, result);
  quat_normalize(result);
  std::memcpy(out, result, sizeof result);
}

bool interpolated_camera_pv(const CameraFrame &older, const CameraFrame &newer, float alpha,
                            float projection_view[16]) noexcept {
  if (!older.valid || !newer.valid) return false;
  float p[16];
  for (int index = 0; index < 16; ++index)
    p[index] = older.projection[index / 4][index % 4] +
               (newer.projection[index / 4][index % 4] - older.projection[index / 4][index % 4]) * alpha;
  float r0[9] = {}, r1[9] = {}, p0[3] = {}, p1[3] = {};
  if (!view_pose(older, r0, p0) || !view_pose(newer, r1, p1)) return false;
  float q0[4], q1[4], q[4], rotation[9];
  mat_to_quat(r0, q0);
  mat_to_quat(r1, q1);
  quat_slerp(q0, q1, alpha, q);
  quat_to_mat(q, rotation);
  float position[3];
  for (int axis = 0; axis < 3; ++axis) position[axis] = p0[axis] + (p1[axis] - p0[axis]) * alpha;
  float view[16];
  identity(view);
  for (int row = 0; row < 3; ++row) {
    float translation = 0.0f;
    for (int column = 0; column < 3; ++column) {
      view[row * 4 + column] = rotation[row * 3 + column];
      translation -= rotation[row * 3 + column] * position[column];
    }
    view[row * 4 + 3] = translation;
  }
  multiply(p, view, projection_view);
  return finite_matrix(projection_view);
}

bool camera_pair_continuous(const CameraFrame &older, const CameraFrame &newer,
                            const InterpolationLimits &limits) noexcept {
  bool cut = false;
  if (!camera_pair_cut(older, newer, limits, cut)) return false;
  return !cut;
}

bool interpolate_object_rigid(const CameraFrame &older_camera, const CameraFrame &newer_camera,
                              const float older_matrix[16], const float newer_matrix[16], float alpha,
                              float out_matrix[16]) noexcept {
  float inverse_older[16], inverse_newer[16], projection_view[16];
  if (!camera_pv_inverse(older_camera, inverse_older) || !camera_pv_inverse(newer_camera, inverse_newer)) return false;
  if (!interpolated_camera_pv(older_camera, newer_camera, alpha, projection_view)) return false;
  float world_older[16], world_newer[16], world[16];
  if (!world_from_capture(inverse_older, older_matrix, world_older) ||
      !world_from_capture(inverse_newer, newer_matrix, world_newer))
    return false;
  if (rigid_rank_deficient(world_older) || rigid_rank_deficient(world_newer)) return false;
  if (!slerp_rigid_world(world_older, world_newer, alpha, world)) return false;
  multiply(projection_view, world, out_matrix);
  return finite_matrix(out_matrix);
}

void interpolate_frame(const TimelineFrame &older, const CameraFrame &older_camera, const TimelineFrame &newer,
                       const CameraFrame &newer_camera, float alpha, const InterpolationLimits &limits,
                       std::vector<DrawTransform> &out, InterpolationCounters &counters) {
  counters.instants += 1;
  out.clear();
  out.reserve(newer.objects.size());

  const float clamped = std::fmax(0.0f, std::fmin(1.0f, alpha));
  const bool blend = clamped > 0.0f && clamped < 1.0f;
  enum class PairState { usable, refused_gap, refused_camera, camera_cut };
  PairState pair_state = PairState::usable;
  float projection_view[16] = {}, inverse_older[16] = {}, inverse_newer[16] = {};
  if (blend) {
    if (!(newer.time.ticks > older.time.ticks) || newer.time.ticks - older.time.ticks > limits.max_pair_ticks) {
      pair_state = PairState::refused_gap;
    } else if (!older_camera.valid || !newer_camera.valid) {
      pair_state = PairState::refused_camera;
    } else {
      bool cut = false;
      if (!camera_pair_cut(older_camera, newer_camera, limits, cut)) {
        pair_state = PairState::refused_camera;
      } else if (cut) {
        pair_state = PairState::camera_cut;
      } else if (!interpolated_camera_pv(older_camera, newer_camera, clamped, projection_view) ||
                 !camera_pv_inverse(older_camera, inverse_older) ||
                 !camera_pv_inverse(newer_camera, inverse_newer)) {
        pair_state = PairState::refused_camera;
      }
    }
    if (pair_state == PairState::usable)
      counters.pairs += 1;
    else
      counters.pairs_refused += 1;
  }

  // Pair the two frames by the timeline's stable identity, in the older frame's order, without collapsing instances.
  std::vector<char> used(older.objects.size(), 0);
  std::vector<const ObjectTransform *> matched(newer.objects.size(), nullptr);
  std::uint64_t paired = 0;
  for (std::size_t older_index = 0; older_index < older.objects.size(); ++older_index) {
    const ObjectTransform &object = older.objects[older_index];
    for (std::size_t newer_index = 0; newer_index < newer.objects.size(); ++newer_index) {
      if (used[newer_index]) continue;
      const ObjectTransform &candidate = newer.objects[newer_index];
      if (candidate.key.id != object.key.id || candidate.key.instance != object.key.instance) continue;
      used[newer_index] = 1;
      matched[newer_index] = &object;
      ++paired;
      break;
    }
  }
  counters.paired += paired;
  counters.only_older += older.objects.size() - paired;

  for (std::size_t index = 0; index < newer.objects.size(); ++index) {
    const ObjectTransform &object = newer.objects[index];
    DrawTransform draw;
    draw.key = object.key;
    const ObjectTransform *older_object = matched[index];
    if (older_object == nullptr) {
      std::memcpy(draw.matrix, object.matrix, sizeof draw.matrix);
      draw.source = TransformSource::captured_newer;
      counters.only_newer += 1;
      counters.nearest += 1;
      out.push_back(draw);
      continue;
    }
    if (!blend || clamped >= 1.0f) {
      std::memcpy(draw.matrix, object.matrix, sizeof draw.matrix);
      draw.source = TransformSource::captured_newer;
      counters.nearest += 1;
      out.push_back(draw);
      continue;
    }
    if (clamped <= 0.0f) {
      std::memcpy(draw.matrix, older_object->matrix, sizeof draw.matrix);
      draw.source = TransformSource::captured_older;
      counters.nearest += 1;
      out.push_back(draw);
      continue;
    }
    if (pair_state != PairState::usable) {
      std::memcpy(draw.matrix, object.matrix, sizeof draw.matrix);
      draw.source = TransformSource::captured_newer;
      counters.nearest += 1;
      if (pair_state == PairState::camera_cut) counters.dropped_discontinuity += 1;
      out.push_back(draw);
      continue;
    }
    // Object-level rules: a rigid world pose on both sides, a plausible step, then slerp; otherwise the measured
    // entrywise fallback of the captured matrices.
    float world_older[16], world_newer[16];
    const bool rigid = world_from_capture(inverse_older, older_object->matrix, world_older) &&
                       world_from_capture(inverse_newer, object.matrix, world_newer) &&
                       !rigid_rank_deficient(world_older) && !rigid_rank_deficient(world_newer);
    if (rigid) {
      float delta[3], distance = 0.0f;
      for (int axis = 0; axis < 3; ++axis) {
        delta[axis] = world_newer[axis * 4 + 3] - world_older[axis * 4 + 3];
        distance += delta[axis] * delta[axis];
      }
      if (std::sqrt(distance) > limits.max_object_translation_step_m) {
        std::memcpy(draw.matrix, object.matrix, sizeof draw.matrix);
        draw.source = TransformSource::captured_newer;
        counters.nearest += 1;
        counters.dropped_discontinuity += 1;
        out.push_back(draw);
        continue;
      }
      float world[16];
      if (slerp_rigid_world(world_older, world_newer, clamped, world)) {
        multiply(projection_view, world, draw.matrix);
        if (finite_matrix(draw.matrix)) {
          draw.source = TransformSource::interpolated;
          counters.interpolated += 1;
          out.push_back(draw);
          continue;
        }
      }
    }
    lerp_matrix(older_object->matrix, object.matrix, clamped, draw.matrix);
    draw.source = TransformSource::interpolated;
    counters.entrywise += 1;
    out.push_back(draw);
  }
}

}  // namespace gt4::coregs3
