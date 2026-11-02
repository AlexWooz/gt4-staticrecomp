// TASK-134: ps2::fp::add, sub and mul over the four lanes of a VU0 macro operation at once, with SSE4.1. The same bits as the scalar
// operations for every input (native/vu_fast_ops_fixture.cpp compares them): this is physics code, and only the form changes. No
// floating point arithmetic happens here: the two float conversions below only read an exponent from an exact power of two or from
// a number that cannot round up. A race spends about a fifth of the EE thread in these operations, mostly matrix products.
#pragma once

#include "cpu.hpp"
#include "fpu.hpp"

#include <cstdint>

#if defined(__SSE4_1__)
#include <smmintrin.h>

namespace ps2::fp4 {

using V = __m128i;

inline V load(const Reg128 &r) { return _mm_loadu_si128(reinterpret_cast<const __m128i *>(&r)); }
inline V splat(std::uint32_t x) { return _mm_set1_epi32(static_cast<int>(x)); }
inline V select(V mask, V yes, V no) { return _mm_blendv_epi8(no, yes, mask); }

// Lane i is written when dest has bit 3 - i, as ps2::vu::lane.
inline void store_lanes(Reg128 &r, unsigned dest, V value) {
    const V wanted = _mm_cmpgt_epi32(_mm_and_si128(splat(dest), _mm_setr_epi32(8, 4, 2, 1)), _mm_setzero_si128());
    _mm_storeu_si128(reinterpret_cast<__m128i *>(&r), select(wanted, value, load(r)));
}

inline V flush(V x) {
    const V zero_exponent = _mm_cmpeq_epi32(_mm_and_si128(x, splat(0x7F800000u)), _mm_setzero_si128());
    return select(zero_exponent, _mm_and_si128(x, splat(fp::kSign)), x);
}

inline V mantissa(V x) { return _mm_or_si128(_mm_and_si128(x, splat(0x7FFFFFu)), splat(0x800000u)); }

inline V pack(V sign, V e, V m) {
    const V normal = _mm_or_si128(_mm_slli_epi32(e, 23), _mm_and_si128(m, splat(0x7FFFFFu)));
    const V over = _mm_cmpgt_epi32(e, _mm_set1_epi32(255));
    const V under = _mm_cmpgt_epi32(_mm_set1_epi32(1), e);
    const V body = select(over, splat(fp::kAbs), _mm_andnot_si128(under, normal));
    return _mm_or_si128(sign, body);
}

inline V mul(V a, V b) {
    const V sign = _mm_and_si128(_mm_xor_si128(a, b), splat(fp::kSign));
    const V ea = _mm_and_si128(_mm_srli_epi32(a, 23), splat(0xFFu)), eb = _mm_and_si128(_mm_srli_epi32(b, 23), splat(0xFFu));
    const V no_value = _mm_or_si128(_mm_cmpeq_epi32(ea, _mm_setzero_si128()), _mm_cmpeq_epi32(eb, _mm_setzero_si128()));
    const V ma = mantissa(a), mb = mantissa(b);
    // 48-bit products of lanes 0 and 2, then 1 and 3. A product with bit 47 set carries: one more shift, one more exponent.
    const V product_even = _mm_mul_epu32(ma, mb);
    const V product_odd = _mm_mul_epu32(_mm_srli_epi64(ma, 32), _mm_srli_epi64(mb, 32));
    const V carry_even = _mm_srli_epi64(product_even, 47), carry_odd = _mm_srli_epi64(product_odd, 47);
    const V even = select(_mm_sub_epi64(_mm_setzero_si128(), carry_even), _mm_srli_epi64(product_even, 24), _mm_srli_epi64(product_even, 23));
    const V odd = select(_mm_sub_epi64(_mm_setzero_si128(), carry_odd), _mm_srli_epi64(product_odd, 24), _mm_srli_epi64(product_odd, 23));
    const V m = _mm_blend_epi16(even, _mm_slli_epi64(odd, 32), 0xCC);
    const V carry = _mm_blend_epi16(carry_even, _mm_slli_epi64(carry_odd, 32), 0xCC);
    const V e = _mm_add_epi32(_mm_add_epi32(ea, eb), _mm_sub_epi32(carry, _mm_set1_epi32(127)));
    return select(no_value, sign, pack(sign, e, m));
}

// 2^n for n in 0..30, as an integer, from the float with that exponent (exact).
inline V power_of_two(V n) { return _mm_cvttps_epi32(_mm_castsi128_ps(_mm_slli_epi32(_mm_add_epi32(n, _mm_set1_epi32(127)), 23))); }

inline V add(V a, V b) {
    a = flush(a);
    b = flush(b);
    const V aa = _mm_and_si128(a, splat(fp::kAbs)), ba = _mm_and_si128(b, splat(fp::kAbs));
    const V a_zero = _mm_cmpeq_epi32(aa, _mm_setzero_si128()), b_zero = _mm_cmpeq_epi32(ba, _mm_setzero_si128());
    const V zero_case = select(b_zero, select(a_zero, _mm_and_si128(a, b), a), b);

    const V swapped = _mm_cmpgt_epi32(ba, aa);
    const V larger = select(swapped, b, a), smaller = select(swapped, a, b);
    const V larger_abs = select(swapped, ba, aa), smaller_abs = select(swapped, aa, ba);
    const V larger_exponent = _mm_srli_epi32(larger_abs, 23);
    const V diff = _mm_min_epi32(_mm_sub_epi32(larger_exponent, _mm_srli_epi32(smaller_abs, 23)), _mm_set1_epi32(31));
    const V ma = _mm_slli_epi32(mantissa(larger_abs), 1);

    // The smaller mantissa (25 bits) >> diff, a shift per lane: (x << 2) * 2^(30 - diff), then the high 32 bits. From diff 25 on it
    // is 0, as the scalar form says for diff > 25; diff 31 gives a factor of 0 (the conversion of 0.5).
    const V shifted = _mm_slli_epi32(mantissa(smaller_abs), 3);
    const V factor = power_of_two(_mm_sub_epi32(_mm_set1_epi32(30), diff));
    const V high_even = _mm_srli_epi64(_mm_mul_epu32(shifted, factor), 32);
    const V high_odd = _mm_mul_epu32(_mm_srli_epi64(shifted, 32), _mm_srli_epi64(factor, 32));
    const V mb = _mm_blend_epi16(high_even, high_odd, 0xCC);

    const V negate = _mm_srai_epi32(_mm_xor_si128(larger, smaller), 31);
    const V sum = _mm_add_epi32(ma, _mm_sub_epi32(_mm_xor_si128(mb, negate), negate));

    // The index of the leading bit of sum. Clearing every bit below a set bit keeps the leading one and means the conversion to
    // float (24 bits) cannot round up to the next power of two.
    const V lead_only = _mm_andnot_si128(_mm_srli_epi32(sum, 1), sum);
    const V lead = _mm_sub_epi32(_mm_srli_epi32(_mm_castps_si128(_mm_cvtepi32_ps(lead_only)), 23), _mm_set1_epi32(127));
    // Bit 24 is the leading place: the mantissa is sum << (24 - lead), or sum >> 1 for a carry into bit 25. The scalar form's
    // (sum << (shift + 1)) >> 2 with shift = 24 - lead. A zero sum is replaced below, whatever these give.
    const V normalised = _mm_srli_epi32(_mm_mullo_epi32(sum, power_of_two(_mm_sub_epi32(_mm_set1_epi32(25), lead))), 2);
    const V e = _mm_add_epi32(larger_exponent, _mm_sub_epi32(lead, _mm_set1_epi32(24)));
    const V packed = pack(_mm_and_si128(larger, splat(fp::kSign)), e, normalised);
    const V value = _mm_andnot_si128(_mm_cmpeq_epi32(sum, _mm_setzero_si128()), packed);
    return select(_mm_or_si128(a_zero, b_zero), zero_case, value);
}

inline V sub(V a, V b) { return add(a, _mm_xor_si128(b, splat(fp::kSign))); }

}  // namespace ps2::fp4

