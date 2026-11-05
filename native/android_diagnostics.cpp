// SPDX-License-Identifier: MIT
// TASK-180 test2 (diagnostics only, no behaviour change): where the heavy threads run on a phone's CPU clusters, the
// clusters' current frequencies, and the thermal status, for the F8 report and the HUD log.
// - Threads: the ones thread_placement names (gt4-ee, gt4-gs, gt4-vu1, gt4-present). Each VBlank-length tick (16 ms) the
//   sampler reads the CPU each one last ran on (/proc/self/task/<tid>/stat, field 39) and counts it per cluster.
// - Clusters: CPUs grouped by /sys/devices/system/cpu/cpuN/cpu_capacity (else cpufreq/cpuinfo_max_freq), smallest
//   first: little, mid, prime (two groups: little, big).
// - Frequencies: cpufreq/policyN/scaling_cur_freq, when readable.
// - Thermal status: AThermal_getCurrentThermalStatus (Android 11+, found at run time), at F7 and at F8.
#include "android_diagnostics.hpp"

#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gt4::android_diag {
namespace {

const char *const kThreads[] = {"gt4-ee", "gt4-gs", "gt4-vu1", "gt4-present"};
constexpr std::size_t kThreadCount = sizeof kThreads / sizeof kThreads[0];

long read_long(const std::string &path) {
  long value = -1;
  if (std::FILE *file = std::fopen(path.c_str(), "r")) {
    if (std::fscanf(file, "%ld", &value) != 1) value = -1;
    std::fclose(file);
  }
  return value;
}

std::string read_line(const std::string &path) {
  char buffer[256] = {};
  std::string out;
  if (std::FILE *file = std::fopen(path.c_str(), "r")) {
    if (std::fgets(buffer, sizeof buffer, file) != nullptr) out = buffer;
    std::fclose(file);
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

struct Clusters {
  std::vector<int> of_cpu;  // cluster index of each CPU
  std::vector<std::string> names;
};

Clusters find_clusters() {
  Clusters clusters;
  const long cpus = sysconf(_SC_NPROCESSORS_CONF);
  std::vector<long> weight(static_cast<std::size_t>(std::max(cpus, 1L)), 0);
  for (long cpu = 0; cpu < cpus; ++cpu) {
    const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu);
    long value = read_long(base + "/cpu_capacity");
    if (value < 0) value = read_long(base + "/cpufreq/cpuinfo_max_freq");
    weight[static_cast<std::size_t>(cpu)] = value;
  }
  std::vector<long> distinct = weight;
  std::sort(distinct.begin(), distinct.end());
  distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
  for (long value : weight)
    clusters.of_cpu.push_back(static_cast<int>(std::lower_bound(distinct.begin(), distinct.end(), value) - distinct.begin()));
  if (distinct.size() == 3) {
    clusters.names = {"little", "mid", "prime"};
  } else if (distinct.size() == 2) {
    clusters.names = {"little", "big"};
  } else {
    for (std::size_t i = 0; i < distinct.size(); ++i) clusters.names.push_back("c" + std::to_string(i));
  }
  return clusters;
}

// The CPU a thread last ran on: field 39 of /proc/self/task/<tid>/stat (fields counted from 1, after the comm).
int last_cpu_of(int tid) {
  const std::string text = read_line("/proc/self/task/" + std::to_string(tid) + "/stat");
  const std::size_t close = text.rfind(')');
  if (close == std::string::npos) return -1;
  int field = 2;
  const char *at = text.c_str() + close + 1;
  while (*at != '\0') {
    while (*at == ' ') ++at;
    if (*at == '\0') break;
    ++field;
    if (field == 39) return std::atoi(at);
    while (*at != '\0' && *at != ' ') ++at;
  }
  return -1;
}

int thermal_status() {
  using Acquire = void *(*)();
  using Status = int (*)(void *);
  static void *manager = nullptr;
  static Status status = nullptr;
  static bool looked = false;
  if (!looked) {
    looked = true;
    if (void *library = dlopen("libandroid.so", RTLD_NOW)) {
      auto acquire = reinterpret_cast<Acquire>(dlsym(library, "AThermal_acquireManager"));
      status = reinterpret_cast<Status>(dlsym(library, "AThermal_getCurrentThermalStatus"));
      if (acquire != nullptr) manager = acquire();
    }
  }
  return manager != nullptr && status != nullptr ? status(manager) : -1;
}

const char *thermal_name(int status) {
  static const char *const kNames[] = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
  return status >= 0 && status <= 6 ? kNames[status] : "unknown";
}

std::string frequencies() {
  std::string out;
  for (int policy = 0; policy < 16; ++policy) {
    const std::string base = "/sys/devices/system/cpu/cpufreq/policy" + std::to_string(policy);
    const long khz = read_long(base + "/scaling_cur_freq");
    if (khz < 0) continue;
    const std::string cpus = read_line(base + "/related_cpus");
    out += (out.empty() ? "" : ", ") + std::string("cpus ") + cpus + " " + std::to_string(khz / 1000) + " MHz";
  }
  return out.empty() ? "not readable" : out;
}

class Sampler {
public:
  Sampler() : clusters_(find_clusters()), counts_(kThreadCount, std::vector<std::uint64_t>(clusters_.names.size(), 0)) {
    thread_ = std::thread([this] { run(); });
    thread_.detach();
  }

  void reset() {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto &row : counts_) std::fill(row.begin(), row.end(), 0);
  }

  std::string shares() {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::string out;
    for (std::size_t thread = 0; thread < kThreadCount; ++thread) {
      std::uint64_t total = 0;
      for (std::uint64_t count : counts_[thread]) total += count;
      if (total == 0) continue;
      out += (out.empty() ? "" : "; ") + std::string(kThreads[thread] + 4) + ":";
      for (std::size_t cluster = 0; cluster < clusters_.names.size(); ++cluster) {
        char part[48];
        std::snprintf(part, sizeof part, " %s %.0f%%", clusters_.names[cluster].c_str(),
                      100.0 * static_cast<double>(counts_[thread][cluster]) / static_cast<double>(total));
        out += part;
      }
    }
    return out.empty() ? "no samples yet" : out;
  }

  std::string layout() const {
    std::string out;
    for (std::size_t cluster = 0; cluster < clusters_.names.size(); ++cluster) {
      std::string cpus;
      for (std::size_t cpu = 0; cpu < clusters_.of_cpu.size(); ++cpu)
        if (clusters_.of_cpu[cpu] == static_cast<int>(cluster)) cpus += (cpus.empty() ? "" : ",") + std::to_string(cpu);
      out += (out.empty() ? "" : ", ") + clusters_.names[cluster] + " " + cpus;
    }
    return out;
  }

private:
  void find_threads() {
    tids_.assign(kThreadCount, -1);
    DIR *tasks = opendir("/proc/self/task");
    if (tasks == nullptr) return;
    while (dirent *entry = readdir(tasks)) {
      if (entry->d_name[0] == '.') continue;
      const std::string comm = read_line(std::string("/proc/self/task/") + entry->d_name + "/comm");
      for (std::size_t thread = 0; thread < kThreadCount; ++thread)
        if (comm == kThreads[thread]) tids_[thread] = std::atoi(entry->d_name);
    }
    closedir(tasks);
  }

  void run() {
    std::uint64_t ticks = 0;
    const bool hud = std::getenv("GT4_HW_HUD") != nullptr;
    for (;;) {
      if (ticks % 60 == 0) find_threads();  // about once a second: threads may start late
      for (std::size_t thread = 0; thread < kThreadCount; ++thread) {
        if (tids_[thread] < 0) continue;
        const int cpu = last_cpu_of(tids_[thread]);
        if (cpu < 0 || cpu >= static_cast<int>(clusters_.of_cpu.size())) continue;
        const std::lock_guard<std::mutex> lock(mutex_);
        ++counts_[thread][static_cast<std::size_t>(clusters_.of_cpu[static_cast<std::size_t>(cpu)])];
      }
      ++ticks;
      if (hud && ticks % 600 == 0) {
        std::fprintf(stderr, "[hud] clusters (last 10 s): %s; frequencies: %s; thermal %s\n", shares().c_str(),
                     frequencies().c_str(), thermal_name(thermal_status()));
        reset();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
  }

  Clusters clusters_;
  std::vector<std::vector<std::uint64_t>> counts_;
  std::vector<int> tids_;
  std::mutex mutex_;
  std::thread thread_;
};

Sampler &sampler() {
  static Sampler instance;
  return instance;
}

}  // namespace

void start() {
  std::fprintf(stderr, "[cpu] clusters: %s; thermal %s\n", sampler().layout().c_str(), thermal_name(thermal_status()));
}

void interval_start() {
  sampler().reset();
  std::fprintf(stderr, "[cpu] F7: thermal status %s; frequencies: %s\n", thermal_name(thermal_status()), frequencies().c_str());
}

void report(std::FILE *output) {
  std::fprintf(output, "[cpu] threads by cluster since F7 (%s): %s\n", sampler().layout().c_str(), sampler().shares().c_str());
  std::fprintf(output, "[cpu] frequencies at F8: %s\n", frequencies().c_str());
  std::fprintf(output, "[cpu] thermal status at F8: %s\n", thermal_name(thermal_status()));
}

}  // namespace gt4::android_diag
