// TASK-134: the VU operations rewritten for speed give the donor's exact bits.
//  - vfix (patched vu_ops.hpp) against the donor's expression: every 32-bit value.
//  - mac_of_result against mac_of_result_scalar: every destination mask over values at the edges of each flag, in all
//    lane positions, and random values.
//  - fp::add, fp::sub and fp::mul (scripts/prepare_fast_headers.py) against copies of the donor's: every pair of exponents with
//    mantissas at the edges, near-equal operands (the long normalisations), and random pairs.
#include "vu_recomp_contract.hpp"

#include <cstdint>
#include <cstdio>
#include <bit>
#include <cstring>
#include <random>
#include <utility>

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

// The donor's operations, as they stand in fpu.hpp at the pinned revision.
std::uint32_t donor_flush(std::uint32_t x) { return ps2::fp::exponent(x) ? x : (x & ps2::fp::kSign); }

std::uint32_t donor_add(std::uint32_t a, std::uint32_t b) {
    using namespace ps2::fp;
    a = donor_flush(a);
    b = donor_flush(b);
    std::uint32_t aa = a & kAbs, ba = b & kAbs;
    if (!ba) return aa ? a : (a & b);
    if (!aa) return b;
    if (ba > aa) {
        std::swap(a, b);
        std::swap(aa, ba);
    }
    int e = static_cast<int>(aa >> 23);
    const int diff = e - static_cast<int>(ba >> 23);
    std::uint32_t ma = mantissa(aa) << 1;
    const std::uint32_t mb = diff > 25 ? 0u : (mantissa(ba) << 1) >> diff;
    if ((a ^ b) & kSign) {
        ma -= mb;
        if (!ma) return 0;
        const int shift = std::countl_zero(ma) - 7;
        ma <<= shift;
        e -= shift;
    } else {
        ma += mb;
        if (ma & 0x2000000u) {
            ma >>= 1;
            ++e;
        }
    }
    return pack(a & kSign, e, ma >> 1);
}

std::uint32_t donor_mul(std::uint32_t a, std::uint32_t b) {
    using namespace ps2::fp;
    const std::uint32_t sign = (a ^ b) & kSign;
    const int ea = static_cast<int>(exponent(a)), eb = static_cast<int>(exponent(b));
    if (!ea || !eb) return sign;
    std::uint64_t m = static_cast<std::uint64_t>(mantissa(a)) * mantissa(b);
    int e = ea + eb - 127;
    if (m >> 47) {
        m >>= 24;
        ++e;
    } else {
        m >>= 23;
    }
    return pack(sign, e, static_cast<std::uint32_t>(m));
}

bool same_fp(std::uint32_t a, std::uint32_t b) {
    const bool same = ps2::fp::add(a, b) == donor_add(a, b) && ps2::fp::sub(a, b) == donor_add(a, b ^ ps2::fp::kSign) &&
                      ps2::fp::mul(a, b) == donor_mul(a, b) && ps2::fp::flush(a) == donor_flush(a);
    if (!same) std::fprintf(stderr, "fp differs: %08x %08x\n", a, b);
    return same;
}

bool check_fp() {
    const std::uint32_t mantissas[] = {0u, 1u, 2u, 0x3FFFFFu, 0x400000u, 0x400001u, 0x555555u, 0x7FFFFEu, 0x7FFFFFu};
    for (std::uint32_t ea = 0; ea < 256; ++ea)
        for (std::uint32_t eb = 0; eb < 256; ++eb)
            for (const std::uint32_t ma : mantissas)
                for (const std::uint32_t mb : mantissas)
                    for (const std::uint32_t signs : {0u, 1u, 2u, 3u}) {
                        const std::uint32_t a = ((signs & 1u) << 31) | (ea << 23) | ma, b = ((signs & 2u) << 30) | (eb << 23) | mb;
                        if (!same_fp(a, b)) return false;
                    }
    std::mt19937 random(134);
    for (std::uint64_t round = 0; round < 400000000ull; ++round) {
        const std::uint32_t a = random();
        // A third fully random, a third with close exponents, a third nearly equal: the subtractions that normalise far.
        std::uint32_t b = random();
        if (round % 3 == 1) b = (b & 0x807FFFFFu) | (((((a >> 23) & 0xFFu) + (b >> 28)) & 0xFFu) << 23);
        if (round % 3 == 2) b = (a ^ (b & 0x80000000u)) + ((b >> 8) & 0xFFu) - 128u;
        if (!same_fp(a, b)) return false;
    }
    return true;
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
    if (!check_mac() || !check_vfix() || !check_fp()) return 1;
    std::puts("native_vu_fast_ops ok: vfix equals the donor's over 2^32 values; mac_of_result equals the scalar form; fp::add, sub and mul equal the donor's");
    return 0;
}
