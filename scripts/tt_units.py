#!/usr/bin/env python
"""Tourist Trophy: translated EE units with TT's host calls and statement replacements (docs/TT-STUDY.md).

Like scripts/camera360_units.py does for v2.00 and Spec II, this copies TT's translated units into a fresh folder and edits
the few places the host must reach that no dispatch hook sees:

  * License demos: at the entry label of TT's license cipher (0x004A4228, v2.00 func_00579E00, reached by a tail jump), one
    host call that may decrypt the demo exactly and return (native/tt_license_decrypt_hook.cpp):
        L_004A4228:
            if (::gt4::tt_license::before_license_decrypt(c)) return;
  * TASK-161 full trigger range (native/trigger_range.hpp): TT's race input (0x00403810; its pressure switch is v2.00
    func_0043BBC0's, with the divisors in saved registers: s1 = 35, s6 = 65, s5 = 20) gets v2.00's four statement
    replacements in the throttle case (flag 0x2, store +8) and the brake case (flag 0x4, store +10): the doubling
    `v1 << 1` becomes pedal_pressure(v1) and the divisor of the divide becomes pedal_divisor(divisor). The other two cases
    (flags 0x10 and 0x8, divisors 20 and 35) are left as v2.00 leaves them.
  * TASK-159 / TASK-160 (native/camera_360.hpp, native/fov_adjust.hpp): host calls between a call's delay slot and the call,
    as camera360_units.py adds them for v2.00: the four angle-difference calls of TT's chase camera spring 0x002E7068 (the
    host acts only at the return label native/game_target.hpp names, so trying another of the four is a relink), the race
    input's channel-handler call (0x00403AC4; a2 = function, a3 = value) and the race camera projection's scale call
    (0x00322DCC, v2.00 func_0037A170).

Each statement edit is found in its own labelled block exactly once, each call line in its unit exactly once, plus a declaration after `#include "units.hpp"`. It refuses a
missing or repeated statement, an output folder that exists, and any result from which removing the insertions does not give
the input back byte for byte. Every other file is copied unchanged. The generated units are game-derived: they stay under
private/.

usage: tt_units.py <translated TT units folder> <fresh output folder>
"""
import argparse
import dataclasses
import hashlib
import json
import shutil
import sys
from pathlib import Path

INCLUDE = '#include "units.hpp"\n'
CALL_MARK = '  if (depth < kMaxDepth)'


@dataclasses.dataclass(frozen=True)
class Edit:
    """One statement replaced (or one line inserted after a label) inside the block that starts at `label`."""
    what: str
    label: str        # e.g. 'L_00403B68'; '' for a call line found anywhere in its unit (exactly once)
    original: str     # the exact line in the block ('' for an insertion right after the label)
    replacement: str  # the new line(s)
    namespace: str
    declaration: str


LICENSE = Edit(
    'license demo exact decrypt (cipher entry 0x004A4228)', 'L_004A4228', '',
    '    if (::gt4::tt_license::before_license_decrypt(c)) return;  // tt_units.py\n',
    'gt4::tt_license', 'bool before_license_decrypt(ps2::Cpu &c);')


def doubling(label, address):
    return Edit(f'pedal doubling at 0x{address:08X}', label, '    set32(c.r[3], c.r[3].w[0] << 1);\n',
                '    set32(c.r[3], ::gt4::trigger_range::pedal_pressure(c.r[3].w[0]));\n',
                'gt4::trigger_range', 'std::uint32_t pedal_pressure(std::uint32_t value);')


def divide(label, address, register):
    return Edit(f'pedal divide at 0x{address:08X}', label, f'    mmi::div(c, 0, c.r[2].w[0], c.r[{register}].w[0]);\n',
                f'    mmi::div(c, 0, c.r[2].w[0], ::gt4::trigger_range::pedal_divisor(c.r[{register}].w[0]));\n',
                'gt4::trigger_range', 'std::uint32_t pedal_divisor(std::uint32_t value);')


def call_site(hook, return_label, slot, unit_call, callee):
    """The translator's line for `jal callee` (delay slot `slot`), with the host call added after the slot, before the call."""
    line = (f'    {{ c.r[31].sd[0] = static_cast<std::int32_t>(0x{return_label:08X}u); {slot}{CALL_MARK} {unit_call}; '
            f'else {{ c.pc = 0x{callee:08X}u; return; }} if (c.pc != 0x{return_label:08X}u) [[unlikely]] return; }}\n')
    namespace, _, name = hook.rpartition('::')
    return Edit(f'{name} at return 0x{return_label:08X}', '', line,
                line.replace(CALL_MARK, f'  ::{hook}(c); if (depth < kMaxDepth)', 1), namespace, f'void {name}(ps2::Cpu &c);')


YAW_HOOK = 'gt4::camera360::before_chase_yaw_call'
CHANNEL_HOOK = 'gt4::camera360::before_pad_analog_channel_call'
PROJECTION_HOOK = 'gt4::fov_adjust::before_projection_scale_call'

