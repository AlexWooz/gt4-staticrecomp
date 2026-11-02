"""Derive the faster IPU video decoder from the pinned MIT donor source (TASK-027, IN-490).

GT4 plays video behind its menus; the donor's decoder took about half of the EE thread there. This derives
runtime/src/ipu/ipu.cpp with the bit reader, the inverse DCT and the colour conversion of native/ipu_fast.hpp and without a
function-local static on the way to every table, and one read of the stream per coefficient. The results are exactly the donor's (native/ipu_fast_fixture.cpp, and the
frames of the video scenes).

Overlay sections are anchored on unique donor text. Removing them must restore the normalized input, as
scripts/prepare_perf_runtime.py checks for its overlays.
"""

import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DONOR_IPU = 'runtime/src/ipu/ipu.cpp'
DONOR_IPU_SHA256 = 'fdd78c5b470e000700ab3e9bcd3c8dba9672fc7fd5c8b3032c7e12bf8fd96f7c'

# (name, first line of the donor text, first line after it, the text that replaces it)
SECTIONS = [
    ('includes', '#include "ipu.hpp"\n', '\n#include "ps2/state.hpp"\n',
     '#include "ipu/ipu.hpp"\n\n#include "ipu/tables.hpp"\n#include "dmac.hpp"\n#include "hw.hpp"\n#include "ipu_fast.hpp"\n'),
    ('cosines', '    double cosines[8][8];\n', '\n    Tables() {\n',
     '    double cosines[8][8];\n    double cosines_by_u[8][8];  // IN-490: cosines[x][u] as [u][x]\n'),
    ('cosines_fill',
     '                cosines[x][u] = (u ? 0.5 : 0.5 / std::sqrt(2.0)) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0);\n',
     '    }\n};\n',
     '                cosines_by_u[u][x] = cosines[x][u] = (u ? 0.5 : 0.5 / std::sqrt(2.0)) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0);\n'),
    ('tables', 'const Tables &tables() {\n', '\nenum Command {',
     '// IN-490: an object of this file, not a function-local static with a check on every table read.\nconst Tables g_tables;\n\n'
     'inline const Tables &tables() { return g_tables; }\n'),
    ('bits_at', 'std::uint32_t bits_at(std::size_t bit, unsigned n) {\n', 'std::uint32_t peek(unsigned n) {\n',
     'std::uint32_t bits_at(std::size_t bit, unsigned n) { return gt4::ipu_fast::read_bits(g_in.data(), g_in.size(), bit, n); }\n\n'),
    ('idct', 'void idct(const int *in, std::int16_t *out) {\n', 'void decode_block(std::int16_t *out',
     'void idct(const int *in, std::int16_t *out) { gt4::ipu_fast::idct(tables().cosines, tables().cosines_by_u, in, out); }\n\n'),
    ('coefficient_list', '    int coef[64] = {};\n', '    int index = 0;\n',
     '    // IN-491: only the coded positions are read below, in the same rising order the loop over 64 visited them.\n'
     '    int coef[64];\n    std::uint8_t coded[64];\n    int coded_count = 0;\n'),
    ('dc_intra', '        coef[0] = g_dc[channel];\n', '        index = 1;\n    } else if (peek(1)) {\n',
     '        coef[0] = g_dc[channel];\n        coded[coded_count++] = 0;\n'),
    ('dc_nonintra', '        coef[0] = get(2) & 1u ? -1 : 1;\n', '        index = 1;\n    }\n    for (;;) {\n',
     '        coef[0] = get(2) & 1u ? -1 : 1;\n        coded[coded_count++] = 0;\n'),
    ('coefficients', '    for (;;) {\n        if (index) {\n            if (table_one ? peek(4) == 6u : peek(2) == 2u) {\n',
     '    const std::uint8_t *scan = alternate_scan() ? kAlternate : kZigzag;\n',
     '''    for (;;) {
        int run = 0, level = 0;
        // IN-491: with 32 bits at hand one read answers the end of block, the code and its sign: three reads per coefficient
        // were the largest part of the decoder. Fewer bits, or an escape code, take the donor's way.
        if (available() >= 32) {
            const std::uint32_t window = bits_at(g_bit, 32);
            if (index && (table_one ? (window >> 28) == 6u : (window >> 30) == 2u)) {
                g_bit += table_one ? 4 : 2;
                break;
            }
            const DctEntry &e = table.entries[window >> (32 - table.bits)];
            if (!e.length) throw BadCode{};
            if (e.run != kEscape) {
                g_bit += e.length + 1u;
                run = e.run;
                level = (window >> (31 - e.length)) & 1u ? -e.level : e.level;
                index += run;
                if (index >= 64) throw BadCode{};
                coded[coded_count++] = static_cast<std::uint8_t>(index);
                coef[index++] = level;
                continue;
            }
        }
        if (index) {
            if (table_one ? peek(4) == 6u : peek(2) == 2u) {
                skip(table_one ? 4 : 2);
                break;
            }
        }
        read_coefficient(table, run, level);
        index += run;
        if (index >= 64) throw BadCode{};
        coded[coded_count++] = static_cast<std::uint8_t>(index);
        coef[index++] = level;
    }
'''),
    ('dequantise', '    for (int k = 0; k < 64; ++k) {\n        const int v = coef[k];\n', '        const int r = scan[k];\n',
     '    for (int i = 0; i < coded_count; ++i) {\n        const int k = coded[i];\n        const int v = coef[k];\n'),
    ('csc_read', '                for (std::uint8_t &v : raw) v = static_cast<std::uint8_t>(get(8));\n',
     '                convert(raw, (cmd >> 27) & 1u);\n',
     '''                // IN-491: GT4 reassembles its pictures on the EE and sends them through CSC: 384 reads of 8 bits per
                // macroblock were half of the decoder. Bytes are bytes when the stream sits on a byte boundary.
                if ((g_bit & 7u) == 0) {
                    if (!have(384 * 8)) throw NeedData{};
                    std::memcpy(raw, g_in.data() + (g_bit >> 3), 384);
                    g_bit += 384 * 8;
                } else {
                    for (std::uint8_t &v : raw) v = static_cast<std::uint8_t>(get(8));
                }
'''),
    ('convert', 'void convert(const std::uint8_t *raw, bool rgb16) {\n', 'void emit_raw16(const std::int16_t *raw) {\n',
     'void convert(const std::uint8_t *raw, bool rgb16) {\n    std::uint8_t pixels[1024];\n'
     '    const std::size_t size = gt4::ipu_fast::convert(raw, rgb16, g_th0, g_th1, pixels);\n'
     '    g_out.insert(g_out.end(), pixels, pixels + size);\n}\n\n'),
]


