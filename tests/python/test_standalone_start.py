"""A start of the game without the launcher (TASK-097): the missing-piece messages and the explicit mode, headless.

The executable under test is `gt4vulkan.exe` of GT4_NATIVE_BUILD. It is copied into a scratch install
(`<install>\\game\\gt4vulkan.exe`) and started with no ELF argument. `GT4_STANDALONE_MESSAGE_FILE` makes it write the text of
its message box to a file, so nothing opens a window. Every case stops before a window or the game would start.
The pure rules (paths, defaults, precedence, command line) are covered by the CTest fixture native_standalone.
Skipped without GT4_NATIVE_BUILD.
"""

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

BUILD = os.environ.get('GT4_NATIVE_BUILD')
EXE = Path(BUILD) / 'gt4vulkan.exe' if BUILD else None


def clean_environment(**extra):
    env = {key: value for key, value in os.environ.items() if not key.startswith(('GT4_', 'PS2_'))}
    env.update(extra)
    return env


@unittest.skipUnless(EXE and EXE.exists(), 'GT4_NATIVE_BUILD (gt4vulkan.exe) is not available')
class StandaloneStartTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = Path(tempfile.mkdtemp(prefix='gt4-standalone-'))
        cls.install = cls.scratch / 'GT4Recomp'
        game = cls.install / 'game'
        game.mkdir(parents=True)
        shutil.copy2(EXE, game / 'gt4vulkan.exe')
        for dll in Path(BUILD).glob('*.dll'):
            shutil.copy2(dll, game)
        cls.exe = game / 'gt4vulkan.exe'

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.scratch, ignore_errors=True)

    def start(self, exe=None, arguments=(), **env):
        message = self.scratch / 'message.txt'
        message.unlink(missing_ok=True)
        # LOCALAPPDATA and APPDATA point into the scratch folder: the run never looks at the real profile.
        profile = {'LOCALAPPDATA': str(self.scratch / 'Local'), 'APPDATA': str(self.scratch / 'Roaming')}
        result = subprocess.run([str(exe or self.exe), *arguments], capture_output=True, text=True, timeout=120,
                                cwd=self.scratch, env=clean_environment(GT4_STANDALONE_MESSAGE_FILE=str(message), **profile, **env))
        return result, message.read_text(encoding='utf-8') if message.exists() else ''

    def prepare_game_files(self, disc_bytes=b'iso', elf_bytes=b'elf'):
        root = self.install / 'data' / 'games' / 'gt4'
        (root / 'prepared').mkdir(parents=True, exist_ok=True)
        (root / 'disc').mkdir(parents=True, exist_ok=True)
        disc = root / 'disc' / 'SCUS-97328.iso'
        disc.write_bytes(disc_bytes)
        (root / 'prepared' / 'SCUS_973.28').write_bytes(elf_bytes)
        receipt = {
            'schema_version': 2, 'launcher_version': '1.3.0', 'payload': 'core+elf', 'game': 'gt4',
            'disc': {'path': 'D:\\Games\\disc.iso', 'size_bytes': 1, 'sha256': '0' * 64, 'serial': 'SCUS-97328',
                     'version': '2.00', 'volume_identifier': 'GT4'},
            'image': {'file': disc.name, 'size_bytes': disc.stat().st_size, 'modified_unix_nanos': disc.stat().st_mtime_ns,
                      'sha256': '0' * 64},
            'qualified_dump': False, 'image_name': '', 'core': {'size_bytes': 1, 'sha256': '0' * 64}, 'original': None,
            'elf': {'size_bytes': len(elf_bytes), 'sha256': '0' * 64}, 'prepared_unix_seconds': 1,
        }
        (root / 'prepared' / 'prepare.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
        return disc

    def test_a_start_outside_an_install_says_where_the_game_must_be_started_from(self):
        result, message = self.start(exe=EXE)
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn('GT4Recomp', message)

    def test_a_game_with_no_prepared_files_says_to_open_the_launcher_once(self):
        shutil.rmtree(self.install / 'data', ignore_errors=True)
        result, message = self.start()
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn('not prepared yet', message)
        self.assertIn('Open GT4Recomp once', message)

    def test_prepared_files_without_gsdx_say_that_the_renderer_is_missing(self):
        self.prepare_game_files()
        shutil.rmtree(self.install / 'data' / 'gs', ignore_errors=True)
        result, message = self.start()
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn('GSdx', message)
        self.assertIn('Open GT4Recomp once', message)

    def test_a_changed_disc_copy_is_refused_like_the_launcher_refuses_it(self):
        disc = self.prepare_game_files()
        disc.write_bytes(b'a different image')  # new size and modified time
        result, message = self.start()
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn('set up again', message)
        self.assertIn('has changed', message)

    def test_a_renderer_that_does_not_match_its_recorded_hash_is_refused(self):
        self.prepare_game_files()
        folder = self.install / 'data' / 'gs' / '1.2.3'
        folder.mkdir(parents=True, exist_ok=True)
        (self.install / 'data' / 'gs' / 'current.txt').write_text('1.2.3\n', encoding='utf-8')
        (folder / 'gt4-gs.exe').write_bytes(b'not the renderer')
        (folder / 'sha256.txt').write_text('0' * 64 + '\n', encoding='utf-8')
        result, message = self.start()
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn('did not pass its check', message)

    def test_a_start_with_an_elf_argument_is_the_explicit_mode_and_does_not_look_for_an_install(self):
        # The donor main reads the arguments: an unknown option prints its usage text and exits 2, as before.
        result, message = self.start(arguments=['no-such-file.elf', '--no-such-option'])
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn('usage: ps2run', result.stderr)
        self.assertEqual(message, '', 'no standalone message in the explicit mode')

    def test_portable_check_still_reaches_the_donor_main(self):
        result, _ = self.start(exe=EXE, arguments=['--portable-check'])
        self.assertIn('[portable]', result.stdout)


if __name__ == '__main__':
    unittest.main()
