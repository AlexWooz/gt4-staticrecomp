"""TASK-080 native display aspect ratios (P1 slice212b/slice215; P2 HUD slice217): capsule derivation and pins, launcher
validation of GT4_DISPLAY_ASPECT / GT4_ASPECT_FOV_EXTRA, the mode pin, the race camera hook on a synthetic guest stack and
(P2) the RaceDisplay::render HUD canvas hook on a synthetic RaceDisplay.

The behaviour tests compile the overlay helpers and hook with native/display_aspect.cpp and a minimal guest model
(clang-cl; skipped when it is absent). Scenarios: 21:9 and 32:9 scale the x column of the stack matrix, 4:3 scales
the y column, 16:9 writes nothing, the unwinding dispatch of the return label and other callers are silent, a1 that is
not sp is refused, and an invalid GT4_DISPLAY_ASPECT stops the process.
"""
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import prepare_license_guard as guard  # noqa: E402
import prepare_perf_runtime as perf  # noqa: E402

CLANG_CL = Path('C:/Program Files/LLVM/bin/clang-cl.exe')
ASSETS = Path(os.environ.get('GT4_ASSETS_ROOT', 'C:/Antigravity/gt4-staticrecomp'))
DONOR_SRC = ASSETS / 'private/references/ps2-web-recomp/runtime/src'
PIN = json.loads((ROOT / 'docs/inputs/slice217-license-guard-runtime.json').read_text(encoding='utf-8'))
PIN_212C = json.loads((ROOT / 'docs/inputs/slice212c-license-guard-runtime.json').read_text(encoding='utf-8'))
PIN_206 = json.loads((ROOT / 'docs/inputs/slice206-license-guard-runtime.json').read_text(encoding='utf-8'))
# TASK-100 S8b: slice277 pins the current guard overlay (per-target Manager add and body shift); slice217/212c/206
# keep the earlier overlay text and its aspect profile.
PIN_S8B = json.loads((ROOT / 'docs/inputs/slice277-license-guard-runtime.json').read_text(encoding='utf-8'))
DONOR_EXCERPT = (
    '#include "ps2/generated.hpp"\n'
    '#include "ps2/system.hpp"\n'
    '\n'
    'void dispatch(Cpu &c, std::uint32_t, std::uint32_t depth) {\n'
    '    const std::uint32_t pc = c.pc;\n'
    '    if (pc & kUnwind) return;\n'
    '    if (pc == kHandlerReturn || pc == kIdle) return;\n'
    '    const UnitInfo *u = unit_for(pc);\n'
    '}\n'
)
GUARD_HOOK = '    if (gt4::license_guard::loader_for_return(c.r[31].w[0])) [[unlikely]] license_demo_guard(c);\n'
ASPECT_HOOK = ('    if (c.r[31].w[0] == gt4::display_aspect::kCameraCallReturn) [[unlikely]] '
               'race_camera_aspect_hook(c);\n')


def source(relative):
    return (ROOT / relative).read_text(encoding='utf-8')


