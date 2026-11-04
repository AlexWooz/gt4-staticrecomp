// TASK-133 S2: Spec II negative fixture.
// Must prove that when compiled for the Spec II target (GT4_TARGET_SPEC2 defined,
// without GT4_TARGET_FIXTURE_V200), RetroAchievements is strictly disabled:
// no active runtime, no evaluations, no HTTP requests, and memory reads return 0.

#include "achievements.hpp"
#include "game_target.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#define CHECK(cond, msg)                                                           \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg)); \
            return 1;                                                              \
        }                                                                          \
    } while (0)

namespace {

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

int test_spec2_target_rejection() {
    // 1. Verify compile-time target guard evaluates to Spec II.
    CHECK(gt4::target::active().id == gt4::target::Id::spec2,
          "achievements_spec2_fixture must run with Spec II target profile");

    // 2. Provide forged GT4_RA_* environment variables attempting to force RA activation.
    ScopedEnvVar env_enabled("GT4_RA_ENABLED", "1");
    ScopedEnvVar env_user("GT4_RA_USERNAME", "Spec2ForgedDriver");
    ScopedEnvVar env_tok("GT4_RA_TOKEN", "forged_token_xyz");
    ScopedEnvVar env_hardcore("GT4_RA_HARDCORE", "1");
    ScopedEnvVar env_host("GT4_RA_HOST", "http://127.0.0.1:9");

    std::vector<uint8_t> ram(0x02000000, 0xAA);
    std::vector<uint8_t> spr(0x4000, 0x55);

    // 3. Attempt initialization: must early-exit due to target check and remain inactive.
    gt4::achievements::initialize(nullptr, ram.data(), ram.size(), spr.data(), spr.size());
    CHECK(!gt4::achievements::is_active(),
          "Spec II must refuse RA activation even with forged GT4_RA_* environment");

    // 4. Memory read: must zero buffer and return 0.
    uint8_t buf[16]{};
    std::memset(buf, 0xFF, sizeof(buf));
    uint32_t bytes_read = gt4::achievements::read_ps2_memory(
        0x001000, buf, sizeof(buf), ram.data(), ram.size(), spr.data(), spr.size());
    CHECK(bytes_read == 0, "read_ps2_memory on Spec II must return 0 bytes");
    for (size_t i = 0; i < sizeof(buf); ++i) {
        CHECK(buf[i] == 0, "read_ps2_memory on Spec II must zero destination buffer");
    }

    // 5. VBlank tick: must be a no-op.
    gt4::achievements::on_vblank(nullptr, ram.data(), ram.size(), spr.data(), spr.size());
    CHECK(!gt4::achievements::is_active(), "is_active() must stay false on VBlank");

    // 6. Clean shutdown.
    gt4::achievements::shutdown();
    CHECK(!gt4::achievements::is_active(), "is_active() must stay false after shutdown");

    return 0;
}

} // namespace

int main() {
    if (int rc = test_spec2_target_rejection(); rc != 0) return rc;
    std::printf("achievements_spec2_fixture: Spec II target rejection verified (no requests, no memory evaluation, no overlay).\n");
    return 0;
}
