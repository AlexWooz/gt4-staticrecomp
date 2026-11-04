// TASK-097: the entry point of the game executable.
//
// The pinned donor `main.cpp` (port slice200) is compiled unchanged with `main` renamed to `gt4_donor_main`
// (native/CMakeLists.txt). This `main` decides how the game was started:
//   - with an ELF argument (the launcher, the harnesses, the pins): explicit mode, straight into the donor main, so nothing
//     changes for them;
//   - without one (a double click, or only `--` options): standalone mode. The game finds its files from its own folder,
//     reads the launcher's config file, applies the built-in defaults and calls the donor main with the command line the
//     launcher would have built. Rules and tests: standalone.hpp / standalone_fixture.cpp.

#include "camera_360.hpp"
#include "controls.hpp"
#include "fov_adjust.hpp"
#include "trigger_range.hpp"
#include "frame_skip.hpp"
#include "standalone.hpp"
#include "window.hpp"

#ifdef GT4_GS_STREAM_CLIENT
#include "gs_stream/gs_stream_client.hpp"
#include "ps2/memory.hpp"
#endif

#include <windows.h>

#include <bcrypt.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

int gt4_donor_main(int argc, char **argv);

#ifdef GT4_HW_SHADER_PREPARE
namespace gt4::hw {
int prepare_shaders_main();  // IN-507: hw_gs/hw_gs_prepare.cpp
}
#endif

namespace {

namespace sa = gt4::standalone;

constexpr int kExitProblem = 3;      // a missing piece: a message was shown
constexpr int kExitAlreadyRunning = 4;

// TASK-161: one switch drives both sides of the full trigger range: the game's pedal scale (the translated statements read
// gt4::trigger_range) and the controls' trigger ranges (the whole byte with it on, 45 and 84 with it off), set before any
// pad is read, so the launcher and a direct start cannot disagree.
void configure_trigger_range() {
  gt4::trigger_range::configure_from_environment();
  gt4::controls::set_pressure_scale(gt4::trigger_range::enabled() ? gt4::controls::PressureScale::full
                                                                  : gt4::controls::PressureScale::stock);
}

std::wstring widen(const std::string &text) {
  if (text.empty()) return {};
  const int length = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
  return out;
}

void show_message(const std::string &title, const std::string &text) {
  // Test knob: GT4_STANDALONE_MESSAGE_FILE=<path> writes the text there instead of opening a message box (headless checks).
  char *file = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&file, &length, "GT4_STANDALONE_MESSAGE_FILE") == 0 && file != nullptr && *file != '\0') {
    std::ofstream(file, std::ios::binary) << title << "\n" << text << "\n";
    std::free(file);
    return;
  }
  std::free(file);
  MessageBoxW(nullptr, widen(text).c_str(), widen(title).c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

std::optional<std::string> sha256_of_file(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return std::nullopt;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::optional<std::string> result;
  if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
    std::vector<char> buffer(1 << 20);
    bool ok = true;
    while (file) {
      file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const std::streamsize got = file.gcount();
      if (got > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(got), 0) < 0) {
        ok = false;
        break;
      }
    }
    unsigned char digest[32];
    if (ok && file.eof() && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0) {
      static const char hex[] = "0123456789abcdef";
      std::string text;
      for (unsigned char byte : digest) {
        text += hex[byte >> 4];
        text += hex[byte & 15];
      }
      result = text;
    }
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return result;
}