EDITS = (
    LICENSE,
    # Throttle (flag 0x2, store +8): sll v1,v1,1 at 0x00403B6C; div by s1 = 35 at 0x00403BA0.
    doubling('L_00403B68', 0x00403B6C), divide('L_00403B90', 0x00403BA0, 17),
    # Brake (flag 0x4, store +10): sll v1,v1,1 at 0x00403BC8; div by s6 = 65 at 0x00403BFC.
    doubling('L_00403BC0', 0x00403BC8), divide('L_00403BEC', 0x00403BFC, 22),
    # TASK-159: the chase camera spring 0x002E7068 calls the angle difference 0x003152A0 four times (yaw candidates).
    call_site(YAW_HOOK, 0x002E7120, 'c.f[12] = c.f[3];', 'unit_0085(c, 63u, depth + 1u)', 0x003152A0),
    call_site(YAW_HOOK, 0x002E71B0, 'c.f[12] = ld32((c.r[17].w[0] + 1424u));', 'unit_0085(c, 63u, depth + 1u)', 0x003152A0),
    call_site(YAW_HOOK, 0x002E7244, 'c.f[13] = ld32((c.r[16].w[0] + 12u));', 'unit_0085(c, 63u, depth + 1u)', 0x003152A0),
    call_site(YAW_HOOK, 0x002E72A0, 'c.f[12] = ld32((c.r[17].w[0] + 1416u));', 'unit_0085(c, 63u, depth + 1u)', 0x003152A0),
    # TASK-159b: the race input's channel handler 0x003FEF48 (jal at 0x00403AC4, delay slot a3 = value).
    call_site(CHANNEL_HOOK, 0x00403ACC, 'c.r[7].d[0] = c.r[8].d[0] + 0ull;', 'unit_00BF(c, 366u, depth + 1u)', 0x003FEF48),
    # TASK-160: the race camera projection's scale call (jal 0x00321AF8 at 0x00322DCC, delay slot mov.s $f13,$f12).
    call_site(PROJECTION_HOOK, 0x00322DD4, 'c.f[13] = c.f[12];', 'unit_0088(c, 203u, depth + 1u)', 0x00321AF8),
)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def block_span(text, label):
    """(start, end) of the block from `label:` to the next label line."""
    marker = f'\n{label}:\n'
    if text.count(marker) != 1:
        raise ValueError(f'label {label} missing or repeated')
    start = text.find(marker) + 1
    end = text.find('\nL_', start + len(marker) - 1)
    return start, (len(text) if end < 0 else end + 1)


def apply(text, edits):
    for edit in edits:
        if not edit.label:
            if text.count(edit.original) != 1:
                raise ValueError(f'{edit.what}: call line found {text.count(edit.original)} times')
            text = text.replace(edit.original, edit.replacement, 1)
            continue
        start, end = block_span(text, edit.label)
        block = text[start:end]
        if edit.original:
            if block.count(edit.original) != 1:
                raise ValueError(f'{edit.what}: statement found {block.count(edit.original)} times in {edit.label}')
            block = block.replace(edit.original, edit.replacement, 1)
        else:
            head = f'{edit.label}:\n'
            block = head + edit.replacement + block[len(head):]
        text = text[:start] + block + text[end:]
    return text


def declarations(edits):
    names = {}
    for edit in edits:
        names.setdefault(edit.namespace, set()).add(edit.declaration)
    lines = ''.join(f'namespace {ns} {{ {" ".join(sorted(names[ns]))} }}\n' for ns in sorted(names))
    return ('\n// Tourist Trophy host calls (native/tt_license_decrypt_hook.cpp, trigger_range.hpp, camera_360.hpp, fov_adjust.hpp), '
            'added by '
            'scripts/tt_units.py.\n' + lines)


def patch(text, edits):
    if text.count(INCLUDE) != 1:
        raise ValueError('expected one units.hpp include')
    header = declarations(edits)
    out = apply(text, edits).replace(INCLUDE, INCLUDE + header, 1)
    # Removing every insertion must restore the input.
    back = out.replace(header, '', 1)
    for edit in edits:
        back = back.replace(edit.replacement, edit.original, 1)
    if back != text:
        raise ValueError('removing the insertions does not restore the unit')
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('units', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args(argv)
    if args.output.exists():
        raise SystemExit('output folder exists; choose a fresh one')
    by_unit = {}
    for edit in EDITS:
        key = f'\n{edit.label}:\n' if edit.label else edit.original
        hosts = [path for path in sorted(args.units.glob('unit_*.cpp')) if key in path.read_bytes().decode('utf-8')]
        if len(hosts) != 1:
            raise SystemExit(f'{edit.what}: found in {len(hosts)} units, expected 1')
        by_unit.setdefault(hosts[0].name, []).append(edit)
    args.output.mkdir(parents=True)
    receipt = {'schema_version': 2, 'edits': [edit.what for edit in EDITS], 'copied': {}, 'patched': {}}
    for path in sorted(args.units.iterdir()):
        if not path.is_file():
            raise SystemExit(f'unexpected entry {path.name}')
        data = path.read_bytes()
        if path.name in by_unit:
            out = patch(data.decode('utf-8'), by_unit[path.name]).encode('utf-8')
            (args.output / path.name).write_bytes(out)
            receipt['patched'][path.name] = {'input_sha256': sha256(data), 'output_sha256': sha256(out),
                                             'edits': [edit.what for edit in by_unit[path.name]]}
        else:
            shutil.copyfile(path, args.output / path.name)
            if sha256((args.output / path.name).read_bytes()) != sha256(data):
                raise SystemExit(f'copy differs: {path.name}')
            receipt['copied'][path.name] = sha256(data)
    (args.output.parent / (args.output.name + '.tt-units.json')).write_text(json.dumps(receipt, indent=2) + '\n',
                                                                          encoding='utf-8')
    print(f'tt-units: {len(EDITS)} edits in {", ".join(sorted(by_unit))}; copied {len(receipt["copied"])} files into '
          f'{args.output}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
