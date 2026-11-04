"""TASK-041 (slice198): performance of the PCSX2 GS path. Launcher pins of slice198, the A/B switches the owner uses
(GT4_MTVU_PRIV_ASYNC, GT4_GS_WAKE_BYTES), the test switch GT4_GS_STREAM_HASH, and the statements of the native code that
the F8 [gs-ipc] section and docs/GS-LIVE.md section 9 rely on.
"""
import hashlib
import json
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'tests/python'))
import external_runtime_probe as probe  # noqa: E402
import play_performance as performance  # noqa: E402

ASSETS = Path(os.environ.get('GT4_ASSETS_ROOT', ROOT))
MANIFEST = 'slice198-pcsx2.json'
EXECUTABLE = 'private/slice198-gs-perf-build/build/gt4vulkan.exe'


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source(relative):
    return (ROOT / relative).read_text(encoding='utf-8')


class Slice198PinTests(unittest.TestCase):
    def manifest(self, name=MANIFEST):
        return json.loads((ROOT / 'docs/inputs' / name).read_text(encoding='utf-8'))

    def test_mode_pcsx2_slice198_stays_selectable_and_slice196_stays(self):
        # Since slice199 --mode pcsx2 points at slice199 (same game build, gt4-gs 0.4.0); slice198 is kept for A/B.
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice198'], (MANIFEST, EXECUTABLE))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice196'],
                         ('slice196-pcsx2.json', 'private/slice196-gs-deinterlace-build/build/gt4vulkan.exe'))
        self.assertIn(MANIFEST, probe.VU1_AOT_MANIFESTS)
        self.assertIn(MANIFEST, probe.VU0_AOT_MANIFESTS)
        self.assertIn('pcsx2-slice198', performance.MODES)
        self.assertIn('pcsx2-slice196', performance.MODES)


    def test_launcher_pin_keeps_the_slice196_inputs(self):
        manifest, previous = self.manifest(), self.manifest('slice196-pcsx2.json')
        self.assertEqual(manifest['slice'], 198)
        self.assertEqual(manifest['measurement_mode'], 'pcsx2')
        self.assertEqual(manifest['runtime']['path'], EXECUTABLE)
        # Same capsules, shaders, VU tables, card seeds and gt4-gs package: the changes are native game-side code only.
        for key in ('source_capsule', 'port_capsule', 'shader_capsule', 'vu1_aot', 'vu0_aot', 'card_seeds',
                    'frozen_game_library', 'gs_stream', 'runtime_dependencies'):
            self.assertEqual(manifest[key], previous[key], key)
        self.assertIn('GT4_MTVU_PRIV_ASYNC', manifest['classification'])

    def test_executable_pins_match_the_build_under_test(self):
        build = os.environ.get('GT4_NATIVE_BUILD')
        if not build or Path(build).resolve() != (ASSETS / 'private/slice198-gs-perf-build/build').resolve():
            self.skipTest('GT4_NATIVE_BUILD is not the slice198 build')
        manifest = self.manifest()
        for entry in [manifest['runtime']] + manifest['runtime_dependencies']:
            path = ASSETS / entry['path']
            self.assertEqual((path.stat().st_size, sha256(path)), (entry['size_bytes'], entry['sha256']), entry['path'])


class Slice199PinTests(unittest.TestCase):
    def manifest(self, name='slice199-pcsx2.json'):
        return json.loads((ROOT / 'docs/inputs' / name).read_text(encoding='utf-8'))

    def test_mode_pcsx2_slice199_keeps_the_slice198_game_build(self):
        # Since slice200 --mode pcsx2 moved on (slice202 now: TASK-058/059 plus TASK-060/062); slice199 stays for A/B.
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice199'], ('slice199-pcsx2.json', EXECUTABLE))
        self.assertIn('pcsx2-slice199', performance.MODES)
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice198'], (MANIFEST, EXECUTABLE))
        self.assertIn('slice199-pcsx2.json', probe.VU1_AOT_MANIFESTS)
        self.assertIn('slice199-pcsx2.json', probe.VU0_AOT_MANIFESTS)
        self.assertIn('pcsx2', performance.MODES)
        self.assertIn('pcsx2-slice198', performance.MODES)
        self.assertIn('pcsx2-slice197', performance.MODES)

    def test_slice199_differs_from_slice198_only_in_the_gs_package(self):
        manifest, previous = self.manifest(), self.manifest(MANIFEST)
        self.assertEqual(manifest['slice'], 199)
        self.assertEqual(manifest['measurement_mode'], 'pcsx2')
        self.assertEqual(manifest['runtime'], previous['runtime'])
        for key in ('source_capsule', 'port_capsule', 'shader_capsule', 'vu1_aot', 'vu0_aot', 'card_seeds',
                    'frozen_game_library', 'environment', 'toolchain'):
            self.assertEqual(manifest[key], previous[key], key)
        self.assertEqual(manifest['gs_stream']['gs_process'], 'private/slice197-gt4gs-release-pkg/gt4-gs.exe')
        self.assertEqual(manifest['gs_stream']['gs_package']['zip']['name'], 'gt4-gs-0.4.0-windows-x64.zip')
        self.assertEqual(manifest['gs_stream']['gs_package']['tree_sha256'],
                         'e9402cf27a2aab468ff66be0dcf289d1c40868f68eba5033ccbb1de40835f223')
        others = [item for item in manifest['runtime_dependencies'] if 'gt4-gs' not in item['path']]
        self.assertEqual(others, [item for item in previous['runtime_dependencies'] if 'gt4-gs' not in item['path']])
        gs_pin = [item for item in manifest['runtime_dependencies'] if item['path'].endswith('gt4-gs.exe')]
        self.assertEqual(gs_pin, [{'path': 'private/slice197-gt4gs-release-pkg/gt4-gs.exe', 'size_bytes': 8928256,
                                   'sha256': '1c4c45daf478a44ec49b3f5fffdd09373406c7ac3d8ea9b82b6e2f34c96fb4f1'}])
        self.assertIn('GT4_MTVU_PRIV_ASYNC', manifest['classification'])
        self.assertIn('GT4GS_ONLY', manifest['gs_stream']['gs_package']['source'])

