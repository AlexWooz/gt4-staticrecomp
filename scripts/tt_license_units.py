#!/usr/bin/env python
"""Tourist Trophy: translated EE units with the exact license demo decrypt call (native/tt_license_decrypt_hook.cpp).

TT's license cipher (0x004A4228, v2.00 func_00579E00) is reached by a tail jump from its wrapper (0x0042F7B0), so no dispatch
hook sees it. Like scripts/camera360_units.py, this copies the translated units into a fresh folder and adds, at the
function's entry label, one host call that may decrypt the demo exactly and return:

    L_004A4228:
        if (::gt4::tt_license::before_license_decrypt(c)) return;

plus a declaration after `#include "units.hpp"`. The host function sets v0 and c.pc (= ra) itself when it returns true,
before the function's prologue, so the translated return protocol is kept.

It refuses a unit without the label or with it more than once, an output folder that exists, and any result from which
removing the insertions does not give the input back byte for byte. Every other file is copied unchanged. The generated
units are game-derived: they stay under private/.

usage: tt_license_units.py <translated TT units folder> <fresh output folder>
"""
import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path

ENTRY = 0x004A4228
LABEL = f'L_{ENTRY:08X}:\n'
CALL = '    if (::gt4::tt_license::before_license_decrypt(c)) return;  // tt_license_units.py\n'
INCLUDE = '#include "units.hpp"\n'
DECLARATION = ('\n// Tourist Trophy: exact license demo decrypt (native/tt_license_decrypt_hook.cpp), added by '
               'scripts/tt_license_units.py.\n'
               'namespace gt4::tt_license { bool before_license_decrypt(ps2::Cpu &c); }\n')


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def patch(text):
    if text.count(LABEL) != 1 or text.count(INCLUDE) != 1:
        raise ValueError('expected one entry label and one units.hpp include')
    out = text.replace(INCLUDE, INCLUDE + DECLARATION, 1).replace(LABEL, LABEL + CALL, 1)
    if out.replace(DECLARATION, '', 1).replace(CALL, '', 1) != text:
        raise ValueError('removing the insertions does not restore the unit')
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('units', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args(argv)
    if args.output.exists():
        raise SystemExit('output folder exists; choose a fresh one')
    hosts = [path for path in sorted(args.units.glob('unit_*.cpp')) if LABEL in path.read_text(encoding='utf-8')]
    if len(hosts) != 1:
        raise SystemExit(f'expected the entry label in exactly one unit, found {len(hosts)}')
    args.output.mkdir(parents=True)
    receipt = {'schema_version': 1, 'entry': f'0x{ENTRY:08X}', 'copied': {}, 'patched': {}}
    for path in sorted(args.units.iterdir()):
        if not path.is_file():
            raise SystemExit(f'unexpected entry {path.name}')
        data = path.read_bytes()
        if path == hosts[0]:
            text = patch(data.decode('utf-8'))
            out = text.encode('utf-8')
            (args.output / path.name).write_bytes(out)
            receipt['patched'][path.name] = {'input_sha256': sha256(data), 'output_sha256': sha256(out)}
        else:
            shutil.copyfile(path, args.output / path.name)
            if sha256((args.output / path.name).read_bytes()) != sha256(data):
                raise SystemExit(f'copy differs: {path.name}')
            receipt['copied'][path.name] = sha256(data)
    receipt_path = args.output.parent / (args.output.name + '.tt-license-units.json')
    receipt_path.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(f'tt-license-units: patched {hosts[0].name}, copied {len(receipt["copied"])} files into {args.output}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