sa::Access real_access() {
  sa::Access access;
  access.stat_file = [](const std::filesystem::path &path) -> std::optional<sa::FileInfo> {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return std::nullopt;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return std::nullopt;
    sa::FileInfo info;
    info.size_bytes = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    // FILETIME counts 100 ns ticks since 1601; the launcher records nanoseconds since 1970 (Rust SystemTime).
    constexpr std::uint64_t kUnixEpochTicks = 116444736000000000ull;
    const std::uint64_t ticks = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    info.modified_unix_nanos = ticks >= kUnixEpochTicks ? (ticks - kUnixEpochTicks) * 100 : 0;
    return info;
  };
  access.is_directory = [](const std::filesystem::path &path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  };
  access.read_text = [](const std::filesystem::path &path) -> std::optional<std::string> {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (text.size() > (4u << 20)) return std::nullopt; // setup receipts and ini files are small
    return text;
  };
  access.sha256_file = sha256_of_file;
  // The game takes the disc path in the ANSI code page. Accents fit it on a Western Windows and go as they are; a path with other
  // characters goes by its short name.
  access.openable_path = [](const std::filesystem::path &path) {
    const std::wstring &wide = path.native();
    if (std::all_of(wide.begin(), wide.end(), [](wchar_t c) { return c < 128; }) || GetACP() == CP_UTF8) return path;
    BOOL lost = FALSE;
    const int fits = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, &lost);
    if (fits > 0 && !lost) return path;
    std::wstring shorter(GetShortPathNameW(wide.c_str(), nullptr, 0), L'\0');
    const DWORD written = shorter.empty() ? 0 : GetShortPathNameW(wide.c_str(), shorter.data(), static_cast<DWORD>(shorter.size()));
    if (written == 0 || written >= shorter.size()) return path;
    shorter.resize(written);
    return std::filesystem::path(shorter);
  };
  access.environment = [](const std::string &name) -> std::optional<std::string> {
    char *value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name.c_str()) != 0 || value == nullptr) return std::nullopt;
    std::string text(value);
    std::free(value);
    return text;
  };
  return access;
}

std::filesystem::path environment_path(const sa::Access &access, const char *name) {
  const std::optional<std::string> value = access.environment(name);
  return value ? std::filesystem::path(*value) : std::filesystem::path();
}

// The launcher's own lock (`launcher.lock` beside the card, LockFileEx on the whole file as Rust's File::try_lock does), so
// a direct start and a launcher start never share a memory card. The handle stays open until the process ends.
bool take_card_lock(const std::filesystem::path &card) {
  std::error_code error;
  std::filesystem::create_directories(card.parent_path(), error);
  const HANDLE file = CreateFileW((card.parent_path() / L"launcher.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return true; // cannot tell; the game itself decides about the card
  OVERLAPPED overlapped{};
  if (LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &overlapped)) return true;
  const DWORD code = GetLastError();
  CloseHandle(file);
  return code != ERROR_LOCK_VIOLATION;
}

// A double click gives the process its own console. Its output then goes to the session's game.log and the console window is
// released. Started from a terminal, or with the output piped or redirected, the streams are left alone.
void redirect_output_to_log(const std::filesystem::path &log) {
  DWORD processes[2];
  const bool own_console = GetConsoleProcessList(processes, 2) == 1;
  DWORD mode = 0;
  const bool stdout_is_console = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
  if (!own_console || !stdout_is_console) return;
  std::FILE *stream = nullptr;
  if (_wfreopen_s(&stream, log.c_str(), L"w", stdout) != 0 || stream == nullptr) return;
  _dup2(_fileno(stdout), _fileno(stderr));
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  FreeConsole();
}

void prune_sessions(const std::filesystem::path &sessions) {
  std::error_code error;
  std::vector<std::string> names;
  for (const auto &entry : std::filesystem::directory_iterator(sessions, error)) {
    if (entry.is_directory(error)) names.push_back(entry.path().filename().string());
  }
  for (const std::string &old : sa::sessions_to_remove(names, sa::kKeepSessions)) std::filesystem::remove_all(sessions / old, error);
}

int run_standalone(int argc, char **argv) {
  _putenv_s("GT4_DIRECT_START", "1");  // the GS client's message says where the renderer is chosen: no launcher will store it
  const sa::Access access = real_access();
  wchar_t module[32768];
  const DWORD length = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
  const std::filesystem::path executable = length > 0 && length < std::size(module) ? std::filesystem::path(module) : std::filesystem::path();

  sa::Check why;
  const std::optional<sa::Layout> layout =
      sa::find_layout(executable, environment_path(access, "LOCALAPPDATA"), environment_path(access, "APPDATA"), access, why);
  if (!layout) {
    show_message("Gran Turismo 4", sa::message_for(why, "Gran Turismo 4"));
    return kExitProblem;
  }
  const char *display_name = std::string(layout->game.id) == "spec2" ? "Gran Turismo 4 Spec II" : "Gran Turismo 4";

  sa::Inputs inputs;
  inputs.layout = &*layout;
  std::vector<std::string> config_notes;
  if (const std::optional<std::string> text = access.read_text(layout->config_dir / sa::kConfigFileName)) {
    sa::Config config = sa::parse_config(*text);
    config_notes = config.warnings;
    inputs.config = std::move(config);
  } else {
    config_notes.push_back(std::string("no ") + sa::kConfigFileName + " in " + layout->config_dir.string() + ": built-in defaults");
  }
  sa::Check check = sa::check_game_files(*layout, access, inputs.disc, inputs.elf);
  sa::Renderer renderer;
  if (check.problem == sa::Problem::None && sa::renderer_needed(inputs, access)) {
    check = sa::find_renderer(*layout, access, renderer);
    if (check.problem == sa::Problem::None) inputs.renderer = &renderer;
  }
  if (check.problem != sa::Problem::None) {
    show_message(display_name, sa::message_for(check, display_name));
    return kExitProblem;
  }
  if (!take_card_lock(layout->card)) {
    show_message(display_name, std::string(display_name) + " is already running.");
    return kExitAlreadyRunning;
  }

  inputs.gs_ini_exists = access.stat_file(layout->config_dir / sa::kGsConfigFileName).has_value();
  inputs.controls_exists = access.stat_file(layout->user_root / "controls.toml").has_value();
  for (int i = 1; i < argc; ++i) inputs.user_arguments.emplace_back(argv[i]);

  const std::uint64_t now = static_cast<std::uint64_t>(std::time(nullptr));
  inputs.session_dir = layout->sessions / sa::session_name(now);
  std::error_code error;
  std::filesystem::create_directories(inputs.session_dir, error);
  prune_sessions(layout->sessions);
  redirect_output_to_log(inputs.session_dir / "game.log");
  if (!error) SetCurrentDirectoryW(inputs.session_dir.c_str());

  const sa::Resolved resolved = sa::resolve(inputs, access);
  std::fprintf(stderr, "[standalone] %s from %s\n", layout->game.id, layout->install_root.string().c_str());
  std::fprintf(stderr, "[standalone] data %s, user %s\n", layout->data_root.string().c_str(), layout->user_root.string().c_str());
  for (const std::string &note : config_notes) std::fprintf(stderr, "[standalone] %s\n", note.c_str());
  for (const auto &[name, value] : resolved.set) {
    _putenv_s(name.c_str(), value.c_str());
    std::fprintf(stderr, "[standalone] %s=%s\n", name.c_str(), value.c_str());
  }

  std::vector<std::string> storage;
  storage.push_back(executable.string());
  storage.insert(storage.end(), resolved.arguments.begin(), resolved.arguments.end());
  std::vector<char *> pointers;
  for (std::string &argument : storage) pointers.push_back(argument.data());
  pointers.push_back(nullptr);
  gt4::camera360::configure_from_environment();  // TASK-159: game.ini camera_360 arrives here as GT4_CAMERA_360
  gt4::fov_adjust::configure_from_environment();  // TASK-160: game.ini fov_adjust arrives here as GT4_FOV_ADJUST
  configure_trigger_range();                       // TASK-161: game.ini trigger_full_range as GT4_TRIGGER_FULL_RANGE
  return gt4_donor_main(static_cast<int>(storage.size()), pointers.data());
}

} // namespace

