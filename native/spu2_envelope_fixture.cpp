// TASK-125 (issue #104): the SPU2 exponential envelope decrease of native/spu2_envelope.hpp against the donor's step.
// For every rate a decrease can use, a voice released at full level must reach silence, and every step the donor did not
// round to 0 must be the donor's own. The counter that spaces the steps out is left aside: it only delays them.

#include "spu2_envelope.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

void require(bool condition, const char *what, int rate) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s (rate %d)\n", what, rate);
    ++failures;
}

// The donor's step size for a decrease at `rate` (runtime/src/iop/spu2.cpp, Envelope::step).
int decrease_delta(int rate) {
    const int shift = rate >> 2;
    return (-8 + (rate & 3)) << std::max(0, 11 - shift);
}

// The donor's exponential step: delta * level / 0x8000, truncated toward zero.
int donor_step(int delta, std::int32_t level) { return delta * level / 0x8000; }

}  // namespace

int main() {
    int donor_stalls = 0;
    std::uint64_t longest = 0;
    for (int rate = 0; rate < 128; ++rate) {
        const int delta = decrease_delta(rate);
        std::int32_t level = 0x7FFF;
        std::uint64_t steps = 0;
        while (level > 0 && steps < 4'000'000) {
            const int step = gt4::spu2_envelope::exponential_decrease(delta, level);
            const int donor = donor_step(delta, level);
            require(step < 0, "a level above zero always falls", rate);
            require(donor == 0 ? step == -1 : step == donor, "the donor's step wherever it was not 0", rate);
            level = std::clamp(level + step, 0, 0x7FFF);
            ++steps;
        }
        require(level == 0, "a released voice reaches silence", rate);
        longest = std::max(longest, steps);
        // The donor itself stops above zero at this rate when its step from full level rounds to 0 before silence.
        std::int32_t donor_level = 0x7FFF;
        for (std::uint64_t step = 0; step < steps + 1 && donor_level > 0; ++step) {
            const int next = donor_step(delta, donor_level);
            if (next == 0) break;
            donor_level = std::clamp(donor_level + next, 0, 0x7FFF);
        }
        if (donor_level > 0) ++donor_stalls;
    }
    require(gt4::spu2_envelope::exponential_decrease(decrease_delta(127), 0) == 0, "a silent level stays silent", 127);
    require(donor_stalls == 128, "the donor stalls above zero at every rate (issue #104)", donor_stalls);
    if (failures) return 1;
    std::fprintf(stderr, "PASS: SPU2 exponential decrease reaches silence at all 128 rates (longest %llu steps); the donor "
                         "stalled above zero at %d of them\n",
                 static_cast<unsigned long long>(longest), donor_stalls);
    return 0;
}
