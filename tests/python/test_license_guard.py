"""TASK-017 license demo guard (slice206 v2, slice205 v1): capsule derivation, pins, and the dispatch glue on synthetic guest RAM.

The behaviour tests compile the overlay helpers and hook with native/license_guard.cpp and a minimal guest model
(clang-cl; skipped when it is absent). Scenarios: a sane layout passes unchanged, an insane count with a failing CRC
is guarded (count set to 0xFFFFFFFF) on both demo loaders, a passing CRC is never guarded, other callers, the
unwinding dispatch of the return label and odd headers are untouched.
"""
import hashlib
import json
import os
import shutil
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
PIN = json.loads((ROOT / 'docs/inputs/slice206-license-guard-runtime.json').read_text(encoding='utf-8'))
PIN_V1 = json.loads((ROOT / 'docs/inputs/slice205-license-guard-runtime.json').read_text(encoding='utf-8'))
# TASK-100 S8b: the current guard overlay (per-target Manager add and body shift) is pinned by the slice277 capsule;
# slice206 and slice262 stay the history of the earlier overlay text.
PIN_S8B = json.loads((ROOT / 'docs/inputs/slice277-license-guard-runtime.json').read_text(encoding='utf-8'))
PIN_262 = json.loads((ROOT / 'docs/inputs/slice262-license-guard-runtime.json').read_text(encoding='utf-8'))
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


class LicenseGuardDerivationTests(unittest.TestCase):
    def test_overlay_sections_match_anchor_table(self):
        sections = perf.overlay_sections(guard.GUARD_OVERLAY_PATH.read_text(encoding='utf-8'),
                                         guard.GUARD_OVERLAY_ANCHORS)
        self.assertEqual(set(sections), {'includes', 'helpers', 'hook'})

    def test_overlay_adds_one_hook_after_the_idle_check(self):
        derived = guard.apply_license_guard(DONOR_EXCERPT)
        hook = '    if (gt4::license_guard::loader_for_return(c.r[31].w[0])) [[unlikely]] license_demo_guard(c);\n'
        self.assertEqual(derived.count(hook), 1)
        self.assertEqual(derived.count('#include "license_guard.hpp"\n'), 1)
        self.assertLess(derived.index('void license_demo_guard(Cpu &c) {'), derived.index('void dispatch(Cpu &c'))
        self.assertLess(derived.index('kIdle) return;\n'), derived.index(hook))
        self.assertLess(derived.index(hook), derived.index('const UnitInfo *u = unit_for(pc);'))

    def test_pin_names_the_donor_files_and_profile(self):
        self.assertEqual(PIN['donor_sha256']['dispatch.cpp'], guard.DISPATCH_PIN['dispatch.cpp'])
        self.assertEqual(PIN['donor_sha256']['kernel.cpp'], perf.HALT_PIN['kernel.cpp'])
        self.assertEqual(PIN['donor_sha256']['iop/cdvdfsv.cpp'], perf.FSV_PIN['iop/cdvdfsv.cpp'])
        self.assertEqual(PIN['license_guard_profile'], 'task017-guard-v2')
        self.assertEqual(PIN_V1['license_guard_profile'], 'task017-guard-v1')
        self.assertEqual(sorted(PIN['source_sha256']), list(guard.CAPSULE_FILES))
        # TASK-100 S8b: the current overlay text is pinned by slice277; the slice206/slice262 pins keep the older text.
        self.assertEqual(PIN_S8B['license_guard_overlay_sha256'], guard.overlay_sha256(guard.GUARD_OVERLAY_PATH))
        self.assertNotEqual(PIN['license_guard_overlay_sha256'], guard.overlay_sha256(guard.GUARD_OVERLAY_PATH))
        self.assertNotEqual(PIN_262['license_guard_overlay_sha256'], guard.overlay_sha256(guard.GUARD_OVERLAY_PATH))
        self.assertEqual(PIN_S8B['license_guard_profile'], PIN_262['license_guard_profile'])

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
            self.assertEqual(receipt['license_guard_overlay_sha256'], PIN_S8B['license_guard_overlay_sha256'])
            with self.assertRaises(ValueError):
                guard.prepare(root / 'private/slice277-license-guard-runtime', root,
                              display_aspect=True, frame_counters=True)
            with self.assertRaises(ValueError):
                guard.prepare(root / 'outside', root)

    def test_capsule_files_match_their_pins_when_present(self):
        for capsule_name, pin in (('slice205-license-guard-runtime', PIN_V1),
                                  ('slice206-license-guard-runtime', PIN),
                                  ('slice277-license-guard-runtime', PIN_S8B)):
            capsule = ASSETS / 'private' / capsule_name
            if not capsule.exists():
                continue
            with self.subTest(capsule=capsule_name):
                self.assertEqual(sorted(p.name for p in capsule.iterdir()),
                                 ['cdvdfsv.cpp', 'dispatch.cpp', 'kernel.cpp', 'runtime.json'])
                for name, expected in pin['source_sha256'].items():
                    self.assertEqual(hashlib.sha256((capsule / name).read_bytes()).hexdigest(), expected, name)
                self.assertEqual(hashlib.sha256((capsule / 'runtime.json').read_bytes()).hexdigest(),
                                 pin['receipt_sha256'])

    def test_v2_changes_only_dispatch_cpp(self):
        self.assertNotEqual(PIN['source_sha256']['dispatch.cpp'], PIN_V1['source_sha256']['dispatch.cpp'])
        for name in ('kernel.cpp', 'cdvdfsv.cpp'):
            self.assertEqual(PIN['source_sha256'][name], PIN_V1['source_sha256'][name], name)
        # TASK-100 S8b moves only the guard overlay: the aspect and frame-counter files stay as slice262 pinned them.
        for name in ('kernel.cpp', 'cdvdfsv.cpp'):
            self.assertEqual(PIN_S8B['source_sha256'][name], PIN_262['source_sha256'][name], name)
        self.assertNotEqual(PIN_S8B['source_sha256']['dispatch.cpp'], PIN_262['source_sha256']['dispatch.cpp'])

    def test_halt_dump_and_fsv_files_equal_the_slice204_capsule_derivation(self):
        if not DONOR_SRC.exists():
            self.skipTest('donor checkout not present')
        sources = guard.derive_sources(ASSETS)
        kernel = perf.apply_halt_dump((DONOR_SRC / 'kernel.cpp').read_bytes().decode('utf-8').replace('\r\n', '\n'))
        fsv = perf.apply_fsv_trace((DONOR_SRC / 'iop/cdvdfsv.cpp').read_bytes().decode('utf-8').replace('\r\n', '\n'))
        self.assertEqual(sources['kernel.cpp'], kernel)
        self.assertEqual(sources['cdvdfsv.cpp'], fsv)


