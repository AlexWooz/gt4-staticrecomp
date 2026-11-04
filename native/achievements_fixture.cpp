#include "achievements.hpp"
#include "achievements_overlay.hpp"

#include "rc_api_request.h"
#include "rc_api_runtime.h"
#include "rc_client.h"
#include "rc_error.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#define CHECK(cond, msg)                                                           \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg)); \
            return 1;                                                              \
        }                                                                          \
    } while (0)

namespace {

struct MockRuntimeContext {
    std::vector<uint8_t> ram;
    std::vector<uint8_t> spr;
    uint32_t triggered_id = 0;
    int triggered_count = 0;
    bool award_request_sent = false;
    std::string last_award_post;
};

uint32_t RC_CCONV mock_read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client) {
    auto* ctx = static_cast<MockRuntimeContext*>(rc_client_get_userdata(client));
    return gt4::achievements::read_ps2_memory(
        address,
        buffer,
        num_bytes,
        ctx->ram.data(),
        ctx->ram.size(),
        ctx->spr.data(),
        ctx->spr.size());
}

void RC_CCONV mock_server_call(
    const rc_api_request_t* request,
    rc_client_server_callback_t callback,
    void* callback_data,
    rc_client_t* client) {
    auto* ctx = static_cast<MockRuntimeContext*>(rc_client_get_userdata(client));
    const std::string post = request->post_data ? request->post_data : "";
    std::string body = "{\"Success\":true}";

    if (post.find("r=login2") != std::string::npos) {
        body = "{\"Success\":true,\"User\":\"TestDriver\",\"Token\":\"tok_abc123\",\"Score\":1250,\"SoftcoreScore\":400,\"Messages\":0}";
    } else if (post.find("r=gameid") != std::string::npos) {
        body = "{\"Success\":true,\"GameID\":20580}";
    } else if (post.find("r=achievementsets") != std::string::npos) {
        body =
            "{\"Success\":true,"
            "\"GameId\":20580,"
            "\"Title\":\"Gran Turismo 4\","
            "\"ConsoleId\":21,"
            "\"ImageIconUrl\":\"https://media.retroachievements.org/Images/091234.png\","
            "\"Sets\":["
            "{"
            "\"AchievementSetId\":1,"
            "\"GameId\":20580,"
            "\"Title\":\"Gran Turismo 4\","
            "\"Type\":\"core\","
            "\"ImageIconUrl\":\"https://media.retroachievements.org/Images/091234.png\","
            "\"Achievements\":["
            "{"
            "\"ID\":55001,"
            "\"Title\":\"Sunday Cup Champion\","
            "\"Description\":\"Win the Sunday Cup.\","
            "\"Points\":10,"
            "\"MemAddr\":\"0xH001020=1_0xH02000010=7\","
            "\"Flags\":3,"
            "\"BadgeName\":\"112233\","
            "\"Author\":\"GT4Dev\","
            "\"Created\":1600000000,"
            "\"Modified\":1600000000"
            "}"
            "],"
            "\"Leaderboards\":[]"
            "}"
            "]"
            "}";
    } else if (post.find("r=patch") != std::string::npos) {
        body =
            "{\"Success\":true,\"PatchData\":{"
            "\"ID\":20580,"
            "\"Title\":\"Gran Turismo 4\","
            "\"ConsoleID\":21,"
            "\"ImageIcon\":\"/Images/091234.png\","
            "\"Achievements\":["
            "{"
            "\"ID\":55001,"
            "\"Title\":\"Sunday Cup Champion\","
            "\"Description\":\"Win the Sunday Cup.\","
            "\"Points\":10,"
            "\"MemAddr\":\"0xH001020=1_0xH02000010=7\","
            "\"Flags\":3,"
            "\"BadgeName\":\"112233\","
            "\"Author\":\"GT4Dev\","
            "\"Created\":1600000000,"
            "\"Modified\":1600000000"
            "}"
            "],"
            "\"Leaderboards\":[]"
            "}}";
    } else if (post.find("r=startsession") != std::string::npos) {
        body = "{\"Success\":true,\"HardcoreUnlocks\":[],\"Unlocks\":[],\"ServerNow\":1700000000}";
    } else if (post.find("r=awardachievement") != std::string::npos) {
        ctx->award_request_sent = true;
        ctx->last_award_post = post;
        body = "{\"Success\":true,\"AchievementID\":55001,\"Score\":1260,\"SoftcoreScore\":400,\"AchievementsRemaining\":0}";
    }

    rc_api_server_response_t resp{};
    resp.body = body.c_str();
    resp.body_length = body.size();
    resp.http_status_code = 200;
    callback(&resp, callback_data);
}

