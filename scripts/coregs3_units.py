"""TASK-090 CoreGS 3.0, phase 1: translated EE units with the observe-only capture hooks (native/coregs3/coregs3_hooks.hpp).

The hooks have to sit at exact guest addresses inside translated code, so they cannot be reached from a dispatch hook:

  * K1: `jal 0x0049D308` at 0x00455A5C, continuation 0x00455A64 (ModelSet2___render, the H1 draw frontier);
  * K3/K4: the return labels of the projection and modelview virtual calls of func_0033EAB8 (L_0033EBB4, L_0033EBF4);
  * K5: the block starts that bracket the main view's car pass (L_0033EE28, L_0033EE54).

This script copies a target's translated units into a fresh folder and inserts one host call at each of those points, plus
the declarations of the two hook namespaces after `#include "units.hpp"` (outside `namespace gen`). The inserted calls
only read guest memory and VU0 registers (native/coregs3/coregs3_hooks.cpp); they never write guest memory and never
touch `c.budget`, so the simulation is unchanged.

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
        'void before_shape_call(ps2::Cpu &c);\n'
        'void after_shape_call(ps2::Cpu &c);\n'
        '}\n'
    ),
    'unit_008F.cpp': (
        'namespace gt4::coregs3 {\n'
        'void projection_set(ps2::Cpu &c);\n'
        'void modelview_set(ps2::Cpu &c);\n'
        'void car_pass_begin(ps2::Cpu &c);\n'
        'void car_pass_end(ps2::Cpu &c);\n'
        '}\n'
    ),
}

# (unit, anchor, placement, inserted line). The anchors are the translated-unit text of the sites named above; the
# comment beside each one is the guest address it belongs to.
INSERTS = [
    ('unit_00D5.cpp',
     '    { c.r[31].sd[0] = static_cast<std::int32_t>(0x00455A64u);  if (depth < kMaxDepth) unit_00E7(c, 53u, depth + 1u); '
     'else { c.pc = 0x0049D308u; return; } if (c.pc != 0x00455A64u) [[unlikely]] return; }\n',
     'before', '    ::gt4::coregs3::before_shape_call(c);\n'),  # K1, 0x00455A5C
    ('unit_00D5.cpp', 'L_00455A64:\n', 'after', '    ::gt4::coregs3::after_shape_call(c);\n'),  # K1 continuation
    ('unit_008F.cpp', 'L_0033EBB4:\n', 'after', '    ::gt4::coregs3::projection_set(c);\n'),  # K3
    ('unit_008F.cpp', 'L_0033EBF4:\n', 'after', '    ::gt4::coregs3::modelview_set(c);\n'),  # K4
    ('unit_008F.cpp', 'L_0033EE28:\n', 'after', '    ::gt4::coregs3::car_pass_begin(c);\n'),  # K5 begin
    ('unit_008F.cpp', 'L_0033EE54:\n', 'after', '    ::gt4::coregs3::car_pass_end(c);\n'),  # K5 end
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
    for unit, anchor, placement, line in INSERTS:
        if unit != name:
            continue
        if placement == 'before':
            instrumented = _replace_once(instrumented, anchor, line + anchor, f'{name}: {anchor.strip()[:40]}')
        else:
            instrumented = _replace_once(instrumented, anchor, anchor + line, f'{name}: {anchor.strip()[:40]}')
    return instrumented


def remove_insertions(name, text):
    """Inverse of apply_insertions, used to prove the insertions are exactly what was added."""
    stripped = text.replace('\n' + DECLARATIONS[name], '', 1)
    for unit, anchor, placement, line in INSERTS:
        if unit != name:
            continue
        if placement == 'before':
            stripped = _replace_once(stripped, line + anchor, anchor, f'{name}: removal')
        else:
            stripped = _replace_once(stripped, anchor + line, anchor, f'{name}: removal')
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
            for unit, anchor, placement, line in INSERTS:
                if unit == entry.name:
                    receipt['insertions'].append({
                        'unit': unit,
                        'anchor': anchor,
                        'placement': placement,
                        'line': line.strip(),
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