def apply_overlay(source):
    """Returns the derived text and the donor text of each section, to check the way back."""
    removed = []
    for name, start, end, replacement in SECTIONS:
        if source.count(start) != 1:
            raise ValueError(f'{name}: the start anchor must appear exactly once in the donor source')
        first = source.index(start)
        last = source.index(end, first)
        removed.append((replacement, source[first:last]))
        source = source[:first] + replacement + source[last:]
    return source, removed


def remove_overlay(source, removed):
    for replacement, original in reversed(removed):
        if source.count(replacement) != 1:
            raise ValueError('derived source lacks an overlay section')
        source = source.replace(replacement, original, 1)
    return source


def read_donor(assets_root):
    raw = (assets_root / 'private/references/ps2-web-recomp' / DONOR_IPU).read_bytes()
    if hashlib.sha256(raw).hexdigest() != DONOR_IPU_SHA256:
        raise ValueError(f'Donor source differs from the pinned version: {DONOR_IPU}')
    return raw.decode('utf-8').replace('\r\n', '\n')


def prepare(output, assets_root=None):
    assets_root = Path(assets_root or ROOT).resolve()
    output = output.resolve()
    private = assets_root / 'private'
    if not output.is_relative_to(private) or output == private:
        raise ValueError('Derived donor code must stay inside the assets checkout private/')
    source = read_donor(assets_root)
    derived, removed = apply_overlay(source)
    if remove_overlay(derived, removed) != source:
        raise ValueError('IPU overlay does not reverse to the donor input')
    output.mkdir(parents=True, exist_ok=True)
    (output / 'ipu.cpp').write_text(derived, encoding='utf-8', newline='\n')
    manifest = {
        'schema_version': 1,
        'donor_source': DONOR_IPU,
        'donor_sha256': DONOR_IPU_SHA256,
        'ipu_sha256': hashlib.sha256(derived.encode('utf-8')).hexdigest(),
        'overlay': 'TASK-027 IN-490: bit reader, inverse DCT and colour conversion of native/ipu_fast.hpp',
    }
    (output / 'runtime.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8', newline='\n')
    return derived


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--assets-root', type=Path)
    options = parser.parse_args()
    prepare(options.output, options.assets_root)
