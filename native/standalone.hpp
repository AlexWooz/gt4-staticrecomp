#pragma once

// TASK-097: the game starts on its own.
//
// The launcher is an easier way to start the game and to manage settings; it is not needed for a start. A start without an
// ELF argument (a double click, or only `--` options) is a *standalone* start: this module works out what the launcher would
// have passed, from the executable's own folder, the launcher's config file and built-in defaults. A start with an ELF
// argument is the *explicit* mode of the launcher, the harnesses and the pins; it never reaches this code, so it runs
// exactly as before (decisions in TASKS.md, TASK-097).
//
// Everything here is pure logic over injected file and environment access (the fixture `native_standalone` fakes both).
// `game_main.cpp` supplies the real ones and the Windows parts (message box, lock, session folder, log redirect).
//
// Order of every value: explicit argument or environment variable > config file > built-in default.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gt4::standalone {

// ---- Which game is this executable -------------------------------------------------------------------------------------

struct GameId {
  const char *id;       // folder name under games\ (data) : "gt4", "spec2"
  const char *elf_name; // the prepared ELF in games\<id>\prepared (the argv0 rule of the donor: the name stays)
};

// `gt4vulkan.exe` -> gt4 (SCUS_973.28), `gt4vulkan-spec2.exe` -> spec2 (SCUS_974.36). Case-insensitive. Other names: none.
std::optional<GameId> game_for_executable(std::string_view file_name);

// ---- Standalone or explicit ---------------------------------------------------------------------------------------------

// True when argv has no ELF argument: argc == 1, or argv[1] starts with "--" and is not "--portable-check".
bool is_standalone_start(int argc, const char *const *argv);

// ---- Injected access ----------------------------------------------------------------------------------------------------

struct FileInfo {
  std::uint64_t size_bytes = 0;
  std::uint64_t modified_unix_nanos = 0; // as the launcher records it (Rust SystemTime since the Unix epoch)
};

struct Access {
  std::function<std::optional<FileInfo>(const std::filesystem::path &)> stat_file; // none: not a regular file
  std::function<bool(const std::filesystem::path &)> is_directory;
  std::function<std::optional<std::string>(const std::filesystem::path &)> read_text; // whole small file
  std::function<std::optional<std::string>(const std::filesystem::path &)> sha256_file; // 64 lowercase hex digits
  std::function<std::optional<std::string>(const std::string &)> environment;           // empty value counts as unset
  // The name the game can open a linked image by (its short name for a path outside the ANSI code page); unset: the path itself.
  std::function<std::filesystem::path(const std::filesystem::path &)> openable_path;
};

// ---- Folders ------------------------------------------------------------------------------------------------------------

struct Layout {
  GameId game{};                      // from the executable's file name
  std::filesystem::path executable;   // <install>\game\gt4vulkan.exe
  std::filesystem::path install_root; // <install>
  std::filesystem::path data_root;    // the data folder that holds this game's files
  std::filesystem::path game_root;    // <data>\games\<id>
  std::filesystem::path prepared;     // <game_root>\prepared
  std::filesystem::path disc_folder;  // <game_root>\disc
  std::filesystem::path card;         // <game_root>\runtime\card.ps2
  std::filesystem::path gs_root;      // <data>\gs
  std::filesystem::path sessions;     // <data>\sessions
  std::filesystem::path user_root;    // <install>\user when that folder exists, else %APPDATA%\GT4Recomp
  std::filesystem::path config_dir;   // <user_root>\config
};

// What can stop a standalone start.
enum class Problem {
  None,
  NotInstalled,    // the executable is not in an install's game folder, or is not a known game executable
  NoGameFiles,     // no setup receipt / prepared files for this game
  GameFilesChanged, // the receipt, the ELF or the disc copy does not pass the launcher's cheap check
  NoRenderer,      // GSdx (gt4-gs) is not installed
  RendererChanged, // gt4-gs.exe does not match the hash recorded at install
};

struct Check {
  Problem problem = Problem::None;
  std::string detail; // what exactly (for the message and the log)
};

// Finds the folders from the executable path. `local_app_data` and `app_data` come from the environment (may be empty).
// Returns none (and `Problem::NotInstalled` in `why`) when the executable is not `<install>\game\gt4vulkan*.exe`.
std::optional<Layout> find_layout(const std::filesystem::path &executable, const std::filesystem::path &local_app_data,
                                  const std::filesystem::path &app_data, const Access &access, Check &why);

