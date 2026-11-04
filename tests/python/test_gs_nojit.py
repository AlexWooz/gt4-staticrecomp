"""TASK-042 (slice197): the JIT-free gt4-gs.exe 0.4.0. Launcher pins of slice197 and a static no-JIT check of the
pinned package (the byte markers of tools/gs_nojit_check/check-nojit.ps1, applied in Python), with the slice196
package (gt4-gs 0.3.2, which still has the recompilers and the GS software JIT) as the positive control.
"""
import hashlib
import json
import os
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import external_runtime_probe as probe  # noqa: E402
import play_performance as performance  # noqa: E402

ASSETS = Path(os.environ.get('GT4_ASSETS_ROOT', ROOT))
MANIFEST = 'slice197-pcsx2.json'
EXECUTABLE = 'private/slice196-gs-deinterlace-build/build/gt4vulkan.exe'
PACKAGE = 'private/slice197-gt4gs-release-pkg'
GS_PROCESS = PACKAGE + '/gt4-gs.exe'
OLD_PACKAGE = 'private/slice196-gt4gs-release-pkg'
CHECK_SCRIPT = ROOT / 'tools/gs_nojit_check/check-nojit.ps1'


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def script_markers():
    """The marker list of check-nojit.ps1 (single-quoted strings between `$markers = @(` and `)`)."""
    text = CHECK_SCRIPT.read_text(encoding='utf-8')
    block = text[text.index('$markers = @('):]
    block = block[:block.index('\n)')]
    return re.findall(r"^\s*'([^']+)'", block, re.MULTILINE)


def markers_in(path):
    data = Path(path).read_bytes()
    return [marker for marker in script_markers() if marker.encode('latin-1') in data]


def package_dir(relative):
    for base in (ROOT, ASSETS):
        if (base / relative).is_dir():
            return base / relative
    return None


class Slice197PinTests(unittest.TestCase):
    def manifest(self):
        return json.loads((ROOT / 'docs/inputs' / MANIFEST).read_text(encoding='utf-8'))

    def test_mode_pcsx2_slice197_stays_selectable_and_keeps_slice196(self):
        # Since slice199 --mode pcsx2 points at slice199; slice197 stays selectable for A/B.
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice197'], (MANIFEST, EXECUTABLE))
        self.assertIn('pcsx2-slice197', performance.MODES)
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice196'], ('slice196-pcsx2.json', EXECUTABLE))
        self.assertIn(MANIFEST, probe.VU1_AOT_MANIFESTS)
        self.assertIn(MANIFEST, probe.VU0_AOT_MANIFESTS)
        self.assertIn('pcsx2', performance.MODES)
        self.assertIn('pcsx2-slice196', performance.MODES)

    def test_game_runtime_is_the_slice196_build(self):
        manifest = self.manifest()
        old = json.loads((ROOT / 'docs/inputs/slice196-pcsx2.json').read_text(encoding='utf-8'))
        self.assertEqual(manifest['runtime'], old['runtime'])
        same = [item for item in manifest['runtime_dependencies'] if item['path'] != GS_PROCESS]
        self.assertEqual(same, [item for item in old['runtime_dependencies'] if 'slice196-gt4gs' not in item['path']])
        self.assertEqual(manifest['source_capsule'], old['source_capsule'])
        self.assertEqual(manifest['port_capsule'], old['port_capsule'])

    def test_launcher_pin_describes_gt4gs_0_4_0(self):
        manifest = self.manifest()
        self.assertEqual(manifest['measurement_mode'], 'pcsx2')
        self.assertEqual(manifest['slice'], 197)
        stream = manifest['gs_stream']
        self.assertEqual(stream['renderer'], 'pcsx2')
        self.assertEqual(stream['gs_process'], GS_PROCESS)
        self.assertIn('version 1.1', stream['protocol'])
        package = stream['gs_package']
        self.assertEqual(package['directory'], PACKAGE)
        self.assertIs(package['data_root'], True)
        self.assertEqual(package['zip']['name'], 'gt4-gs-0.4.0-windows-x64.zip')
        self.assertTrue(package['release'].endswith('/releases/tag/gt4gs-v0.4.0'))
        self.assertIn('GT4GS_ONLY=ON', package['source'])
        self.assertIn('Arbitrary Code Guard', package['no_jit'])
        self.assertIn(GS_PROCESS, [item['path'] for item in manifest['runtime_dependencies']])
        self.assertNotIn('TASK-042)', ' '.join(manifest['limits']))

    def test_package_matches_its_pin_when_present(self):
        manifest = self.manifest()
        package = package_dir(PACKAGE)
        if package is None:
            self.skipTest('slice197 gt4-gs package not present')
        digest, files = probe.package_tree_digest(package)
        self.assertEqual((digest, files), (manifest['gs_stream']['gs_package']['tree_sha256'],
                                           manifest['gs_stream']['gs_package']['files']))
        source = (package / 'SOURCE.txt').read_text(encoding='utf-8')
        self.assertIn('Source ref: gt4gs-v0.4.0', source)
        self.assertIn('GT4GS_ONLY=ON', source)
        self.assertNotRegex(source, r'av(codec|format|util)|swscale|x264')
        pin = next(item for item in manifest['runtime_dependencies'] if item['path'] == GS_PROCESS)
        self.assertEqual(((package / 'gt4-gs.exe').stat().st_size, sha256(package / 'gt4-gs.exe')),
                         (pin['size_bytes'], pin['sha256']))


