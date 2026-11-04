"""Installed-folder paths and the --portable-check self-test (TASK-059).

The game must run from a folder with no repository path: shaders in <exe folder>\\shaders and the GS package in
<exe folder>\\gs\\gt4-gs.exe. The tests copy the built executable into scratch folders and check what it resolves.
Needs GT4_NATIVE_BUILD (a build directory with gt4vulkan.exe) and GT4_ASSETS_ROOT (the checkout that holds the pinned
shaders, default: the main checkout). Skipped when they are absent.
"""

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = os.environ.get('GT4_NATIVE_BUILD')
ASSETS = Path(os.environ.get('GT4_ASSETS_ROOT', 'C:/Antigravity/gt4-staticrecomp')).resolve()
SHADER_NAMES = ('upload', 'copy_read', 'copy_write', 'load_clut', 'display', 'raster', 'resolve', 'snapshot', 'decode')


def pinned_shader_dir():
    manifest = json.loads((ROOT / 'docs/inputs/slice202-pcsx2.json').read_text(encoding='utf-8'))
    return ASSETS / manifest['shader_capsule']


def executable():
    if not BUILD:
        return None
    candidate = Path(BUILD) / 'gt4vulkan.exe'
    return candidate if candidate.exists() else None


def release_distribution_build():
    """True when the build under test was configured with GT4_RELEASE_DISTRIBUTION=ON (TASK-081)."""
    cache = Path(BUILD) / 'CMakeCache.txt' if BUILD else None
    if not cache or not cache.exists():
        return False
    return 'GT4_RELEASE_DISTRIBUTION:BOOL=ON' in cache.read_text(encoding='utf-8', errors='replace')


@unittest.skipUnless(executable() and pinned_shader_dir().is_dir(),
                     'GT4_NATIVE_BUILD (gt4vulkan.exe) or the pinned shader folder is not available')
class PortableInstallTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = Path(tempfile.mkdtemp(prefix='gt4-portable-'))
        cls.exe = cls.scratch / 'install' / 'gt4.exe'
        cls.exe.parent.mkdir()
        shutil.copyfile(executable(), cls.exe)
        # TASK-075: gt4.exe (the build's gt4vulkan.exe) imports SDL3.dll; the installed folder carries it, as make_distribution writes it.
        shutil.copyfile(ASSETS / 'private/tooling/sdl3-3.4.18/runtime/SDL3.dll', cls.exe.parent / 'SDL3.dll')
        # A missing DLL must fail a child quietly, never open a system error dialog.
        if os.name == 'nt':
            import ctypes
            ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.scratch, ignore_errors=True)

    def run_check(self, env_extra=None):
        environment = dict(os.environ)
        environment.pop('GT4_GS_PROCESS', None)
        environment.update(env_extra or {})
        result = subprocess.run([str(self.exe), '--portable-check'], capture_output=True, text=True,
                                timeout=60, env=environment, cwd=str(self.exe.parent))
        return result.returncode, result.stdout

    def install_shaders(self, names):
        folder = self.exe.parent / 'shaders'
        shutil.rmtree(folder, ignore_errors=True)
        folder.mkdir()
        for name in names:
            shutil.copyfile(pinned_shader_dir() / f'{name}.spv', folder / f'{name}.spv')

    def test_installed_shaders_and_gs_package_are_found_next_to_the_executable(self):
        self.install_shaders(SHADER_NAMES)
        gs = self.exe.parent / 'gs'
        gs.mkdir(exist_ok=True)
        (gs / 'gt4-gs.exe').write_bytes(b'placeholder: the check does not start the process')
        code, output = self.run_check()
        self.assertEqual(code, 0, output)
        self.assertIn('(installed, next to the executable)', output)
        self.assertIn('shaders: complete (9 of 9 present)', output)
        self.assertIn('GS process: ', output)
        self.assertIn('(installed package)', output)
        self.assertNotIn('build-time fallback', output)
        shutil.rmtree(gs)

    def test_missing_shader_fails_the_check_and_names_the_file(self):
        self.install_shaders(SHADER_NAMES[:-1])
        code, output = self.run_check()
        self.assertEqual(code, 1, output)
        self.assertIn('missing shader: ', output)
        self.assertIn('INCOMPLETE', output)
        self.assertIn('result: shaders missing', output)

    def test_gs_process_override_is_reported_before_the_installed_package(self):
        self.install_shaders(SHADER_NAMES)
        code, output = self.run_check({'GT4_GS_PROCESS': 'D:/elsewhere/gt4-gs.exe'})
        self.assertEqual(code, 0, output)
        self.assertIn('GS process (GT4_GS_PROCESS): D:/elsewhere/gt4-gs.exe', output)

    def test_no_installed_shaders_uses_the_build_time_folder(self):
        if release_distribution_build():
            self.skipTest('release build: no build-time fallback (see the release test below)')
        shutil.rmtree(self.exe.parent / 'shaders', ignore_errors=True)
        code, output = self.run_check()
        self.assertIn('build-time fallback', output)
        self.assertIn('build-time shader fallback: present (development build)', output)
        self.assertIn('GS process: none', output)
        self.assertIn('result: ', output)

    def test_release_build_has_no_fallback_and_reports_missing_shaders(self):
        """TASK-081: a release build never names a build-machine folder; without a shaders folder the check fails."""
        if not release_distribution_build():
            self.skipTest('development build: it keeps the build-time fallback')
        shutil.rmtree(self.exe.parent / 'shaders', ignore_errors=True)
        code, output = self.run_check()
        self.assertEqual(code, 1, output)
        self.assertIn('build-time shader fallback: none (release build)', output)
        self.assertIn('missing; a release build has no build-time fallback', output)
        self.assertIn('result: shaders missing', output)
        self.assertNotIn('Antigravity', output)
        self.assertNotIn('private', output.replace(str(self.exe.parent), ''))

    def test_release_executable_holds_no_build_machine_path(self):
        """TASK-081: no absolute path (ASCII or UTF-16) of the build machine and no user folder in a release build."""
        if not release_distribution_build():
            self.skipTest('development build: it embeds the build-time shader folder on purpose')
        data = executable().read_bytes()
        needles = [b'Antigravity', b'gt4-staticrecomp', b'gt4-wt-', b'\\Users\\', b'/Users/', b'private/slice',
                   b'private\\slice']
        user = os.environ.get('USERNAME')
        if user and len(user) >= 4:
            needles.append(user.encode('ascii', 'ignore'))
        for needle in needles:
            self.assertNotIn(needle, data, needle)
            self.assertNotIn(needle.decode('ascii').encode('utf-16-le'), data, needle)


if __name__ == '__main__':
    unittest.main()
