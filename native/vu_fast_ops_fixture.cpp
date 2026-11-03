// TASK-134: the VU operations rewritten for speed give the donor's exact bits.
//  - vfix (patched vu_ops.hpp) against the donor's expression: every 32-bit value.
//  - mac_of_result against mac_of_result_scalar: every destination mask over values at the edges of each flag, in all
//    lane positions, and random values.
//  - clip_result against clip_result_scalar: every 32-bit value in each compared lane, against limits at the edges, and random.
//  - fp::add, fp::sub and fp::mul (scripts/prepare_fast_headers.py) against copies of the donor's: every pair of exponents with
//    mantissas at the edges, near-equal operands (the long normalisations), and random pairs.
//  - fp4 (four lanes at once) against the same copies, on the same inputs; the VU0 macro operations against their definition.
#include "vu_recomp_contract.hpp"
#include "ps2/vu.hpp"

#include <cstdint>
#include <cstdio>
#include <bit>
#include <cstring>
#include <iterator>
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

// Four pairs at a time through fp4, each lane against the donor's scalar operation.
struct Fp4Batch {
    std::uint32_t a[4] = {}, b[4] = {};
    int count = 0;

    bool check() {
        ps2::Reg128 ra{}, rb{}, sum{}, difference{}, product{};
        for (int lane = 0; lane < 4; ++lane) {
            ra.w[lane] = a[lane];
            rb.w[lane] = b[lane];
        }
        const ps2::fp4::V va = ps2::fp4::load(ra), vb = ps2::fp4::load(rb);
        ps2::fp4::store_lanes(sum, 15u, ps2::fp4::add(va, vb));
        ps2::fp4::store_lanes(difference, 15u, ps2::fp4::sub(va, vb));
        ps2::fp4::store_lanes(product, 15u, ps2::fp4::mul(va, vb));
        for (int lane = 0; lane < 4; ++lane) {
            if (sum.w[lane] != donor_add(a[lane], b[lane]) || difference.w[lane] != donor_add(a[lane], b[lane] ^ ps2::fp::kSign) ||
                product.w[lane] != donor_mul(a[lane], b[lane])) {
                std::fprintf(stderr, "fp4 differs: lane %d %08x %08x: add %08x %08x, sub %08x %08x, mul %08x %08x\n", lane, a[lane], b[lane],
                             sum.w[lane], donor_add(a[lane], b[lane]), difference.w[lane], donor_add(a[lane], b[lane] ^ ps2::fp::kSign),
                             product.w[lane], donor_mul(a[lane], b[lane]));
                return false;
            }
        }
        return true;
    }

    bool add(std::uint32_t x, std::uint32_t y) {
        a[count] = x;
        b[count] = y;
        if (++count < 4) return true;
        count = 0;
        return check();
    }
};

bool same_fp(std::uint32_t a, std::uint32_t b) {
    const bool same = ps2::fp::add(a, b) == donor_add(a, b) && ps2::fp::sub(a, b) == donor_add(a, b ^ ps2::fp::kSign) &&
                      ps2::fp::mul(a, b) == donor_mul(a, b) && ps2::fp::flush(a) == donor_flush(a);
    if (!same) std::fprintf(stderr, "fp differs: %08x %08x\n", a, b);
    return same;
}

