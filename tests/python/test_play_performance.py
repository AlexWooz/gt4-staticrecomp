import json
import msvcrt
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import play_performance as performance
import play_external_runtime as launcher
from external_runtime_probe import PERF_MANIFEST, PERF_RUNTIME, PERF_PROFILES, VU1_RECORD_MANIFEST, VU1_RECORD_RUNTIME


class PerformanceLauncherTests(unittest.TestCase):
    def test_wrapper_selects_qualified_profile(self):
        with tempfile.TemporaryDirectory() as folder, patch.dict(os.environ):
            os.environ.pop('GT4_FORCE_480P', None)
            root = Path(folder)
            for mode in PERF_PROFILES:
                arguments = ['perf', '--assets-root', str(root)]
                if mode != 'detailed': arguments.extend(['--mode', mode])
                with patch.object(performance, 'ROOT', root), patch.object(performance, 'play') as play, \
                     patch.object(sys, 'argv', arguments):
                    self.assertEqual(performance.main(), 0)
                # Release 1.0.0 W1: --mode main (and, from TASK-036, --mode pcsx2) holds 480p by default;
                # the other modes keep the donor request.
                expected = {'force_480p': True} if mode in ('main', 'pcsx2', 'pcsx2-slice268', 'license-guard', 'pcsx2-coverage') else {}
                play.assert_called_once_with(root, root / 'docs/inputs' / PERF_PROFILES[mode][0], performance=True, **expected)

    def test_main_mode_holds_480p_by_default_with_hidden_opt_out(self):
        with patch.dict(os.environ):
            os.environ.pop('GT4_FORCE_480P', None)
            self.assertTrue(performance.force_480p_requested(performance.argparse.Namespace(mode='main', force_480p=False)))
            os.environ['GT4_FORCE_480P'] = '0'
            self.assertFalse(performance.force_480p_requested(performance.argparse.Namespace(mode='main', force_480p=False)))
            os.environ['GT4_FORCE_480P'] = '1'
            self.assertTrue(performance.force_480p_requested(performance.argparse.Namespace(mode='main', force_480p=False)))
            os.environ.pop('GT4_FORCE_480P', None)
            # pcsx2 holds 480p by default with the same opt-out (owner, 2026-10-16).
            self.assertTrue(performance.force_480p_requested(performance.argparse.Namespace(mode='pcsx2', force_480p=False)))
            self.assertTrue(performance.force_480p_requested(performance.argparse.Namespace(mode='license-guard', force_480p=False)))
            os.environ['GT4_FORCE_480P'] = '0'
            self.assertFalse(performance.force_480p_requested(performance.argparse.Namespace(mode='pcsx2', force_480p=False)))
            os.environ.pop('GT4_FORCE_480P', None)
            # Other modes never default to 480p; --force-480p always does.
            self.assertFalse(performance.force_480p_requested(performance.argparse.Namespace(mode='mtvu', force_480p=False)))
            self.assertTrue(performance.force_480p_requested(performance.argparse.Namespace(mode='mtvu', force_480p=True)))

    def test_main_pin_is_slice178_with_mtvu_runtime(self):
        self.assertEqual(PERF_PROFILES['main'], ('slice178-main.json', 'private/slice178-main-build/build/gt4vulkan.exe'))
        self.assertEqual(PERF_PROFILES['mtvu'][0], 'slice172-mtvu.json')

    def test_lock_failure_and_profile_rejection_preserve_card(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            inputs = root / 'docs/inputs'
            inputs.mkdir(parents=True)
            manifest = inputs / PERF_MANIFEST
            evidence = {'profile': 'performance', 'perf_schema_version': 1,
                        'keyboard_input_fix': 'digital_only_slice136',
                        'capture_activation_implemented': False, 'runtime': {'path': PERF_RUNTIME}}
            manifest.write_text(json.dumps(evidence))
            directory = root / 'private/manual-play'
            directory.mkdir(parents=True)
            card = directory / 'card.ps2'
            card.write_bytes(b'c' * 8650752)
            with patch.object(launcher, 'ROOT', root), patch.object(launcher, 'run_probe') as execute:
                with (directory / 'session.lock').open('w+b') as lock:
                    lock.write(b'0'); lock.flush(); lock.seek(0)
                    msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
                    try:
                        with self.assertRaisesRegex(ValueError, 'already running'):
                            launcher.play(root, manifest, performance=True)
                    finally:
                        lock.seek(0); msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
                execute.assert_not_called()
                manifest.write_text(json.dumps(dict(evidence, profile='old')))
                with self.assertRaisesRegex(ValueError, 'qualified'):
                    launcher.play(root, manifest, performance=True)
            self.assertEqual(card.read_bytes(), b'c' * 8650752)
            self.assertFalse(any(directory.glob('session-*')))

    def test_vu1_record_mode_selects_only_its_pinned_manifest(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            with patch.object(performance, 'ROOT', root), patch.object(performance, 'play') as play, \
                 patch.object(sys, 'argv', ['perf', '--assets-root', str(root), '--mode', 'vu1-record']):
                self.assertEqual(performance.main(), 0)
            play.assert_called_once_with(root, root / 'docs/inputs' / VU1_RECORD_MANIFEST,
                                         performance=True, vu1_record=True)
            with patch.object(performance, 'ROOT', root), patch.object(performance, 'play') as play, \
                 patch.object(sys, 'argv', ['perf', '--assets-root', str(root)]):
                self.assertEqual(performance.main(), 0)
            self.assertEqual(play.call_args.kwargs, {'performance': True})

    def test_vu1_record_launch_passes_flag_and_protects_card(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            inputs = root / 'docs/inputs'
            inputs.mkdir(parents=True)
            manifest = inputs / VU1_RECORD_MANIFEST
            evidence = {'profile': 'performance', 'perf_schema_version': 1,
                        'keyboard_input_fix': 'digital_only_slice136', 'capture_activation_implemented': False,
                        'measurement_mode': 'vu-split', 'vu_scope_profile': 'unit_split_run_fast_only',
                        'runtime': {'path': VU1_RECORD_RUNTIME}, 'default_card_seed': {'path': 'private/seed.ps2'}}
            manifest.write_text(json.dumps(evidence))
            directory = root / 'private/manual-play'
            directory.mkdir(parents=True)
            card = directory / 'card.ps2'
            card.write_bytes(b'c' * 8650752)
            with patch.object(launcher, 'ROOT', root), patch.object(launcher, 'run_probe') as execute:
                self.assertEqual(launcher.play(root, manifest, performance=True, vu1_record=True), None)
                self.assertTrue(execute.call_args.kwargs['vu1_record'])
                self.assertTrue(execute.call_args.kwargs['performance'])
                execute.reset_mock()
                launcher.play(root, manifest, performance=True)
                self.assertFalse(execute.call_args.kwargs['vu1_record'])
            self.assertEqual(card.read_bytes(), b'c' * 8650752)