class NoJitMarkerTests(unittest.TestCase):
    def test_script_lists_the_code_generator_markers(self):
        markers = script_markers()
        for expected in ('@Xbyak@@', 'GSDrawScanlineCodeGenerator', '?AVrecMicroVU1@@', 'Recompiler Cache',
                         'VifUnpackSSE_', 'x86Emitter::', '\\pcsx2\\x86\\'):
            self.assertIn(expected, markers)

    def test_pinned_package_has_no_markers_and_enables_code_guard(self):
        package = package_dir(PACKAGE)
        if package is None:
            self.skipTest('slice197 gt4-gs package not present')
        for module in [package / 'gt4-gs.exe'] + sorted(package.glob('*.dll')):
            self.assertEqual(markers_in(module), [], module.name)
        data = (package / 'gt4-gs.exe').read_bytes()
        self.assertIn(b'GT4GS no-JIT', data)
        self.assertIn(b'SetProcessMitigationPolicy', data)
        ffmpeg = [dll.name for dll in package.glob('*.dll') if re.match(r'(av|sw|postproc|x264)', dll.name, re.IGNORECASE)]
        self.assertEqual(ffmpeg, [])

    def test_gt4gs_0_3_2_is_the_positive_control(self):
        package = package_dir(OLD_PACKAGE)
        if package is None:
            self.skipTest('slice196 gt4-gs package not present')
        found = markers_in(package / 'gt4-gs.exe')
        self.assertIn('@Xbyak@@', found)
        self.assertIn('GSDrawScanlineCodeGenerator', found)
        self.assertIn('Recompiler Cache', found)
        self.assertNotIn(b'GT4GS no-JIT', (package / 'gt4-gs.exe').read_bytes())


def setting_row(text, setting_id):
    """The [[setting]] block of one row, from its id to the next row (rows are not in a fixed order)."""
    start = text.index(f'id = "{setting_id}"')
    end = text.find('[[setting]]', start)
    return text[start:] if end == -1 else text[start:end]


class SettingsTableTests(unittest.TestCase):
    def test_software_raster_rows_name_the_c_rasterizer(self):
        text = (ROOT / 'docs/launcher/gs-settings.toml').read_text(encoding='utf-8')
        renderer = setting_row(text, 'graphics_api')
        hacks = setting_row(text, 'user_hacks')
        for row in (renderer, hacks):
            self.assertIn('C rasterizer', row)
            self.assertIn('0.4.0', row)


if __name__ == '__main__':
    unittest.main()
