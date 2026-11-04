"""Assemble a distribution folder of the game executable (TASK-059).

The folder holds what an installed game needs and nothing else:
  gt4.exe                    the game executable (built as gt4vulkan.exe, renamed at distribution: TASK-108) (pinned in the runtime manifest, checked by size and SHA-256)
  shaders/*.spv              the nine compute shaders next to the executable (pinned, checked)
  <runtime DLLs>             the Visual C++ runtime the executable links to, copied from the local Visual Studio
                             redistributable folder (see --crt-dir)
  SDL3.dll                   SDL 3.4.18 (zlib), the gamepad layer (TASK-075), pinned in docs/inputs/slice210-sdl3.json
  licenses/LICENSE-SDL3.txt  the SDL licence (zlib)
  licenses/                  the repository MIT license, the donor MIT license and the third-party notes
  MANIFEST.json              every file with its size and SHA-256, the pins used and the excluded items

Not included, by decision: the gt4-gs GS package (GPL-3.0, downloaded by the launcher into <folder>\\gs, decision 0070)
and any game data (disc image, CORE.GT4, reconstructed ELF, memory cards, caches, captures).

The output must stay inside the assets checkout's private/ folder and must not exist yet (or be empty).

Usage:
  python scripts/make_distribution.py private/slice206-dist --assets-root C:/Antigravity/gt4-staticrecomp \
      --exe private/slice206-build/build/gt4vulkan.exe --runtime-manifest docs/inputs/slice206-pcsx2.json
"""

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADER_NAMES = ('upload', 'copy_read', 'copy_write', 'load_clut', 'display', 'raster', 'resolve', 'snapshot', 'decode')
# Visual C++ runtime DLLs of the clang-cl /MD build (checked against the executable's imports by --crt-dir).
CRT_DLLS = ('vcruntime140.dll', 'msvcp140.dll', 'msvcp140_atomic_wait.dll')  # dumpbin /dependents of gt4vulkan.exe (slice200)
EXCLUDED = (
    'gt4-gs package (GPL-3.0; downloaded by the launcher into <folder>\\gs, decision 0070)',
    'game data: disc image, CORE.GT4, reconstructed ELF, memory cards, caches and captures',
    'development build folders and private/ outputs',
)


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def find_crt_directory():
    """The newest Visual C++ x64 CRT redistributable folder of the local Visual Studio installation, or None."""
    vswhere = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe')
    if not vswhere.exists():
        return None
    result = subprocess.run([str(vswhere), '-latest', '-products', '*', '-property', 'installationPath'],
                            capture_output=True, text=True, check=False)
    install = result.stdout.strip()
    if not install:
        return None
    candidates = sorted(Path(install).glob(r'VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT'))
    return candidates[-1] if candidates else None


SDL3_PIN = 'docs/inputs/slice210-sdl3.json'
SDL3_FOLDER = 'private/tooling/sdl3-3.4.18'


def sdl3_files(assets_root):
    """The SDL3 runtime DLL and its licence, as pinned in docs/inputs/slice210-sdl3.json (checked by size and SHA-256)."""
    pin = json.loads((ROOT / SDL3_PIN).read_text(encoding='utf-8'))
    folder = assets_root / SDL3_FOLDER
    dll = pin['files']['runtime']
    licence = pin['files']['license']
    source_dll = folder / dll['path']
    source_licence = folder / licence['path']
    check_pin(source_dll, dll['size_bytes'], dll['sha256'], 'SDL3.dll')
    check_pin(source_licence, licence['size_bytes'], licence['sha256'], 'SDL3 licence')
    return source_dll, source_licence


def pinned_files(runtime_manifest):
    """Pinned executable and shader entries from the runtime manifest."""
    manifest = json.loads(Path(runtime_manifest).read_text(encoding='utf-8'))
    runtime = manifest['runtime']
    shaders = {}
    for item in manifest['runtime_dependencies']:
        name = Path(item['path']).name
        if item['path'].startswith(manifest['shader_capsule'] + '/') and name.endswith('.spv'):
            shaders[name[:-4]] = item
    return runtime, shaders


def check_pin(path, expected_size, expected_sha, label):
    size = path.stat().st_size
    actual = sha256_of(path)
    if size != expected_size or actual != expected_sha:
        raise ValueError(f'{label} differs from its pin: {size} bytes {actual[:12]}, pin {expected_size} {expected_sha[:12]}')
    return actual


