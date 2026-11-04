"""TASK-060/061/062 (slice201): cursor auto-hide, the "Compiling shaders" notice and the performance overlay, protocol 1.2
and the stable gt4-gs 1.0.0 pin. Launcher pins, the GT4_CURSOR_AUTOHIDE switch, and the statements of the native code
that docs/GS-STREAM-PROTOCOL.md (version 1.2) and docs/GS-LIVE.md section 11 rely on.
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
import gs_settings  # noqa: E402
import play_performance as performance  # noqa: E402

ASSETS = Path(os.environ.get('GT4_ASSETS_ROOT', ROOT))
MANIFEST = 'slice201-pcsx2.json'
EXECUTABLE = 'private/slice201-osd-build/build/gt4vulkan.exe'
PACKAGE = 'private/slice201-gt4gs-release-pkg'


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source(relative):
    return (ROOT / relative).read_text(encoding='utf-8')


def manifest(name=MANIFEST):
    return json.loads((ROOT / 'docs/inputs' / name).read_text(encoding='utf-8'))


class Slice201PinTests(unittest.TestCase):
    def test_mode_pcsx2_slice201_stays_and_pcsx2_selects_slice202(self):
        # slice202 combines slice200 (TASK-058/059) and slice201 (TASK-060/062) with the same gt4-gs 1.0.0 package.
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice201'], (MANIFEST, EXECUTABLE))
        # slice206 (TASK-017) is the mode pcsx2 pin now; slice203 stays as pcsx2-slice203, slice202 as pcsx2-slice202.
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice202'], ('slice202-pcsx2.json', 'private/slice202-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2'], ('slice279-pcsx2.json', 'private/slice279-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice268'], ('slice268-pcsx2.json', 'private/slice268-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.SPEC2_RELEASE, ('slice280-spec2.json', 'private/slice280-build/build/gt4vulkan-spec2.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice215'], ('slice215-pcsx2.json', 'private/slice215-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice214'], ('slice214-pcsx2.json', 'private/slice214-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice213'], ('slice213-pcsx2.json', 'private/slice213-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice211'], ('slice211-pcsx2.json', 'private/slice211-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice210'], ('slice210-pcsx2.json', 'private/slice210-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice209'], ('slice209-pcsx2.json', 'private/slice209-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice206'], ('slice206-pcsx2.json', 'private/slice206-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice203'], ('slice203-pcsx2.json', 'private/slice203-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice200'], ('slice200-pcsx2.json', 'private/slice200-build/build/gt4vulkan.exe'))
        combined = manifest('slice202-pcsx2.json')
        self.assertEqual(combined['gs_stream'], manifest()['gs_stream'])
        self.assertEqual(combined['runtime_dependencies'], manifest()['runtime_dependencies'])
        for key in ('port_capsule', 'memcard_capsule', 'memcard_capsule_sha256'):
            self.assertEqual(combined[key], manifest('slice200-pcsx2.json')[key], key)
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice199'],
                         ('slice199-pcsx2.json', 'private/slice198-gs-perf-build/build/gt4vulkan.exe'))
        for name in ('pcsx2', 'pcsx2-slice201', 'pcsx2-slice200', 'pcsx2-slice199', 'pcsx2-slice198', 'pcsx2-slice197', 'pcsx2-slice196'):
            self.assertIn(name, performance.MODES)
        self.assertIn(MANIFEST, probe.VU1_AOT_MANIFESTS)
        self.assertIn(MANIFEST, probe.VU0_AOT_MANIFESTS)

    def test_slice201_pins_the_published_stable_gt4gs_1_0_0(self):
        pin, previous = manifest(), manifest('slice199-pcsx2.json')
        self.assertEqual(pin['slice'], 201)
        self.assertEqual(pin['measurement_mode'], 'pcsx2')
        self.assertEqual(pin['runtime']['path'], EXECUTABLE)
        for key in ('source_capsule', 'port_capsule', 'shader_capsule', 'vu1_aot', 'vu0_aot', 'card_seeds',
                    'frozen_game_library', 'environment', 'toolchain'):
            self.assertEqual(pin[key], previous[key], key)
        stream = pin['gs_stream']
        package = stream['gs_package']
        self.assertEqual(stream['gs_process'], PACKAGE + '/gt4-gs.exe')
        self.assertEqual(package['directory'], PACKAGE)
        self.assertEqual(package['files'], 140)
        self.assertEqual(package['zip']['name'], 'gt4-gs-1.0.0-windows-x64.zip')
        self.assertEqual(package['zip']['sha256'], '2e8780196e9cb1ab134aabed464fa156a1a824db8e762304f4025c04f6f3c5a2')
        self.assertEqual(package['release'], 'https://github.com/moesuito/gt4recomp-gs/releases/tag/gt4gs-v1.0.0')
        self.assertIn('stable release', package['source'])
        self.assertIn('1.2', stream['protocol'])
        others = [item for item in pin['runtime_dependencies'] if 'gt4-gs' not in item['path']]
        self.assertEqual(others, [item for item in previous['runtime_dependencies'] if 'gt4-gs' not in item['path']])
        gs_pin = [item for item in pin['runtime_dependencies'] if item['path'].endswith('gt4-gs.exe')]
        self.assertEqual(gs_pin, [{'path': PACKAGE + '/gt4-gs.exe', 'size_bytes': 8955392,
                                   'sha256': '53464022052d26b99e6779cd352cc65e4ce492ef7fd320d14d30860095bb4b61'}])

    def test_pinned_files_match_when_present(self):
        pin = manifest()
        runtime = ASSETS / pin['runtime']['path']
        if not runtime.exists():
            self.skipTest('slice201 build not present')
        for entry in [pin['runtime']] + pin['runtime_dependencies']:
            path = ASSETS / entry['path']
            self.assertEqual((path.stat().st_size, sha256(path)), (entry['size_bytes'], entry['sha256']), entry['path'])
        package = pin['gs_stream']['gs_package']
        self.assertEqual(probe.package_tree_digest(ASSETS / package['directory']), (package['tree_sha256'], package['files']))


class CursorSwitchTests(unittest.TestCase):
    def test_cursor_switch_is_validated_and_recorded(self):
        import test_external_runtime_probe as probe_tests
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            probe_tests.ProbeTests().make_workspace(root)
            with patch.dict(probe.os.environ, {'GT4_CURSOR_AUTOHIDE': 'off'}), \
                    patch.object(probe.subprocess, 'run') as run:
                with self.assertRaisesRegex(ValueError, 'GT4_CURSOR_AUTOHIDE must be 0 or 1'):
                    probe.run_probe(root, root / 'private/refused', 100, 1, [])
                run.assert_not_called()
            self.assertFalse((root / 'private/refused').exists())

    def test_native_parser_and_row_agree(self):
        native = source('native/cursor_autohide.cpp')
        self.assertIn('GT4_CURSOR_AUTOHIDE must be unset, 0 or 1', native)
        self.assertIn('kDefaultHideDelayMs = 3000', source('native/cursor_autohide.hpp'))
        row = gs_settings.rows_by_id(gs_settings.load_table())['cursor_autohide']
        self.assertEqual((row['env'], row['default'], row['written']), ('GT4_CURSOR_AUTOHIDE', 'true', 'never'))
        self.assertIn('parse_policy(std::getenv("GT4_CURSOR_AUTOHIDE"))', source('native/window.cpp'))
        self.assertIn('set_cursor_policy(', source('native/window.cpp'))

    def test_ctest_registers_the_cursor_fixture(self):
        self.assertIn('add_test(NAME native_cursor_autohide COMMAND $<TARGET_FILE:cursor_autohide_fixture>)',
                      source('CMakeLists.txt'))


class ProtocolTests(unittest.TestCase):
    def test_version_1_2_records(self):
        header = source('native/gs_stream/gs_stream_protocol.h')
        self.assertIn('kVersionMinor = 3;', header)  # 1.3 (TASK-080) keeps every 1.2 record
        self.assertIn('osd_stats = 11,', header)
        self.assertIn('cursor = 6,', header)
        self.assertIn('kReadyFlagOsdStats = 8u;', header)
        self.assertIn('static_assert(sizeof(OsdStatsPayload) == 64);', header)
        self.assertIn('kCursorFlagAutoHide = 1u;', header)

    def test_client_sends_1_2_records_only_to_a_1_2_consumer(self):
        client = source('native/gs_stream/gs_stream_client.cpp')
        self.assertIn('if (session_ && ready_.version_minor >= 2)', client)
        self.assertIn('osd_wanted_ = ready_.version_minor >= 2 && (ready_.flags & proto::kReadyFlagOsdStats) != 0;', client)
        # OSD_STATS go to the ring only: never through emit() (capture file and stream hash).
        body = client[client.index('void osd_after_vsync()'):]
        body = body[:body.index('\n  }\n')]
        self.assertIn('session_->post(RecordType::osd_stats', body)
        self.assertNotIn('emit(', body)
        self.assertIn('constexpr std::uint64_t kOsdStatsIntervalUs = 500000;', source('native/gs_stream/gs_stream_client.hpp'))

    def test_protocol_document_names_version_1_2(self):
        text = source('docs/GS-STREAM-PROTOCOL.md')
        self.assertTrue(text.startswith('# GS stream protocol, version 1.3 (normative)'))
        for phrase in ('6 cursor (version 1.2', '| OSD_STATS | 11 |', '8 (version 1.2) the consumer draws a performance overlay'):
            self.assertIn(phrase, text)


if __name__ == '__main__':
    unittest.main()
