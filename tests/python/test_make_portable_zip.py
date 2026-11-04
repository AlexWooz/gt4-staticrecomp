r"""The portable zip of a release (TASK-098, scripts/make_portable_zip.py), tested on fixture files only."""

import hashlib
import importlib.util
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('make_portable_zip', ROOT / 'scripts/make_portable_zip.py')
portable = importlib.util.module_from_spec(spec)
spec.loader.exec_module(portable)


class PortableZipTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='gt4-portable-zip-')
        self.base = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)
        self.exe = self.base / 'GT4Recomp.exe'
        self.exe.write_bytes(b'launcher')
        self.sdl = self.base / 'SDL3.dll'
        self.sdl.write_bytes(b'sdl')
        self.game = self.base / 'game'
        (self.game / 'shaders').mkdir(parents=True)
        (self.game / 'gt4.exe').write_bytes(b'game')
        (self.game / 'gt4-spec2.exe').write_bytes(b'spec2')
        (self.game / 'shaders' / 'raster.spv').write_bytes(b'spv')
        (self.game / 'vcruntime140.dll').write_bytes(b'crt')

    def make(self, out='out', **extra):
        return portable.make_zip(self.base / out, '1.3.0', self.exe, self.sdl, self.game, **extra)

    def test_the_zip_holds_the_installer_files_and_empty_user_and_data(self):
        target, digest = self.make()
        self.assertEqual(target.name, 'GT4Recomp-1.3.0-portable.zip')
        with zipfile.ZipFile(target) as archive:
            names = archive.namelist()
            self.assertEqual(archive.testzip(), None)
            for expected in ('GT4Recomp.exe', 'SDL3.dll', 'NOTICE.md', 'LICENSE.txt', 'PORTABLE.txt', 'game/gt4.exe',
                             'game/gt4-spec2.exe', 'game/shaders/raster.spv', 'game/vcruntime140.dll'):
                self.assertIn(expected, names)
            self.assertIn('user/', names)
            self.assertIn('data/', names)
            self.assertEqual([name for name in names if name.startswith(('user/', 'data/')) and not name.endswith('/')], [],
                             'user and data are empty')
            self.assertEqual(archive.read('GT4Recomp.exe'), b'launcher')
            self.assertEqual(archive.read('game/shaders/raster.spv'), b'spv')
        self.assertEqual(digest, hashlib.sha256(target.read_bytes()).hexdigest())

    def test_a_test_build_carries_its_label_in_the_zip_name(self):
        target, _ = portable.make_zip(self.base / 'rc', '1.3.0-rc5', self.exe, self.sdl, self.game)
        self.assertEqual(target.name, 'GT4Recomp-1.3.0-rc5-portable.zip')

    def test_a_version_that_is_not_a_release_version_is_refused(self):
        for bad in ('1.3', '1.3.0-rc', '1.3.0-rc0', '1.3.0-beta1', 'v1.3.0', '../1.3.0'):
            with self.assertRaises(ValueError, msg=bad):
                portable.make_zip(self.base / 'bad', bad, self.exe, self.sdl, self.game)

    def test_the_zip_is_the_same_files_the_installer_stages(self):
        nsis = (ROOT / 'installer/gt4recomp.nsi').read_text(encoding='utf-8')
        for staged in ('/oname=GT4Recomp.exe', '/oname=SDL3.dll', '/oname=NOTICE.md', '/oname=LICENSE.txt'):
            self.assertIn(staged, nsis)
        with zipfile.ZipFile(self.make()[0]) as archive:
            names = set(archive.namelist())
        for name in ('GT4Recomp.exe', 'SDL3.dll', 'NOTICE.md', 'LICENSE.txt'):
            self.assertIn(name, names)

    def test_the_output_is_deterministic(self):
        first, first_digest = self.make('one')
        second, second_digest = self.make('two')
        self.assertEqual(first_digest, second_digest)
        self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_the_sums_file_gets_the_zip_line(self):
        sums = self.base / 'SHA256SUMS'
        sums.write_text('abc123  GT4Recomp-Setup-1.3.0.exe\n', encoding='utf-8')
        target, digest = self.make(sums_file=sums)
        lines = sums.read_text(encoding='utf-8').splitlines()
        self.assertEqual(lines, ['abc123  GT4Recomp-Setup-1.3.0.exe', f'{digest}  {target.name}'])

    def test_game_data_in_the_game_folder_is_refused(self):
        for forbidden in ('Gran Turismo 4.iso', 'CORE.GT4', 'card.ps2', 'SCUS_973.28'):
            (self.game / forbidden).write_bytes(b'data')
            with self.assertRaises(ValueError, msg=forbidden):
                self.make(f'refused-{forbidden}')
            (self.game / forbidden).unlink()

    def test_an_existing_zip_or_an_output_inside_the_game_folder_is_refused(self):
        self.make()
        with self.assertRaises(ValueError):
            self.make()
        with self.assertRaises(ValueError):
            portable.make_zip(self.game / 'out', '1.3.0', self.exe, self.sdl, self.game)

    def test_a_missing_input_is_refused(self):
        self.exe.unlink()
        with self.assertRaises(ValueError):
            self.make()


if __name__ == '__main__':
    unittest.main()
