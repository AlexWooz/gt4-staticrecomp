// SPDX-License-Identifier: MIT
// TASK-180 (docs/ANDROID-PORT-PLAN.md section 4): the entry point of libgt4.so, called by SDL3's SDLActivity on its game
// thread (SDL_main). The game is started only by the launcher's Play (the app's GameActivity), with the launch contract
// the Windows launcher gives gt4vulkan.exe:
//   arguments    the command line of gt4l_core::launch (ELF, --ntsc, --disc, --memcard, --misses, ...). The disc image is a
//                document the app holds a read permission to; GameActivity opens it and passes --disc fd:<descriptor>,
//                and the game reads the image through that descriptor (never a copy).
//   environment  the game.ini values (GT4_*), set by GameActivity before SDL_main, plus:
//                GT4_ANDROID_LOG  the session's game.log (stdout and stderr go there and to logcat, tag GT4Recomp);
//                GT4_ANDROID_CWD  the session folder, the working directory (the Windows launcher's working directory).
#include "game_start.hpp"
#include "platform/platform.hpp"

#include "ps2/disc.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <android/log.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

// The game's lines in logcat (`adb logcat -s GT4Recomp-game GT4Recomp-launcher`); the launcher's own use
// GT4Recomp-launcher.
constexpr const char *kLogTag = "GT4Recomp-game";

// stdout and stderr go to the session's game.log (when the launcher names one) and to logcat: a pipe read by a thread
// that writes both.
void redirect_output(const char *log_path) {
  int pipe_ends[2];
  if (pipe(pipe_ends) != 0) return;
  std::FILE *log = log_path != nullptr ? std::fopen(log_path, "w") : nullptr;
  dup2(pipe_ends[1], STDOUT_FILENO);
  dup2(pipe_ends[1], STDERR_FILENO);
  close(pipe_ends[1]);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  std::thread([read_end = pipe_ends[0], log] {
    std::string line;
    char buffer[4096];
    for (;;) {
      const ssize_t got = read(read_end, buffer, sizeof buffer);
      if (got <= 0) break;
      if (log != nullptr) {
        std::fwrite(buffer, 1, static_cast<std::size_t>(got), log);
        std::fflush(log);
      }
      for (ssize_t i = 0; i < got; ++i) {
        if (buffer[i] == '\n') {
          __android_log_write(ANDROID_LOG_INFO, kLogTag, line.c_str());
          line.clear();
        } else {
          line.push_back(buffer[i]);
        }
      }
    }
  }).detach();
}

constexpr const char *kDescriptorPrefix = "fd:";

// The disc image through the descriptor GameActivity opened (pread: no shared file position, any thread).
bool open_disc_descriptor(int descriptor) {
  struct stat status {};
  if (descriptor < 0 || fstat(descriptor, &status) != 0 || status.st_size <= 0) return false;
  const std::string name = "disc image (descriptor " + std::to_string(descriptor) + ")";
  return ps2::disc::open(name, static_cast<std::uint64_t>(status.st_size),
                         [descriptor](std::uint64_t offset, void *out, std::size_t bytes) -> std::size_t {
                           std::size_t done = 0;
                           while (done < bytes) {
                             const ssize_t got = pread(descriptor, static_cast<char *>(out) + done, bytes - done,
                                                       static_cast<off_t>(offset + done));
                             if (got <= 0) break;
                             done += static_cast<std::size_t>(got);
                           }
                           return done;
                         });
}

// The process ends here with the game's result, so Android records it (ApplicationExitInfo: exit, status = code) and
// the launcher tells a clean end from a crash. The log thread gets a moment to write the last lines.
[[noreturn]] void end_process(int code) {
  std::fprintf(stderr, "[android] the game process ends with %d\n", code);
  std::fflush(stdout);
  std::fflush(stderr);
  usleep(300 * 1000);
  _exit(code);
}

}  // namespace

namespace gt4::hw {
int prepare_shaders_main();  // hw_gs/hw_gs_prepare.cpp (IN-507)
}

