// Runtime contract for functions emitted by gt4_vu_recomp.
//
// Every emitted VU1 program has the signature ProgramFunction. It starts at
// its entry PC with the caller's Vu0Regs and VU1 data memory, and ends in one
// of two states:
//
// Completed
//   The program reached its end (E bit) or handed off to the interpreter with
//   ps2::vu::interpret(1, pc). regs.tpc holds the next PC.
//
// BudgetExhausted
//   The step budget ran out before the next backward-control-transfer segment
//   started. No instruction of that segment has executed. Registers, flags,
//   VI, data memory and the pending-branch state are committed at the
//   instruction boundary context.resume_pc; regs.tpc is not written. The caller
//   must continue with ps2::vu::interpret(1, context.resume_pc), which runs the
//   interpreter with its own 1,000,000-instruction cap.
//
// Budget accounting. context.budget starts at kInterpreterInstructionBudget.
// The function charges the budget at entry and at every backward transfer (a
// branch or fallthrough to a PC at or before the transferring instruction).
// Each charge is the number of instructions in the largest forward-only region
// that starts at the target, so the charge is an upper bound on the
// instructions the following segment can execute. Generated code therefore
// never executes more instructions than the interpreter's budget before it
// returns BudgetExhausted. Charges are per loop head, not per instruction.
//
// Known limit. The interpreter's FlagPipe history (in-flight MAC/clip values
// written in the last few instructions) is not transferred on resume. MAC and
// clip reads inside that window can differ from an uninterrupted run.
#pragma once

#include "ps2/vu_ops.hpp"

#include <cstdint>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace gt4::vugen {

enum class RunStatus : std::uint32_t {
    Completed = 0,
    BudgetExhausted = 1,
};

struct RunContext {
    std::uint32_t budget = 0;
    std::uint32_t resume_pc = 0;
    // VU0 only (slice 160): exact interpreter hand-off, run(0, pc) with the AOT gate off. The
    // caller (runtime overlay) sets it; VU1 programs never call it.
    void (*exact_run)(std::uint32_t pc) = nullptr;
};

// VU0 data memory is 4 KiB: qword index masked to 256 qwords (donor unit_of(0): data_qwords - 1).
inline std::uint8_t *qword_vu0(std::uint8_t *memory, std::uint32_t index) { return memory + (index & 255u) * 16u; }

using ProgramFunction = RunStatus (*)(ps2::Vu0Regs &regs, std::uint8_t *vu_memory, RunContext &context);

// Clip word after one CLIP instruction, bit-identical to ps2::vuops::clip_value. The
// donor takes the previous clip word from the register (r.clip), so the clip word is a
// chain through every CLIP instruction. The emitted code passes its own copy of that
// word, and every CLIP instruction must update it, observed or not.
inline std::uint32_t clip_result_scalar(std::uint32_t previous_clip, const ps2::Reg128 &s, std::uint32_t w) {
    const std::int32_t lim = ps2::fp::key(w & ps2::fp::kAbs);
    std::uint32_t f = 0;
    for (int i = 0; i < 3; ++i) {
        const std::int32_t x = ps2::fp::key(s.w[i]);
        if (x > lim) f |= 1u << (i * 2);
        if (x < -lim) f |= 2u << (i * 2);
    }
    return ((previous_clip << 6) | f) & 0xFFFFFFu;
}