void RC_CCONV mock_event_handler(const rc_client_event_t* event, rc_client_t* client) {
    auto* ctx = static_cast<MockRuntimeContext*>(rc_client_get_userdata(client));
    if (event->type == RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED && event->achievement) {
        ctx->triggered_id = event->achievement->id;
        ctx->triggered_count += 1;
    }
}

int test_config_parsing() {
    std::unordered_map<std::string, std::string> env = {
        {"GT4_RA_ENABLED", "1"},
        {"GT4_RA_HARDCORE", "true"},
        {"GT4_RA_ENCORE", "0"},
        {"GT4_RA_USERNAME", "BSpecBob"},
        {"GT4_RA_TOKEN", "secret_token_99"},
        {"GT4_RA_CACHE_DIR", "C:\\Temp\\GT4RA"},
    };
    const auto cfg = gt4::achievements::parse_config([&](const char* key) -> std::string {
        auto it = env.find(key);
        return it != env.end() ? it->second : "";
    });
    CHECK(cfg.enabled, "GT4_RA_ENABLED=1 should enable achievements");
    CHECK(cfg.hardcore, "GT4_RA_HARDCORE=true should enable hardcore");
    CHECK(!cfg.encore, "GT4_RA_ENCORE=0 should disable encore");
    CHECK(cfg.username == "BSpecBob", "username should match");
    CHECK(cfg.token == "secret_token_99", "token should match");
    CHECK(cfg.game_hash == gt4::achievements::kGt4NtscUHash, "default hash should be GT4 NTSC-U hash");
    return 0;
}

int test_ps2_memory_mapping() {
    std::vector<uint8_t> ram(0x02000000, 0);
    std::vector<uint8_t> spr(0x4000, 0);

    ram[0x001234] = 0xAB;
    ram[0x001235] = 0xCD;
    spr[0x0040] = 0x5A;
    spr[0x0041] = 0x6B;

    uint8_t buf[4]{};

    // Standard EE RAM read.
    uint32_t n = gt4::achievements::read_ps2_memory(0x001234, buf, 2, ram.data(), ram.size(), spr.data(), spr.size());
    CHECK(n == 2 && buf[0] == 0xAB && buf[1] == 0xCD, "EE RAM read failed");

    // Uncached mirror 0x20000000 read.
    std::memset(buf, 0, sizeof(buf));
    n = gt4::achievements::read_ps2_memory(0x20001234, buf, 2, ram.data(), ram.size(), spr.data(), spr.size());
    CHECK(n == 2 && buf[0] == 0xAB && buf[1] == 0xCD, "Uncached mirror EE RAM read failed");

    // rcheevos Scratchpad flat address 0x02000040.
    std::memset(buf, 0, sizeof(buf));
    n = gt4::achievements::read_ps2_memory(0x02000040, buf, 2, ram.data(), ram.size(), spr.data(), spr.size());
    CHECK(n == 2 && buf[0] == 0x5A && buf[1] == 0x6B, "rcheevos scratchpad read failed");

    // Hardware PS2 Scratchpad address 0x70000040.
    std::memset(buf, 0, sizeof(buf));
    n = gt4::achievements::read_ps2_memory(0x70000040, buf, 2, ram.data(), ram.size(), spr.data(), spr.size());
    CHECK(n == 2 && buf[0] == 0x5A && buf[1] == 0x6B, "Hardware scratchpad read failed");

    return 0;
}