class DisplayAspectDerivationTests(unittest.TestCase):
    def test_overlay_sections_match_anchor_table(self):
        sections = perf.overlay_sections(guard.ASPECT_OVERLAY_PATH.read_text(encoding='utf-8'),
                                         guard.ASPECT_OVERLAY_ANCHORS)
        self.assertEqual(set(sections), {'includes', 'helpers', 'hook'})

    def test_aspect_hook_follows_the_license_guard_hook(self):
        derived = guard.apply_display_aspect(guard.apply_license_guard(DONOR_EXCERPT))
        self.assertEqual(derived.count(ASPECT_HOOK), 1)
        self.assertEqual(derived.count(GUARD_HOOK), 1)
        self.assertEqual(derived.count('#include "display_aspect.hpp"\n'), 1)
        self.assertLess(derived.index(GUARD_HOOK), derived.index(ASPECT_HOOK))
        self.assertLess(derived.index(ASPECT_HOOK), derived.index('const UnitInfo *u = unit_for(pc);'))
        self.assertLess(derived.index('void race_camera_aspect_hook(Cpu &c) {'), derived.index('void dispatch(Cpu &c'))

    def test_without_the_option_the_derivation_keeps_the_current_guard(self):
        if not DONOR_SRC.exists():
            self.skipTest('donor checkout not present')
        plain = guard.derive_sources(ASSETS)
        digest = hashlib.sha256(plain['dispatch.cpp'].encode('utf-8')).hexdigest()
        self.assertNotIn('display_aspect', plain['dispatch.cpp'])
        self.assertNotEqual(digest, PIN_206['source_sha256']['dispatch.cpp'])  # S8b moved the guard overlay
        for name in ('kernel.cpp', 'cdvdfsv.cpp'):
            self.assertEqual(hashlib.sha256(plain[name].encode('utf-8')).hexdigest(), PIN_206['source_sha256'][name])

    def test_pin_profile_and_overlay_hashes(self):
        self.assertEqual(PIN['license_guard_profile'], 'task017-guard-v2+task080-aspect-v2')
        self.assertEqual(PIN['display_aspect_overlay_sha256'], guard.overlay_sha256(guard.ASPECT_OVERLAY_PATH))
        # TASK-100 S8b: the current guard overlay is pinned by slice277; slice217 keeps the older text.
        self.assertEqual(PIN_S8B['license_guard_overlay_sha256'], guard.overlay_sha256(guard.GUARD_OVERLAY_PATH))
        self.assertNotEqual(PIN['license_guard_overlay_sha256'], guard.overlay_sha256(guard.GUARD_OVERLAY_PATH))
        self.assertEqual(PIN['capsule'], 'private/slice217-license-guard-runtime')
        # slice212c (P1) stays as history: same guard, kernel and cdvdfsv; only dispatch.cpp (the overlay) changed.
        self.assertEqual(PIN_212C['license_guard_profile'], 'task017-guard-v2+task080-aspect-v1')
        self.assertEqual(PIN_212C['license_guard_overlay_sha256'], PIN['license_guard_overlay_sha256'])
        self.assertNotEqual(PIN_212C['source_sha256']['dispatch.cpp'], PIN['source_sha256']['dispatch.cpp'])
        for name in ('kernel.cpp', 'cdvdfsv.cpp'):
            self.assertEqual(PIN['source_sha256'][name], PIN_206['source_sha256'][name], name)
        self.assertNotEqual(PIN['source_sha256']['dispatch.cpp'], PIN_206['source_sha256']['dispatch.cpp'])

    def test_fresh_derivation_matches_the_pin_when_the_donor_is_present(self):
        if not DONOR_SRC.exists():
            self.skipTest('donor checkout not present')
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            donor = root / 'private/references/ps2-web-recomp/runtime/src'
            for name in ('dispatch.cpp', 'kernel.cpp', 'iop/cdvdfsv.cpp'):
                (donor / name).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(DONOR_SRC / name, donor / name)
            receipt = guard.prepare(root / 'private/slice277-license-guard-runtime', root,
                                    display_aspect=True, frame_counters=True)
            self.assertEqual(receipt['source_sha256'], PIN_S8B['source_sha256'])
            self.assertEqual(receipt['license_guard_profile'], PIN_S8B['license_guard_profile'])

    def test_capsule_files_match_the_pin_when_present(self):
        capsule = ASSETS / PIN['capsule']
        if not capsule.exists():
            self.skipTest('capsule not present')
        self.assertEqual(sorted(p.name for p in capsule.iterdir()),
                         ['cdvdfsv.cpp', 'dispatch.cpp', 'kernel.cpp', 'runtime.json'])
        for name, expected in PIN['source_sha256'].items():
            self.assertEqual(hashlib.sha256((capsule / name).read_bytes()).hexdigest(), expected, name)
        self.assertEqual(hashlib.sha256((capsule / 'runtime.json').read_bytes()).hexdigest(), PIN['receipt_sha256'])

    def test_cmake_accepts_the_capsule_and_registers_the_fixture(self):
        cmake = source('native/CMakeLists.txt')
        self.assertIn('private/slice212c-license-guard-runtime', cmake)
        self.assertIn('docs/inputs/slice212c-license-guard-runtime.json', cmake)
        self.assertIn('private/slice217-license-guard-runtime', cmake)
        self.assertIn('docs/inputs/slice217-license-guard-runtime.json', cmake)
        self.assertIn('display_aspect.cpp', cmake)
        self.assertIn('add_test(NAME native_display_aspect COMMAND $<TARGET_FILE:display_aspect_fixture>)',
                      source('CMakeLists.txt'))

    def test_host_glue(self):
        window = source('native/window.cpp')
        self.assertIn('gt4::display_aspect::configured_setting();', window)
        # TASK-080 addendum: the window opens on GT4_MONITOR at GT4_OUTPUT_SIZE, per-monitor DPI aware.
        self.assertIn('gt4::display_aspect::configured_output_size();', window)
        self.assertIn('gt4::window_mode::parse_monitor(std::getenv("GT4_MONITOR"))', window)
        self.assertIn('DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2', window)
        self.assertIn('gt4::display_aspect::set_window_client_size(LOWORD(second), HIWORD(second));', window)
        self.assertIn('gt4::display_aspect::set_presentation_sink(present_race_view);', window)
        self.assertIn('gt4::display_aspect::at_guest_vblank(ps2::g_ram, 32u << 20);', source('native/mtvu_runtime.cpp'))
        vulkan = source('native/native_vulkan.cpp')
        self.assertIn('race_view_fill ? VK_FILTER_LINEAR : VK_FILTER_NEAREST', vulkan)


