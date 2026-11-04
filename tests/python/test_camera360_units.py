"""TASK-159: the translated units with the 360-degree chase camera calls (scripts/camera360_units.py).

Rules checked:
  * each host call goes after its call's delay slot and before the call, the declaration after the include (outside
    `namespace gen`), and nothing else changes;
  * removing the insertions gives the input back; a unit hooked twice, a missing or doubled call line, a wrong unit hash, an
    existing output folder and an extra change are refused;
  * each target's call lines are the translator's form of the call sites in the table, and their return labels are the ones
    native/game_target.hpp gives the host, so the units and the host hooks agree;
  * on the real units (skipped when the private folders are absent): each pinned unit holds its lines once and the hooked copy
    qualifies.
"""
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import camera360_units as units  # noqa: E402

ASSETS = Path(os.environ.get('GT4_ASSETS_ROOT', 'C:/Antigravity/gt4-staticrecomp'))
UNIT_FOLDERS = {
    'v200': ASSETS / 'private/slice217-ee-aspect-producer/generated',
    # The Spec II units live in a worktree's private/ee-spec2/generated (docs/BUILD.md, TASK-134), or GT4_SPEC2_UNITS.
    'spec2': Path(os.environ.get('GT4_SPEC2_UNITS', ROOT / 'private/ee-spec2/generated')),
}

YAW = units.CallSite(units.YAW_HOOK, 0x00100010, 0x00100018, 0x00100100,
                     units.call_line(0x00100018, 'c.f[13] = ld32((c.r[16].w[0]));', 'unit_0002', 7, 0x00100100))
PEDAL = units.CallSite(units.CHANNEL_HOOK, 0x00100030, 0x00100038, 0x00100200,
                       units.call_line(0x00100038, 'set32(c.r[5], 0u + 2u);', 'unit_0001', 9, 0x00100200))
TEXT = (units.INCLUDE_LINE + '\nnamespace gen {\n\nusing namespace ps2;\n\nvoid unit_0001(Cpu &c, std::uint32_t entry, '
        'std::uint32_t depth) {\nL_00100010:\n' + YAW.line + 'L_00100018:\n    c.budget -= 1;\n' + PEDAL.line +
        'L_00100038:\n    c.budget -= 1;\n}\n\n}\n')
UNIT = units.Unit('unit_0001.cpp', units.sha256(TEXT.encode()), (YAW, PEDAL))


def game_target_value(field, table):
    """The value of `field` in kV200Hooks or kSpec2Hooks of native/game_target.hpp."""
    text = (ROOT / 'native/game_target.hpp').read_text(encoding='utf-8')
    body = text.split(f'inline constexpr HookAddresses {table} = {{', 1)[1].split('};', 1)[0]
    match = re.search(r'/\* ' + field + r' \*/ (0x[0-9A-Fa-f]+|\d+)u', body)
    return int(match.group(1), 0)