// TASK-180 test2: the shader prepare (the launcher's `--prepare-shaders` on Windows), run by ShaderPrepareService in its
// own process (":shaders") before a CoreGS 2.0 session. `environment` is the launch plan's (NAME=VALUE: the same shader
// cache folder, renderer and card choice as the game); `log` receives stdout and stderr directly (no pipe: the last
// "shaders ready" line must be in the file when the process ends). The launcher reads the progress lines from it. The
// process ends inside prepare_shaders_main (exit 0: the cache is ready).
extern "C" JNIEXPORT void JNICALL Java_io_github_moesuito_gt4recomp_ShaderPrepareService_nativePrepare(JNIEnv *env, jclass,
                                                                                                         jobjectArray environment,
                                                                                                         jstring log) {
  const char *log_path = env->GetStringUTFChars(log, nullptr);
  const int file = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  env->ReleaseStringUTFChars(log, log_path);
  if (file >= 0) {
    dup2(file, STDOUT_FILENO);
    dup2(file, STDERR_FILENO);
    close(file);
  }
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  const jsize count = env->GetArrayLength(environment);
  for (jsize i = 0; i < count; ++i) {
    auto entry = static_cast<jstring>(env->GetObjectArrayElement(environment, i));
    const char *text = env->GetStringUTFChars(entry, nullptr);
    const std::string line = text;
    env->ReleaseStringUTFChars(entry, text);
    env->DeleteLocalRef(entry);
    const std::size_t equals = line.find('=');
    if (equals != std::string::npos && equals > 0)
      gt4::platform::set_environment(line.substr(0, equals).c_str(), line.substr(equals + 1).c_str());
  }
  std::fprintf(stderr, "[shaders] prepare in process %d\n", static_cast<int>(getpid()));
  __android_log_print(ANDROID_LOG_INFO, "GT4Recomp-game", "shader prepare in process %d", static_cast<int>(getpid()));
  const int result = gt4::hw::prepare_shaders_main();  // ends the process itself when it finishes
  std::fprintf(stderr, "[shaders] prepare stopped with %d\n", result);
  std::fflush(stdout);
  _exit(result);
}

int main(int argc, char **argv) {
  redirect_output(std::getenv("GT4_ANDROID_LOG"));
#if defined(__aarch64__)
  constexpr const char *kAbi = "arm64-v8a";
#else
  constexpr const char *kAbi = "x86_64";
#endif
  std::fprintf(stderr, "[android] GT4Recomp game library (%s), process %d, %d arguments\n", kAbi, static_cast<int>(getpid()),
               argc - 1);
  if (argc < 2) {
    gt4::platform::show_error("Gran Turismo 4", "Start Gran Turismo 4 from its launcher: open the Gran Turismo 4 app and "
                                                "press Play.");
    end_process(3);
  }
  // The EE runs on this thread (SDL's game thread): its stack size matters for the large translated functions (Android
  // plan risk 7; Windows runs it on the main thread's 1 MB).
  pthread_attr_t attributes;
  if (pthread_getattr_np(pthread_self(), &attributes) == 0) {
    std::size_t stack_bytes = 0;
    pthread_attr_getstacksize(&attributes, &stack_bytes);
    pthread_attr_destroy(&attributes);
    std::fprintf(stderr, "[android] game thread stack %zu KiB\n", stack_bytes / 1024);
  }
  if (const char *folder = std::getenv("GT4_ANDROID_CWD")) {
    std::error_code error;
    std::filesystem::current_path(folder, error);
    if (error) std::fprintf(stderr, "[android] cannot enter %s: %s\n", folder, error.message().c_str());
  }

  // The launch contract's command line, with the disc descriptor taken out: the image is opened here, so the runtime
  // (which opens --disc by path) finds it open.
  std::vector<std::string> storage = {"libgt4.so"};
  bool disc_ready = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--disc" && i + 1 < argc && std::strncmp(argv[i + 1], kDescriptorPrefix, 3) == 0) {
      const int descriptor = std::atoi(argv[i + 1] + 3);
      disc_ready = open_disc_descriptor(descriptor);
      ++i;
      continue;
    }
    storage.push_back(argument);
  }
  if (!disc_ready) {
    std::fprintf(stderr, "[android] the disc image could not be opened from its descriptor\n");
    gt4::platform::show_error("Gran Turismo 4", "The disc image cannot be read. Choose it again in the launcher "
                                                "(Settings > Games).");
    end_process(3);
  }
  std::vector<char *> pointers;
  for (std::string &argument : storage) {
    std::fprintf(stderr, "[android] argument %s\n", argument.c_str());
    pointers.push_back(argument.data());
  }
  pointers.push_back(nullptr);
  gt4::start::note_guest_ram();
  const int result = gt4::start::run_explicit(static_cast<int>(storage.size()), pointers.data());
  end_process(result);
}
