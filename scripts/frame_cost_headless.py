"""Headless frame-cost runs for TASK-084 (docs/FRAME-COST-STUDY.md sections 1 and 8).

Runs one pinned game build with no window, on a seed-card copy, and records everything a comparison needs:

  * conditions (build sha256, gt4-gs sha256, environment, CPU load before/after, builder check);
  * stderr (the F8-equivalent `GT4_MTVU_REPORT_EVERY=N` reports, `[mtvu]`, `[gs-ipc]` and `[frame]` lines);
  * the frame dumps of the schedule, hashed (`dumps.sha256`) and optionally removed to save disk
    (--keep-dumps keeps them for a visual check).

Scenes and their press schedules are the study's: menus (12 presses, 4,200 VBlanks), day race (Arcade, Single
Race, El Capitan, Ford Ka, 5 AI, 16,000 VBlanks), night race (Clubman Stage Route 5, 16,000 VBlanks) and showroom
(the day-race schedule cut at 7,000 VBlanks, which ends in the car showroom).

The script never touches an owner card: it copies one of the pinned seeds and works inside the run directory.
Nothing is written outside --out.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SCENES = {
    'menus': {
        'frames': 4200,
        'card': 'private/slice113-video/widescreen-seed/card.ps2',
        'presses': [f'{frame}:{button}:30' for frame, button in
                    ((600, 'start'), (900, 'cross'), (1200, 'start'), (1500, 'down'), (1800, 'cross'),
                     (2100, 'start'), (2400, 'down'), (2700, 'cross'), (3000, 'start'), (3300, 'down'),
                     (3600, 'cross'), (3900, 'start'))],
    },
    'day': {
        'frames': 16000,
        'card': 'private/slice110-menu-pilot/name-control4/card.ps2',
        'presses': ['600:start:30', '900:cross:30', '1500:cross:30', '2300:right:10', '2450:cross:10',
                    '3500:cross:10', '3700:cross:10', '6100:down:8', '6250:cross:10', '7000:cross:10',
                    '7200:cross:10', '7400:cross:10', '8300:cross:10', '8800:cross:7000'],
    },
    'night': {
        'frames': 16000,
        'card': 'private/slice110-menu-pilot/name-control4/card.ps2',
        'presses': ['600:start:30', '900:cross:30', '1500:cross:30', '2300:down:10', '2450:cross:10',
                    '3500:right:10', '3700:right:10', '3900:cross:10', '4100:cross:10', '6500:down:8',
                    '6650:cross:10', '7400:cross:10', '7600:cross:10', '7800:cross:10', '8700:cross:10',
                    '9200:cross:7000'],
    },
    'showroom': {
        'frames': 7000,
        'card': 'private/slice110-menu-pilot/name-control4/card.ps2',
        'presses': ['600:start:30', '900:cross:30', '1500:cross:30', '2300:right:10', '2450:cross:10',
                    '3500:cross:10', '3700:cross:10', '6100:down:8', '6250:cross:10', '7000:cross:10',
                    '7200:cross:10', '7400:cross:10', '8300:cross:10', '8800:cross:7000'],
    },
}

# The gt4-gs.ini the probe writes for performance runs (TASK-036 minimal settings, 1x), plus surfaceless dumping.
# HWDownloadMode (TASK-116 step 0): 0 = Accurate (GSHardwareDownloadMode Enabled), 2 = NoReadbacks, the launcher's
# Performance mode (docs/launcher/gs-settings.toml, performance_mode).
GS_INI = """; gt4-gs.ini written by scripts/frame_cost_headless.py (the probe's TASK-036 settings, 1x).
[EmuCore/GS]
Renderer = 14
upscale_multiplier = {scale}
accurate_blending_unit = 3
deinterlace_mode = 0
AspectRatio = Auto 4:3/3:2
VsyncEnable = true
DisableMailboxPresentation = true
HWDownloadMode = {readback}

[Folders]
Cache = {cache}

[GT4GS]
DataRoot = {data}
ProhibitDynamicCode = false
Surfaceless = true
{dump_lines}"""


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


# TASK-116 step 0: --readback to the HWDownloadMode value of the generated gt4-gs.ini (docs/launcher/gs-settings.toml).
READBACK_MODES = {'accurate': 0, 'no-readbacks': 2}


def builders_running():
    try:
        output = subprocess.run(['tasklist'], capture_output=True, text=True, timeout=30).stdout.lower()
    except (OSError, subprocess.SubprocessError):
        return []
    names = ('ninja.exe', 'clang-cl.exe', 'cl.exe', 'link.exe', 'lld-link.exe', 'cargo.exe', 'rustc.exe',
             'msbuild.exe', 'makensis.exe', 'gt4vulkan.exe', 'gt4-gs.exe')
    return [name for name in names if name in output]


def with_textures(ini_text, textures):
    """Adds the launcher's custom-texture keys to a gt4-gs.ini: [Folders] Textures and LoadTextureReplacements.

    TASK-100 S7c: gt4-gs 1.2.1 reads the folder recursively, but PCSX2's loader switch defaults to off, so the launcher
    always writes it (docs/launcher/gs-settings.toml, texture_load). Both keys are only written when a pack folder is
    named, so every existing run stays byte-identical.
    """
    if textures is None:
        return ini_text
    folders = '[Folders]\n'
    section = '[EmuCore/GS]\n'
    if ini_text.count(folders) != 1 or ini_text.count(section) != 1:
        raise ValueError('the gt4-gs.ini template needs one [EmuCore/GS] and one [Folders] section')
    text = ini_text.replace(folders, folders + f'Textures = {textures}\n')
    return text.replace(section, section + 'LoadTextureReplacements = true\n')


def game_arguments(exe, elf_copy, iso, card, frames, presses, donor_frames_dir=None, frame_every=0):
    """The gt4vulkan command line for one headless run (pure, so the harness contract is testable)."""
    card = Path(card)
    arguments = [str(exe), str(elf_copy), '--ntsc', '--disc', str(iso), '--frames', str(frames),
                 '--memcard', str(card), '--misses', str(card.parent / 'misses.txt')]
    if donor_frames_dir is not None and frame_every:
        # The donor renderer dumps from the game process; in pcsx2 mode the GS process dumps instead (DumpEvery).
        arguments += ['--frames-dir', str(donor_frames_dir), '--frame-every', str(frame_every)]
    for press in presses:
        arguments += ['--press', press]
    return arguments


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', required=True, help='build directory that holds gt4vulkan.exe (or gt4vulkan-spec2.exe)')
    parser.add_argument('--exe', help='executable name inside --build (default gt4vulkan.exe; Spec II uses gt4vulkan-spec2.exe)')
    parser.add_argument('--scene', required=True, choices=sorted(SCENES))
    parser.add_argument('--out', required=True, help='fresh run directory (created)')
    parser.add_argument('--assets-root', default=str(ROOT))
    parser.add_argument('--card', help='memory-card image to copy into the run (default: the scene seed card)')
    parser.add_argument('--frames', type=int, help='override the scene frame count')
    parser.add_argument('--frame-every', type=int, default=100, help='frame dump interval (0 = no dumps)')
    parser.add_argument('--renderer', choices=('pcsx2', 'donor'), default='pcsx2')
    parser.add_argument('--scale', type=int, default=1, help='gt4-gs internal resolution (1 = the owner 480p look)')
    parser.add_argument('--readback', choices=('accurate', 'no-readbacks'), default='accurate',
                        help='gt4-gs HWDownloadMode: accurate (0, the default) or no-readbacks (2, Performance mode)')
    parser.add_argument('--gs-exe', help='gt4-gs.exe used for pcsx2 runs')
    parser.add_argument('--gs-sha256', help='expected gt4-gs.exe SHA-256, forwarded as GT4_GS_PROCESS_SHA256')
    parser.add_argument('--elf', help='game ELF (default the pinned v2.00 reconstructed ELF)')
    parser.add_argument('--iso', help='disc image (default the v2.00 ISO)')
    parser.add_argument('--textures', help='custom texture pack folder for gt4-gs [Folders] Textures (pcsx2 only)')
    parser.add_argument('--press', action='append', default=[], metavar='FRAME:BUTTON:FRAMES',
                        help='override the scene press schedule (repeatable; TASK-100 S7c uses the Spec II boot order)')
    parser.add_argument('--env', action='append', default=[], metavar='KEY=VALUE',
                        help='extra environment (repeatable); GT4_ switches for A/B')
    parser.add_argument('--keep-dumps', action='store_true', help='keep the frame dumps after hashing')
    parser.add_argument('--allow-builders', action='store_true', help='run even while a build tool is active')
    options = parser.parse_args()

    assets = Path(options.assets_root)
    build = (assets / options.build).resolve() if not Path(options.build).is_absolute() else Path(options.build)
    exe = build / (options.exe or 'gt4vulkan.exe')
    if not exe.exists():
        raise SystemExit(f'no executable at {exe}')
    run = Path(options.out).resolve()
    if run.exists() and any(run.iterdir()):
        raise SystemExit(f'refusing to reuse {run}')
    scene = SCENES[options.scene]
    frames = options.frames or scene['frames']
    iso = Path(options.iso) if options.iso else assets / 'Gran Turismo 4 (USA) (v2.00).iso'
    elf = Path(options.elf) if options.elf else assets / 'private/reconstructed/SCUS_973.28.elf'
    card = Path(options.card) if options.card else assets / scene['card']
    for path in (iso, elf, card):
        if not path.exists():
            raise SystemExit(f'missing input: {path}')
    textures = Path(options.textures).resolve() if options.textures else None
    if textures is not None and not textures.is_dir():
        raise SystemExit(f'missing texture folder: {textures}')

    running = builders_running()
    if running and not options.allow_builders:
        raise SystemExit(f'build tools or the game are running ({", ".join(running)}); measurements would be noisy')

    run.mkdir(parents=True, exist_ok=True)
    shutil.copy2(elf, run / 'SCUS_973.28')
    shutil.copy2(card, run / 'card.ps2')

    environment = {key: value for key, value in os.environ.items()
                   if not (key.startswith('PS2_') or key.startswith('GT4_') or key.startswith('GT4GS'))}
    environment.update({'GT4_PERF': '1', 'PS2_SCALE': '1', 'PS2_GS': 'gpu', 'GT4_FORCE_WIDESCREEN': '1',
                        'GT4_MTVU_REPORT_EVERY': '500'})
    if options.renderer == 'pcsx2':
        gs_exe = Path(options.gs_exe or assets / 'private/slice203-gt4gs-release-pkg/gt4-gs.exe')
        if not gs_exe.exists():
            raise SystemExit(f'missing gt4-gs: {gs_exe}')
        dump_lines = ''
        if options.frame_every:
            # gt4-gs treats DumpEvery = 0 as "every frame", so the keys are omitted when no dumps are wanted.
            dump_lines = f'DumpDir = {run / "gs-frames"}\nDumpEvery = {options.frame_every}\n'
        ini = with_textures(GS_INI.format(scale=options.scale, readback=READBACK_MODES[options.readback],
                                          cache=assets / 'private/slice193-gt4gs-cache',
                                          data=assets / 'private/slice193-gt4gs-data', dump_lines=dump_lines),
                            str(textures) if textures else None)
        (run / 'gt4-gs.ini').write_text(ini, encoding='utf-8')
        environment.update({'GT4_RENDERER': 'pcsx2', 'GT4_GS_PROCESS': str(gs_exe),
                            'GT4_GS_CONFIG': str(run / 'gt4-gs.ini')})
        if options.gs_sha256:
            environment['GT4_GS_PROCESS_SHA256'] = options.gs_sha256
    elif options.textures:
        raise SystemExit('--textures needs --renderer pcsx2 (the donor renderer has no texture packs)')
    for assignment in options.env:
        key, _, value = assignment.partition('=')
        if not key or not _:
            raise SystemExit(f'--env wants KEY=VALUE, got {assignment!r}')
        environment[key] = value

    arguments = game_arguments(exe, run / 'SCUS_973.28', iso, run / 'card.ps2', frames,
                               options.press or scene['presses'],
                               donor_frames_dir=run / 'frames' if options.renderer == 'donor' else None,
                               frame_every=options.frame_every)
    if options.frame_every and options.renderer == 'donor':
        (run / 'frames').mkdir(exist_ok=True)

    conditions = {
        'scene': options.scene,
        'frames': frames,
        'frame_every': options.frame_every,
        'renderer': options.renderer,
        'scale': options.scale,
        'readback': options.readback,
        'build': str(exe),
        'build_sha256': sha256(exe),
        'elf': str(elf),
        'elf_sha256': sha256(elf),
        'iso': str(iso),
        'card_sha256': sha256(card),
        'gs_exe': environment.get('GT4_GS_PROCESS'),
        'gs_exe_sha256': sha256(environment['GT4_GS_PROCESS']) if 'GT4_GS_PROCESS' in environment else None,
        'gs_process_sha256_expected': options.gs_sha256,
        'textures': str(textures) if textures else None,
        'presses': options.press or scene['presses'],
        'gt4_environment': {key: value for key, value in environment.items() if key.startswith(('GT4_', 'PS2_'))},
        'builders_before': running,
    }
    started = time.time()
    with open(run / 'stderr.log', 'wb') as stderr, open(run / 'stdout.log', 'wb') as stdout:
        result = subprocess.run(arguments, cwd=run, env=environment, stdout=stdout, stderr=stderr)
    conditions['seconds'] = round(time.time() - started, 2)
    conditions['exit_code'] = result.returncode
    conditions['builders_after'] = builders_running()

    dump_folder = run / 'frames' if options.renderer == 'donor' else run / 'gs-frames'
    dumps = sorted(dump_folder.glob('*.ppm' if options.renderer == 'donor' else '*.png')) if options.frame_every else []
    with open(run / 'dumps.sha256', 'w', encoding='utf-8') as listing:
        for dump in dumps:
            listing.write(f'{sha256(dump)}  {dump.name}\n')
    conditions['dumps'] = len(dumps)
    if dumps and not options.keep_dumps:
        shutil.rmtree(dump_folder)
    (run / 'conditions.json').write_text(json.dumps(conditions, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps({'run': str(run), 'exit': result.returncode, 'seconds': conditions['seconds'],
                      'dumps': len(dumps)}, indent=2))
    return 0 if result.returncode == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
