"""Distribution folder assembly (TASK-059, scripts/make_distribution.py).

Checks the release 1.3.0 pins directly: docs/inputs/slice279-pcsx2.json (gt4vulkan.exe) and
docs/inputs/slice280-spec2.json (gt4vulkan-spec2.exe), with GT4_ASSETS_ROOT (the checkout that holds the pinned
executables, shaders and the donor licence; default: the main checkout). Skipped when the release slices are absent.
The distribution is written under the assets checkout's private/ folder, as the script requires, and removed
afterwards.
"""

import importlib.util
import json
import os
import shutil
import sys
import tempfile
import unittest
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASSETS = Path(os.environ.get('GT4_ASSETS_ROOT', 'C:/Antigravity/gt4-staticrecomp')).resolve()
sys.path.insert(0, str(ROOT / 'scripts'))
import external_runtime_probe as probe  # noqa: E402
# The distribution test pins the release 1.3.0 executables (v2.00 and Spec II).
PIN = ROOT / 'docs/inputs' / probe.PERF_PROFILES['pcsx2'][0]
PIN_SPEC2 = ROOT / 'docs/inputs' / probe.SPEC2_RELEASE[0]

spec = importlib.util.spec_from_file_location('make_distribution', ROOT / 'scripts/make_distribution.py')
dist = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dist)


def pinned_exe(pin):
    manifest = json.loads(pin.read_text(encoding='utf-8'))
    path = ASSETS / manifest['runtime']['path']
    return path if path.exists() else None


def executable():
    return pinned_exe(PIN)


def spec2_executable():
    return pinned_exe(PIN_SPEC2)


@unittest.skipUnless(executable() and spec2_executable() and (ASSETS / 'private/references/ps2-web-recomp/LICENSE').exists(),
                     'the release 1.3.0 slices or the donor licence are not available')
class MakeDistributionTest(unittest.TestCase):
    def setUp(self):
        self.output = ASSETS / 'private' / f'slice218-dist-test-{uuid.uuid4().hex[:8]}'
        self.scratch = Path(tempfile.mkdtemp(prefix='gt4-dist-test-'))

    def tearDown(self):
        shutil.rmtree(self.output, ignore_errors=True)
        shutil.rmtree(self.scratch, ignore_errors=True)

    def assemble(self, exe, output=None):
        return dist.assemble(output or self.output, ASSETS, exe, PIN, ASSETS / 'private/slice153-pacing-shaders',
                             None, allow_missing_crt=True, second_exe=spec2_executable(),
                             second_runtime_manifest=PIN_SPEC2)

    def test_folder_holds_the_executable_shaders_and_licences_only(self):
        manifest = self.assemble(executable())
        names = sorted(item['path'] for item in manifest['files'])
        self.assertIn('gt4.exe', names)
        # TASK-101 (1.3.0): both game executables ship in the installed folder.
        self.assertIn('gt4-spec2.exe', names)
        self.assertEqual(manifest['executable_spec2_sha256'],
                         json.loads(PIN_SPEC2.read_text(encoding='utf-8'))['runtime']['sha256'])
        for shader in dist.SHADER_NAMES:
            self.assertIn(f'shaders/{shader}.spv', names)
        self.assertIn('licenses/LICENSE-GT4Recomp.txt', names)
        self.assertIn('licenses/LICENSE-ps2-web-recomp.txt', names)
        self.assertIn('licenses/THIRD_PARTY.md', names)
        # TASK-075: SDL3.dll and its licence (zlib) ship next to the executable.
        self.assertIn('SDL3.dll', names)
        self.assertIn('licenses/LICENSE-SDL3.txt', names)
        self.assertTrue((self.output / 'MANIFEST.json').exists())
        # No gt4-gs package, no game data and no memory card in the folder.
        self.assertFalse((self.output / 'gs').exists())
        for path in self.output.rglob('*'):
            self.assertNotIn(path.suffix.lower(), ('.iso', '.elf', '.ps2', '.lib', '.gt4', '.gscap', '.bin'), path)
        recorded = json.loads((self.output / 'MANIFEST.json').read_text(encoding='utf-8'))
        self.assertEqual(recorded['executable_sha256'], manifest['executable_sha256'])
        for item in recorded['files']:
            self.assertEqual(dist.sha256_of(self.output / item['path']), item['sha256'], item['path'])

    def test_a_changed_executable_fails_its_pin(self):
        changed = self.scratch / 'gt4.exe'
        data = bytearray(executable().read_bytes())
        data[-1] ^= 0xFF
        changed.write_bytes(bytes(data))
        with self.assertRaises(ValueError):
            self.assemble(changed)
        self.assertFalse(self.output.exists() and any(self.output.iterdir()))

    def test_output_outside_private_is_refused(self):
        with self.assertRaises(ValueError):
            self.assemble(executable(), output=self.scratch / 'dist')


class ReleasePinTests(unittest.TestCase):
    """TASK-100/TASK-101: the release 1.3.0 pins describe the files on disk."""

    def test_release_pins_match_their_files_when_present(self):
        for pin_path in (PIN, PIN_SPEC2):
            if not pin_path.exists():
                self.skipTest(f'{pin_path.name} is not present')
            manifest = json.loads(pin_path.read_text(encoding='utf-8'))
            exe = ASSETS / manifest['runtime']['path']
            if not exe.exists():
                continue
            self.assertEqual((exe.stat().st_size, dist.sha256_of(exe)),
                             (manifest['runtime']['size_bytes'], manifest['runtime']['sha256']), pin_path.name)
            for item in manifest['runtime_dependencies']:
                path = ASSETS / item['path']
                if not path.exists():
                    continue
                self.assertEqual((path.stat().st_size, dist.sha256_of(path)),
                                 (item['size_bytes'], item['sha256']), item['path'])
            package = manifest['gs_stream']['gs_package']
            self.assertEqual(package['zip']['name'], 'gt4-gs-1.2.1-windows-x64.zip')
            self.assertEqual(package['release'], 'https://github.com/moesuito/gt4recomp-gs/releases/tag/gt4gs-v1.2.1')
            self.assertEqual(probe.package_tree_digest(ASSETS / package['directory']),
                             (package['tree_sha256'], package['files']))

    def test_spec2_pin_carries_the_spec2_library_and_derived_elf(self):
        if not PIN_SPEC2.exists():
            self.skipTest('slice280-spec2.json is not present')
        manifest = json.loads(PIN_SPEC2.read_text(encoding='utf-8'))
        self.assertEqual(manifest['target'], 'spec2')
        self.assertEqual(manifest['source_capsule'], 'private/slice276-spec2-iop-runtime')
        self.assertEqual(manifest['license_guard_capsule'], 'private/slice277-license-guard-runtime')
        self.assertEqual(manifest['frozen_game_library']['sha256'], manifest['ee_library']['sha256'])
        self.assertEqual(manifest['spec2_derived_elf']['sha256'],
                         '36f60b1a989b343e98ca55b1f07f051aa83a3fa1a4fcb4acb409adb148afc640')
        library = ASSETS / manifest['frozen_game_library']['path']
        if library.exists():
            self.assertEqual((library.stat().st_size, dist.sha256_of(library)),
                             (manifest['frozen_game_library']['size_bytes'],
                              manifest['frozen_game_library']['sha256']))


if __name__ == '__main__':
    unittest.main()
