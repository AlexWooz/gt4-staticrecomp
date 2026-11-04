#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace gt4::achievements {

// Official RetroAchievements hash for Gran Turismo 4 (USA, SCUS-97328 v2.00).
inline constexpr const char* kGt4NtscUHash = "b7e2e47572f4163a216ea85eed3c6c63";

enum class Language {
    EnUs,
    PtBr,
};

// Resolves a language string (e.g. from GT4_RA_LANGUAGE) into Language, falling back to EnUs.
Language resolve_language(std::string_view lang_str);
const char* language_tag(Language lang);

// Localized application-owned toast strings (internal catalog).
const char* toast_sign_in_failed_title(Language lang);
const char* toast_sign_in_failed_fallback_sub(Language lang);
const char* toast_signed_in_title(Language lang);
const char* toast_load_game_failed_title(Language lang);
const char* toast_load_game_failed_fallback_sub(Language lang);
std::string toast_game_loaded_summary(Language lang, uint32_t unlocked, uint32_t total, uint32_t pts_unlocked, uint32_t pts_total);
const char* toast_achievement_unlocked_header(Language lang, bool hardcore);
const char* toast_achievement_fallback_title(Language lang);
const char* toast_game_mastered_header(Language lang, bool hardcore);
std::string toast_game_mastered_summary(Language lang, uint32_t total, uint32_t pts);
const char* toast_progress_indicator_fallback_title(Language lang);
const char* toast_leaderboard_started_header(Language lang);
const char* toast_leaderboard_submitted_header(Language lang);
const char* toast_leaderboard_canceled_header(Language lang);
const char* toast_leaderboard_canceled_subtitle(Language lang);
const char* toast_leaderboard_fallback_title(Language lang);
const char* toast_disconnected_title(Language lang);
const char* toast_disconnected_subtitle(Language lang);
const char* toast_reconnected_title(Language lang);
const char* toast_reconnected_subtitle(Language lang);

struct Config {
    bool enabled = false;
    bool hardcore = false;
    bool encore = false;
    std::string username;
    std::string token;
    std::string password;
    std::string game_hash = kGt4NtscUHash;
    std::string host;
    std::wstring cache_dir;
    std::string disc_path;
    Language language = Language::EnUs;
};

// Parses configuration using a custom environment getter (returns empty string if unset).
Config parse_config(const std::function<std::string(const char*)>& get_env);

// Reads configuration from the current process environment (GT4_RA_*).
Config config_from_environment();

// Maps rcheevos PS2 addresses into the 32 MiB EE RAM buffer and 16 KiB Scratchpad buffer.
uint32_t read_ps2_memory(
    uint32_t address,
    uint8_t* buffer,
    uint32_t num_bytes,
    const uint8_t* ram,
    size_t ram_size,
    const uint8_t* spr,
    size_t spr_size);

// Lifecycle hooks called from native/window.cpp.
void initialize(HWND game_window, const uint8_t* ram, size_t ram_size, const uint8_t* spr, size_t spr_size);
void on_vblank(HWND game_window, const uint8_t* ram, size_t ram_size, const uint8_t* spr, size_t spr_size);
void on_window_pos_changed(HWND game_window);
void reset_session();
void shutdown();
bool is_active();

} // namespace gt4::achievements
