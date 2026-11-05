// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// TASK-180: the performance HUD's system side outside Windows (hw_hud_system.cpp is the Win32 form: GDI glyphs, PDH
// GPU counters). Android:
// - glyphs: drawn by the game activity with the system's monospace font (GameActivity.glyphAtlas, through SDL's JNI
//   environment), in the same strip layout as the GDI atlas;
// - CPU: the process's CPU time over wall time and processor threads, twice a second (as /proc/self/stat would give);
// - GPU: the Adreno driver's own busy share (kgsl sysfs) when the system lets this app read it; otherwise unknown;
// - refresh rate: the display's current rate (GameActivity.displayRefreshHz; phones change it on the fly);
// - memory: the resident size from /proc/self/statm.
// A reading that is not available stays unknown (the panel shows "n/a"), never estimated.
#include "hw_hud_system.hpp"

#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

namespace gt4::hw::hud {

#ifdef __ANDROID__
namespace {

// Calls an instance method of the game activity. `call` gets the JNI environment, the activity and the method id; the
// local references are released here.
template <typename Call>
bool with_activity_method(const char *name, const char *signature, Call call) {
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) return false;
  jclass type = env->GetObjectClass(activity);
  jmethodID method = env->GetMethodID(type, name, signature);
  bool done = false;
  if (method != nullptr) {
    call(env, activity, method);
    done = !env->ExceptionCheck();
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(type);
  env->DeleteLocalRef(activity);
  return done;
}

std::uint32_t display_refresh_hz() {
  float hertz = 0.0f;
  with_activity_method("displayRefreshHz", "()F", [&](JNIEnv *env, jobject activity, jmethodID method) {
    hertz = env->CallFloatMethod(activity, method);
  });
  return hertz > 0.0f ? static_cast<std::uint32_t>(hertz + 0.5f) : 0u;
}

// The Adreno driver's busy share: "gpu_busy_percentage" ("37 %"), else "gpubusy" (busy and total ticks of the last
// window). Many phones keep these from apps (SELinux); then the GPU line stays unknown.
double kgsl_busy_percent() {
  if (std::FILE *file = std::fopen("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", "r")) {
    int percent = -1;
    const int read = std::fscanf(file, "%d", &percent);
    std::fclose(file);
    if (read == 1 && percent >= 0 && percent <= 100) return percent;
  }
  if (std::FILE *file = std::fopen("/sys/class/kgsl/kgsl-3d0/gpubusy", "r")) {
    long long busy = 0, total = 0;
    const int read = std::fscanf(file, "%lld %lld", &busy, &total);
    std::fclose(file);
    if (read == 2 && total > 0 && busy >= 0 && busy <= total) return 100.0 * static_cast<double>(busy) / static_cast<double>(total);
  }
  return -1.0;
}

}  // namespace
#endif

GlyphAtlas make_glyph_atlas(int pixel_height) {
  GlyphAtlas atlas;
#ifdef __ANDROID__
  // GameActivity.glyphAtlas(pixel height) returns {cell width, cell height, coverage...}: the 95 glyphs side by side,
  // one row of cells, coverage 0-255, row-major over the whole strip (GlyphAtlas::at).
  constexpr int glyphs = GlyphAtlas::kLast - GlyphAtlas::kFirst + 1;
  const bool ok = with_activity_method("glyphAtlas", "(I)[I", [&](JNIEnv *env, jobject activity, jmethodID method) {
    auto array = static_cast<jintArray>(env->CallObjectMethod(activity, method, static_cast<jint>(pixel_height)));
    if (env->ExceptionCheck() || array == nullptr) return;
    const jsize length = env->GetArrayLength(array);
    std::vector<jint> values(static_cast<std::size_t>(length));
    env->GetIntArrayRegion(array, 0, length, values.data());
    env->DeleteLocalRef(array);
    if (length < 2) return;
    const int width = values[0], height = values[1];
    const std::size_t expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * glyphs;
    if (width <= 0 || height <= 0 || static_cast<std::size_t>(length) != expected + 2) return;
    atlas.cell_width = width;
    atlas.cell_height = height;
    atlas.coverage.resize(expected);
    for (std::size_t i = 0; i < expected; ++i) atlas.coverage[i] = static_cast<std::uint8_t>(values[i + 2] & 0xFF);
  });
  if (!ok || atlas.coverage.empty()) {
    std::fprintf(stderr, "[hud] no glyphs from the game activity: the panel draws without text\n");
    return {};
  }
  std::fprintf(stderr, "[hud] glyphs: system monospace font, cells %dx%d for %d px\n", atlas.cell_width, atlas.cell_height,
               pixel_height);
#else
  (void)pixel_height;
#endif
  return atlas;
}

SystemSampler::SystemSampler(void *window, std::function<void(const SystemReading &)> publish)
    : window_(window), publish_(std::move(publish)), thread_([this] { run(); }) {}

SystemSampler::~SystemSampler() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  stop_changed_.notify_all();
  thread_.join();
}

void SystemSampler::run() {
  const long threads = sysconf(_SC_NPROCESSORS_ONLN);
  const auto process_cpu_s = [] {
    timespec value{};
    return clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) == 0 ? static_cast<double>(value.tv_sec) + value.tv_nsec / 1e9
                                                                 : -1.0;
  };
  double last_cpu = process_cpu_s();
  auto last_wall = std::chrono::steady_clock::now();
#ifdef __ANDROID__
  bool gpu_reported = false;
#endif
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_) {
    stop_changed_.wait_for(lock, std::chrono::milliseconds(kPeriodMs));
    if (stop_) break;
    SystemReading reading;
    const double cpu = process_cpu_s();
    const auto wall = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(wall - last_wall).count();
    if (cpu >= 0.0 && last_cpu >= 0.0 && seconds > 0.0 && threads > 0)
      reading.cpu_percent = (cpu - last_cpu) / seconds / static_cast<double>(threads) * 100.0;
    last_cpu = cpu;
    last_wall = wall;
    if (std::FILE *statm = std::fopen("/proc/self/statm", "r")) {
      unsigned long size = 0, resident = 0;
      if (std::fscanf(statm, "%lu %lu", &size, &resident) == 2)
        reading.memory_mb = static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
      std::fclose(statm);
    }
    lock.unlock();
#ifdef __ANDROID__
    reading.refresh_hz = display_refresh_hz();
    reading.gpu_percent = kgsl_busy_percent();
    if (!gpu_reported) {
      gpu_reported = true;
      std::fprintf(stderr, "[hud] GPU busy share: %s\n",
                   reading.gpu_percent >= 0.0 ? "the Adreno driver's (kgsl sysfs)" : "not readable by this app (shown n/a)");
    }
#endif
    publish_(reading);
    lock.lock();
  }
}

}  // namespace gt4::hw::hud
