"""Synthetic controls for the external pilot's input and completion gates."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import external_runtime_probe as probe


def file_pin(path):
    data = path.read_bytes()
    return {'size_bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


class ProbeTests(unittest.TestCase):
    def performance_manifest(self, root, mode=None):
        evidence = json.loads((root / 'docs/inputs/slice114-native-base.json').read_text())
        name, runtime_path = probe.PERF_PROFILES[mode] if mode else (probe.PERF_MANIFEST, probe.PERF_RUNTIME)
        runtime = root / runtime_path
        runtime.parent.mkdir(parents=True)
        runtime.write_bytes((root / evidence['runtime']['path']).read_bytes())
        evidence['runtime']['path'] = runtime_path
        evidence.update(profile='performance', perf_schema_version=1,
                        keyboard_input_fix='digital_only_slice136', capture_activation_implemented=False)
        if mode: evidence['measurement_mode'] = mode
        if mode in ('vu-split', 'vu1-aot', 'gs-async', 'pacing', 'vu1-flags', 'vu1-full', 'quickfix', 'vu1-all', 'quickfix2', 'quickfix3', 'vu0-aot', 'main', 'mtvu-measure', 'mtvu', 'pcsx2-stream', 'pcsx2', 'pcsx2-slice268', 'license-guard', 'pcsx2-coverage'): evidence['vu_scope_profile'] = 'unit_split_run_fast_only'
        manifest = root / 'docs/inputs' / name
        manifest.write_text(json.dumps(evidence))
        return manifest, evidence

    def test_matched_modes_select_exact_pins_and_record_mode(self):
        for mode in probe.PERF_PROFILES:
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                self.make_workspace(root)
                manifest, evidence = self.performance_manifest(root, mode)
                def execute(arguments, **options):
                    self.assertEqual(Path(arguments[0]), root / evidence['runtime']['path'])
                    self.assertNotIn('--press', arguments)
                    options['stdout'].write(b'[ee] stopped after 5 frames, 2 cycles: window closed\n')
                    return subprocess.CompletedProcess(arguments, 0)
                with patch.object(probe.subprocess, 'run', side_effect=execute):
                    result = probe.run_probe(root, root / 'private/pair', 0, 1, [],
                        runtime_manifest=manifest, window=True, interactive=True, performance=True)
                self.assertEqual(result['measurement_mode'], mode)

    def test_matched_profile_rejects_crossed_mode_or_binary(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_workspace(root)
            manifest, evidence = self.performance_manifest(root, 'frame-only')
            for bad in (dict(evidence, measurement_mode='detailed'),
                        dict(evidence, runtime=dict(evidence['runtime'], path=probe.PERF_RUNTIME))):
                with self.subTest(bad=bad), patch.object(probe.subprocess, 'run') as execute:
                    manifest.write_text(json.dumps(bad))
                    with self.assertRaisesRegex(ValueError, 'qualified'):
                        probe.run_probe(root, root / 'private/pair', 0, 1, [],
                            runtime_manifest=manifest, window=True, interactive=True, performance=True)
                    execute.assert_not_called()
                    self.assertFalse((root / 'private/pair').exists())

    def test_vu_split_requires_explicit_narrow_scope_before_output(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_workspace(root)
            manifest, evidence = self.performance_manifest(root, 'vu-split')
            for scope in (None, 'all_vu', True):
                manifest.write_text(json.dumps(dict(evidence, vu_scope_profile=scope)))
                with self.subTest(scope=scope), patch.object(probe.subprocess, 'run') as execute:
                    with self.assertRaisesRegex(ValueError, 'qualified'):
                        probe.run_probe(root, root / 'private/split', 0, 1, [],
                            runtime_manifest=manifest, window=True, interactive=True, performance=True)
                    execute.assert_not_called()
                    self.assertFalse((root / 'private/split').exists())

    def test_performance_profile_and_environment_are_isolated(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_workspace(root)
            manifest, evidence = self.performance_manifest(root)
            contaminated = {'PS2_TIME': '1,100', 'PS2_DUMP_AUDIO': 'old.wav', 'PS2_ONE_THREAD': '1',
                            'GT4_PERF': '0', 'GT4_PERF_PERIOD': '1', 'GT4_OBSERVATION_OUTPUT': 'old.json'}
            def execute(arguments, **options):
                environment = options['env']
                self.assertEqual(environment['GT4_PERF'], '1')
                self.assertEqual(environment['PS2_SCALE'], '1')
                for name in contaminated:
                    if name != 'GT4_PERF': self.assertNotIn(name, environment)
                options['stdout'].write(b'[ee] stopped after 5 frames, 2 cycles: window closed\n')
                return subprocess.CompletedProcess(arguments, 0)
            with patch.dict(probe.os.environ, dict(contaminated, VK_ICD_FILENAMES='test-icd.json')), patch.object(probe.subprocess, 'run', side_effect=execute):
                result = probe.run_probe(root, root / 'private/perf', 0, 1, [],
                    runtime_manifest=manifest, window=True, interactive=True, performance=True)
            self.assertTrue(result['manual_session_closed'])
            self.assertEqual(result['performance_environment']['GT4_PERF'], '1')
            self.assertEqual(result['vulkan_loader_environment']['VK_ICD_FILENAMES'], 'test-icd.json')

    def test_performance_rejects_invalid_identity_before_output(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_workspace(root)
            manifest, evidence = self.performance_manifest(root)
            for key, value in [('profile', 'observation'), ('perf_schema_version', True),
                               ('keyboard_input_fix', 'old'), ('capture_activation_implemented', True)]:
                bad = dict(evidence, **{key: value})
                manifest.write_text(json.dumps(bad))
                with self.assertRaises(ValueError), patch.object(probe.subprocess, 'run') as execute:
                    probe.run_probe(root, root / 'private/perf', 0, 1, [], runtime_manifest=manifest,
                                    window=True, interactive=True, performance=True)
                execute.assert_not_called()
                self.assertFalse((root / 'private/perf').exists())
            manifest.write_text(json.dumps(evidence))
            (root / probe.PERF_RUNTIME).write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'pinned'):
                probe.run_probe(root, root / 'private/perf', 0, 1, [], runtime_manifest=manifest,
                                window=True, interactive=True, performance=True)
            self.assertFalse((root / 'private/perf').exists())

    def test_performance_accepts_the_task076_keyboard_identity(self):
        root = Path(__file__).resolve().parents[2]
        manifest = (root / 'docs/inputs' / probe.PERF_PROFILES['pcsx2'][0]).resolve()
        evidence = json.loads(manifest.read_text(encoding='utf-8'))
        self.assertIn(evidence['keyboard_input_fix'], probe.PERFORMANCE_KEYBOARD_FIXES)
        probe.verify_performance_profile(manifest, evidence, root)

    def capture_manifest(self, root):
        evidence = json.loads((root / 'docs/inputs/slice114-native-base.json').read_text())
        qualified = root / probe.CAPTURE_RUNTIME
        qualified.parent.mkdir(parents=True)
        qualified.write_bytes((root / evidence['runtime']['path']).read_bytes())
        evidence['runtime']['path'] = probe.CAPTURE_RUNTIME
        evidence.update(profile='observation', capture_activation_implemented=True, capture_schema_version=1)
        manifest = root / 'docs/inputs' / probe.CAPTURE_MANIFEST
        manifest.write_text(json.dumps(evidence))
        return manifest

    def test_interactive_mode_preserves_completion_gate_and_sets_capture_environment(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_workspace(root)
            def execute(arguments, **options):
                self.assertIn('--window', arguments)
                self.assertNotIn('--trace-syscalls', arguments)
                self.assertNotIn('--frames-dir', arguments)
                self.assertEqual(options['env']['PS2_DUMP_AUDIO'], str(root / 'private/play/audio.wav'))
                options['stdout'].write(b'[ee] stopped after 77 frames, 99 cycles: window closed\n')
                return subprocess.CompletedProcess(arguments, 0)
            with patch.object(probe.subprocess, 'run', side_effect=execute):
                result = probe.run_probe(root, root / 'private/play', 0, 1, [],
                                         window=True, capture_audio=True, interactive=True)
            self.assertTrue(result['manual_session_closed'])
            self.assertFalse(result['completed_requested_frames'])
            for options in ({'window': False}, {'window': True, 'presses': ['1:start']}):
                with self.assertRaisesRegex(ValueError, 'Interactive play requires'):
                    probe.run_probe(root, root / 'private/reject', 0, 1,
                                    options.get('presses', []), window=options['window'], interactive=True)

    def test_inherited_audio_capture_cannot_overwrite_external_files(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_workspace(root)
            def execute(arguments, **options):
                self.assertNotIn('PS2_DUMP_AUDIO', options['env'])
                self.assertNotIn('GT4_OBSERVATION_OUTPUT', options['env'])
                options['stdout'].write(b'[ee] stopped after 100 frames, 9 cycles: frame limit reached\n')
                return subprocess.CompletedProcess(arguments, 0)
            with patch.dict(probe.os.environ, {'PS2_DUMP_AUDIO': 'protected.wav', 'GT4_OBSERVATION_OUTPUT': 'protected.json'}), \
                 patch.object(probe.subprocess, 'run', side_effect=execute):
                probe.run_probe(root, root / 'private/audio', 100, 1, [])

    def test_manual_shape_capture_uses_only_fresh_session_output(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_workspace(root)
            manifest = self.capture_manifest(root)
            output = root / 'private/manual-shapes'
            def execute(arguments, **options):
                capture = Path(options['env']['GT4_OBSERVATION_OUTPUT'])
                self.assertEqual(capture, output / 'shape-capture.json')
                self.assertFalse(capture.exists())
                self.assertNotIn('--press', arguments)
                capture.write_bytes(b'{"synthetic":true}\n')
                options['stdout'].write(b'[ee] stopped after 77 frames, 99 cycles: window closed\n')
                return subprocess.CompletedProcess(arguments, 0)
            with patch.dict(probe.os.environ, {'GT4_OBSERVATION_OUTPUT': 'protected.json'}), \
                    patch.object(probe.subprocess, 'run', side_effect=execute):
                result = probe.run_probe(root, output, 0, 1, [], runtime_manifest=manifest,
                                         window=True, interactive=True, capture_shapes=True)
            self.assertTrue(result['manual_session_closed'])
            self.assertTrue(result['shape_capture_created'])
            self.assertEqual(result['shape_capture_file'], file_pin(output / 'shape-capture.json'))

    def test_shape_capture_requires_qualified_manual_profile_before_outputs(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_workspace(root)
            manifest = self.capture_manifest(root)
            evidence = json.loads(manifest.read_text())
            for field in ('profile', 'capture_activation_implemented', 'capture_schema_version'):
                evidence.pop(field)
            bad_modes = ({}, {'profile': 'baseline', 'capture_activation_implemented': True, 'capture_schema_version': 1},
                         {'profile': 'observation', 'capture_activation_implemented': False, 'capture_schema_version': 1},
                         {'profile': 'observation', 'capture_activation_implemented': True, 'capture_schema_version': 2},
                         {'profile': 'observation', 'capture_activation_implemented': True, 'capture_schema_version': True})
            for mode in bad_modes:
                manifest.write_text(json.dumps({**evidence, **mode}))
                with patch.object(probe.subprocess, 'run') as execute:
                    with self.assertRaisesRegex(ValueError, 'qualified manual observation runtime'):
                        probe.run_probe(root, root / 'private/reject-shapes', 0, 1, [], runtime_manifest=manifest,
                                        window=True, interactive=True, capture_shapes=True)
                    execute.assert_not_called()
                self.assertFalse((root / 'private/reject-shapes').exists())
            with self.assertRaisesRegex(ValueError, 'owner-controlled'):
                probe.run_probe(root, root / 'private/reject-headless', 1, 1, [], capture_shapes=True)

    def test_capture_separates_runtime_checkout_from_owner_assets_and_outputs(self):
        with tempfile.TemporaryDirectory() as folder:
            root, runtime = Path(folder) / 'assets', Path(folder) / 'runtime'
            self.make_workspace(root)
            self.make_workspace(runtime)
            manifest = self.capture_manifest(runtime)
            def execute(arguments, **options):
                self.assertEqual(Path(arguments[0]), runtime / probe.CAPTURE_RUNTIME)
                self.assertEqual(options['env']['GT4_OBSERVATION_OUTPUT'], str(root / 'private/play/shape-capture.json'))
                options['stdout'].write(b'[ee] stopped after 77 frames, 99 cycles: window closed\n')
                return subprocess.CompletedProcess(arguments, 0)
            with patch.object(probe.subprocess, 'run', side_effect=execute):
                result = probe.run_probe(root, root / 'private/play', 0, 1, [], runtime_manifest=manifest,
                                         window=True, interactive=True, runtime_workspace=runtime, capture_shapes=True)
            self.assertTrue(result['manual_session_closed'])
            self.assertFalse(result['shape_capture_created'])

    def test_capture_rejects_other_manifest_runtime_path_and_changed_executable(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_workspace(root)
            manifest = self.capture_manifest(root)
            original = json.loads(manifest.read_text())
            other = manifest.with_name('other.json')
            other.write_text(json.dumps(original))
            nested = manifest.parent / 'nested' / probe.CAPTURE_MANIFEST
            nested.parent.mkdir()
            nested.write_text(json.dumps(original))
            output = root / 'private/rejected-contract'
            wrong_path = {**original, 'runtime': {**original['runtime'], 'path': 'private/donor/ps2run.exe'}}
            for selected, evidence in ((other, original), (nested, original), (manifest, wrong_path)):
                selected.write_text(json.dumps(evidence))
                with patch.object(probe.subprocess, 'run') as execute:
                    with self.assertRaisesRegex(ValueError, 'qualified manual observation'):
                        probe.run_probe(root, output, 0, 1, [], runtime_manifest=selected,
                                        window=True, interactive=True, capture_shapes=True)
                    execute.assert_not_called()
                self.assertFalse(output.exists())
            manifest.write_text(json.dumps(original))
            (root / probe.CAPTURE_RUNTIME).write_bytes(b'changed candidate')
            with patch.object(probe.subprocess, 'run') as execute:
                with self.assertRaisesRegex(ValueError, 'pinned size/hash'):
                    probe.run_probe(root, output, 0, 1, [], runtime_manifest=manifest,
                                    window=True, interactive=True, capture_shapes=True)
                execute.assert_not_called()
            self.assertFalse(output.exists())

    def test_persistent_manual_card_is_backed_up_and_never_reseeded(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_workspace(root)
            (root / 'docs/inputs/slice110-external-menu-pilot.json').write_text(json.dumps({'artifacts': []}))
            card = root / 'private/manual/card.ps2'
            card.parent.mkdir()
            original = b'a' * 8650752
            card.write_bytes(original)
            def execute(arguments, **options):
                self.assertEqual(arguments[arguments.index('--memcard') + 1], str(card))
                card.write_bytes(b'b' * 8650752)
                options['stdout'].write(b'[ee] stopped after 77 frames, 99 cycles: window closed\n')
                return subprocess.CompletedProcess(arguments, 0)
            with patch.object(probe.subprocess, 'run', side_effect=execute):
                result = probe.run_probe(root, root / 'private/play', 0, 1, [],
                                         window=True, interactive=True, play_card=card)
            self.assertEqual((root / 'private/play/card-before.ps2').read_bytes(), original)
            self.assertNotEqual(result['play_card_before'], result['play_card_after'])
            self.assertEqual(card.read_bytes(), b'b' * 8650752)
            evidence = root / 'docs/inputs/slice110-external-menu-pilot.json'
            evidence.write_text(json.dumps({'artifacts': [{'path': card.relative_to(root).as_posix()}]}))
            with self.assertRaisesRegex(ValueError, 'pinned evidence artifact'):
                probe.run_probe(root, root / 'private/protected', 0, 1, [], window=True,
                                interactive=True, play_card=card)
            self.assertFalse((root / 'private/protected').exists())
            self.assertEqual(card.read_bytes(), b'b' * 8650752)
            card.write_bytes(b'bad size')
            with self.assertRaisesRegex(ValueError, 'unexpected size'):
                probe.run_probe(root, root / 'private/reject', 0, 1, [], window=True, interactive=True, play_card=card)

    def make_workspace(self, root):
        inputs = root / 'docs/inputs'
        inputs.mkdir(parents=True)
        paths = {
            'runner': 'private/donor/ps2run.exe',
            'core': 'private/fingerprint-check/CORE.GT4',
            'elf': 'private/reconstructed/SCUS_973.28.elf',
            'disc': 'Gran Turismo 4 (USA) (v2.00).iso',
        }
        for name, relative in paths.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(('synthetic ' + name).encode())
            paths[name] = path
        executable = dict(file_pin(paths['runner']), path='private/donor/ps2run.exe')
        (inputs / 'slice114-native-base.json').write_text(json.dumps({'runtime': executable}))
        elf = file_pin(paths['elf'])
        (inputs / 'usa-v2.00-native.json').write_text(json.dumps({
            'native_elf_size': elf['size_bytes'], 'native_elf_sha256': elf['sha256']}))
        (inputs / 'usa-v2.00.json').write_text(json.dumps({
            'disc': file_pin(paths['disc']), 'files': {'/CORE.GT4;1': file_pin(paths['core'])}}))
        return paths

    def test_changed_inputs_are_rejected_before_launch_or_output(self):
        for changed in ('runner', 'core', 'elf', 'disc'):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                paths = self.make_workspace(root)
                paths[changed].write_bytes(b'changed')
                output = root / 'private/new-run'
                with patch.object(probe.subprocess, 'run') as execute:
                    with self.assertRaisesRegex(ValueError, 'pinned size/hash'):
                        probe.run_probe(root, output, 100, 2, [])
                    execute.assert_not_called()
                self.assertFalse(output.exists())

    def test_existing_output_and_outside_private_are_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_workspace(root)
            existing = root / 'private/existing'
            existing.mkdir()
            (existing / 'evidence.txt').write_text('keep')
            for output in (existing, root / 'public-run', root / 'private'):
                with self.subTest(output=output), patch.object(probe.subprocess, 'run') as execute:
                    with self.assertRaises(ValueError):
                        probe.run_probe(root, output, 100, 2, [])
                    execute.assert_not_called()
            self.assertEqual((existing / 'evidence.txt').read_text(), 'keep')

    def test_zero_exit_is_not_completion_when_guest_stopped_early(self):
        stopped = '[ee] stopped after 12 frames, 55 cycles: missing code at 0x00102000\n'
        self.assertFalse(probe.summarize_exit(stopped, 100, 0, False)['completed_requested_frames'])
        self.assertFalse(probe.summarize_exit('', 100, 0, False)['completed_requested_frames'])
        complete = '[ee] stopped after 100 frames, 5500 cycles: frame limit reached\n'
        self.assertTrue(probe.summarize_exit(complete, 100, 0, False)['completed_requested_frames'])
        self.assertFalse(probe.summarize_exit(complete, 100, 0, True)['completed_requested_frames'])
        self.assertFalse(probe.summarize_exit(complete, 100, 9, False)['completed_requested_frames'])

    def test_arguments_keep_disc_path_and_presses_as_literal_values(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            paths = self.make_workspace(root)
            output = root / 'private/new-run'

            def execute(arguments, **options):
                self.assertEqual(arguments[arguments.index('--disc') + 1], str(paths['disc']))
                self.assertEqual(arguments[-2:], ['--press', '30:left+cross:6'])
                self.assertEqual(Path(arguments[1]).name, 'SCUS_973.28')
                self.assertNotIn('shell', options)
                self.assertFalse((output / 'card.ps2').exists())
                options['stdout'].write(b'[ee] stopped after 100 frames, 5500 cycles: frame limit reached\n')
                return subprocess.CompletedProcess(arguments, 0)

            with patch.object(probe.subprocess, 'run', side_effect=execute):
                result = probe.run_probe(root, output, 100, 2, ['30:left+cross:6'])
            self.assertTrue(result['completed_requested_frames'])
            self.assertEqual((output / 'SCUS_973.28').read_bytes(), paths['elf'].read_bytes())
            self.assertEqual(json.loads((output / 'result.json').read_text())['reported_frames'], 100)

    def test_corrupted_elf_copy_is_rejected_before_launch(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_workspace(root)
            output = root / 'private/new-run'

            def corrupt_copy(source, destination):
                destination.write_bytes(b'corrupt copy')

            with patch.object(probe.shutil, 'copyfile', side_effect=corrupt_copy):
                with patch.object(probe.subprocess, 'run') as execute:
                    with self.assertRaisesRegex(ValueError, 'pinned size/hash'):
                        probe.run_probe(root, output, 100, 2, [])
                    execute.assert_not_called()

    def test_pinned_card_seed_is_copied_and_original_is_preserved(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_workspace(root)
            seed = root / 'private/seed.ps2'
            seed.write_bytes(b'synthetic saved profile')
            pin = dict(file_pin(seed), path='private/seed.ps2')
            (root / 'docs/inputs/slice110-external-menu-pilot.json').write_text(
                json.dumps({'artifacts': [pin]}))

            def execute(arguments, **options):
                card = Path(arguments[arguments.index('--memcard') + 1])
                self.assertEqual(card.read_bytes(), seed.read_bytes())
                card.write_bytes(b'guest writes copied card')
                options['stdout'].write(b'[ee] stopped after 100 frames, 5500 cycles: frame limit reached\n')
                return subprocess.CompletedProcess(arguments, 0)

            with patch.object(probe.subprocess, 'run', side_effect=execute):
                result = probe.run_probe(root, root / 'private/reboot', 100, 2, [], seed)
            self.assertTrue(result['completed_requested_frames'])
            self.assertEqual(result['card_seed'], pin)
            self.assertEqual(seed.read_bytes(), b'synthetic saved profile')
            seed.write_bytes(b'changed seed')
            with patch.object(probe.subprocess, 'run') as execute:
                with self.assertRaisesRegex(ValueError, 'pinned size/hash'):
                    probe.run_probe(root, root / 'private/changed-seed', 100, 2, [], seed)
                execute.assert_not_called()
            self.assertFalse((root / 'private/changed-seed').exists())
            for invalid_seed, message in ((root / 'outside.ps2', 'inside private'),
                                          (root / 'private/unpinned.ps2', 'no pinned artifact')):
                with self.subTest(seed=invalid_seed), patch.object(probe.subprocess, 'run') as execute:
                    with self.assertRaisesRegex(ValueError, message):
                        probe.run_probe(root, root / 'private/invalid-seed', 100, 2, [], invalid_seed)
                    execute.assert_not_called()
                self.assertFalse((root / 'private/invalid-seed').exists())

    def test_timeout_preserves_log_and_reports_incomplete_run(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_workspace(root)
            output = root / 'private/new-run'

            def execute(arguments, **options):
                options['stdout'].write(b'partial guest log\n')
                raise subprocess.TimeoutExpired(arguments, options['timeout'])

            with patch.object(probe.subprocess, 'run', side_effect=execute):
                result = probe.run_probe(root, output, 100, 2, [])
            self.assertTrue(result['timed_out'])
            self.assertFalse(result['completed_requested_frames'])
            self.assertEqual((output / 'probe.log').read_text(), 'partial guest log\n')


class Vu1RecordTests(unittest.TestCase):
    def pinned_manifest(self, root, name, runtime_path, mode, **extra):
        evidence = json.loads((root / 'docs/inputs/slice114-native-base.json').read_text())
        runtime = root / runtime_path
        runtime.parent.mkdir(parents=True)
        runtime.write_bytes((root / evidence['runtime']['path']).read_bytes())
        evidence['runtime']['path'] = runtime_path
        evidence.update(profile='performance', perf_schema_version=1,
                        keyboard_input_fix='digital_only_slice136', capture_activation_implemented=False,
                        measurement_mode=mode, vu_scope_profile='unit_split_run_fast_only')
        evidence.update(extra)
        manifest = root / 'docs/inputs' / name
        manifest.write_text(json.dumps(evidence))
        return manifest, evidence

    def vu1_manifest(self, root):
        return self.pinned_manifest(root, probe.VU1_RECORD_MANIFEST, probe.VU1_RECORD_RUNTIME, 'vu-split')

    def test_recording_env_and_summary_in_vu1_mode(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            ProbeTests.make_workspace(self, root)
            manifest, _ = self.vu1_manifest(root)
            seen = {}
            def execute(arguments, **options):
                corpus = Path(options['env']['PS2_VU_RECORD'])
                seen['corpus'] = corpus
                self.assertFalse(corpus.exists())
                corpus.mkdir()
                (corpus / ('A' * 16 + '_0040.bin')).write_bytes(bytes(16384))
                (corpus / 'short_0001.bin').write_bytes(b'x' * 100)
                options['stdout'].write(b'[ee] stopped after 0 frames, 2 cycles: window closed\n')
                return subprocess.CompletedProcess(arguments, 0)
            contaminated = {'PS2_VU_RECORD': 'stale', 'PS2_VU_COUNT': '0', 'PS2_TIME': '1,100'}
            with patch.dict(probe.os.environ, contaminated), patch.object(probe.subprocess, 'run', side_effect=execute):
                result = probe.run_probe(root, root / 'private/vu1', 0, 1, [], runtime_manifest=manifest,
                    window=True, interactive=True, performance=True, vu1_record=True)
            self.assertTrue(result['manual_session_closed'])
            self.assertEqual(seen['corpus'].name, probe.VU1_CORPUS_NAME)
            self.assertEqual(seen['corpus'].parent, (root / 'private/vu1').resolve())
            self.assertEqual(result['vu1_record_environment']['PS2_VU_COUNT'], '1')
            summary = json.loads((seen['corpus'].parent / probe.VU1_CORPUS_SUMMARY).read_text())
            self.assertEqual((summary['file_count'], summary['total_bytes']), (2, 16384 + 100))
            good = next(item for item in summary['files'] if item['name'].endswith('_0040.bin'))
            self.assertEqual((good['key'], good['entry']), ('A' * 16, '0040'))
            self.assertEqual(good['sha256'], hashlib.sha256(bytes(16384)).hexdigest())
            self.assertEqual([item['name'] for item in summary['rejected']], ['short_0001.bin'])
            self.assertEqual(set(summary['rejected'][0]['reasons']), {'wrong_size', 'unexpected_name'})
            self.assertEqual(result['vu1_corpus']['rejected_count'], 1)

    def test_default_vu_split_never_records_or_writes_summary(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            ProbeTests.make_workspace(self, root)
            manifest, _ = self.pinned_manifest(root, probe.PERF_PROFILES['vu-split'][0],
                probe.PERF_PROFILES['vu-split'][1], 'vu-split')
            def execute(arguments, **options):
                self.assertNotIn('PS2_VU_RECORD', options['env'])
                self.assertNotIn('PS2_VU_COUNT', options['env'])
                options['stdout'].write(b'[ee] stopped after 0 frames, 2 cycles: window closed\n')
                return subprocess.CompletedProcess(arguments, 0)
            with patch.dict(probe.os.environ, {'PS2_VU_RECORD': 'stale', 'PS2_VU_COUNT': '1'}), \
                 patch.object(probe.subprocess, 'run', side_effect=execute):
                result = probe.run_probe(root, root / 'private/split', 0, 1, [], runtime_manifest=manifest,
                    window=True, interactive=True, performance=True)
            self.assertNotIn('vu1_corpus', result)
            self.assertFalse((root / 'private/split' / probe.VU1_CORPUS_SUMMARY).exists())

    def test_vu1_manifest_and_flag_must_match(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            ProbeTests.make_workspace(self, root)
            vu1_manifest, _ = self.vu1_manifest(root)
            split_manifest, _ = self.pinned_manifest(root, probe.PERF_PROFILES['vu-split'][0],
                probe.PERF_PROFILES['vu-split'][1], 'vu-split')
            cases = [(split_manifest, True, True), (vu1_manifest, False, True), (vu1_manifest, True, False)]
            for manifest, record, performance in cases:
                with self.subTest(manifest=manifest.name, record=record, performance=performance), \
                     patch.object(probe.subprocess, 'run') as execute:
                    with self.assertRaisesRegex(ValueError, 'VU1|qualified'):
                        probe.run_probe(root, root / 'private/bad', 0, 1, [], runtime_manifest=manifest,
                            window=True, interactive=True, performance=performance, vu1_record=record)
                    execute.assert_not_called()
                    self.assertFalse((root / 'private/bad').exists())

    def test_corpus_must_be_fresh(self):
        with tempfile.TemporaryDirectory() as folder:
            run = Path(folder) / 'session'
            self.assertEqual(probe.prepare_vu1_corpus(run), run / probe.VU1_CORPUS_NAME)
            (run / probe.VU1_CORPUS_NAME).mkdir(parents=True)
            with self.assertRaisesRegex(ValueError, 'already exists'):
                probe.prepare_vu1_corpus(run)

    def test_summary_flags_wrong_size_bad_name_and_non_files(self):
        with tempfile.TemporaryDirectory() as folder:
            corpus = Path(folder) / 'vu1-corpus'
            self.assertEqual(probe.summarize_vu1_corpus(corpus)['file_count'], 0)
            corpus.mkdir()
            (corpus / ('0' * 16 + '_0000.bin')).write_bytes(bytes(16384))
            (corpus / ('1' * 16 + '_0010.bin')).write_bytes(bytes(16383))
            (corpus / 'notes.txt').write_bytes(bytes(16384))
            (corpus / ('2' * 16 + '_0020.bin')).mkdir()
            summary = probe.summarize_vu1_corpus(corpus)
            self.assertEqual(summary['file_count'], 4)
            reasons = {item['name']: set(item['reasons']) for item in summary['rejected']}
            self.assertEqual(reasons['1' * 16 + '_0010.bin'], {'wrong_size'})
            self.assertEqual(reasons['notes.txt'], {'unexpected_name'})
            self.assertIn('not_regular_file', reasons['2' * 16 + '_0020.bin'])
            self.assertEqual(len(summary['rejected']), 3)
            self.assertEqual(summary['files'][0]['sha256'], hashlib.sha256(bytes(16384)).hexdigest())


if __name__ == '__main__':
    unittest.main()