bool check_fp() {
    Fp4Batch batch;
    const std::uint32_t mantissas[] = {0u, 1u, 2u, 0x3FFFFFu, 0x400000u, 0x400001u, 0x555555u, 0x7FFFFEu, 0x7FFFFFu};
    for (std::uint32_t ea = 0; ea < 256; ++ea)
        for (std::uint32_t eb = 0; eb < 256; ++eb)
            for (const std::uint32_t ma : mantissas)
                for (const std::uint32_t mb : mantissas)
                    for (const std::uint32_t signs : {0u, 1u, 2u, 3u}) {
                        const std::uint32_t a = ((signs & 1u) << 31) | (ea << 23) | ma, b = ((signs & 2u) << 30) | (eb << 23) | mb;
                        if (!same_fp(a, b) || !batch.add(a, b)) return false;
                    }
    std::mt19937 random(134);
    for (std::uint64_t round = 0; round < 400000000ull; ++round) {
        const std::uint32_t a = random();
        // A third fully random, a third with close exponents, a third nearly equal: the subtractions that normalise far.
        std::uint32_t b = random();
        if (round % 3 == 1) b = (b & 0x807FFFFFu) | (((((a >> 23) & 0xFFu) + (b >> 28)) & 0xFFu) << 23);
        if (round % 3 == 2) b = (a ^ (b & 0x80000000u)) + ((b >> 8) & 0xFFu) - 128u;
        if (!same_fp(a, b) || !batch.add(a, b)) return false;
    }
    return true;
}

