"""Derive the SPU2 source whose exponential envelope decrease reaches silence (TASK-125, issue #104; idea from PR #118).

The donor's Envelope::step truncates delta * level / 0x8000 toward zero, so a quiet released voice stops at a level above
zero and is never turned off: the oil change and car wash bubbles of GT4 Auto play until the game is closed. The derived
runtime/src/iop/spu2.cpp takes that step from native/spu2_envelope.hpp, which keeps the donor's step and only moves a step
of 0 at a level above zero to -1. Everything else in the file is the donor's.

Overlay sections are anchored on unique donor text. Removing them must restore the normalized input, as
scripts/prepare_perf_runtime.py checks for its overlays.
"""

import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DONOR_SPU2 = 'runtime/src/iop/spu2.cpp'
DONOR_SPU2_SHA256 = '84809a6aa1892da83caa3858b3e7a344b447210d049ab3c2a264696217caa4d9'

# (name, first line of the donor text, first line after it, the text that replaces it). The derived file keeps the donor's
# includes; the build gives it runtime/src/iop first, then native/ for spu2_envelope.hpp.
SECTIONS = [
    ('includes', '#include "spu2.hpp"\n', '\n#include "hw.hpp"\n',
     '#include "spu2.hpp"\n\n#include "spu2_envelope.hpp"\n'),
    ('exponential_decrease', '        if (exponential && decrease) delta = delta * level / 0x8000;\n',
     '        level = std::clamp(level + delta, 0, 0x7FFF);\n',
     '        // TASK-125: at least one unit down while the level is above zero (native/spu2_envelope.hpp).\n'
     '        if (exponential && decrease) delta = gt4::spu2_envelope::exponential_decrease(delta, level);\n'),
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
    raw = (assets_root / 'private/references/ps2-web-recomp' / DONOR_SPU2).read_bytes()
    if hashlib.sha256(raw).hexdigest() != DONOR_SPU2_SHA256:
        raise ValueError(f'Donor source differs from the pinned version: {DONOR_SPU2}')
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
        raise ValueError('SPU2 overlay does not reverse to the donor input')
    output.mkdir(parents=True, exist_ok=True)
    (output / 'spu2.cpp').write_text(derived, encoding='utf-8', newline='\n')
    manifest = {
        'schema_version': 1,
        'donor_source': DONOR_SPU2,
        'donor_sha256': DONOR_SPU2_SHA256,
        'spu2_sha256': hashlib.sha256(derived.encode('utf-8')).hexdigest(),
        'overlay': 'TASK-125: the exponential envelope decrease reaches silence (native/spu2_envelope.hpp)',
    }
    (output / 'runtime.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8', newline='\n')
    return derived


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--assets-root', type=Path)
    options = parser.parse_args()
    prepare(options.output, options.assets_root)
