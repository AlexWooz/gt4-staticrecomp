r"""Make the portable zip of a GT4Recomp release (TASK-098): GT4Recomp-<version>-portable.zip.

The zip holds the same program files as the installer (installer/gt4recomp.nsi stages exactly these):
  GT4Recomp.exe     the portable build of the launcher (cargo feature `portable`, launcher/tools/build-release.ps1 -Portable)
  SDL3.dll          SDL 3.4.18 beside the launcher
  NOTICE.md, LICENSE.txt
  game/             the game folder the installer ships (GAME_DIR: gt4vulkan.exe, gt4vulkan-spec2.exe, DLLs, shaders, licences)
plus two empty folders and a short note:
  user/             settings, controls, saves, replays, backups of the player (the installer build keeps these in %APPDATA%)
  data/             the disc copy, prepared files, gt4-gs, caches, logs (the installer build keeps these in <install>\data)
  PORTABLE.txt      how to use the folder

The zip root is the folder content (no wrapping folder), so "Extract all" into a new folder gives a ready copy and the
launcher's own update (portable_update.rs) swaps the same names. No game data goes in: a disc image, CORE.GT4, a rebuilt ELF
or a memory card in the game folder fails the build. The output is deterministic (sorted names, fixed times).

With --sums-file the zip's line, `<sha256>  <name>`, is appended to that SHA256SUMS file (created when missing), so the release
lists the portable zip next to the setup program.

--version carries the label of a test build: --version 1.3.0-rc5 makes GT4Recomp-1.3.0-rc5-portable.zip.

Usage:
  python scripts/make_portable_zip.py <out dir> --version 1.3.0 --launcher-exe <GT4Recomp.exe> --sdl3-dll <SDL3.dll> \
      --game-dir <game folder> [--sums-file <out dir>/SHA256SUMS]
"""

import argparse
import hashlib
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EMPTY_FOLDERS = ('user', 'data')
# Names that must never be in the game folder: the player's disc and the files made from it.
FORBIDDEN_SUFFIXES = ('.iso', '.bin', '.elf', '.ps2', '.mc2', '.gt4')
FORBIDDEN_NAMES = ('core.gt4', 'scus_973.28', 'scus_973.28.elf', 'card.ps2')
# The release version: numeric, with the label of a test build (TASK-109): 1.3.0 or 1.3.0-rc5. The name matches the asset the
# launcher looks for (releases.rs package_name_for).
VERSION_PATTERN = re.compile(r'^\d+\.\d+\.\d+(-rc[1-9][0-9]*)?$')
FIXED_TIME = (2026, 1, 1, 0, 0, 0)
PORTABLE_NOTE = """GT4Recomp, portable copy
========================

This folder is the whole program. Start GT4Recomp.exe. Carry the folder to another PC or drive and it works there.

  user\\   your settings, controls, saves and replays
  data\\   the copy of your disc, the prepared game files, the renderer, logs
  game\\   the game programs (replaced by updates)

The folder must be somewhere you can write (not Program Files). GT4Recomp updates itself in place: it never touches user\\
or data\\. WebView2 (Microsoft Edge runtime) must be installed on the PC; Windows 11 has it.
"""


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def check_game_dir(game_dir):
    """Refuses a game folder with game data in it; returns the files (relative paths, sorted)."""
    files = sorted(path for path in game_dir.rglob('*') if path.is_file())
    if not files:
        raise ValueError(f'{game_dir} has no files')
    for path in files:
        name = path.name.lower()
        if name in FORBIDDEN_NAMES or name.endswith(FORBIDDEN_SUFFIXES):
            raise ValueError(f'{path} looks like game data; the portable zip carries only program files')
    return [path.relative_to(game_dir) for path in files]


def add_file(archive, source, name):
    info = zipfile.ZipInfo(name, FIXED_TIME)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o100644 << 16
    archive.writestr(info, Path(source).read_bytes())


def add_folder(archive, name):
    info = zipfile.ZipInfo(name.rstrip('/') + '/', FIXED_TIME)
    info.external_attr = (0o40755 << 16) | 0x10
    archive.writestr(info, b'')


def make_zip(out_dir, version, launcher_exe, sdl3_dll, game_dir, notice=None, licence=None, sums_file=None):
    if not VERSION_PATTERN.match(str(version)):
        raise ValueError(f'version must look like 1.3.0 or 1.3.0-rc5, got {version!r}')
    out_dir = Path(out_dir).resolve()
    launcher_exe, sdl3_dll, game_dir = Path(launcher_exe), Path(sdl3_dll), Path(game_dir).resolve()
    notice = Path(notice) if notice else ROOT / 'NOTICE.md'
    licence = Path(licence) if licence else ROOT / 'LICENSE'
    for label, path in (('launcher', launcher_exe), ('SDL3.dll', sdl3_dll), ('NOTICE.md', notice), ('LICENSE', licence)):
        if not path.is_file():
            raise ValueError(f'{label} not found: {path}')
    if not game_dir.is_dir():
        raise ValueError(f'game folder not found: {game_dir}')
    if out_dir == game_dir or game_dir in out_dir.parents:
        raise ValueError('the output folder must not be inside the game folder')
    game_files = check_game_dir(game_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    name = f'GT4Recomp-{version}-portable.zip'
    target = out_dir / name
    if target.exists():
        raise ValueError(f'{target} exists; choose a fresh output folder')
    with zipfile.ZipFile(target, 'w') as archive:
        add_file(archive, launcher_exe, 'GT4Recomp.exe')
        add_file(archive, sdl3_dll, 'SDL3.dll')
        add_file(archive, notice, 'NOTICE.md')
        add_file(archive, licence, 'LICENSE.txt')
        info = zipfile.ZipInfo('PORTABLE.txt', FIXED_TIME)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o100644 << 16
        archive.writestr(info, PORTABLE_NOTE.replace('\n', '\r\n'))
        for relative in game_files:
            add_file(archive, game_dir / relative, 'game/' + relative.as_posix())
        for folder in EMPTY_FOLDERS:
            add_folder(archive, folder)
    digest = sha256_of(target)
    if sums_file:
        sums_file = Path(sums_file)
        existing = sums_file.read_text(encoding='utf-8') if sums_file.exists() else ''
        kept = [line for line in existing.splitlines() if line.strip() and line.split()[-1] != name]
        kept.append(f'{digest}  {name}')
        sums_file.write_text('\n'.join(kept) + '\n', encoding='utf-8', newline='\n')
    return target, digest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('out_dir', type=Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--launcher-exe', type=Path, required=True)
    parser.add_argument('--sdl3-dll', type=Path, required=True)
    parser.add_argument('--game-dir', type=Path, required=True)
    parser.add_argument('--notice', type=Path)
    parser.add_argument('--license', type=Path, dest='licence')
    parser.add_argument('--sums-file', type=Path)
    options = parser.parse_args(argv)
    try:
        target, digest = make_zip(options.out_dir, options.version, options.launcher_exe, options.sdl3_dll, options.game_dir,
                                  options.notice, options.licence, options.sums_file)
    except ValueError as error:
        print(f'error: {error}', file=sys.stderr)
        return 1
    print(f'{target} ({target.stat().st_size} bytes)')
    print(f'sha256 {digest}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