class LicenseGuardLauncherPinTests(unittest.TestCase):
    """Launcher modes and pins agree: pcsx2 = slice206 (guard v2), license-guard = slice205 (guard v1, history)."""

    def test_mode_pin_label_runtime_and_capsule(self):
        import external_runtime_probe as probe
        name, runtime = probe.PERF_PROFILES['license-guard']
        manifest = json.loads((ROOT / 'docs/inputs' / name).read_text(encoding='utf-8'))
        self.assertEqual(manifest['measurement_mode'], 'license-guard')
        self.assertEqual(manifest['runtime']['path'], runtime)
        self.assertEqual(manifest['slice'], 205)
        self.assertEqual(manifest['license_guard_source_sha256'], PIN_V1['source_sha256'])
        self.assertEqual(manifest['license_guard_receipt_sha256'], PIN_V1['receipt_sha256'])
        self.assertIn(name, probe.VU1_AOT_MANIFESTS)
        self.assertIn(name, probe.VU0_AOT_MANIFESTS)

    def test_mode_pcsx2_is_the_slice218_build_and_older_builds_are_history(self):
        # TASK-080 P2 (slice217, tests/python/test_display_aspect.py): the slice215 build with the slice217 capsule
        # (the slice206 guard plus the race camera aspect hook and the HUD canvas hook); slice218 keeps that capsule.
        import external_runtime_probe as probe
        # This test describes the slice217/218/240 guard history; keep it on the slice240 profile.
        name, runtime = probe.PERF_PROFILES['pcsx2-slice240']
        self.assertEqual((name, runtime), ('slice240-pcsx2.json', 'private/slice240-game-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice215'], ('slice215-pcsx2.json', 'private/slice215-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice214'], ('slice214-pcsx2.json', 'private/slice214-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice213'], ('slice213-pcsx2.json', 'private/slice213-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice211'], ('slice211-pcsx2.json', 'private/slice211-build/build/gt4vulkan.exe'))
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice206'],
                         ('slice206-pcsx2.json', 'private/slice206-build/build/gt4vulkan.exe'))
        manifest = json.loads((ROOT / 'docs/inputs' / name).read_text(encoding='utf-8'))
        self.assertEqual(manifest['measurement_mode'], 'pcsx2')
        self.assertEqual(manifest['runtime']['path'], runtime)
        self.assertEqual(manifest['slice'], 240)
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice217'], ('slice217-pcsx2.json', 'private/slice217-build/build/gt4vulkan.exe'))
        # The slice217 capsule is the slice206 guard plus the aspect hooks: kernel.cpp and cdvdfsv.cpp are unchanged.
        aspect_pin = json.loads((ROOT / 'docs/inputs/slice217-license-guard-runtime.json').read_text(encoding='utf-8'))
        self.assertEqual(manifest['license_guard_capsule'], aspect_pin['capsule'])
        self.assertEqual(manifest['license_guard_source_sha256'], aspect_pin['source_sha256'])
        self.assertEqual(manifest['license_guard_receipt_sha256'], aspect_pin['receipt_sha256'])
        for unchanged in ('kernel.cpp', 'cdvdfsv.cpp'):
            self.assertEqual(manifest['license_guard_source_sha256'][unchanged], PIN['source_sha256'][unchanged], unchanged)
        self.assertIn(name, probe.VU1_AOT_MANIFESTS)
        self.assertIn(name, probe.VU0_AOT_MANIFESTS)
        self.assertEqual(probe.PERF_PROFILES['pcsx2-slice203'],
                         ('slice203-pcsx2.json', 'private/slice203-build/build/gt4vulkan.exe'))

    def test_pin_keeps_the_slice203_inputs(self):
        base = json.loads((ROOT / 'docs/inputs/slice203-pcsx2.json').read_text(encoding='utf-8'))
        for pin_name in ('slice205-license-guard.json', 'slice206-pcsx2.json'):
            manifest = json.loads((ROOT / 'docs/inputs' / pin_name).read_text(encoding='utf-8'))
            for key in ('frozen_game_library', 'card_seeds', 'runtime_dependencies', 'gs_stream', 'vu1_aot', 'vu0_aot',
                        'source_capsule', 'port_capsule', 'shader_capsule', 'memcard_capsule', 'memcard_capsule_sha256'):
                self.assertEqual(manifest[key], base[key], (pin_name, key))
            self.assertNotEqual(manifest['runtime']['sha256'], base['runtime']['sha256'])


