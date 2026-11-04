"""TASK-084: headless frame-cost harness, capsule pins and launcher pins (docs/FRAME-COST-STUDY.md).

Pure checks; they never start the game and never read private game data. The capsule/launcher pin tests build their own
synthetic capsules in a temporary folder, so they run everywhere.
"""
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import frame_cost_headless as harness  # noqa: E402
import pin_frame_cost_guard as guard_pin  # noqa: E402
import pin_frame_cost_launcher as launcher_pin  # noqa: E402
import pin_frame_cost_runtime as runtime_pin  # noqa: E402
import prepare_perf_runtime as perf  # noqa: E402
import prepare_license_guard as guard  # noqa: E402


class SceneScheduleTests(unittest.TestCase):
    def test_scenes_match_the_study_schedules(self):
        self.assertEqual(harness.SCENES['menus']['frames'], 4200)
        self.assertEqual(len(harness.SCENES['menus']['presses']), 12)
        self.assertEqual(harness.SCENES['day']['frames'], 16000)
        self.assertEqual(harness.SCENES['night']['frames'], 16000)
        # The race schedules end with a long cross hold that keeps the car on track to the frame limit.
        self.assertEqual(harness.SCENES['day']['presses'][-1], '8800:cross:7000')
        self.assertEqual(harness.SCENES['night']['presses'][-1], '9200:cross:7000')
        # The showroom scene is the day-race schedule cut right after the car showroom (VBlanks 6000-7000).
        self.assertEqual(harness.SCENES['showroom']['frames'], 7000)
        self.assertEqual(harness.SCENES['showroom']['presses'], harness.SCENES['day']['presses'])
        for scene in harness.SCENES.values():
            frames = [int(press.split(':')[0]) for press in scene['presses']]
            self.assertEqual(frames, sorted(frames), 'presses are in frame order')
            for press in scene['presses']:
                frame, button, count = press.split(':')
                self.assertTrue(frame.isdigit() and button.isalpha() and count.isdigit(), press)


class GsIniTests(unittest.TestCase):
    def render(self, dump_lines='', readback=0):
        return harness.GS_INI.format(scale=1, readback=readback, cache='C:/cache', data='C:/data',
                                     dump_lines=dump_lines)

    def test_readback_option_maps_to_the_download_mode(self):
        # TASK-116 step 0: --readback writes the launcher's HWDownloadMode into the generated ini.
        self.assertEqual(harness.READBACK_MODES, {'accurate': 0, 'no-readbacks': 2})
        self.assertIn('HWDownloadMode = 0\n', self.render())
        self.assertIn('HWDownloadMode = 2\n', self.render(readback=harness.READBACK_MODES['no-readbacks']))

    def test_no_dump_keys_when_dumps_are_off(self):
        text = self.render()
        self.assertIn('Surfaceless = true', text)
        self.assertNotIn('DumpEvery', text)
        self.assertNotIn('DumpDir', text)

    def test_dump_keys_when_dumps_are_on(self):
        text = self.render('DumpDir = C:/run/gs-frames\nDumpEvery = 1000\n')
        self.assertIn('DumpEvery = 1000', text)
        self.assertIn('DumpDir = C:/run/gs-frames', text)
        # gt4-gs treats DumpEvery = 0 as "every frame"; the harness never writes it.
        self.assertNotIn('DumpEvery = 0', text)

    def test_texture_key_is_added_only_when_a_pack_is_named(self):
        text = self.render()
        self.assertNotIn('Textures =', text)
        textured = harness.with_textures(text, 'C:/pack')
        self.assertIn('[Folders]\nTextures = C:/pack\n', textured)
        self.assertEqual(textured.count('[Folders]'), 1)
        # PCSX2's custom-texture loader switch defaults to off; the launcher always writes it with a pack.
        self.assertIn('[EmuCore/GS]\nLoadTextureReplacements = true\n', textured)
        self.assertEqual(textured.count('[EmuCore/GS]'), 1)
        with self.assertRaises(ValueError):
            harness.with_textures('no folders section', 'C:/pack')


