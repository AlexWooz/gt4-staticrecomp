"""TASK-079 GT4_FORCE_WIDESCREEN: probe environment, mode pin and the pinned ELF records behind the native write.

The C++ fixture (native_race_widescreen) covers the guest-RAM logic on synthetic RAM. These tests cover the launcher
side (scripts/external_runtime_probe.py) and, when the pinned ELF is present, the preset records that the native
write copies (func_00439B30 initialises them from D_006A6188 and D_006A61B8).
"""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import external_runtime_probe as probe
import test_external_runtime_probe as probe_tests

# private/ is not in the worktree: the pinned ELF comes from the assets checkout (GT4_ASSETS_ROOT, default: main).
ELF = Path(os.environ.get('GT4_ASSETS_ROOT', 'C:/Antigravity/gt4-staticrecomp')) / 'private/reconstructed/SCUS_973.28.elf'


def elf_bytes(vaddr, length):
    data = ELF.read_bytes()
    phoff = struct.unpack_from('<I', data, 0x1C)[0]
    phnum = struct.unpack_from('<H', data, 0x2C)[0]
    for index in range(phnum):
        kind, offset, segment_vaddr, _, file_size, _, _, _ = struct.unpack_from('<8I', data, phoff + index * 32)
        if kind == 1 and segment_vaddr <= vaddr and vaddr + length <= segment_vaddr + file_size:
            start = offset + (vaddr - segment_vaddr)
            return data[start:start + length]
    raise AssertionError(f'0x{vaddr:08X} is not in a loaded segment')


class WidescreenElfRecordTests(unittest.TestCase):
    """The two source records of the aspect presets (Confirmed in slice211 from the pinned ELF)."""

    @classmethod
    def setUpClass(cls):
        if not ELF.is_file():
            raise unittest.SkipTest('pinned ELF not present')

    def test_source_records_are_4_3_and_wide(self):
        four_three = struct.unpack('<11f', elf_bytes(0x6A6188, 44))
        wide = struct.unpack('<11f', elf_bytes(0x6A61B8, 44))
        self.assertEqual((four_three[3], four_three[4]), (275.0, 205.0))
        self.assertEqual((wide[3], wide[4]), (425.0, 240.0))
        self.assertLess(four_three[3] / four_three[4], 1.5)
        self.assertGreaterEqual(wide[3] / wide[4], 1.5)


class WidescreenProbeTests(unittest.TestCase):
    def run_probe(self, root, name, environment, **options):
        def execute(arguments, **kwargs):
            kwargs['stdout'].write(b'[ee] stopped after 100 frames, 1 cycles: frame limit reached\n')
            return subprocess.CompletedProcess(arguments, 0)
        with patch.dict(probe.os.environ, environment, clear=True), \
             patch.object(probe.subprocess, 'run', side_effect=execute) as run:
            result = probe.run_probe(root, root / 'private' / name, 100, 1, [], **options)
        return result, run

    def test_unset_passes_the_on_default_and_records_it(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            probe_tests.ProbeTests().make_workspace(root)
            result, run = self.run_probe(root, 'default', {})
            self.assertEqual(result['force_widescreen_environment'], {'GT4_FORCE_WIDESCREEN': '1'})
            self.assertEqual(run.call_args.kwargs['env']['GT4_FORCE_WIDESCREEN'], '1')

    def test_zero_is_passed_through_and_recorded(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            probe_tests.ProbeTests().make_workspace(root)
            result, run = self.run_probe(root, 'off', {'GT4_FORCE_WIDESCREEN': '0'})
            self.assertEqual(result['force_widescreen_environment'], {'GT4_FORCE_WIDESCREEN': '0'})
            self.assertEqual(run.call_args.kwargs['env']['GT4_FORCE_WIDESCREEN'], '0')

    def test_unknown_values_refuse_before_any_output(self):
        for value in ('', 'yes', '16:9', '2'):
            with tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                probe_tests.ProbeTests().make_workspace(root)
                with patch.dict(probe.os.environ, {'GT4_FORCE_WIDESCREEN': value}, clear=True), \
                     patch.object(probe.subprocess, 'run') as run:
                    with self.assertRaisesRegex(ValueError, 'GT4_FORCE_WIDESCREEN must be 0 or 1'):
                        probe.run_probe(root, root / 'private/unknown', 100, 1, [])
                    run.assert_not_called()
                self.assertFalse((root / 'private/unknown').exists())

    def test_pcsx2_mode_is_slice217_and_older_builds_are_history(self):
        # TASK-080 (slice215, P2 slice217): the TASK-079 hold is still in it (hardened: settle rule); GT4_FORCE_WIDESCREEN
        # stays.
        self.assertEqual(probe.PERF_PROFILES['pcsx2'], ('slice279-pcsx2.json', 'private/slice279-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice215'], ('slice215-pcsx2.json', 'private/slice215-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice214'], ('slice214-pcsx2.json', 'private/slice214-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice213'], ('slice213-pcsx2.json', 'private/slice213-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice211'], ('slice211-pcsx2.json', 'private/slice211-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice210'],
                         ('slice210-pcsx2.json', 'private/slice210-build/build/gt4vulkan.exe'))
        self.assertIn('slice211-pcsx2.json', probe.VU0_AOT_MANIFESTS)
        self.assertIn('slice211-pcsx2.json', probe.VU1_AOT_MANIFESTS)
        self.assertIn('slice213-pcsx2.json', probe.VU0_AOT_MANIFESTS)
        self.assertIn('slice213-pcsx2.json', probe.VU1_AOT_MANIFESTS)
        self.assertIn('slice214-pcsx2.json', probe.VU0_AOT_MANIFESTS)
        self.assertIn('slice214-pcsx2.json', probe.VU1_AOT_MANIFESTS)


if __name__ == '__main__':
    unittest.main()
