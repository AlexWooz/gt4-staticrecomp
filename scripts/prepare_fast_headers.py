"""Derive the runtime headers used by game code from the pinned MIT donor, with exact faster forms (TASK-134).

The frozen donor's runtime/include/ps2 is copied whole into a build-tree folder that comes first on the include path, so a quoted
include between its headers stays inside the copy. Three operations differ, each with the same bits as the donor's for every
input (native/vu_fast_ops_fixture.cpp):

  - fp::add and fp::mul, the exact soft float behind the EE FPU and the VU0 macro operations, without the branches a processor
    cannot predict: which operand is larger, whether the signs differ, whether the product carries. They are a fifth of the EE
    thread's CPU in a race, and that is physics code, so only the form may change.
  - vuops::vfix decides "normal" with an integer compare instead of a float compare of the same bits. It runs for every VU1 result.

Donor text that is not found exactly once stops the build: a change in the donor must be looked at.

usage: prepare_fast_headers.py <donor root> <output folder>
"""

import hashlib
import shutil
import sys
from pathlib import Path

FP_FLUSH = 'inline std::uint32_t flush(std::uint32_t x) { return exponent(x) ? x : (x & kSign); }\n'
FP_FLUSH_FAST = 'inline std::uint32_t flush(std::uint32_t x) { return x & (exponent(x) ? 0xFFFFFFFFu : kSign); }\n'

FP_ADD = '''    if (ba > aa) {
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
'''
FP_ADD_FAST = '''    // TASK-134: the donor's steps, with its three unpredictable branches as selections and one normalising shift.
    const bool swapped = ba > aa;
    const std::uint32_t larger = swapped ? b : a, smaller = swapped ? a : b;
    const std::uint32_t larger_abs = swapped ? ba : aa, smaller_abs = swapped ? aa : ba;
    const int larger_exponent = static_cast<int>(larger_abs >> 23);
    const int diff = larger_exponent - static_cast<int>(smaller_abs >> 23);
    const std::uint32_t ma = mantissa(larger_abs) << 1;
    const std::uint32_t mb = diff > 25 ? 0u : (mantissa(smaller_abs) << 1) >> diff;
    const std::uint32_t negate = ((larger ^ smaller) & kSign) ? 0xFFFFFFFFu : 0u;
    const std::uint32_t sum = ma + ((mb ^ negate) - negate);
    if (!sum) return 0;
    // Bit 24 is the leading place. A carry reaches bit 25 (shift -1); a subtraction can leave it far below.
    const int shift = std::countl_zero(sum) - 7;
    return pack(larger & kSign, larger_exponent - shift, (sum << (shift + 1)) >> 2);
'''

FP_MUL = '''    int e = ea + eb - 127;
    if (m >> 47) {
        m >>= 24;
        ++e;
    } else {
        m >>= 23;
    }
    return pack(sign, e, static_cast<std::uint32_t>(m));
'''
FP_MUL_FAST = '''    const int carry = static_cast<int>(m >> 47);  // TASK-134: the donor's branch on the carry, as arithmetic
    return pack(sign, ea + eb - 127 + carry, static_cast<std::uint32_t>(m >> (23 + carry)));
'''

VFIX = 'const v4i normal = (v4f)clamped >= (v4f)(v4i){0x00800000, 0x00800000, 0x00800000, 0x00800000};'
VFIX_FAST = 'const v4i normal = clamped > (v4i){0x007FFFFF, 0x007FFFFF, 0x007FFFFF, 0x007FFFFF};'


def replace_once(text, old, new, name):
    newline = '\r\n' if '\r\n' in text else '\n'
    old = old.replace('\n', newline)
    if text.count(old) != 1:
        raise SystemExit(f'{name}: the donor text to replace is not there exactly once:\n{old}')
    return text.replace(old, new.replace('\n', newline))


def main():
    donor = Path(sys.argv[1]) / 'runtime' / 'include' / 'ps2'
    output = Path(sys.argv[2]) / 'ps2'
    if output.exists():
        shutil.rmtree(output)
    shutil.copytree(donor, output)

    path = output / 'fpu.hpp'
    text = path.read_bytes().decode('utf-8')
    text = replace_once(text, FP_FLUSH, FP_FLUSH_FAST, 'fpu.hpp flush')
    text = replace_once(text, FP_ADD, FP_ADD_FAST, 'fpu.hpp add')
    text = replace_once(text, FP_MUL, FP_MUL_FAST, 'fpu.hpp mul')
    path.write_bytes(text.encode('utf-8'))

    path = output / 'vu_ops.hpp'
    text = path.read_bytes().decode('utf-8')
    text = replace_once(text, VFIX, VFIX_FAST, 'vu_ops.hpp vfix')
    path.write_bytes(text.encode('utf-8'))
    print('fast headers:', hashlib.sha256((output / 'fpu.hpp').read_bytes()).hexdigest()[:16],
          hashlib.sha256((output / 'vu_ops.hpp').read_bytes()).hexdigest()[:16])


if __name__ == '__main__':
    main()