class GameArgumentsTests(unittest.TestCase):
    def test_spec2_overrides_reach_the_command_line(self):
        arguments = harness.game_arguments('gt4vulkan-spec2.exe', 'run/SCUS_973.28', 'spec2.iso', 'run/card.ps2',
                                           4200, ['600:start:30'])
        self.assertEqual(arguments[0], 'gt4vulkan-spec2.exe')
        self.assertIn('spec2.iso', arguments)
        self.assertEqual(arguments[arguments.index('--frames') + 1], '4200')
        self.assertIn('600:start:30', arguments)
        # pcsx2 mode never passes the donor dump switches (the GS process dumps with DumpEvery).
        self.assertNotIn('--frames-dir', arguments)

    def test_donor_mode_keeps_the_frame_dump_switches(self):
        arguments = harness.game_arguments('gt4vulkan.exe', 'run/SCUS_973.28', 'day.iso', 'run/card.ps2',
                                           100, [], donor_frames_dir='run/frames', frame_every=50)
        self.assertIn('--frames-dir', arguments)
        self.assertEqual(arguments[arguments.index('--frame-every') + 1], '50')


class CardOverrideTests(unittest.TestCase):
    def test_card_option_is_offered_for_owner_card_runs(self):
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/frame_cost_headless.py'), '--help'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--card', result.stdout)