@unittest.skipUnless(CLANG_CL.exists(), 'clang-cl is not installed')
class LicenseGuardBehaviourTests(unittest.TestCase):
    """Compiles the overlay helpers and hook with a guest RAM model and runs one demo load per scenario."""

    MODEL = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "license_guard.hpp"
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
std::uint32_t ld32(std::uint32_t a) { std::uint32_t v; std::memcpy(&v, g_ram + (a & (kRamSize - 1)), 4); return v; }
void st32(std::uint32_t a, std::uint32_t v) { std::memcpy(g_ram + (a & (kRamSize - 1)), &v, 4); }
@HELPERS@
void dispatch_model(Cpu &c) {
    const std::uint32_t pc = c.pc;  // the donor dispatch() local; the hook does not need it
    (void)pc;
@HOOK@
}
}  // namespace ps2

int main(int argc, char **argv) {
    using namespace ps2;
    const char scenario = argc > 1 ? argv[1][0] : 's';
    const std::uint32_t buffer = 0x01D0E500u, offset = 0x40u, size = 0x1BB8u, body = buffer + offset;
    std::uint8_t *plain = g_ram + body;
    for (std::uint32_t index = 0; index < 0x100u; ++index) plain[index] = static_cast<std::uint8_t>(index * 7u + 3u);
    st32(body + 0x1AA4u, 0u);
    st32(body + 0x1AA8u, 0x5BFu);
    st32(body + 0x1AACu, 0x10Eu);
    if (scenario == 'i' || scenario == 'c') st32(body + 0x1AA4u, 0x7F7F7F7Fu);
    std::uint32_t key = (gt4::license_guard::crc32(plain, size) & 0x00FFFFFFu) | (std::uint32_t{plain[0]} << 24);
    if (scenario != 'c') key ^= 0x00010000u;  // our FPU: the decrypted body fails the game CRC
    st32(buffer, scenario == 'h' ? 0x44u : offset);
    st32(buffer + 4u, size);
    st32(buffer + 8u, 0x40000u);
    st32(buffer + 12u, key);
    Cpu c{};
    c.pc = gt4::license_guard::kLoadDemoInputs;
    c.r[31].w[0] = gt4::license_guard::kLoaderReturn;
    if (scenario == 'o') c.r[31].w[0] = 0x00110350u;  // another indirect call: not a demo load
    if (scenario == 'r') {                             // second loader 0x00110230 -> vtbl[57] (owner B5 path)
        c.pc = 0x003D5F00u;                            // RaceNetRallyBattle virtual_56
        c.r[31].w[0] = gt4::license_guard::kReplayLoaderReturn;
        st32(body + 0x1AA4u, 0x7F7F7F7Fu);
    }
    if (scenario == 'u') {                             // unwinding: dispatch of the return label itself
        c.pc = gt4::license_guard::kLoaderReturn;
        st32(body + 0x1AA4u, 0x7F7F7F7Fu);
    }
    c.r[17].w[0] = buffer;
    c.r[5].w[0] = body;
    dispatch_model(c);
    std::printf("count=0x%08X guarded=%u\n", ld32(body + 0x1AA4u), gt4::license_guard::guarded_count());
    return 0;
}
'''

    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        sections = perf.overlay_sections(guard.GUARD_OVERLAY_PATH.read_text(encoding='utf-8'),
                                         guard.GUARD_OVERLAY_ANCHORS)
        source = Path(cls.folder.name) / 'harness.cpp'
        source.write_text(cls.MODEL.replace('@HELPERS@', sections['helpers']).replace('@HOOK@', sections['hook']),
                          encoding='utf-8')
        cls.exe = Path(cls.folder.name) / 'harness.exe'
        subprocess.run([str(CLANG_CL), '/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/I', str(ROOT / 'native'),
                        str(source), str(ROOT / 'native/license_guard.cpp'), '/Fe:' + str(cls.exe)],
                       check=True, capture_output=True, cwd=cls.folder.name)

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def run_harness(self, scenario):
        return subprocess.run([str(self.exe), scenario], capture_output=True, text=True, check=True)

    def test_sane_layout_with_crc_fail_passes_unchanged(self):
        result = self.run_harness('s')
        self.assertEqual(result.stdout.strip(), 'count=0x00000000 guarded=0')
        self.assertIn('[license-guard] loader 0x001100D0 target 0x003EAD18 demo 0x01D0E540 size 0x1BB8', result.stderr)
        self.assertIn('crc=fail layout=ok count=0x00000000 -> unchanged', result.stderr)

    def test_insane_count_with_crc_fail_is_guarded(self):
        result = self.run_harness('i')
        self.assertEqual(result.stdout.strip(), 'count=0xFFFFFFFF guarded=1')
        self.assertIn('crc=fail layout=bad count=0x7F7F7F7F -> skipped (count set to 0xFFFFFFFF, guarded load 1)',
                      result.stderr)
        self.assertEqual(result.stderr.count('[license-guard]'), 1)

    def test_crc_pass_is_never_guarded(self):
        result = self.run_harness('c')
        self.assertEqual(result.stdout.strip(), 'count=0x7F7F7F7F guarded=0')
        self.assertIn('crc=ok layout=bad count=0x7F7F7F7F -> unchanged', result.stderr)

    def test_other_caller_is_silent_and_untouched(self):
        result = self.run_harness('o')
        self.assertEqual(result.stdout.strip(), 'count=0x00000000 guarded=0')
        self.assertEqual(result.stderr, '')

    def test_second_loader_insane_count_is_guarded(self):
        result = self.run_harness('r')
        self.assertEqual(result.stdout.strip(), 'count=0xFFFFFFFF guarded=1')
        self.assertIn('[license-guard] loader 0x00110230 target 0x003D5F00 demo 0x01D0E540', result.stderr)
        self.assertIn('crc=fail layout=bad count=0x7F7F7F7F -> skipped', result.stderr)

    def test_unwinding_dispatch_of_the_return_label_is_silent(self):
        result = self.run_harness('u')
        self.assertEqual(result.stdout.strip(), 'count=0x7F7F7F7F guarded=0')
        self.assertEqual(result.stderr, '')

    def test_unexpected_header_is_reported_and_untouched(self):
        result = self.run_harness('h')
        self.assertEqual(result.stdout.strip(), 'count=0x00000000 guarded=0')
        self.assertIn('header offset 0x44 size 0x1BB8 arg1 0x01D0E540 unexpected -> unchanged', result.stderr)


@unittest.skipUnless(CLANG_CL.exists(), 'clang-cl is not installed')
class LicenseGuardSpec2BehaviourTests(unittest.TestCase):
    """TASK-100 S8b: the same overlay compiled with GT4_TARGET_SPEC2.

    Spec II loader A returns to 0x0010CB54, the virtual is 0x002012C8 (loadDemoInputs), and the loader hands it a
    pointer 0x80 into the body; the virtual adds 0x1A24, so the guard must write at body + 0x1AA4 (the Manager stream
    the CRC-confirmed demos carry). Loader B returns to 0x0010CCE4.
    """

    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        sections = perf.overlay_sections(guard.GUARD_OVERLAY_PATH.read_text(encoding='utf-8'),
                                         guard.GUARD_OVERLAY_ANCHORS)
        model = (LicenseGuardBehaviourTests.MODEL.replace('@HELPERS@', sections['helpers'])
                 .replace('@HOOK@', sections['hook'])
                 .replace('c.r[5].w[0] = body;', 'c.r[5].w[0] = body + 0x80u;')
                 .replace('c.r[31].w[0] = 0x00110350u;', 'c.r[31].w[0] = 0x0010CB58u;'))
        source = Path(cls.folder.name) / 'harness.cpp'
        source.write_text(model, encoding='utf-8')
        cls.exe = Path(cls.folder.name) / 'harness.exe'
        subprocess.run([str(CLANG_CL), '/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/DGT4_TARGET_SPEC2',
                        '/I', str(ROOT / 'native'), str(source), str(ROOT / 'native/license_guard.cpp'),
                        '/Fe:' + str(cls.exe)], check=True, capture_output=True, cwd=cls.folder.name)

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def run_harness(self, scenario):
        return subprocess.run([str(self.exe), scenario], capture_output=True, text=True, check=True)

    def test_shifted_pointer_insane_count_is_guarded_at_the_manager(self):
        result = self.run_harness('i')
        self.assertEqual(result.stdout.strip(), 'count=0xFFFFFFFF guarded=1')
        self.assertIn('[license-guard] loader 0x0010CA18 target 0x002012C8 demo 0x01D0E5C0', result.stderr)
        self.assertIn('layout=bad count=0x7F7F7F7F -> skipped (count set to 0xFFFFFFFF, guarded load 1) '
                      'manager 0x01D0FFE4', result.stderr)

    def test_shifted_pointer_sane_layout_passes_unchanged(self):
        result = self.run_harness('s')
        self.assertEqual(result.stdout.strip(), 'count=0x00000000 guarded=0')
        self.assertIn('[license-guard] loader 0x0010CA18 target 0x002012C8 demo 0x01D0E5C0', result.stderr)
        self.assertIn('crc=fail layout=ok count=0x00000000 -> unchanged', result.stderr)

    def test_second_spec2_loader_insane_count_is_guarded(self):
        result = self.run_harness('r')
        self.assertEqual(result.stdout.strip(), 'count=0xFFFFFFFF guarded=1')
        self.assertIn('[license-guard] loader 0x0010CBA8 target 0x003D5F00 demo 0x01D0E5C0', result.stderr)

    def test_another_return_address_is_silent(self):
        result = self.run_harness('o')
        self.assertEqual(result.stdout.strip(), 'count=0x00000000 guarded=0')
        self.assertEqual(result.stderr, '')


if __name__ == '__main__':
    unittest.main()
