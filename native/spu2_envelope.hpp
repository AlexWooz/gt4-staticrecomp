#pragma once

// TASK-125 (issue #104): the SPU2 envelope's exponential decrease, used by the derived runtime/src/iop/spu2.cpp
// (scripts/prepare_spu2_runtime.py) for the sustain and release phases and the volume sweeps.
//
// The donor computes the step as delta * level / 0x8000. Integer division truncates toward zero, so once the product is
// smaller than 0x8000 the step is 0 and the level never falls further: a released voice stays above zero, is never turned
// off and keeps playing until the game is closed (the oil change and car wash bubbles of issue #104). A decreasing step is
// at least one unit while the level is above zero, so every exponential release reaches silence; every other step is the
// donor's own.

#include <cstdint>

namespace gt4::spu2_envelope {

constexpr int exponential_decrease(int delta, std::int32_t level) {
    const int step = delta * level / 0x8000;
    return step == 0 && level > 0 ? -1 : step;
}

}  // namespace gt4::spu2_envelope
