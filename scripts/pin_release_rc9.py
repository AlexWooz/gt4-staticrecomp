"""Pin the 1.3.0-rc9 game builds (TASK-134): docs/inputs/slice<N>-pcsx2.json and slice<N>-spec2.json.

Each manifest starts from the rc8 pin of its target (slice382) and replaces what rc9 changes: the executable and the EE library, now
rebuilt from the same translated units with this executable's compiler, SSE4.1 and a profile of the whole game; on Spec II also its own
VU0 programs. The inputs named by the base pin stay verified by the Python tests against the files on disk.

The output file must not exist: a new slice always gets a new pin file (docs/inputs are history, never edited).

usage: pin_release_rc9.py <slice> --target gt4|spec2 --exe <path under assets> --library <path under assets> [--assets-root <folder>]
       [--vu0-dir <path under assets> --vu0-image <path under assets>] [--date 2026-11-02]
"""

import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def identity(path):
    data = Path(path).read_bytes()
    return len(data), hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('slice', type=int)
    parser.add_argument('--target', choices=('gt4', 'spec2'), required=True)
    parser.add_argument('--exe', required=True)
    parser.add_argument('--library', required=True)
    parser.add_argument('--assets-root', type=Path, default=Path('C:/Antigravity/gt4-staticrecomp'))
    parser.add_argument('--vu0-dir')
    parser.add_argument('--vu0-image')
    parser.add_argument('--date', default='2026-11-02')
    options = parser.parse_args()

    name = 'pcsx2' if options.target == 'gt4' else 'spec2'
    output = ROOT / f'docs/inputs/slice{options.slice}-{name}.json'
    if output.exists():
        raise SystemExit(f'{output} exists: a new slice gets a new pin file')
    pin = json.loads((ROOT / f'docs/inputs/slice382-{name}.json').read_text(encoding='utf-8'))

    size, sha = identity(options.assets_root / options.exe)
    pin['runtime'] = {'path': options.exe, 'size_bytes': size, 'sha256': sha}
    size, sha = identity(options.assets_root / options.library)
    library = {'path': options.library, 'size_bytes': size, 'sha256': sha}
    pin['frozen_game_library'] = library
    base_units = 'slice217 aspect units' if options.target == 'gt4' else 'the units pinned by docs/inputs/spec2-ee-generated.json'
    pin['ee_library'] = dict(library, profile='rebuilt',
                             generated_units=base_units,
                             recipe='scripts/coregs2_bench/pgo-all.sh: clang-cl 22.1.3, SSE4.1, the derived headers of '
                                    'scripts/prepare_fast_headers.py, and a profile of this target')
    if options.vu0_dir:
        vu0 = dict(pin['vu0_aot'])
        vu0['generated_dir'] = options.vu0_dir
        vu0['image'] = options.vu0_image
        vu0['image_sha256'] = identity(options.assets_root / options.vu0_image)[1]
        vu0['table_sha256'] = identity(options.assets_root / options.vu0_dir / 'vu0_generated.cpp')[1]
        vu0['manifest_sha256'] = identity(options.assets_root / options.vu0_dir / 'manifest.json')[1]
        vu0['entry_policy'] = 'vcallms: 14 entries of the Spec II EE code (docs/BUILD.md, TASK-134)'
        vu0.pop('entry_list_sha256', None)
        pin['vu0_aot'] = vu0
    pin['slice'] = options.slice
    pin['date'] = options.date
    pin['branch'] = 'task/134-game-code-optimization-study'
    pin['baseline_commit'] = f'slice382-{name} (release 1.3.0-rc8)'
    pin['classification'] = (f'1.3.0-rc9 {"v2.00" if options.target == "gt4" else "Spec II"} candidate: the CPU work of TASK-134 on top of '
                             'rc8, with PGO and ThinLTO of the whole game. Game state, clock and frames identical to rc8.')
    pin['purpose'] = 'TASK-134: rc9, the CPU optimisation.'
    pin['status'] = "test build on the owner's desktop and notebook; not uploaded"
    output.write_text(json.dumps(pin, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(f'{output.name}: exe {pin["runtime"]["sha256"][:16]} library {library["sha256"][:16]}')


if __name__ == '__main__':
    main()