int test_rcheevos_session_and_unlock() {
    MockRuntimeContext ctx;
    ctx.ram.assign(0x00200000, 0);
    ctx.spr.assign(0x4000, 0);

    rc_client_t* client = rc_client_create(&mock_read_memory, &mock_server_call);
    CHECK(client != nullptr, "rc_client_create failed");
    rc_client_set_userdata(client, &ctx);
    rc_client_set_event_handler(client, &mock_event_handler);
    rc_client_set_hardcore_enabled(client, 1);

    int login_res = -1;
    rc_client_begin_login_with_token(
        client,
        "TestDriver",
        "tok_abc123",
        [](int result, const char* /*error*/, rc_client_t* /*c*/, void* userdata) {
            *static_cast<int*>(userdata) = result;
        },
        &login_res);
    CHECK(login_res == RC_OK, "rc_client_begin_login_with_token should succeed");

    int load_res = -1;
    rc_client_begin_load_game(
        client,
        gt4::achievements::kGt4NtscUHash,
        [](int result, const char* /*error*/, rc_client_t* /*c*/, void* userdata) {
            *static_cast<int*>(userdata) = result;
        },
        &load_res);
    CHECK(load_res == RC_OK, "rc_client_begin_load_game should succeed");

    // Initial frames: conditions not met (also clears RC_MINIMUM_UNPAUSED_FRAMES).
    for (int i = 0; i < 6; ++i) {
        rc_client_do_frame(client);
    }
    CHECK(ctx.triggered_count == 0, "achievement should not trigger before memory matches");

    // Set EE RAM 0x001020 = 1 and Scratchpad 0x02000010 (offset 0x10) = 7.
    ctx.ram[0x001020] = 1;
    ctx.spr[0x000010] = 7;
    rc_client_do_frame(client);

    CHECK(ctx.triggered_count == 1, "achievement should trigger when EE RAM and Scratchpad match");
    CHECK(ctx.triggered_id == 55001, "triggered achievement ID should be 55001");
    CHECK(ctx.award_request_sent, "r=awardachievement should be sent to the server");
    CHECK(ctx.last_award_post.find("a=55001") != std::string::npos, "award request should include achievement ID");
    CHECK(ctx.last_award_post.find("h=1") != std::string::npos, "award request should include hardcore=1");

    rc_client_destroy(client);
    return 0;
}

struct ScopedEnvVar {
    std::string key;
    std::string prev_val;
    bool had_prev = false;

    ScopedEnvVar(const char* k, const char* v) : key(k) {
        char buf[1024]{};
        DWORD len = GetEnvironmentVariableA(k, buf, sizeof(buf));
        if (len > 0 && len < sizeof(buf)) {
            prev_val = std::string(buf, len);
            had_prev = true;
        }
        if (v) {
            SetEnvironmentVariableA(k, v);
        } else {
            SetEnvironmentVariableA(k, nullptr);
        }
    }

    ~ScopedEnvVar() {
        if (had_prev) {
            SetEnvironmentVariableA(key.c_str(), prev_val.c_str());
        } else {
            SetEnvironmentVariableA(key.c_str(), nullptr);
        }
    }
};