// TASK-134: the same word with the lanes compared at once (native_vu_fast_ops compares it with the scalar form). fp::key per
// lane: 0 for a zero exponent, otherwise the magnitude, negated for a negative sign. The pairs (above, below) of x, y and z
// interleave into flag bits 0-5 by unpacking the two masks.
#if defined(__SSE2__)
inline std::uint32_t clip_result(std::uint32_t previous_clip, const ps2::Reg128 &s, std::uint32_t w) {
    const __m128i value = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&s));
    const __m128i zero_exponent = _mm_cmpeq_epi32(_mm_and_si128(value, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128());
    const __m128i negative = _mm_srai_epi32(value, 31);
    const __m128i magnitude = _mm_and_si128(value, _mm_set1_epi32(0x7FFFFFFF));
    const __m128i key = _mm_andnot_si128(zero_exponent, _mm_sub_epi32(_mm_xor_si128(magnitude, negative), negative));
    const std::int32_t lim = ps2::fp::key(w & ps2::fp::kAbs);
    const __m128i above = _mm_cmpgt_epi32(key, _mm_set1_epi32(lim));
    const __m128i below = _mm_cmplt_epi32(key, _mm_set1_epi32(-lim));
    const auto mask_of = [](__m128i x) { return static_cast<std::uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(x))); };
    const std::uint32_t xy = mask_of(_mm_unpacklo_epi32(above, below));     // x above, x below, y above, y below
    const std::uint32_t z = mask_of(_mm_unpackhi_epi32(above, below)) & 3u;  // z above, z below (w is not compared)
    return ((previous_clip << 6) | xy | (z << 4)) & 0xFFFFFFu;
}
#else
inline std::uint32_t clip_result(std::uint32_t previous_clip, const ps2::Reg128 &s, std::uint32_t w) {
    return clip_result_scalar(previous_clip, s, w);
}
#endif

// MAC word of one flag-setting result over its destination lanes. Same bits as the
// donor flags() in vu_ops.hpp: lane bit 3 - i is zero, bit 7 - i is sign, bit 15 - i
// is overflow-clamped. The emitted code uses it when a MAC value is kept without the
// status update.
inline std::uint32_t mac_of_result_scalar(const ps2::Reg128 &value, unsigned dest) {
    std::uint32_t mac = 0;
    for (int i = 0; i < 4; ++i) {
        if (!(dest & (8u >> i))) continue;
        const std::uint32_t bit = 3u - static_cast<std::uint32_t>(i);
        const std::uint32_t x = value.w[i];
        if (!(x & 0x7F800000u)) mac |= 1u << bit;
        if (x & 0x80000000u) mac |= 1u << (bit + 4);
        if ((x & 0x7FFFFFFFu) == 0x7FFFFFFFu) mac |= 1u << (bit + 12);
    }
    return mac;
}

// TASK-134: the same bits without a branch per lane (native_vu_fast_ops compares it with the scalar form). The lanes are
// reversed first, because lane i owns bit 3 - i, which is also how `dest` numbers them.
#if defined(__SSE2__)
inline std::uint32_t mac_of_result(const ps2::Reg128 &value, unsigned dest) {
    const __m128i lanes = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i *>(&value)), 0x1B);
    const auto mask_of = [](__m128i x) { return static_cast<std::uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(x))); };
    const std::uint32_t zero = mask_of(_mm_cmpeq_epi32(_mm_and_si128(lanes, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128()));
    const std::uint32_t sign = mask_of(lanes);
    const std::uint32_t over = mask_of(_mm_cmpeq_epi32(_mm_and_si128(lanes, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7FFFFFFF)));
    return (zero | (sign << 4) | (over << 12)) & (dest * 0x1011u);
}
#else
inline std::uint32_t mac_of_result(const ps2::Reg128 &value, unsigned dest) { return mac_of_result_scalar(value, dest); }
#endif

struct ProgramEntry {
    std::uint64_t key;
    std::uint32_t entry;
    // The function is valid only for images whose image-wide flag-use bit equals
    // this value (see image_flags_used in vurecomp.hpp). The registrar must match it.
    bool image_flags_used;
    // The image-wide status-use bit (image_status_used in program_identity.hpp). When false,
    // the emitted code does not maintain VU1 status and the status field is not compared.
    bool image_status_used;
    // Lowercase SHA-256 of the identity stream (program_identity.hpp); checked by the runtime.
    const char *identity_sha256;
    ProgramFunction function;
};

// Same unit and value as the donor interpreter's per-call cap (vu_micro.cpp run_fast).
constexpr std::uint32_t kInterpreterInstructionBudget = 1'000'000u;

}