class DisplayAspectLauncherTests(unittest.TestCase):
    def test_validators_follow_the_game_rules(self):
        import external_runtime_probe as probe
        self.assertIsNone(probe.display_aspect_ratio(None))
        self.assertIsNone(probe.display_aspect_ratio('auto'))
        self.assertAlmostEqual(probe.display_aspect_ratio('16:9'), 16 / 9)
        self.assertAlmostEqual(probe.display_aspect_ratio('21:9'), 21 / 9)
        self.assertAlmostEqual(probe.display_aspect_ratio('3440:1440'), 3440 / 1440)
        self.assertAlmostEqual(probe.display_aspect_ratio('2.39'), 2.39)
        for bad in ('', 'Auto', '0:9', '16:0', '16/9', ' 16:9', '100:1', '1:100', '-1', 'nan', '1e3', '16:9:1'):
            with self.subTest(value=bad), self.assertRaises(ValueError):
                probe.display_aspect_ratio(bad)
        self.assertEqual(probe.aspect_fov_extra(None), 1.0)
        self.assertAlmostEqual(probe.aspect_fov_extra('1.37'), 1.37)
        for bad in ('', '0', '0.4', '3.5', 'x'):
            with self.subTest(value=bad), self.assertRaises(ValueError):
                probe.aspect_fov_extra(bad)
        # TASK-080 addendum: GT4_OUTPUT_SIZE and GT4_MONITOR, the rules of native/display_aspect.cpp and window_mode.cpp.
        self.assertIsNone(probe.output_size(None))
        self.assertEqual(probe.output_size('3440x1440'), (3440, 1440))
        self.assertEqual(probe.output_size('320x320'), (320, 320))
        for bad in ('', '1920', '1920X1080', '1920x', 'x1080', '319x240', '16385x8192', '1920x1080x1', ' 1920x1080',
                    '5000x320', '1920.5x1080'):
            with self.subTest(value=bad), self.assertRaises(ValueError):
                probe.output_size(bad)
        self.assertEqual(probe.monitor_device(None), '')
        self.assertEqual(probe.monitor_device(r'\\.\DISPLAY2'), r'\\.\DISPLAY2')
        prefix = '\\\\.\\'
        for bad in ('DISPLAY2', prefix, prefix + 'DISPLAY 2', prefix + 'D' * 40):
            with self.subTest(value=bad), self.assertRaises(ValueError):
                probe.monitor_device(bad)

    def test_mode_pcsx2_is_slice218_and_older_builds_are_history(self):
        import external_runtime_probe as probe
        name, runtime = probe.PERF_PROFILES['pcsx2-slice240']
        self.assertEqual((name, runtime), ('slice240-pcsx2.json', 'private/slice240-game-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice217'],
                         ('slice217-pcsx2.json', 'private/slice217-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice215'],
                         ('slice215-pcsx2.json', 'private/slice215-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice214'],
                         ('slice214-pcsx2.json', 'private/slice214-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice212b'],
                         ('slice212b-pcsx2.json', 'private/slice212b-build/build/gt4vulkan.exe'))
        manifest = json.loads((ROOT / 'docs/inputs' / name).read_text(encoding='utf-8'))
        self.assertEqual(manifest['measurement_mode'], 'pcsx2')
        self.assertEqual(manifest['runtime']['path'], runtime)
        self.assertEqual(manifest['slice'], 240)
        self.assertEqual(manifest['license_guard_capsule'], PIN['capsule'])
        self.assertEqual(manifest['license_guard_source_sha256'], PIN['source_sha256'])
        self.assertEqual(manifest['license_guard_receipt_sha256'], PIN['receipt_sha256'])
        self.assertIn(name, probe.VU1_AOT_MANIFESTS)
        self.assertIn(name, probe.VU0_AOT_MANIFESTS)
        # slice218 = the slice217 recipe (capsule, EE library profile "aspect", port, shaders, gt4-gs) with the TASK-080
        # addendum host code (GT4_OUTPUT_SIZE, GT4_MONITOR, DPI awareness): only the executable changes.
        base = json.loads((ROOT / 'docs/inputs/slice217-pcsx2.json').read_text(encoding='utf-8'))
        for key in ('card_seeds', 'gs_stream', 'vu1_aot', 'vu0_aot', 'source_capsule', 'frozen_game_library',
                    'license_guard_capsule', 'license_guard_receipt_sha256', 'license_guard_source_sha256',
                    'ee_library', 'port_capsule', 'shader_capsule', 'memcard_capsule', 'memcard_capsule_sha256'):
            self.assertEqual(manifest[key], base[key], key)
        self.assertNotEqual(manifest['runtime']['sha256'], base['runtime']['sha256'])
        # slice217 itself = the slice215 recipe with the P2 capsule and the re-translated library.
        older = json.loads((ROOT / 'docs/inputs/slice215-pcsx2.json').read_text(encoding='utf-8'))
        self.assertNotEqual(base['frozen_game_library'], older['frozen_game_library'])

    def test_protocol_1_3_is_documented(self):
        doc = source('docs/GS-STREAM-PROTOCOL.md')
        self.assertTrue(doc.startswith('# GS stream protocol, version 1.3 (normative)'))
        self.assertIn('display_aspect', doc)
        header = source('native/gs_stream/gs_stream_protocol.h')
        self.assertIn('display_aspect = 7,', header)
        self.assertIn('bad_argument = 7,', header)
        client = source('native/gs_stream/gs_stream_client.cpp')
        self.assertIn('if (ready_.version_minor < 3) {', client)
        # P2 (slice217): fill carries the target aspect x 1e6 in args[1] (0 = the whole window), so a custom ratio
        # wider than the window is letterboxed by the consumer instead of stretched.
        self.assertIn('control(proto::ControlKind::display_aspect, wanted, target, 0);', client)
        self.assertIn('target aspect', doc)
        self.assertIn('2440000', doc)