int test_disabled_and_invalid_config() {
    // 1. Explicitly disabled via parse_config.
    {
        std::unordered_map<std::string, std::string> env = {
            {"GT4_RA_ENABLED", "0"},
            {"GT4_RA_USERNAME", "Player1"},
            {"GT4_RA_TOKEN", "token123"},
        };
        const auto cfg = gt4::achievements::parse_config([&](const char* key) -> std::string {
            auto it = env.find(key);
            return it != env.end() ? it->second : "";
        });
        CHECK(!cfg.enabled, "GT4_RA_ENABLED=0 must yield enabled=false");
    }

    // 2. Missing credentials via parse_config.
    {
        std::unordered_map<std::string, std::string> env = {
            {"GT4_RA_ENABLED", "1"},
        };
        const auto cfg = gt4::achievements::parse_config([&](const char* key) -> std::string {
            auto it = env.find(key);
            return it != env.end() ? it->second : "";
        });
        CHECK(cfg.enabled, "enabled flag parsed");
        CHECK(cfg.username.empty(), "username must be empty");
        CHECK(cfg.token.empty() && cfg.password.empty(), "credentials must be empty");
    }

    // 3. Runtime initialization with GT4_RA_ENABLED=0.
    {
        ScopedEnvVar env_enabled("GT4_RA_ENABLED", "0");
        ScopedEnvVar env_user("GT4_RA_USERNAME", "ValidUser");
        ScopedEnvVar env_tok("GT4_RA_TOKEN", "ValidToken");

        std::vector<uint8_t> ram(0x10000, 0);
        std::vector<uint8_t> spr(0x4000, 0);
        gt4::achievements::initialize(nullptr, ram.data(), ram.size(), spr.data(), spr.size());
        CHECK(!gt4::achievements::is_active(), "is_active() must be false when GT4_RA_ENABLED=0");
        gt4::achievements::shutdown();
    }

    // 4. Runtime initialization with missing credentials.
    {
        ScopedEnvVar env_enabled("GT4_RA_ENABLED", "1");
        ScopedEnvVar env_user("GT4_RA_USERNAME", nullptr);
        ScopedEnvVar env_tok("GT4_RA_TOKEN", nullptr);
        ScopedEnvVar env_pwd("GT4_RA_PASSWORD", nullptr);

        std::vector<uint8_t> ram(0x10000, 0);
        std::vector<uint8_t> spr(0x4000, 0);
        gt4::achievements::initialize(nullptr, ram.data(), ram.size(), spr.data(), spr.size());
        CHECK(!gt4::achievements::is_active(), "is_active() must be false when credentials are missing");
        gt4::achievements::shutdown();
    }

    return 0;
}

int test_clean_shutdown_and_reinit() {
    // Fake credentials pointing to discarded loopback port.
    ScopedEnvVar env_enabled("GT4_RA_ENABLED", "1");
    ScopedEnvVar env_user("GT4_RA_USERNAME", "MockPilot");
    ScopedEnvVar env_tok("GT4_RA_TOKEN", "mock_tok_555");
    ScopedEnvVar env_host("GT4_RA_HOST", "http://127.0.0.1:9");

    std::vector<uint8_t> ram(0x10000, 0);
    std::vector<uint8_t> spr(0x4000, 0);

    // Initial session start.
    gt4::achievements::initialize(nullptr, ram.data(), ram.size(), spr.data(), spr.size());
    CHECK(gt4::achievements::is_active(), "Runtime should activate with valid configuration");

    // Clean shutdown.
    gt4::achievements::shutdown();
    CHECK(!gt4::achievements::is_active(), "Runtime should be inactive after shutdown");

    // Reinitialization.
    gt4::achievements::initialize(nullptr, ram.data(), ram.size(), spr.data(), spr.size());
    CHECK(gt4::achievements::is_active(), "Runtime should successfully re-activate");

    // Final shutdown.
    gt4::achievements::shutdown();
    CHECK(!gt4::achievements::is_active(), "Runtime should be inactive after final shutdown");

    return 0;
}

int test_inactive_normal_behavior() {
    gt4::achievements::shutdown();
    CHECK(!gt4::achievements::is_active(), "must be inactive");

    // Calling hooks when inactive must be safe no-ops.
    std::vector<uint8_t> ram(0x1000, 0);
    std::vector<uint8_t> spr(0x1000, 0);
    gt4::achievements::on_vblank(nullptr, ram.data(), ram.size(), spr.data(), spr.size());
    gt4::achievements::on_window_pos_changed(nullptr);
    gt4::achievements::reset_session();
    gt4::achievements::shutdown();
    CHECK(!gt4::achievements::is_active(), "must remain inactive");
    return 0;
}