class RuntimePinTests(unittest.TestCase):
    def make_capsule(self, root, name='slice900-test-runtime'):
        capsule = root / 'private' / name
        capsule.mkdir(parents=True)
        sources = {'system.cpp': b'// system\n', 'dmac.cpp': b'// dmac\n'}
        receipt = {'donor_revision': 'x', 'donor_sha256': {}, 'source_sha256': {},
                   'mtvu_runtime_profile': 'task084-frame-cost-v1', 'gs_stream_profile': 'task035-seam-v1'}
        for source, data in sources.items():
            (capsule / source).write_bytes(data)
            receipt['source_sha256'][source] = hashlib.sha256(data).hexdigest()
        (capsule / 'runtime.json').write_text(json.dumps(receipt), encoding='utf-8')
        base = root / 'docs/inputs/slice200-gs-stream-runtime.json'
        base.parent.mkdir(parents=True)
        base.write_text(json.dumps({'inputs': {'PORT': {'main.cpp': 'abc'}, 'SHADERS': {}}}), encoding='utf-8')
        return capsule

    def test_runtime_pin_copies_hashes_and_refuses_an_existing_file(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            capsule = self.make_capsule(root)
            with patch.object(runtime_pin, 'ROOT', root):
                manifest, output = runtime_pin.pin(capsule.name, root)
                self.assertEqual(manifest['inputs']['PORT'], {'main.cpp': 'abc'})
                self.assertEqual(manifest['source_sha256']['system.cpp'],
                                 hashlib.sha256((capsule / 'system.cpp').read_bytes()).hexdigest())
                self.assertEqual(manifest['receipt_sha256'],
                                 hashlib.sha256((capsule / 'runtime.json').read_bytes()).hexdigest())
                self.assertEqual(manifest['mtvu_runtime_profile'], 'task084-frame-cost-v1')
                with self.assertRaisesRegex(ValueError, 'preserve the existing pin'):
                    runtime_pin.pin(capsule.name, root)

    def test_runtime_pin_rejects_a_source_changed_after_the_receipt(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            capsule = self.make_capsule(root)
            (capsule / 'system.cpp').write_bytes(b'// edited\n')
            with patch.object(runtime_pin, 'ROOT', root):
                with self.assertRaisesRegex(ValueError, 'changed after the receipt'):
                    runtime_pin.pin(capsule.name, root)


class GuardPinTests(unittest.TestCase):
    def test_guard_pin_requires_the_frame_counters_profile(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            capsule = root / 'private/slice900-license-guard-runtime'
            capsule.mkdir(parents=True)
            (root / 'docs/inputs').mkdir(parents=True)
            (capsule / 'kernel.cpp').write_bytes(b'// kernel\n')
            receipt = {'source_sha256': {'kernel.cpp': hashlib.sha256(b'// kernel\n').hexdigest()}}
            (capsule / 'runtime.json').write_text(json.dumps(receipt), encoding='utf-8')
            with patch.object(guard_pin, 'ROOT', root):
                with self.assertRaisesRegex(ValueError, 'frame-counters'):
                    guard_pin.pin(capsule.name, root)
            receipt['frame_counters_profile'] = 'task084-frame-counters-v1'
            (capsule / 'runtime.json').write_text(json.dumps(receipt), encoding='utf-8')
            with patch.object(guard_pin, 'ROOT', root):
                manifest, output = guard_pin.pin(capsule.name, root)
                self.assertEqual(manifest['receipt_sha256'],
                                 hashlib.sha256((capsule / 'runtime.json').read_bytes()).hexdigest())
                self.assertTrue(output.exists())


class LauncherPinTests(unittest.TestCase):
    def test_launcher_pin_maps_the_slice_fields(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            build = root / 'private/slice900-build/build'
            build.mkdir(parents=True)
            exe = build / 'gt4vulkan.exe'
            exe.write_bytes(b'fake executable')
            (root / 'docs/inputs').mkdir(parents=True)
            (root / 'docs/inputs/slice240-pcsx2.json').write_text(json.dumps({
                'slice': 240, 'runtime': {'path': 'old', 'size_bytes': 0, 'sha256': '0'},
                'card_seeds': [{'path': 'seed'}], 'inputs': {}, 'purpose': 'old'}), encoding='utf-8')
            (root / 'docs/inputs/slice900-test-license-guard-runtime.json').write_text(json.dumps({
                'receipt_sha256': 'receipt', 'source_sha256': {'kernel.cpp': 'hash'}}), encoding='utf-8')
            with patch.object(launcher_pin, 'ROOT', root):
                manifest, output = launcher_pin.pin(900, 'slice900-test-runtime',
                                                    'slice900-test-license-guard-runtime',
                                                    'private/slice900-build/build', root)
            self.assertEqual(manifest['slice'], 900)
            self.assertEqual(manifest['runtime']['size_bytes'], len(b'fake executable'))
            self.assertEqual(manifest['runtime']['sha256'], hashlib.sha256(b'fake executable').hexdigest())
            self.assertEqual(manifest['source_capsule'], 'private/slice900-test-runtime')
            self.assertEqual(manifest['license_guard_receipt_sha256'], 'receipt')
            self.assertEqual(manifest['card_seeds'], [{'path': 'seed'}])
            self.assertTrue(output.exists())


class FrameCostPatchTests(unittest.TestCase):
    def test_frame_cost_hooks_are_only_added_with_the_option(self):
        system = ('void run_due_events() {\n    gt4::mtvu::pump();\n    sync_iop();\n}\n'
                  '        c.pc &= ~kUnwind;\n        run_due_events();\n        kernel::service(c);\n'
                  'void idle(Cpu &c) {\n    std::uint64_t next = next_event_time();\n')
        derived = perf.apply_frame_cost('system.cpp', system)
        self.assertIn('gt4::perf::Category::run_due_events', derived)
        self.assertIn('gt4::perf::Category::kernel_service', derived)
        self.assertIn('gt4::perf::Category::guest_idle', derived)
        # The default derivation of the same text keeps the old hooks only (reversal is checked inside apply_frame_cost).
        self.assertNotIn('Category::guest_idle', system)

    def test_kernel_counters_are_added_on_top_of_the_halt_dump(self):
        source = ('#include "ps2/system.hpp"\n'
                  'void switch_to(Cpu &c, Thread *next) {\n'
                  'void handle_syscall(Cpu &c, std::uint32_t pc) {\n    std::uint32_t n = c.r[3].w[0];\n')
        derived = perf.apply_kernel_frame_counters(source)
        self.assertIn('#include "perf_probe.hpp"', derived)
        self.assertIn('gt4::perf::count_thread_switch();', derived)
        self.assertIn('gt4::perf::SyscallScope gt4_syscall_scope(c.r[3].w[0]);', derived)

    def test_derivations_without_the_option_are_unchanged(self):
        # The frame-cost tables must not leak into the --mtvu or --gs-stream derivations of older capsules: without
        # --frame-cost the GS stream walk keeps its historical form (no fast-path guard, no scope).
        self.assertIn('system.cpp', perf.FRAME_COST_PATCHES)
        self.assertIn('dmac.cpp', perf.FRAME_COST_PATCHES)
        before, after = perf.GS_STREAM_PATCHES['gif.cpp'][2]
        self.assertIn('walk(g_paths[path], data, qwc, stream_sink);', after)
        self.assertNotIn('stream_signals_pre_applied', after)
        # And the frame-cost GS stream patch is exactly the guard plus the scope.
        before_fc, after_fc = perf.FRAME_COST_GS_STREAM_PATCHES['gif.cpp'][0]
        self.assertIn('stream_signals_pre_applied', after_fc)
        self.assertIn('Category::gif_walk', after_fc)


if __name__ == '__main__':
    unittest.main()
