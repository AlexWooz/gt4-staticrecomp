// TASK-134: the VU operations rewritten for speed give the donor's exact bits.
//  - vfix (patched vu_ops.hpp) against the donor's expression: every 32-bit value.
//  - mac_of_result against mac_of_result_scalar: every destination mask over values at the edges of each flag, in all
//    lane positions, and random values.
#include "vu_recomp_contract.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

namespace {

using namespace ps2::vuops;

// The donor's vfix, as it stands in vu_ops.hpp at the pinned revision.
v4u donor_vfix(v4u x) {
    const v4i clamped = __builtin_elementwise_min((v4i)(x & 0x7FFFFFFFu), (v4i){0x7F7FFFFF, 0x7F7FFFFF, 0x7F7FFFFF, 0x7F7FFFFF});
    const v4i normal = (v4f)clamped >= (v4f)(v4i){0x00800000, 0x00800000, 0x00800000, 0x00800000};
    return (x & 0x80000000u) | (v4u)(clamped & normal);
}

bool check_vfix() {
    std::uint64_t differences = 0;
    for (std::uint64_t value = 0; value < (1ull << 32); value += 4) {
        const auto base = static_cast<std::uint32_t>(value);
        const v4u x{base, base + 1u, base + 2u, base + 3u};
        const v4u ours = vfix(x), donor = donor_vfix(x);
        const v4u different = ours ^ donor;
        differences += (different[0] | different[1] | different[2] | different[3]) != 0;
    }
    if (differences != 0) std::fprintf(stderr, "vfix differs from the donor in %llu groups\n", static_cast<unsigned long long>(differences));
    return differences == 0;
}

bool same_mac(const ps2::Reg128 &value) {
    for (unsigned dest = 0; dest < 16; ++dest) {
        if (gt4::vugen::mac_of_result(value, dest) != gt4::vugen::mac_of_result_scalar(value, dest)) {
            std::fprintf(stderr, "mac differs: %08x %08x %08x %08x dest %u\n", value.w[0], value.w[1], value.w[2], value.w[3], dest);
            return false;
        }
    }
    return true;
}

bool check_mac() {
    const std::uint32_t edges[] = {0u, 1u, 0x007FFFFFu, 0x00800000u, 0x3F800000u, 0x7F7FFFFFu, 0x7F800000u, 0x7FFFFFFEu, 0x7FFFFFFFu,
                                   0x80000000u, 0x80000001u, 0x807FFFFFu, 0x80800000u, 0xBF800000u, 0xFF7FFFFFu, 0xFFFFFFFFu};
    for (const std::uint32_t a : edges)
        for (const std::uint32_t b : edges)
            for (const std::uint32_t c : edges)
                for (const std::uint32_t d : edges) {
                    ps2::Reg128 value{};
                    value.w[0] = a; value.w[1] = b; value.w[2] = c; value.w[3] = d;
                    if (!same_mac(value)) return false;
                }
    std::mt19937 random(134);
    for (int round = 0; round < 2000000; ++round) {
        ps2::Reg128 value{};
        for (int lane = 0; lane < 4; ++lane) value.w[lane] = random();
        if (!same_mac(value)) return false;
    }
    return true;
}

} // namespace

int main() {
    if (!check_mac() || !check_vfix()) return 1;
    std::puts("native_vu_fast_ops ok: vfix equals the donor's over 2^32 values; mac_of_result equals the scalar form");
    return 0;
}