int main(int argc, char **argv) {
#ifdef GT4_HW_SHADER_PREPARE
  if (argc == 2 && std::string(argv[1]) == "--prepare-shaders") return gt4::hw::prepare_shaders_main();
#endif
#ifdef GT4_GS_STREAM_CLIENT
  // TASK-116 step 0 (P0 gate): the guest RAM window that turns a READ_FIFO host pointer into an EE address in the
  // GT4_GS_READBACK_LOG lines (native/gs_stream/gs_readback_probe.hpp). Nothing happens when the switch is unset.
  gt4::gs_stream::set_guest_ram(ps2::g_ram, ps2::kRamSize);
#endif
  if (!sa::is_standalone_start(argc, argv)) {
    // IN-520: the window sets up the recording and replay files itself; a headless run replays here.
    bool window = false;
    for (int i = 1; i < argc; ++i) {
      window = window || std::strcmp(argv[i], "--window") == 0 || std::strcmp(argv[i], "--fullscreen") == 0;
    }
    gt4::frame_skip::start_from_environment();
    gt4::camera360::configure_from_environment();  // TASK-159: an invalid GT4_CAMERA_360 stops the start here
    gt4::fov_adjust::configure_from_environment();  // TASK-160: an invalid GT4_FOV_ADJUST stops the start here
    configure_trigger_range();                       // TASK-161: an invalid GT4_TRIGGER_FULL_RANGE stops the start here
    if (!window && !install_input_replay_without_window()) return 1;
    return gt4_donor_main(argc, argv);
  }
  return run_standalone(argc, argv);
}