// The macro operations against their definition: each selected lane by the scalar operation, the others untouched, vf0 never written.
bool check_macro() {
    using namespace ps2;
    std::mt19937 random(1340);
    const auto word = [&random]() -> std::uint32_t {
        const std::uint32_t kind = random() % 8;
        const std::uint32_t bits = random();
        if (kind == 0) return bits & 0x807FFFFFu;           // zero or a denormal
        if (kind == 1) return bits | 0x7F800000u;           // the largest exponent
        return (bits & 0x807FFFFFu) | ((110u + random() % 40u) << 23);  // ordinary numbers that interact
    };
    enum Op { Add, Sub, Mul, Madd, Msub };
    const auto scalar = [](Op op, std::uint32_t acc, std::uint32_t s, std::uint32_t t) {
        switch (op) {
            case Add: return donor_add(s, t);
            case Sub: return donor_add(s, t ^ fp::kSign);
            case Mul: return donor_mul(s, t);
            case Madd: return donor_add(acc, donor_mul(s, t));
            default: return donor_add(acc, donor_mul(s, t) ^ fp::kSign);
        }
    };
    for (int round = 0; round < 2000000; ++round) {
        Vu0Regs v{};
        for (int reg = 0; reg < 4; ++reg)
            for (int lane = 0; lane < 4; ++lane) v.vf[reg].w[lane] = word();
        for (int lane = 0; lane < 4; ++lane) v.acc.w[lane] = word();
        const unsigned dest = random() % 16u, fd = random() % 4u, fs = 1u + random() % 3u, ft = 1u + random() % 3u;
        const std::uint32_t bc = word();
        const int op = static_cast<int>(random() % 15u);
        const Vu0Regs before = v;
        Reg128 expected = op >= 10 ? before.acc : before.vf[fd];
        const Op kinds[] = {Add, Sub, Mul, Madd, Msub};
        const Op kind = kinds[op % 5];
        for (int lane = 0; lane < 4; ++lane)
            if (dest & (8u >> lane)) expected.w[lane] = scalar(kind, before.acc.w[lane], before.vf[fs].w[lane], op >= 5 && op < 10 ? bc : before.vf[ft].w[lane]);
        switch (op) {
            case 0: vu::add(v, dest, fd, fs, ft); break;
            case 1: vu::sub(v, dest, fd, fs, ft); break;
            case 2: vu::mul(v, dest, fd, fs, ft); break;
            case 3: vu::madd(v, dest, fd, fs, ft); break;
            case 4: vu::msub(v, dest, fd, fs, ft); break;
            case 5: vu::addbc(v, dest, fd, fs, bc); break;
            case 6: vu::subbc(v, dest, fd, fs, bc); break;
            case 7: vu::mulbc(v, dest, fd, fs, bc); break;
            case 8: vu::maddbc(v, dest, fd, fs, bc); break;
            case 9: vu::msubbc(v, dest, fd, fs, bc); break;
            case 10: vu::adda(v, dest, fs, ft); break;
            case 11: vu::suba(v, dest, fs, ft); break;
            case 12: vu::mula(v, dest, fs, ft); break;
            case 13: vu::madda(v, dest, fs, ft); break;
            default: vu::msuba(v, dest, fs, ft); break;
        }
        const Reg128 &got = op >= 10 ? v.acc : v.vf[fd];
        const bool written_zero = op < 10 && fd == 0;
        const Reg128 &wanted = written_zero ? before.vf[0] : expected;
        if (std::memcmp(&got, &wanted, sizeof got) != 0) {
            std::fprintf(stderr, "macro operation %d differs (dest %u fd %u fs %u ft %u)\n", op, dest, fd, fs, ft);
            return false;
        }
        // The accumulator forms with a broadcast share their lines with the forms above.
        Vu0Regs w = before, x = before;
        Reg128 from_bc{};
        for (int lane = 0; lane < 4; ++lane) from_bc.w[lane] = bc;
        x.vf[ft] = from_bc;
        if (fs != ft) {
            switch (op % 5) {
                case 0: vu::addabc(w, dest, fs, bc); vu::adda(x, dest, fs, ft); break;
                case 1: vu::subabc(w, dest, fs, bc); vu::suba(x, dest, fs, ft); break;
                case 2: vu::mulabc(w, dest, fs, bc); vu::mula(x, dest, fs, ft); break;
                case 3: vu::maddabc(w, dest, fs, bc); vu::madda(x, dest, fs, ft); break;
                default: vu::msubabc(w, dest, fs, bc); vu::msuba(x, dest, fs, ft); break;
            }
            if (std::memcmp(&w.acc, &x.acc, sizeof w.acc) != 0) {
                std::fprintf(stderr, "broadcast accumulator operation %d differs\n", op % 5);
                return false;
            }
        }
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

bool same_clip(std::uint32_t previous, const ps2::Reg128 &value, std::uint32_t w) {
    if (gt4::vugen::clip_result(previous, value, w) == gt4::vugen::clip_result_scalar(previous, value, w)) return true;
    std::fprintf(stderr, "clip differs: %08x %08x %08x w %08x\n", value.w[0], value.w[1], value.w[2], w);
    return false;
}

// Every 32-bit value in each of x, y and z against a limit that changes with the value (all of the limit edges in turn), then
// random values against limits at the edges and random limits.
bool check_clip() {
    const std::uint32_t limits[] = {0u, 0x00000001u, 0x007FFFFFu, 0x00800000u, 0x3F800000u, 0x7F7FFFFFu, 0x7F800000u, 0x7FFFFFFFu,
                                    0x80000000u, 0x807FFFFFu, 0xBF800000u, 0xFFFFFFFFu};
    std::uint32_t previous = 0x123456u;
    for (std::uint64_t base = 0; base < (1ull << 32); ++base) {
        ps2::Reg128 value{};
        const auto x = static_cast<std::uint32_t>(base);
        value.w[0] = x;
        value.w[1] = x ^ 0x80000000u;
        value.w[2] = ~x;
        value.w[3] = x * 2654435761u;
        const std::uint32_t w = limits[base % std::size(limits)] ^ (static_cast<std::uint32_t>(base >> 8) & 0x00000F0Fu);
        if (!same_clip(previous, value, w)) return false;
        previous = previous * 1664525u + 1013904223u;
    }
    std::mt19937 random(1341);
    for (int round = 0; round < 20000000; ++round) {
        ps2::Reg128 value{};
        for (int lane = 0; lane < 4; ++lane) value.w[lane] = random();
        const std::uint32_t w = round % 2 ? random() : limits[random() % std::size(limits)];
        if (!same_clip(random(), value, w)) return false;
    }
    return true;
}

} // namespace

int main() {
    if (!check_mac() || !check_clip() || !check_vfix() || !check_fp() || !check_macro()) return 1;
    std::puts("native_vu_fast_ops ok: vfix equals the donor's over 2^32 values; mac_of_result and clip_result equal their scalar forms; fp::add, sub and mul and fp4 equal the donor's; the macro operations equal their definition");
    return 0;
}
