"""GS async launcher mode (slice 152): profile pin, option validation and forwarding. No game is started."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import play_performance as performance
import play_external_runtime as launcher
import external_runtime_probe as probe

ROOT = Path(__file__).resolve().parents[2]
ASYNC_RUNTIME = 'private/slice152-gs-async-build/build/gt4vulkan.exe'
ASYNC_MANIFEST = 'slice152-gs-async.json'


def async_evidence(**overrides):
    evidence = {'profile': 'performance', 'perf_schema_version': 1,
                'keyboard_input_fix': 'digital_only_slice136', 'capture_activation_implemented': False,
                'measurement_mode': 'gs-async', 'vu_scope_profile': 'unit_split_run_fast_only',
                'runtime': {'path': ASYNC_RUNTIME}, 'default_card_seed': {'path': 'private/seed.ps2'},
                'standard_card_seed': {'path': 'private/seed.ps2'}}
    evidence.update(overrides)
    return evidence


class AsyncProfileTests(unittest.TestCase):
    def test_profile_pins_the_async_binary_and_manifest(self):
        self.assertIn('gs-async', probe.PERF_PROFILES)
        manifest, runtime = probe.PERF_PROFILES['gs-async']
        self.assertEqual(manifest, ASYNC_MANIFEST)
        self.assertEqual(runtime, ASYNC_RUNTIME)
        self.assertIn('gs-async', performance.MODES)

    def test_manifest_resolves_to_async_runtime_and_mode(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            manifest = root / 'docs/inputs' / ASYNC_MANIFEST
            self.assertEqual(probe.expected_performance_pin(manifest, root), (ASYNC_RUNTIME, 'gs-async'))

    def test_verify_accepts_only_the_async_evidence_contract(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            manifest = root / 'docs/inputs' / ASYNC_MANIFEST
            probe.verify_performance_profile(manifest, async_evidence(), root)
            with self.assertRaisesRegex(ValueError, 'qualified'):
                probe.verify_performance_profile(manifest, async_evidence(measurement_mode='vu1-aot'), root)
            with self.assertRaisesRegex(ValueError, 'qualified'):
                probe.verify_performance_profile(manifest, async_evidence(vu_scope_profile='full'), root)
            with self.assertRaisesRegex(ValueError, 'qualified'):
                probe.verify_performance_profile(manifest, async_evidence(runtime={'path': 'private/other.exe'}), root)

    def test_both_aot_manifests_accept_verify_and_interpret_options(self):
        self.assertEqual(set(probe.VU1_AOT_MANIFESTS), {'slice151-vu1-aot.json', ASYNC_MANIFEST, 'slice153-pacing.json', 'slice154-vu1-flags.json', 'slice155-vu1-full.json', 'slice156-quickfix.json', 'slice157-vu1-all.json', 'slice158-quickfix2.json', 'slice164-quickfix3.json', 'slice160-vu0-aot.json', 'slice166-main.json', 'slice169-mtvu-measure.json', 'slice170-mtvu.json',
                                             'slice171-mtvu.json', 'slice172-mtvu.json', 'slice178-main.json', 'slice191-pcsx2-stream.json',
                                             'slice193-pcsx2.json', 'slice194-pcsx2.json', 'slice195-pcsx2.json', 'slice196-pcsx2.json',
                                             'slice197-pcsx2.json', 'slice198-pcsx2.json', 'slice199-pcsx2.json',
                                             'slice200-pcsx2.json', 'slice201-pcsx2.json', 'slice202-pcsx2.json',
                                             'slice203-pcsx2.json', 'slice205-license-guard.json', 'slice206-pcsx2.json', 'slice208-pcsx2.json',
                                             'slice209-pcsx2.json', 'slice210-pcsx2.json', 'slice211-pcsx2.json',
                                             'slice212b-pcsx2.json', 'slice213-pcsx2.json', 'slice214-pcsx2.json',
                                             'slice215-pcsx2.json', 'slice217-pcsx2.json',
                                             'slice218-pcsx2.json', 'slice240-pcsx2.json', 'slice260-pcsx2.json', 'slice262-pcsx2.json',
                                             'slice268-pcsx2.json', 'slice279-pcsx2.json'})


class AsyncManifestTests(unittest.TestCase):
    """The committed pins must describe the files on disk when they exist (checked without starting the game)."""

    def test_launcher_manifest_lists_the_async_runtime_and_nine_shaders(self):
        manifest = json.loads((ROOT / 'docs/inputs' / ASYNC_MANIFEST).read_text(encoding='utf-8'))
        self.assertEqual(manifest['measurement_mode'], 'gs-async')
        self.assertEqual(manifest['runtime']['path'], ASYNC_RUNTIME)
        self.assertEqual(len(manifest['runtime_dependencies']), 9)
        self.assertEqual(manifest['vu_scope_profile'], 'unit_split_run_fast_only')
        for dependency in manifest['runtime_dependencies']:
            self.assertTrue(dependency['path'].startswith('private/slice152-gs-async-shaders/'))

    def test_runtime_pins_match_files_when_the_build_exists(self):
        manifest = json.loads((ROOT / 'docs/inputs' / ASYNC_MANIFEST).read_text(encoding='utf-8'))
        for entry in [manifest['runtime']] + manifest['runtime_dependencies']:
            path = ROOT / entry['path']
            if not path.exists():
                self.skipTest(f'Local build output not present: {entry["path"]}')
            data = path.read_bytes()
            self.assertEqual(len(data), entry['size_bytes'], entry['path'])
            self.assertEqual(hashlib.sha256(data).hexdigest(), entry['sha256'], entry['path'])

    def test_runtime_source_manifest_pins_the_async_port_and_shaders(self):
        manifest = json.loads((ROOT / 'docs/inputs/slice152-gs-async-runtime.json').read_text(encoding='utf-8'))
        self.assertEqual(set(manifest['inputs']['PORT']), {'gpu.cpp', 'gpu.hpp', 'gs.cpp', 'main.cpp'})
        self.assertEqual(len(manifest['inputs']['SHADERS']), 9)
        port = ROOT / 'private/slice152-gs-async-port'
        if port.exists():
            for name, expected in manifest['inputs']['PORT'].items():
                actual = hashlib.sha256((port / name).read_bytes()).hexdigest()
                self.assertEqual(actual, expected, name)


class AsyncWrapperTests(unittest.TestCase):
    def run_wrapper(self, arguments, root):
        with patch.object(performance, 'ROOT', root), patch.object(performance, 'play') as play, \
             patch.object(sys, 'argv', ['perf', '--assets-root', str(root)] + arguments):
            code = performance.main()
        return code, play

    def test_async_mode_selects_its_manifest_without_extra_options(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            code, play = self.run_wrapper(['--mode', 'gs-async'], root)
            self.assertEqual(code, 0)
            play.assert_called_once_with(root, root / 'docs/inputs' / ASYNC_MANIFEST, performance=True)

    def test_verify_and_interpret_only_are_forwarded(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            code, play = self.run_wrapper(['--mode', 'gs-async', '--vu1-verify', '50'], root)
            self.assertEqual(code, 0)
            play.assert_called_once_with(root, root / 'docs/inputs' / ASYNC_MANIFEST, performance=True, vu1_verify=50)
            code, play = self.run_wrapper(['--mode', 'gs-async', '--vu1-interpret-only'], root)
            play.assert_called_once_with(root, root / 'docs/inputs' / ASYNC_MANIFEST, performance=True, vu1_interpret_only=True)

    def test_options_are_rejected_outside_the_aot_modes_and_before_play(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for arguments in (['--mode', 'detailed', '--vu1-verify', '5'],
                              ['--mode', 'vu-split', '--vu1-interpret-only'],
                              ['--mode', 'gs-async', '--vu1-verify', '-1'],
                              ['--mode', 'gs-async', '--vu1-verify', '5', '--vu1-interpret-only']):
                code, play = self.run_wrapper(arguments, root)
                self.assertEqual(code, 1, arguments)
                play.assert_not_called()


class AsyncPlayForwardingTests(unittest.TestCase):
    def test_play_passes_verify_period_to_the_probe(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            inputs = root / 'docs/inputs'
            inputs.mkdir(parents=True)
            manifest = inputs / ASYNC_MANIFEST
            manifest.write_text(json.dumps(async_evidence()))
            with patch.object(launcher, 'ROOT', root), patch.object(launcher, 'run_probe', return_value={'manual_session_closed': True}) as execute, \
                 patch.object(launcher, 'verify_performance_profile'):
                launcher.play(root, manifest, performance=True, vu1_verify=50)
            self.assertEqual(execute.call_args.kwargs['vu1_verify'], 50)
            self.assertFalse(execute.call_args.kwargs['vu1_interpret_only'])


if __name__ == '__main__':
    unittest.main()