// ---- Setup receipt (prepare.json) ---------------------------------------------------------------------------------------

struct Receipt {
  std::string image_file;
  std::string image_linked_path;  // UTF-8; where the player keeps the image (decision 0075), relative to the install folder when
                                  // it lies inside it. Empty: a copy under `disc`
  std::uint64_t image_size = 0;
  std::uint64_t image_modified_unix_nanos = 0;
  std::uint64_t elf_size = 0;
};

// Reads `image.file`, `image.linked_path` (optional), `image.size_bytes`, `image.modified_unix_nanos` and `elf.size_bytes`. Empty when any other is missing
// (a receipt of schema 1 has no image and is reported as changed).
std::optional<Receipt> parse_receipt(std::string_view json);

// The launcher's cheap check at every Play (`check_installed`): the receipt, the ELF size, the disc copy's size and
// modified time. Fills `disc` and `elf` on success.
Check check_game_files(const Layout &layout, const Access &access, std::filesystem::path &disc, std::filesystem::path &elf);

// ---- Renderer -----------------------------------------------------------------------------------------------------------

struct Renderer {
  std::filesystem::path executable;
  std::string recorded_sha256;
};

// `<gs>\current.txt` names a version; `<gs>\<version>\gt4-gs.exe` must hash to `sha256.txt` (gs_release::installed).
Check find_renderer(const Layout &layout, const Access &access, Renderer &renderer);

// ---- Config file (game.ini) ---------------------------------------------------------------------------------------------

inline constexpr const char *kConfigFileName = "game.ini";
inline constexpr const char *kGsConfigFileName = "gt4-gs.ini";

struct Config {
  std::map<std::string, std::string> values;
  std::vector<std::string> warnings; // unknown keys and refused values, for the log
};

// `key=value` lines, `#` comments. Unknown keys and values outside the allowed set are dropped with a warning, never fatal.
Config parse_config(std::string_view text);

using Environment = std::vector<std::pair<std::string, std::string>>;

// The variables a config stands for (the ones the launcher sets at Play): fullscreen, present_mode, monitor, output_size,
// async_gs, performance_report, check_vu_code.
Environment config_environment(const Config &config);

// ---- Defaults and the final environment ---------------------------------------------------------------------------------

// GSdx, 480p, 16:9 hold, automatic ultrawide, fullscreen, vsync.
Environment default_environment();

struct Resolved {
  Environment set;                    // variables to put in the process environment (only those that were unset)
  std::vector<std::string> arguments; // argv[1..] for the donor main: ELF first
  bool needs_renderer = false;        // GT4_RENDERER resolves to pcsx2 and GT4_GS_PROCESS is not given
};

struct Inputs {
  const Layout *layout = nullptr;
  std::filesystem::path elf;
  std::filesystem::path disc;
  std::filesystem::path session_dir;
  const Renderer *renderer = nullptr; // when set and needed
  std::optional<Config> config;
  bool gs_ini_exists = false;
  bool controls_exists = false;
  std::vector<std::string> user_arguments; // argv[1..] as the user gave them (all `--` options)
};

// Builds the environment (precedence above) and the command line the launcher would pass:
//   <elf> --ntsc --disc <iso> --frames 0 --memcard <card> --misses <session>\misses.txt --window
// User options replace their default (--disc, --memcard, --frames, --misses, --pal, --fullscreen) and everything else is
// appended unchanged. `--no-window` (not a donor option) leaves out the window flag: a headless standalone run for tests.
Resolved resolve(const Inputs &inputs, const Access &access);

// Whether the renderer files are needed: GT4_RENDERER is unset or "pcsx2", GT4_GS_PROCESS is not given and game.ini does
// not choose CoreGS 2.0 for GT4 (`inputs.layout` and `inputs.config` are read).
bool renderer_needed(const Inputs &inputs, const Access &access);

// ---- Session folder, messages -------------------------------------------------------------------------------------------

inline constexpr std::size_t kKeepSessions = 20;

// `standalone-<unix seconds>`.
std::string session_name(std::uint64_t unix_seconds);

// Of the folder names in `sessions`, the `standalone-*` ones to remove so that `keep` remain (oldest first by name).
std::vector<std::string> sessions_to_remove(std::vector<std::string> names, std::size_t keep);

// The native message for a problem: what is missing, and what to do (open GT4Recomp once).
std::string message_for(const Check &check, std::string_view display_name);

} // namespace gt4::standalone