class SwitchTests(unittest.TestCase):
    def test_privileged_async_switch_default_matches_the_native_default(self):
        header = source('native/mtvu_runtime.hpp')
        native_default = re.search(r'kPrivilegedAsyncDefault = (true|false);', header).group(1)
        self.assertEqual(probe.MTVU_SWITCHES['GT4_MTVU_PRIV_ASYNC'], '1' if native_default == 'true' else '0')
        self.assertIn('parse_flag("GT4_MTVU_PRIV_ASYNC"', source('native/mtvu_runtime.cpp'))

    def test_privileged_async_bad_value_is_refused_before_output(self):
        import test_external_runtime_probe as probe_tests
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            probe_tests.ProbeTests().make_workspace(root)
            with patch.dict(probe.os.environ, {'GT4_MTVU_PRIV_ASYNC': 'yes'}), \
                    patch.object(probe.subprocess, 'run') as run:
                with self.assertRaisesRegex(ValueError, 'GT4_MTVU_PRIV_ASYNC must be 0 or 1'):
                    probe.run_probe(root, root / 'private/refused', 100, 1, [])
                run.assert_not_called()
            self.assertFalse((root / 'private/refused').exists())

    def test_wake_bytes_reaches_owner_runs_and_the_hash_is_test_only(self):
        self.assertIn('GT4_GS_WAKE_BYTES', probe.GS_USER_SETTINGS)
        self.assertIn('GT4_GS_WAKE_BYTES', probe.GS_STREAM_NAMES)
        self.assertIn('GT4_GS_STREAM_HASH', probe.GS_STREAM_NAMES)
        self.assertNotIn('GT4_GS_STREAM_HASH', probe.GS_USER_SETTINGS)
        # TASK-117: the no-flare answer is a game-side switch, so headless runs keep it and owner runs use the default.
        self.assertIn('GT4_GS_HIDE_FLARES', probe.GS_STREAM_NAMES)
        self.assertNotIn('GT4_GS_HIDE_FLARES', probe.GS_USER_SETTINGS)
        environment = {'GT4_GS_WAKE_BYTES': '0', 'GT4_GS_STREAM_HASH': '1', 'GT4_RENDERER': 'donor'}
        evidence = {'gs_stream': {'renderer': 'pcsx2', 'gs_process': 'private/x/gt4-gs.exe'},
                    'runtime_dependencies': [{'path': 'private/x/gt4-gs.exe', 'sha256': '0' * 64}]}
        probe.gs_stream_environment(environment, evidence, ROOT, performance=True)
        self.assertEqual(environment.get('GT4_GS_WAKE_BYTES'), '0')
        self.assertNotIn('GT4_GS_STREAM_HASH', environment)
        self.assertEqual(environment['GT4_RENDERER'], 'pcsx2')


class NativeStatementTests(unittest.TestCase):
    def test_default_wake_threshold_is_256_kib(self):
        self.assertIn('kDefaultDataWakeBytes = 256u << 10;', source('native/gs_stream/gs_stream_ipc.hpp'))
        self.assertIn('"GT4_GS_WAKE_BYTES"', source('native/gs_stream/gs_stream_client.cpp'))

    def test_only_gif_and_priv_records_are_batched(self):
        body = source('native/gs_stream/gs_stream_ipc.cpp')
        body = body[body.index('void Session::post('):]
        body = body[:body.index('\n}') + 2]
        self.assertIn('type == RecordType::gif || type == RecordType::priv', body)
        # The consumer is woken before the producer waits for ring space.
        self.assertLess(body.index('wake_if_waiting'), body.index('wait_until'))

    def test_privileged_sync_is_skipped_only_with_an_ordered_consumer(self):
        runtime = source('native/mtvu_runtime.cpp')
        self.assertIn('r.settings.privileged_async && r.ordered_privileged != nullptr && enabled()', runtime)
        bridge = source('native/mtvu_bridge.cpp')
        self.assertIn('if (gt4::gs_stream::stream_renderer())', bridge)
        self.assertIn('set_ordered_privileged_consumer(&gt4::gs_stream::post_privileged_now)', bridge)

    def test_f8_section_has_the_summary_and_per_register_lines(self):
        client = source('native/gs_stream/gs_stream_client.cpp')
        for text in ('[gs-ipc] summary: EE critical path', '[gs-ipc] privileged writes/frame',
                     '[gs-ipc] EE wait for the VU1 worker before privileged writes',
                     '[gs-ipc] GS process woken from sleep per frame', '[gs-ipc] GS process CPU thread (STATS vsyncs'):
            self.assertIn(text, client)


if __name__ == '__main__':
    unittest.main()