#else  // no SSE4.1: the scalar operations, lane by lane

namespace ps2::fp4 {

struct V {
    std::uint32_t w[4];
};

inline V load(const Reg128 &r) { return V{{r.w[0], r.w[1], r.w[2], r.w[3]}}; }
inline V splat(std::uint32_t x) { return V{{x, x, x, x}}; }
inline void store_lanes(Reg128 &r, unsigned dest, V value) {
    for (int i = 0; i < 4; ++i)
        if (dest & (8u >> i)) r.w[i] = value.w[i];
}
inline V add(V a, V b) { return V{{fp::add(a.w[0], b.w[0]), fp::add(a.w[1], b.w[1]), fp::add(a.w[2], b.w[2]), fp::add(a.w[3], b.w[3])}}; }
inline V sub(V a, V b) { return V{{fp::sub(a.w[0], b.w[0]), fp::sub(a.w[1], b.w[1]), fp::sub(a.w[2], b.w[2]), fp::sub(a.w[3], b.w[3])}}; }
inline V mul(V a, V b) { return V{{fp::mul(a.w[0], b.w[0]), fp::mul(a.w[1], b.w[1]), fp::mul(a.w[2], b.w[2]), fp::mul(a.w[3], b.w[3])}}; }

}  // namespace ps2::fp4

#endif