def assemble(output, assets_root, exe, runtime_manifest, shaders_dir, crt_dir, allow_missing_crt,
             second_exe=None, second_runtime_manifest=None):
    assets_root = Path(assets_root).resolve()
    output = Path(output).resolve()
    private = (assets_root / 'private').resolve()
    if not output.is_relative_to(private) or output == private:
        raise ValueError('The distribution folder must be inside the assets checkout private/ folder')
    if output.exists() and any(output.iterdir()):
        raise ValueError(f'{output} exists and is not empty')
    exe = Path(exe).resolve()
    shaders_dir = Path(shaders_dir).resolve()
    runtime, pinned_shaders = pinned_files(runtime_manifest)
    if set(pinned_shaders) != set(SHADER_NAMES):
        raise ValueError('The runtime manifest does not pin exactly the nine compute shaders')

    output.mkdir(parents=True, exist_ok=True)
    manifest_files = []

    def record(relative, source):
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        manifest_files.append({'path': relative.replace('\\', '/'), 'bytes': target.stat().st_size,
                               'sha256': sha256_of(target)})

    exe_sha = check_pin(exe, runtime['size_bytes'], runtime['sha256'], 'gt4.exe')
    record('gt4.exe', exe)
    second_sha = None
    if second_exe is not None:
        # TASK-101 (release 1.3.0): the second game's executable, shipped beside the first (its own pin).
        if second_runtime_manifest is None:
            raise ValueError('--second-exe needs --second-runtime-manifest')
        second_manifest = json.loads(Path(second_runtime_manifest).read_text(encoding='utf-8'))
        second_runtime = second_manifest['runtime']
        second_path = Path(second_exe).resolve()
        second_sha = check_pin(second_path, second_runtime['size_bytes'], second_runtime['sha256'], 'gt4-spec2.exe')
        record('gt4-spec2.exe', second_path)

    for name in SHADER_NAMES:
        item = pinned_shaders[name]
        source = shaders_dir / f'{name}.spv'
        check_pin(source, item['size_bytes'], item['sha256'], f'shader {name}.spv')
        record(f'shaders/{name}.spv', source)

    sdl3_dll, sdl3_licence = sdl3_files(assets_root)
    record('SDL3.dll', sdl3_dll)
    record('licenses/LICENSE-SDL3.txt', sdl3_licence)

    if crt_dir is None:
        if not allow_missing_crt:
            raise ValueError('No Visual C++ CRT redistributable folder found; pass --crt-dir or --no-crt')
    else:
        crt_dir = Path(crt_dir)
        for name in CRT_DLLS:
            source = crt_dir / name
            if not source.exists():
                raise ValueError(f'the CRT folder lacks {name}')
            record(name, source)

    licenses = {
        'licenses/LICENSE-GT4Recomp.txt': ROOT / 'LICENSE',
        'licenses/LICENSE-ps2-web-recomp.txt': assets_root / 'private/references/ps2-web-recomp/LICENSE',
        'licenses/THIRD_PARTY.md': ROOT / 'native/THIRD_PARTY.md',
    }
    for relative, source in licenses.items():
        if not source.exists():
            raise ValueError(f'missing licence file {source}')
        record(relative, source)

    manifest = {
        'schema_version': 1,
        'runtime_manifest': runtime_manifest.name if hasattr(runtime_manifest, 'name') else str(runtime_manifest),
        'executable_sha256': exe_sha,
        'runtime_manifest_spec2': (Path(second_runtime_manifest).name if second_runtime_manifest else None),
        'executable_spec2_sha256': second_sha,
        'files': manifest_files,
        'excluded': list(EXCLUDED),
        'crt_directory': str(crt_dir) if crt_dir else None,
        'sdl3': {'version': '3.4.18', 'pin': SDL3_PIN, 'dll': 'SDL3.dll'},
    }
    (output / 'MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8', newline='\n')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('output', type=Path)
    parser.add_argument('--assets-root', type=Path, default=ROOT)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--runtime-manifest', type=Path, required=True)
    parser.add_argument('--second-exe', type=Path, help='TASK-101: the second game executable (built as gt4vulkan-spec2.exe; shipped as gt4-spec2.exe)')
    parser.add_argument('--second-runtime-manifest', type=Path, help='pin of the second executable (slice280-spec2.json)')
    parser.add_argument('--shaders', type=Path, help='folder with the pinned .spv files (default: the manifest shader capsule)')
    parser.add_argument('--crt-dir', type=Path, help='Visual C++ CRT redistributable folder (default: found by vswhere)')
    parser.add_argument('--no-crt', action='store_true', help='do not copy the Visual C++ runtime DLLs')
    options = parser.parse_args()
    assets_root = options.assets_root.resolve()
    shaders = options.shaders or (assets_root / json.loads(options.runtime_manifest.read_text(encoding='utf-8'))['shader_capsule'])
    crt = None if options.no_crt else (options.crt_dir or find_crt_directory())
    manifest = assemble(options.output, assets_root, options.exe, options.runtime_manifest, shaders, crt, options.no_crt,
                        second_exe=options.second_exe, second_runtime_manifest=options.second_runtime_manifest)
    print(f'distribution written: {options.output.resolve()} ({len(manifest["files"])} files)')
    print(f'gt4.exe sha256 {manifest["executable_sha256"]}')
    if manifest["executable_spec2_sha256"]:
        print(f'gt4-spec2.exe sha256 {manifest["executable_spec2_sha256"]}')


if __name__ == '__main__':
    main()