class HookTextTests(unittest.TestCase):
    def test_calls_go_between_the_delay_slot_and_the_call(self):
        hooked = units.hook_unit_text(TEXT, UNIT)
        self.assertIn('c.f[13] = ld32((c.r[16].w[0])); ::gt4::camera360::before_chase_yaw_call(c); if (depth < kMaxDepth) '
                      'unit_0002(', hooked)
        self.assertIn('set32(c.r[5], 0u + 2u); ::gt4::camera360::before_pad_analog_channel_call(c); if (depth < kMaxDepth) '
                      'unit_0001(', hooked)
        self.assertTrue(hooked.startswith(units.INCLUDE_LINE + units.declaration(UNIT)))
        self.assertIn('void before_chase_yaw_call(ps2::Cpu &c); void before_pad_analog_channel_call(ps2::Cpu &c);', hooked)
        self.assertLess(hooked.index('namespace gt4::camera360'), hooked.index('namespace gen {'))
        self.assertEqual(hooked.count('::gt4::camera360::'), 2)
        self.assertEqual(units.unhook_unit_text(hooked, UNIT), TEXT)
        units.qualify_unit(TEXT, hooked, UNIT)

    def test_refusals(self):
        hooked = units.hook_unit_text(TEXT, UNIT)
        with self.assertRaises(ValueError):
            units.hook_unit_text(hooked, UNIT)  # already hooked
        with self.assertRaises(ValueError):
            units.hook_unit_text(TEXT.replace(PEDAL.line, ''), UNIT)  # a call line missing
        with self.assertRaises(ValueError):
            units.hook_unit_text(TEXT + YAW.line, UNIT)  # a call line twice
        with self.assertRaises(ValueError):
            units.qualify_unit(TEXT, hooked.replace('c.budget -= 1;', 'c.budget -= 2;', 1), UNIT)  # an extra change
        with self.assertRaises(ValueError):
            units.unhook_unit_text(TEXT, UNIT)

    def test_make_units_changes_only_the_listed_units_and_refuses_reuse(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'generated'
            source.mkdir()
            (source / 'unit_0001.cpp').write_bytes(TEXT.encode())
            (source / 'unit_0002.cpp').write_bytes(b'#include "units.hpp"\n')
            (source / 'table.cpp').write_bytes(b'table\n')
            receipt = units.make_units('t', source, Path(folder) / 'out', {'t': (UNIT,)})
            self.assertEqual([row['unit'] for row in receipt['units']], ['unit_0001.cpp'])
            self.assertEqual(len(receipt['units'][0]['sites']), 2)
            self.assertEqual(receipt['files'], 3)
            self.assertEqual((Path(folder) / 'out/unit_0002.cpp').read_bytes(), b'#include "units.hpp"\n')
            self.assertTrue((Path(folder) / 'out.camera360-units.json').is_file())
            with self.assertRaises(ValueError):
                units.make_units('t', source, Path(folder) / 'out', {'t': (UNIT,)})  # output exists
            wrong = units.Unit(UNIT.name, '0' * 64, UNIT.sites)
            with self.assertRaises(ValueError):
                units.make_units('t', source, Path(folder) / 'out2', {'t': (wrong,)})  # not the pinned translation


class TargetTableTests(unittest.TestCase):
    def test_call_lines_name_their_call_sites(self):
        for name, target in units.TARGETS.items():
            for unit in target:
                for site in unit.sites:
                    with self.subTest(target=name, site=hex(site.call_site)):
                        self.assertEqual(site.return_label, site.call_site + 8)
                        self.assertIn(f'static_cast<std::int32_t>(0x{site.return_label:08X}u)', site.line)
                        self.assertIn(f'c.pc = 0x{site.callee:08X}u; return;', site.line)
                        self.assertIn(f'if (c.pc != 0x{site.return_label:08X}u) [[unlikely]] return;', site.line)
                        self.assertEqual(len(unit.sha256), 64)
                        # The unit holds the 16 KiB window of the call site (units start at 0x00100000).
                        self.assertEqual(unit.name, f'unit_{(site.call_site - 0x00100000) >> 14:04X}.cpp')
                        if site.hook == units.CHANNEL_HOOK:
                            self.assertEqual(site.callee, {'v200': 0x004386F0, 'spec2': 0x00314870}[name])

    def test_host_table_agrees(self):
        for target, table in (('v200', 'kV200Hooks'), ('spec2', 'kSpec2Hooks')):
            sites = [site for unit in units.TARGETS[target] for site in unit.sites]
            yaw = [site.return_label for site in sites if site.hook == units.YAW_HOOK]
            loops = [site.return_label for site in sites if site.hook == units.CHANNEL_HOOK]
            host = (game_target_value('camera_yaw_call_return', table), game_target_value('camera_analog_loop_return', table))
            with self.subTest(target=target):
                self.assertEqual(len(yaw), 1)
                self.assertEqual(len(loops), 1)
                self.assertEqual(host, (yaw[0], loops[0]))
        self.assertEqual(game_target_value('camera_pad_stick_x', 'kV200Hooks'), 0x0086F99E)


class RealUnitTests(unittest.TestCase):
    def test_pinned_units(self):
        for target, folder in UNIT_FOLDERS.items():
            for unit in units.TARGETS[target]:
                path = folder / unit.name
                if not path.is_file():
                    continue  # private units absent on this machine
                with self.subTest(target=target, unit=unit.name):
                    original = path.read_bytes()
                    self.assertEqual(units.sha256(original), unit.sha256)
                    text = original.decode('ascii')
                    for site in unit.sites:
                        self.assertEqual(text.count(site.line), 1)
                    units.qualify_unit(text, units.hook_unit_text(text, unit), unit)


if __name__ == '__main__':
    unittest.main()