@unittest.skipUnless(CLANG_CL.exists(), 'clang-cl is not installed')
class RaceCameraHookBehaviourTests(unittest.TestCase):
    """Compiles the overlay helpers and hook with a guest RAM model and runs one camera call per scenario."""

    MODEL = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "display_aspect.hpp"
namespace ps2 {
inline constexpr std::uint32_t kRamSize = 32u << 20;
union alignas(16) Reg128 {
    std::uint64_t d[2];
    std::uint32_t w[4];
};
struct Cpu {
    Reg128 r[32];
    std::uint32_t pc;
};
std::uint8_t g_ram[kRamSize];
@HELPERS@
void dispatch_model(Cpu &c) {
    const std::uint32_t pc = c.pc;
    (void)pc;
@HOOK@
}
}  // namespace ps2

int main(int argc, char **argv) {
    using namespace ps2;
    const char scenario = argc > 1 ? argv[1][0] : 'n';
    const std::uint32_t sp = 0x01FFE000u;
    const float matrix[16] = {1.299f, 0, 0, 0, 0, 2.3f, 0, 0, 0.125f, -0.25f, 1, 0, 0, 0, 0, 1};
    std::memcpy(g_ram + sp, matrix, sizeof matrix);
    Cpu c{};
    c.pc = 0x004A0000u;                        // the camera's virtual (any target)
    c.r[31].w[0] = gt4::display_aspect::kCameraCallReturn;
    c.r[29].w[0] = sp;
    c.r[5].w[0] = sp;
    if (scenario == 'u') c.pc = gt4::display_aspect::kCameraCallReturn;  // unwinding dispatch of the return label
    if (scenario == 'o') c.r[31].w[0] = 0x00388B0Cu;                     // the first virtual call in initMonitor
    if (scenario == 'a') c.r[5].w[0] = sp + 0x40u;                       // a1 is not sp
    dispatch_model(c);
    if (scenario == 'h') {
        // P2: RaceDisplay::render dispatched with a0 = a RaceDisplay whose HUD matrix is the game's 16:9 canvas.
        const std::uint32_t self = 0x01C044C0u;
        const float l = -106.666664f, r = 746.666687f, t = 480.0f, b = 0.0f;
        const float hud[16] = {2.0f / (r - l), 0, 0, 0, 0, 2.0f / (b - t), 0, 0, 0, 0, -1.0f, 0,
                               (l + r) / (l - r), (t + b) / (t - b), 0, 1.0f};
        std::memcpy(g_ram + self + gt4::display_aspect::kHudMatrixOffset, hud, sizeof hud);
        Cpu h{};
        h.pc = gt4::display_aspect::kRaceDisplayRender;
        h.r[31].w[0] = 0x0039F000u;
        h.r[4].w[0] = self;
        dispatch_model(h);
        float out_hud[16];
        std::memcpy(out_hud, g_ram + self + gt4::display_aspect::kHudMatrixOffset, sizeof out_hud);
        std::printf("hud_m0=%.9f hud_m12=%.9f hud_same=%d ", out_hud[0], out_hud[12],
                    std::memcmp(out_hud, hud, sizeof hud) == 0 ? 1 : 0);
    }
    float out[16];
    std::memcpy(out, g_ram + sp, sizeof out);
    std::printf("m0=%.6f m5=%.6f m8=%.6f m9=%.6f same=%d\n", out[0], out[5], out[8], out[9],
                std::memcmp(out, matrix, sizeof out) == 0 ? 1 : 0);
    return 0;
}
'''

    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        sections = perf.overlay_sections(guard.ASPECT_OVERLAY_PATH.read_text(encoding='utf-8'),
                                         guard.ASPECT_OVERLAY_ANCHORS)
        harness = Path(cls.folder.name) / 'harness.cpp'
        harness.write_text(cls.MODEL.replace('@HELPERS@', sections['helpers']).replace('@HOOK@', sections['hook']),
                           encoding='utf-8')
        cls.exe = Path(cls.folder.name) / 'harness.exe'
        subprocess.run([str(CLANG_CL), '/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '-D_CRT_SECURE_NO_WARNINGS',
                        '/I', str(ROOT / 'native'), str(harness), str(ROOT / 'native/display_aspect.cpp'),
                        '/Fe:' + str(cls.exe)], check=True, capture_output=True, cwd=cls.folder.name)

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def run_harness(self, scenario, aspect=None, extra=None, check=True, output=None):
        env = {k: v for k, v in os.environ.items()
               if k not in ('GT4_DISPLAY_ASPECT', 'GT4_ASPECT_FOV_EXTRA', 'GT4_OUTPUT_SIZE')}
        if aspect is not None:
            env['GT4_DISPLAY_ASPECT'] = aspect
        if extra is not None:
            env['GT4_ASPECT_FOV_EXTRA'] = extra
        if output is not None:
            env['GT4_OUTPUT_SIZE'] = output
        return subprocess.run([str(self.exe), scenario], capture_output=True, text=True, check=check, env=env)

    @staticmethod
    def f32(value):
        return struct.unpack('<f', struct.pack('<f', value))[0]

    def values(self, result):
        fields = dict(item.split('=') for item in result.stdout.split())
        return {key: float(value) for key, value in fields.items()}

    def test_16_9_and_auto_headless_write_nothing(self):
        for aspect in ('16:9', None):
            with self.subTest(aspect=aspect):
                result = self.run_harness('n', aspect)
                self.assertEqual(self.values(result)['same'], 1)
                self.assertIn('unchanged (x 1.000000, y 1.000000)', result.stderr)

    def test_21_9_scales_the_x_column(self):
        result = self.run_harness('n', '21:9')
        values = self.values(result)
        self.assertAlmostEqual(values['m0'], self.f32(1.299) * 16 / 21, places=5)
        self.assertAlmostEqual(values['m8'], 0.125 * 16 / 21, places=5)
        self.assertAlmostEqual(values['m5'], 2.3, places=5)
        self.assertIn('[aspect] race camera hook 1', result.stderr)
        self.assertIn('scaled (x 0.761905, y 1.000000)', result.stderr)

    def test_32_9_halves_x_and_the_fov_factor_widens_more(self):
        self.assertAlmostEqual(self.values(self.run_harness('n', '32:9'))['m0'], self.f32(1.299) / 2, places=5)
        widened = self.values(self.run_harness('n', '32:9', '1.25'))
        self.assertAlmostEqual(widened['m0'], self.f32(1.299) / 2 / 1.25, places=5)

    def test_4_3_is_vert_plus(self):
        values = self.values(self.run_harness('n', '4:3'))
        self.assertAlmostEqual(values['m5'], 2.3 * 0.75, places=5)
        self.assertAlmostEqual(values['m9'], -0.25 * 0.75, places=5)
        self.assertAlmostEqual(values['m0'], 1.299, places=5)

    def test_unwinding_and_other_callers_are_silent(self):
        for scenario in ('u', 'o'):
            with self.subTest(scenario=scenario):
                result = self.run_harness(scenario, '21:9')
                self.assertEqual(self.values(result)['same'], 1)
                self.assertNotIn('race camera hook', result.stderr)

    def test_a1_that_is_not_sp_is_refused(self):
        result = self.run_harness('a', '21:9')
        self.assertEqual(self.values(result)['same'], 1)
        self.assertIn('is not sp', result.stderr)

    def test_render_hook_widens_the_hud_canvas(self):
        result = self.run_harness('h', '21:9')
        values = self.values(result)
        self.assertEqual(values['hud_same'], 0)
        self.assertAlmostEqual(values['hud_m0'], 2 / 1120, places=7)       # canvas 320 -/+ 240 x 21/9 = -240 .. 880
        self.assertAlmostEqual(values['hud_m12'], 640 / -1120, places=6)
        self.assertIn('HUD canvas of RaceDisplay 0x01C044C0', result.stderr)
        self.assertIn('left -240.000 right 880.000', result.stderr)

    def test_render_hook_writes_nothing_at_16_9(self):
        for aspect in ('16:9', None):
            with self.subTest(aspect=aspect):
                result = self.run_harness('h', aspect)
                self.assertEqual(self.values(result)['hud_same'], 1)
                self.assertNotIn('HUD canvas', result.stderr)

    def test_output_size_drives_auto(self):
        # TASK-080 addendum: the launcher sends GT4_DISPLAY_ASPECT=auto with GT4_OUTPUT_SIZE.
        same = self.run_harness('n', 'auto', output='1920x1080')
        self.assertEqual(self.values(same)['same'], 1)
        self.assertIn('auto (output size 1920x1080, 16:9', same.stderr)
        self.assertIn('unchanged (x 1.000000, y 1.000000)', same.stderr)
        wide = self.run_harness('n', None, output='3440x1440')
        self.assertAlmostEqual(self.values(wide)['m0'], self.f32(1.299) * (16 / 9) / (3440 / 1440), places=5)
        self.assertIn('scaled (x 0.744186, y 1.000000)', wide.stderr)
        self.assertAlmostEqual(self.values(self.run_harness('n', None, output='2560x1080'))['m0'],
                               self.f32(1.299) * 0.75, places=5)
        hud = self.values(self.run_harness('h', None, output='2560x1080'))
        self.assertEqual(hud['hud_same'], 0)
        fixed = self.values(self.run_harness('n', '16:9', output='3440x1440'))
        self.assertEqual(fixed['same'], 1, 'a fixed GT4_DISPLAY_ASPECT wins over the output size')

    def test_invalid_output_size_stops_the_process(self):
        result = self.run_harness('n', None, check=False, output='1920*1080')
        self.assertEqual(result.returncode, 1)
        self.assertIn('GT4_OUTPUT_SIZE', result.stderr)
        self.assertIn('game not started', result.stderr)

    def test_invalid_value_stops_the_process(self):
        result = self.run_harness('n', '16/9', check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn('GT4_DISPLAY_ASPECT', result.stderr)
        self.assertIn('game not started', result.stderr)


if __name__ == '__main__':
    unittest.main()
