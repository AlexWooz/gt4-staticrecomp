"""TASK-090 CoreGS 3.0, phase 1: translated EE units with the observe-only capture hooks (native/coregs3/coregs3_hooks.hpp).

The hooks have to sit at exact guest addresses inside translated code, so they cannot be reached from a dispatch hook:

  * K1: `jal 0x0049D308` at 0x00455A5C, continuation 0x00455A64 (ModelSet2___render, the H1 draw frontier);
  * direct K1 sites (format version 5): the `jal 0x0049D308` calls that do not pass through ModelSet2___render
    (car env map 0x0044E920/0x0044EE24, BillboardSet 0x004507AC, tachometer 0x003EC4A0);
  * K3/K4: the return labels of the projection and modelview virtual calls of func_0033EAB8 (L_0033EBB4, L_0033EBF4);
  * K5: the block starts that bracket the main view's car pass (L_0033EE28, L_0033EE54);
  * K7 (format version 5): the pass markers of func_0033EAB8 (env map, sky, far underlay, course, reflection, flares,
    concourse, render_after, mirror, crowd/pitmen, particles, post) and of the HUD panel dispatch.

This script copies a target's translated units into a fresh folder and inserts one host call at each of those points, plus
the declarations of the two hook namespaces after `#include "units.hpp"` (outside `namespace gen`). The inserted calls
only read guest memory and VU0 registers (native/coregs3/coregs3_hooks.cpp); they never write guest memory and never
touch `c.budget`, so the simulation is unchanged. The direct shape-call sites get the call after the `a0` assignment
inside the call statement (`inside_after`), because the shape pointer is loaded there.

It refuses an anchor that is missing or appears more than once, a unit that does not look like translated code, and any
output from which removing the insertions does not give the input back byte for byte. A receipt
(<output>.coregs3-units.json) records the inventories.

usage: coregs3_units.py <translated units folder> <fresh output folder>
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

DECLARATIONS = {
    'unit_00D5.cpp': (
        'namespace gt4::coregs3 {\n'
        'void before_shape_call(ps2::Cpu &c, unsigned call_site);\n'
        'void after_shape_call(ps2::Cpu &c);\n'
        '}\n'
    ),
    'unit_00D3.cpp': (
        'namespace gt4::coregs3 {\n'
        'void before_shape_call(ps2::Cpu &c, unsigned call_site);\n'
        'void after_shape_call(ps2::Cpu &c);\n'
        '}\n'
    ),
    'unit_00D4.cpp': (
        'namespace gt4::coregs3 {\n'
        'void before_shape_call(ps2::Cpu &c, unsigned call_site);\n'
        'void after_shape_call(ps2::Cpu &c);\n'
        '}\n'
    ),
    'unit_00BB.cpp': (
        'namespace gt4::coregs3 {\n'
        'void before_shape_call(ps2::Cpu &c, unsigned call_site);\n'
        'void after_shape_call(ps2::Cpu &c);\n'
        '}\n'
    ),
    'unit_008F.cpp': (
        'namespace gt4::coregs3 {\n'
        'void projection_set(ps2::Cpu &c);\n'
        'void modelview_set(ps2::Cpu &c);\n'
        'void car_pass_begin(ps2::Cpu &c);\n'
        'void car_pass_end(ps2::Cpu &c);\n'
        'void pass_begin(ps2::Cpu &c, unsigned pass);\n'
        'void pass_end(ps2::Cpu &c, unsigned pass);\n'
        '}\n'
    ),
}

# The call statements of the direct pgluCallShape sites (unit_00D3 env map, unit_00D4 BillboardSet, unit_00BB tachometer).
CALL_44E920 = (
    '    { c.r[31].sd[0] = static_cast<std::int32_t>(0x0044E920u); set32(c.r[4], c.r[18].w[0] + 32000u);  '
    'if (depth < kMaxDepth) unit_00E7(c, 53u, depth + 1u); else { c.pc = 0x0049D308u; return; } '
    'if (c.pc != 0x0044E920u) [[unlikely]] return; }\n')
CALL_44EE24 = (
    '    { c.r[31].sd[0] = static_cast<std::int32_t>(0x0044EE24u); set32(c.r[4], c.r[18].w[0] + 32000u);  '
    'if (depth < kMaxDepth) unit_00E7(c, 53u, depth + 1u); else { c.pc = 0x0049D308u; return; } '
    'if (c.pc != 0x0044EE24u) [[unlikely]] return; }\n')
CALL_4507AC = (
    '    { c.r[31].sd[0] = static_cast<std::int32_t>(0x004507ACu); set32(c.r[4], ld32((c.r[3].w[0])));  '
    'if (depth < kMaxDepth) unit_00E7(c, 53u, depth + 1u); else { c.pc = 0x0049D308u; return; } '
    'if (c.pc != 0x004507ACu) [[unlikely]] return; }\n')
CALL_3EC4A0 = (
    '    { c.r[31].sd[0] = static_cast<std::int32_t>(0x003EC4A0u); set32(c.r[4], ld32((c.r[17].w[0] + 2144u)));  '
    'if (depth < kMaxDepth) unit_00E7(c, 53u, depth + 1u); else { c.pc = 0x0049D308u; return; } '
    'if (c.pc != 0x003EC4A0u) [[unlikely]] return; }\n')

# The pass call statements of func_0033EAB8 and of the panel dispatch (RacePS2Base__render), keyed by their return
# address. Each K7 begin is inserted before the call; the end is inserted after the return label, except for
# render_after and mirror, whose return labels are merge points reached without the call, so their end is inserted
# right after the call statement.
CALL_33E9F0 = (
    '    { c.r[31].sd[0] = static_cast<std::int32_t>(0x0033E9F0u); set32(c.r[5], ld32((c.r[3].w[0] + 4u)));  '
    'if (depth < kMaxDepth) unit_00D3(c, 274u, depth + 1u); else { c.pc = 0x0044E660u; return; } '
    'if (c.pc != 0x0033E9F0u) [[unlikely]] return; }\n')  # car env map/sphere map (func_0044E660)
CALL_33EC28 = (
    '    { c.r[31].sd[0] = static_cast<std::int32_t>(0x0033EC28u); set32(c.r[6], 0u - 1u);  '
    'if (depth < kMaxDepth) unit_00A5(c, 387u, depth + 1u); else { c.pc = 0x00396B80u; return; } '
    'if (c.pc != 0x0033EC28u) [[unlikely]] return; }\n')  # sky
CALL_33EC58 = (
    '    { c.r[31].sd[0] = static_cast<std::int32_t>(0x0033EC58u); c.r[4].d[0] = c.r[16].d[0] + 0ull;  '
    'if (depth < kMaxDepth) unit_00A5(c, 409u, depth + 1u); else { c.pc = 0x00396D40u; return; } '
    'if (c.pc != 0x0033EC58u) [[unlikely]] return; }\n')  # far underlay
CALL_33EC90 = (
    '    { const std::uint32_t t = c.r[3].w[0]; c.r[31].sd[0] = static_cast<std::int32_t>(0x0033EC90u); '
    'set32(c.r[4], c.r[17].w[0] + c.r[4].w[0]);  c.pc = t; call(c, t, depth + 1u); '
    'if (c.pc != 0x0033EC90u) [[unlikely]] return; }\n')  # course (method +0x370)
CALL_33ECE0 = (
    '    { const std::uint32_t t = c.r[3].w[0]; c.r[31].sd[0] = static_cast<std::int32_t>(0x0033ECE0u); '
    'set32(c.r[4], c.r[17].w[0] + c.r[4].w[0]);  c.pc = t; call(c, t, depth + 1u); '
    'if (c.pc != 0x0033ECE0u) [[unlikely]] return; }\n')  # reflection (method +0x378)
CALL_33ED5C = (
    '    { c.r[31].sd[0] = static_cast<std::int32_t>(0x0033ED5Cu); set32(c.r[4], c.r[17].w[0] + 13800u);  '
    'if (depth < kMaxDepth) unit_00B9(c, 84u, depth + 1u); else { c.pc = 0x003E5338u; return; } '
    'if (c.pc != 0x0033ED5Cu) [[unlikely]] return; }\n')  # flares
CALL_33EDA8 = (
    '    { const std::uint32_t t = c.r[3].w[0]; c.r[31].sd[0] = static_cast<std::int32_t>(0x0033EDA8u); '
    'set32(c.r[4], c.r[17].w[0] + c.r[4].w[0]);  c.pc = t; call(c, t, depth + 1u); '
    'if (c.pc != 0x0033EDA8u) [[unlikely]] return; }\n')  # concourse (method +0x3A0)
CALL_33EEF8 = (
    '    { const std::uint32_t t = c.r[3].w[0]; c.r[31].sd[0] = static_cast<std::int32_t>(0x0033EEF8u); '
    'set32(c.r[4], c.r[17].w[0] + c.r[4].w[0]);  c.pc = t; call(c, t, depth + 1u); '
    'if (c.pc != 0x0033EEF8u) [[unlikely]] return; }\n')  # render_after (method +0x3A8)
CALL_33F0AC = (
    '    { const std::uint32_t t = c.r[3].w[0]; c.r[31].sd[0] = static_cast<std::int32_t>(0x0033F0ACu); '
    'set32(c.r[4], c.r[17].w[0] + c.r[4].w[0]);  c.pc = t; call(c, t, depth + 1u); '
    'if (c.pc != 0x0033F0ACu) [[unlikely]] return; }\n')  # back mirror (method +0x3C0)
CALL_33F140 = (
    '    { c.r[31].sd[0] = static_cast<std::int32_t>(0x0033F140u); c.r[4].d[0] = c.r[16].d[0] + 0ull;  '
    'if (depth < kMaxDepth) unit_00AE(c, 36u, depth + 1u); else { c.pc = 0x003B8848u; return; } '
    'if (c.pc != 0x0033F140u) [[unlikely]] return; }\n')  # crowd/pitmen
CALL_33F15C = (
    '    { c.r[31].sd[0] = static_cast<std::int32_t>(0x0033F15Cu); set32(c.r[4], c.r[17].w[0] + 12856u);  '
    'if (depth < kMaxDepth) unit_00B0(c, 221u, depth + 1u); else { c.pc = 0x003C2838u; return; } '
    'if (c.pc != 0x0033F15Cu) [[unlikely]] return; }\n')  # particles
CALL_33F184 = (
    '    { const std::uint32_t t = c.r[3].w[0]; c.r[31].sd[0] = static_cast<std::int32_t>(0x0033F184u); '
    'set32(c.r[4], c.r[17].w[0] + c.r[4].w[0]);  c.pc = t; call(c, t, depth + 1u); '
    'if (c.pc != 0x0033F184u) [[unlikely]] return; }\n')  # post (method +0x3E8)
CALL_33DA84 = (
    '    { const std::uint32_t t = c.r[6].w[0]; c.r[31].sd[0] = static_cast<std::int32_t>(0x0033DA84u); '
    'set32(c.r[4], c.r[16].w[0] + c.r[4].w[0]);  c.pc = t; call(c, t, depth + 1u); '
    'if (c.pc != 0x0033DA84u) [[unlikely]] return; }\n')  # HUD panel (method +0x358)

# (unit, anchor, placement, inserted line). The anchors are the translated-unit text of the sites named above; the
# comment beside each one is the guest address it belongs to. Placement 'before' inserts above the anchor line,
# 'after' below it.
INSERTS = [
    ('unit_00D5.cpp',
     '    { c.r[31].sd[0] = static_cast<std::int32_t>(0x00455A64u);  if (depth < kMaxDepth) unit_00E7(c, 53u, depth + 1u); '
     'else { c.pc = 0x0049D308u; return; } if (c.pc != 0x00455A64u) [[unlikely]] return; }\n',
     'before', '    ::gt4::coregs3::before_shape_call(c, 0u);\n'),  # K1, 0x00455A5C, ModelSet2
    ('unit_00D5.cpp', 'L_00455A64:\n', 'after', '    ::gt4::coregs3::after_shape_call(c);\n'),  # K1 continuation
    # The direct call statements set a0 (r4) inside the statement itself, so the hook goes after that assignment.
    ('unit_00D3.cpp', CALL_44E920, 'inside_after', '    ::gt4::coregs3::before_shape_call(c, 2u);\n',
     'set32(c.r[4], c.r[18].w[0] + 32000u);'),  # env map, 0x44E920
    ('unit_00D3.cpp', 'L_0044E920:\n', 'after', '    ::gt4::coregs3::after_shape_call(c);\n'),
    ('unit_00D3.cpp', CALL_44EE24, 'inside_after', '    ::gt4::coregs3::before_shape_call(c, 2u);\n',
     'set32(c.r[4], c.r[18].w[0] + 32000u);'),  # env map, 0x44EE24
    ('unit_00D3.cpp', 'L_0044EE24:\n', 'after', '    ::gt4::coregs3::after_shape_call(c);\n'),
    ('unit_00D4.cpp', CALL_4507AC, 'inside_after', '    ::gt4::coregs3::before_shape_call(c, 1u);\n',
     'set32(c.r[4], ld32((c.r[3].w[0])));'),  # billboard, 0x4507AC
    ('unit_00D4.cpp', 'L_004507AC:\n', 'after', '    ::gt4::coregs3::after_shape_call(c);\n'),
    ('unit_00BB.cpp', CALL_3EC4A0, 'inside_after', '    ::gt4::coregs3::before_shape_call(c, 3u);\n',
     'set32(c.r[4], ld32((c.r[17].w[0] + 2144u)));'),  # tachometer, 0x3EC4A0
    ('unit_00BB.cpp', 'L_003EC4A0:\n', 'after', '    ::gt4::coregs3::after_shape_call(c);\n'),
    ('unit_008F.cpp', 'L_0033EBB4:\n', 'after', '    ::gt4::coregs3::projection_set(c);\n'),  # K3
    ('unit_008F.cpp', 'L_0033EBF4:\n', 'after', '    ::gt4::coregs3::modelview_set(c);\n'),  # K4
    ('unit_008F.cpp', 'L_0033EE28:\n', 'after', '    ::gt4::coregs3::car_pass_begin(c);\n'),  # K5 begin
    ('unit_008F.cpp', 'L_0033EE54:\n', 'after', '    ::gt4::coregs3::car_pass_end(c);\n'),  # K5 end
    # K7 pass markers (format version 5). Pass ids follow PassId in native/coregs3/coregs3_scene.hpp.
    ('unit_008F.cpp', CALL_33E9F0, 'before', '    ::gt4::coregs3::pass_begin(c, 1u);\n'),  # env map begin
    ('unit_008F.cpp', 'L_0033E9F0:\n', 'after', '    ::gt4::coregs3::pass_end(c, 1u);\n'),
    ('unit_008F.cpp', CALL_33EC28, 'before', '    ::gt4::coregs3::pass_begin(c, 2u);\n'),  # sky begin
    ('unit_008F.cpp', 'L_0033EC28:\n', 'after', '    ::gt4::coregs3::pass_end(c, 2u);\n'),
    ('unit_008F.cpp', CALL_33EC58, 'before', '    ::gt4::coregs3::pass_begin(c, 3u);\n'),  # far underlay begin
    ('unit_008F.cpp', 'L_0033EC58:\n', 'after', '    ::gt4::coregs3::pass_end(c, 3u);\n'),
    ('unit_008F.cpp', CALL_33EC90, 'before', '    ::gt4::coregs3::pass_begin(c, 5u);\n'),  # course begin
    ('unit_008F.cpp', 'L_0033EC90:\n', 'after', '    ::gt4::coregs3::pass_end(c, 5u);\n'),
    ('unit_008F.cpp', CALL_33ECE0, 'before', '    ::gt4::coregs3::pass_begin(c, 7u);\n'),  # reflection begin
    ('unit_008F.cpp', 'L_0033ECE0:\n', 'after', '    ::gt4::coregs3::pass_end(c, 7u);\n'),
    ('unit_008F.cpp', CALL_33ED5C, 'before', '    ::gt4::coregs3::pass_begin(c, 11u);\n'),  # flares begin
    ('unit_008F.cpp', 'L_0033ED5C:\n', 'after', '    ::gt4::coregs3::pass_end(c, 11u);\n'),
    ('unit_008F.cpp', CALL_33EDA8, 'before', '    ::gt4::coregs3::pass_begin(c, 16u);\n'),  # concourse begin
    ('unit_008F.cpp', 'L_0033EDA8:\n', 'after', '    ::gt4::coregs3::pass_end(c, 16u);\n'),
    # render_after and mirror: the return labels are merge points reached without the call, so the end follows the call.
    ('unit_008F.cpp', CALL_33EEF8, 'before', '    ::gt4::coregs3::pass_begin(c, 9u);\n'),  # render_after begin
    ('unit_008F.cpp', CALL_33EEF8, 'after', '    ::gt4::coregs3::pass_end(c, 9u);\n'),
    ('unit_008F.cpp', CALL_33F0AC, 'before', '    ::gt4::coregs3::pass_begin(c, 10u);\n'),  # mirror begin
    ('unit_008F.cpp', CALL_33F0AC, 'after', '    ::gt4::coregs3::pass_end(c, 10u);\n'),
    ('unit_008F.cpp', CALL_33F140, 'before', '    ::gt4::coregs3::pass_begin(c, 12u);\n'),  # crowd/pitmen begin
    ('unit_008F.cpp', 'L_0033F140:\n', 'after', '    ::gt4::coregs3::pass_end(c, 12u);\n'),
    ('unit_008F.cpp', CALL_33F15C, 'before', '    ::gt4::coregs3::pass_begin(c, 13u);\n'),  # particles begin
    ('unit_008F.cpp', 'L_0033F15C:\n', 'after', '    ::gt4::coregs3::pass_end(c, 13u);\n'),
    ('unit_008F.cpp', CALL_33F184, 'before', '    ::gt4::coregs3::pass_begin(c, 14u);\n'),  # post begin
    ('unit_008F.cpp', 'L_0033F184:\n', 'after', '    ::gt4::coregs3::pass_end(c, 14u);\n'),
    ('unit_008F.cpp', CALL_33DA84, 'before', '    ::gt4::coregs3::pass_begin(c, 15u);\n'),  # panel begin
    ('unit_008F.cpp', 'L_0033DA84:\n', 'after', '    ::gt4::coregs3::pass_end(c, 15u);\n'),
]

INCLUDE_LINE = '#include "units.hpp"\n'


class UnitError(ValueError):
    pass


def _replace_once(text, before, after, what):
    if text.count(before) != 1:
        raise UnitError(f'{what}: anchor must appear exactly once (found {text.count(before)})')
    return text.replace(before, after)


def apply_insertions(name, text):
    """Returns the instrumented text of one unit. Raises UnitError when an anchor is missing or ambiguous."""
    if INCLUDE_LINE not in text:
        raise UnitError(f'{name}: not a translated unit (no units.hpp include)')
    instrumented = _replace_once(text, INCLUDE_LINE, INCLUDE_LINE + '\n' + DECLARATIONS[name], f'{name}: declarations')
    for entry in INSERTS:
        unit, anchor, placement, line = entry[:4]
        split = entry[4] if len(entry) > 4 else ''
        if unit != name:
            continue
        what = f'{name}: {anchor.strip()[:40]}'
        if placement == 'before':
            instrumented = _replace_once(instrumented, anchor, line + anchor, what)
        elif placement == 'after':
            instrumented = _replace_once(instrumented, anchor, anchor + line, what)
        elif placement == 'inside_after':
            if instrumented.count(anchor) != 1:
                raise UnitError(f'{what}: anchor must appear exactly once (found {instrumented.count(anchor)})')
            if anchor.count(split) != 1:
                raise UnitError(f'{what}: the argument assignment must appear exactly once')
            instrumented = instrumented.replace(anchor, anchor.replace(split, split + '\n' + line))
        else:
            raise UnitError(f'{what}: unknown placement {placement}')
    return instrumented


def remove_insertions(name, text):
    """Inverse of apply_insertions, used to prove the insertions are exactly what was added."""
    stripped = text.replace('\n' + DECLARATIONS[name], '', 1)
    for entry in INSERTS:
        unit, anchor, placement, line = entry[:4]
        split = entry[4] if len(entry) > 4 else ''
        if unit != name:
            continue
        if placement == 'before':
            stripped = _replace_once(stripped, line + anchor, anchor, f'{name}: removal')
        elif placement == 'after':
            stripped = _replace_once(stripped, anchor + line, anchor, f'{name}: removal')
        elif placement == 'inside_after':
            modified = anchor.replace(split, split + '\n' + line)
            stripped = _replace_once(stripped, modified, anchor, f'{name}: removal')
        else:
            raise UnitError(f'{name}: unknown placement {placement}')
    return stripped


def instrument(source, output):
    source = Path(source).absolute()
    output = Path(output).absolute()
    if not source.is_dir():
        raise UnitError(f'source units folder does not exist: {source}')
    if output.exists():
        raise UnitError(f'preserve existing output; choose a fresh folder: {output}')
    if source == output:
        raise UnitError('output must be a fresh folder, not the input')
    wanted = {unit for unit, *_ in INSERTS}
    missing = [unit for unit in wanted if not (source / unit).is_file()]
    if missing:
        raise UnitError(f'missing units: {", ".join(sorted(missing))}')

    output.mkdir(parents=True)
    receipt = {
        'task': 'TASK-090 CoreGS 3.0 phase 1',
        'schema_version': 1,
        'insertions': [],
        'files': {'copied': 0, 'instrumented': 0, 'total': 0},
    }
    total = 0
    for entry in sorted(source.iterdir()):
        if not entry.is_file():
            continue
        total += 1
        data = entry.read_bytes()
        text = data.decode('utf-8')
        if entry.name in wanted:
            instrumented = apply_insertions(entry.name, text)
            if remove_insertions(entry.name, instrumented) != text:
                raise UnitError(f'{entry.name}: removal does not restore the input byte for byte')
            (output / entry.name).write_text(instrumented, encoding='utf-8', newline='\n')
            receipt['files']['instrumented'] += 1
            for item in INSERTS:
                unit, anchor, placement, line = item[:4]
                split = item[4] if len(item) > 4 else ''
                if unit == entry.name:
                    receipt['insertions'].append({
                        'unit': unit,
                        'anchor': anchor,
                        'placement': placement,
                        'line': line.strip(),
                        **({'split': split} if split else {}),
                    })
        else:
            (output / entry.name).write_bytes(data)
            receipt['files']['copied'] += 1
    receipt['files']['total'] = total
    receipt['source_sha256'] = {
        name: hashlib.sha256((source / name).read_bytes()).hexdigest() for name in sorted(wanted)
    }
    receipt['output_sha256'] = {
        name: hashlib.sha256((output / name).read_bytes()).hexdigest() for name in sorted(wanted)
    }
    receipt_path = output.with_suffix('.coregs3-units.json')
    receipt_path.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8', newline='\n')
    return receipt


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('source', type=Path, help='translated units folder of the target (read only)')
    parser.add_argument('output', type=Path, help='fresh output folder')
    options = parser.parse_args(argv)
    try:
        receipt = instrument(options.source, options.output)
    except UnitError as error:
        print(f'coregs3-units: {error}', file=sys.stderr)
        return 1
    print(json.dumps(receipt['files'], indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