int test_overlay_toast_animation() {
    float alpha = 0.0f;
    float slide = 0.0f;

    gt4::achievements::AchievementsOverlay::compute_toast_anim(0.0, 4.5, alpha, slide);
    CHECK(alpha >= 0.0f && alpha < 0.05f && slide > 20.0f, "toast should start transparent and slid right");

    gt4::achievements::AchievementsOverlay::compute_toast_anim(1.5, 4.5, alpha, slide);
    CHECK(alpha == 1.0f && slide == 0.0f, "toast should be fully opaque during hold phase");

    gt4::achievements::AchievementsOverlay::compute_toast_anim(4.5, 4.5, alpha, slide);
    CHECK(alpha == 0.0f, "toast should be transparent at end of duration");
    return 0;
}

int test_language_catalog_and_fallback() {
    using gt4::achievements::Language;
    using gt4::achievements::resolve_language;
    using gt4::achievements::language_tag;

    // 1. Language parser resolution & fallback
    CHECK(resolve_language("pt-BR") == Language::PtBr, "pt-BR should resolve to PtBr");
    CHECK(std::string(language_tag(Language::PtBr)) == "pt-BR", "PtBr tag should be pt-BR");

    CHECK(resolve_language("en-US") == Language::EnUs, "en-US should resolve to EnUs");
    CHECK(std::string(language_tag(Language::EnUs)) == "en-US", "EnUs tag should be en-US");

    CHECK(resolve_language("") == Language::EnUs, "empty string should fall back to EnUs");
    CHECK(resolve_language("es-ES") == Language::EnUs, "unknown locale should fall back to EnUs");
    CHECK(resolve_language("pt") == Language::EnUs, "partial locale should fall back to EnUs");
    CHECK(resolve_language("invalid_locale_xyz") == Language::EnUs, "garbage should fall back to EnUs");

    // 2. parse_config integration
    {
        std::unordered_map<std::string, std::string> env{
            {"GT4_RA_ENABLED", "1"},
            {"GT4_RA_USERNAME", "Player"},
            {"GT4_RA_TOKEN", "tok"},
            {"GT4_RA_LANGUAGE", "pt-BR"},
        };
        auto cfg = gt4::achievements::parse_config([&](const char* key) -> std::string {
            auto it = env.find(key);
            return it != env.end() ? it->second : "";
        });
        CHECK(cfg.language == Language::PtBr, "GT4_RA_LANGUAGE=pt-BR should configure PtBr");
    }
    {
        std::unordered_map<std::string, std::string> env{
            {"GT4_RA_ENABLED", "1"},
            {"GT4_RA_USERNAME", "Player"},
            {"GT4_RA_TOKEN", "tok"},
            {"GT4_RA_LANGUAGE", "fr-FR"},
        };
        auto cfg = gt4::achievements::parse_config([&](const char* key) -> std::string {
            auto it = env.find(key);
            return it != env.end() ? it->second : "";
        });
        CHECK(cfg.language == Language::EnUs, "unknown GT4_RA_LANGUAGE should fall back to EnUs");
    }

    // 3. String catalog: en-US
    CHECK(std::string(gt4::achievements::toast_sign_in_failed_title(Language::EnUs)) == "Sign-in failed", "en-US sign in failed title");
    CHECK(std::string(gt4::achievements::toast_signed_in_title(Language::EnUs)) == "Signed in", "en-US signed in title");
    CHECK(std::string(gt4::achievements::toast_load_game_failed_title(Language::EnUs)) == "Could not load achievement set", "en-US load game failed title");
    CHECK(std::string(gt4::achievements::toast_achievement_unlocked_header(Language::EnUs, true)) == "HARDCORE ACHIEVEMENT UNLOCKED", "en-US ach unlocked hc");
    CHECK(std::string(gt4::achievements::toast_achievement_unlocked_header(Language::EnUs, false)) == "ACHIEVEMENT UNLOCKED", "en-US ach unlocked soft");
    CHECK(std::string(gt4::achievements::toast_game_mastered_header(Language::EnUs, true)) == "GAME MASTERED · HARDCORE", "en-US mastered hc");
    CHECK(gt4::achievements::toast_game_loaded_summary(Language::EnUs, 5, 20, 50, 200) == "Unlocked 5 of 20 (50/200 pts)", "en-US game loaded summary");
    CHECK(gt4::achievements::toast_game_mastered_summary(Language::EnUs, 20, 200) == "All 20 achievements unlocked (200 pts)!", "en-US game mastered summary");
    CHECK(std::string(gt4::achievements::toast_leaderboard_started_header(Language::EnUs)) == "LEADERBOARD STARTED", "en-US lb started");
    CHECK(std::string(gt4::achievements::toast_leaderboard_canceled_subtitle(Language::EnUs)) == "Attempt canceled.", "en-US lb canceled");
    CHECK(std::string(gt4::achievements::toast_disconnected_title(Language::EnUs)) == "Server connection interrupted", "en-US disconnected title");
    CHECK(std::string(gt4::achievements::toast_reconnected_title(Language::EnUs)) == "Reconnected to server", "en-US reconnected title");

    // 4. String catalog: pt-BR
    CHECK(std::string(gt4::achievements::toast_sign_in_failed_title(Language::PtBr)) == "Falha ao iniciar sessão", "pt-BR sign in failed title");
    CHECK(std::string(gt4::achievements::toast_signed_in_title(Language::PtBr)) == "Sessão iniciada", "pt-BR signed in title");
    CHECK(std::string(gt4::achievements::toast_load_game_failed_title(Language::PtBr)) == "Não foi possível carregar as conquistas", "pt-BR load game failed title");
    CHECK(std::string(gt4::achievements::toast_achievement_unlocked_header(Language::PtBr, true)) == "CONQUISTA HARDCORE DESBLOQUEADA", "pt-BR ach unlocked hc");
    CHECK(std::string(gt4::achievements::toast_achievement_unlocked_header(Language::PtBr, false)) == "CONQUISTA DESBLOQUEADA", "pt-BR ach unlocked soft");
    CHECK(std::string(gt4::achievements::toast_game_mastered_header(Language::PtBr, true)) == "JOGO DOMINADO · HARDCORE", "pt-BR mastered hc");
    CHECK(gt4::achievements::toast_game_loaded_summary(Language::PtBr, 5, 20, 50, 200) == "5 de 20 desbloqueadas (50/200 pts)", "pt-BR game loaded summary");
    CHECK(gt4::achievements::toast_game_mastered_summary(Language::PtBr, 20, 200) == "Todas as 20 conquistas desbloqueadas (200 pts)!", "pt-BR game mastered summary");
    CHECK(std::string(gt4::achievements::toast_leaderboard_started_header(Language::PtBr)) == "PLACAR INICIADO", "pt-BR lb started");
    CHECK(std::string(gt4::achievements::toast_leaderboard_canceled_subtitle(Language::PtBr)) == "Tentativa cancelada.", "pt-BR lb canceled");
    CHECK(std::string(gt4::achievements::toast_disconnected_title(Language::PtBr)) == "Conexão com o servidor interrompida", "pt-BR disconnected title");
    CHECK(std::string(gt4::achievements::toast_reconnected_title(Language::PtBr)) == "Reconectado ao servidor", "pt-BR reconnected title");

    return 0;
}

} // namespace

int main() {
    if (int rc = test_config_parsing(); rc != 0) return rc;
    if (int rc = test_disabled_and_invalid_config(); rc != 0) return rc;
    if (int rc = test_clean_shutdown_and_reinit(); rc != 0) return rc;
    if (int rc = test_inactive_normal_behavior(); rc != 0) return rc;
    if (int rc = test_ps2_memory_mapping(); rc != 0) return rc;
    if (int rc = test_rcheevos_session_and_unlock(); rc != 0) return rc;
    if (int rc = test_overlay_toast_animation(); rc != 0) return rc;
    if (int rc = test_language_catalog_and_fallback(); rc != 0) return rc;
    std::printf("achievements_fixture: all tests passed.\n");
    return 0;
}
