"""TASK-159, TASK-095: the translated units with the 360-degree chase camera calls and the 2D overlay canvas calls
(scripts/camera360_units.py).

Rules checked:
  * each host call goes after its call's delay slot and before the call, the declaration after the include (outside
    `namespace gen`, one line per namespace), and nothing else changes;
  * removing the insertions gives the input back; a unit hooked twice, a missing or doubled call line, a wrong unit hash, an
    existing output folder and an extra change are refused;
  * each target's call lines are the translator's form of the call sites in the table, and their return labels are the ones
    native/game_target.hpp gives the host, so the units and the host hooks agree;
  * TASK-095 statement sites: each is replaced inside its own labelled block only, exactly once, and removing it gives the
    input back;
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
PEDAL = units.CallSite(units.PEDAL_HOOK, 0x00100030, 0x00100038, 0x00100200,
                       units.call_line(0x00100038, units.PEDAL_SLOT, 'unit_0001', 9, 0x00100200))
TEXT = (units.INCLUDE_LINE + '\nnamespace gen {\n\nusing namespace ps2;\n\nvoid unit_0001(Cpu &c, std::uint32_t entry, '
        'std::uint32_t depth) {\nL_00100010:\n' + YAW.line + 'L_00100018:\n    c.budget -= 1;\n' + PEDAL.line +
        'L_00100038:\n    c.budget -= 1;\n}\n\n}\n')
UNIT = units.Unit('unit_0001.cpp', units.sha256(TEXT.encode()), (YAW, PEDAL))
# TASK-095: a unit with a camera call and an overlay call declares both namespaces.
OVERLAY = units.CallSite(units.OVERLAY_HOOK, 0x00100050, 0x00100058, 0x00100300,
                         units.call_line(0x00100058, 'c.f[17] = c.f[21];', 'unit_0003', 11, 0x00100300))
MIXED_TEXT = TEXT.replace('}\n\n}\n', OVERLAY.line + 'L_00100058:\n    c.budget -= 1;\n}\n\n}\n')
MIXED = units.Unit('unit_0001.cpp', units.sha256(MIXED_TEXT.encode()), (YAW, PEDAL, OVERLAY))


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
        self.assertIn('c.r[4].d[0] = c.r[7].d[0] + 0ull; ::gt4::camera360::before_analog_pedal_call(c); if (depth < kMaxDepth) '
                      'unit_0001(', hooked)
        self.assertTrue(hooked.startswith(units.INCLUDE_LINE + units.declaration(UNIT)))
        self.assertIn('void before_analog_pedal_call(ps2::Cpu &c); void before_chase_yaw_call(ps2::Cpu &c);', hooked)
        self.assertLess(hooked.index('namespace gt4::camera360'), hooked.index('namespace gen {'))
        self.assertEqual(hooked.count('::gt4::camera360::'), 2)
        self.assertEqual(units.unhook_unit_text(hooked, UNIT), TEXT)
        units.qualify_unit(TEXT, hooked, UNIT)

    def test_two_namespaces(self):
        hooked = units.hook_unit_text(MIXED_TEXT, MIXED)
        self.assertIn("c.f[17] = c.f[21]; ::gt4::display_aspect::before_overlay_ortho_call(c); if (depth < kMaxDepth) "
                      "unit_0003(", hooked)
        self.assertIn('namespace gt4::camera360 { void before_analog_pedal_call(ps2::Cpu &c); '
                      'void before_chase_yaw_call(ps2::Cpu &c); }\n'
                      'namespace gt4::display_aspect { void before_overlay_ortho_call(ps2::Cpu &c); }\n', hooked)
        self.assertLess(hooked.index('namespace gt4::display_aspect'), hooked.index('namespace gen {'))
        self.assertEqual(units.unhook_unit_text(hooked, MIXED), MIXED_TEXT)
        units.qualify_unit(MIXED_TEXT, hooked, MIXED)
        with self.assertRaises(ValueError):
            units.hook_unit_text(hooked, MIXED)  # already hooked

    def test_statement_sites(self):
        stored = '    st32((c.r[18].w[0] + 36u), c.f[0]);\n'
        text = (units.INCLUDE_LINE + '\nnamespace gen {\n\nvoid unit_00E4(Cpu &c, std::uint32_t entry, std::uint32_t depth) {\n'
                'L_00490290:\n' + stored + '    c.budget -= 1;\nL_00490374:\n' + stored + '}\n\n}\n')
        first = units.text_scale_site(0x004902C8, 'L_00490290', 36, 0, 'x')
        second = units.text_scale_site(0x00490390, 'L_00490374', 36, 0, 'x')
        unit = units.Unit('unit_00E4.cpp', units.sha256(text.encode()), (first, second))
        hooked = units.hook_unit_text(text, unit)
        self.assertEqual(hooked.count('st32((c.r[18].w[0] + 36u), ::gt4::display_aspect::overlay_text_scale_x(c.f[0]));'), 2)
        self.assertIn('namespace gt4::display_aspect { std::uint32_t overlay_text_scale_x(std::uint32_t value); }\n', hooked)
        self.assertEqual(units.unhook_unit_text(hooked, unit), text)
        units.qualify_unit(text, hooked, unit)
        # The same statement twice in one block, or missing from its block, is refused.
        with self.assertRaises(ValueError):
            units.hook_unit_text(text.replace('L_00490374:\n', ''), unit)
        with self.assertRaises(ValueError):
            units.hook_unit_text(text.replace('L_00490290:\n' + stored, 'L_00490290:\n'), unit)

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
                    if isinstance(site, units.StatementSite):
                        with self.subTest(target=name, statement=hex(site.address)):
                            self.assertEqual(unit.name, f'unit_{(site.address - 0x00100000) >> 14:04X}.cpp')
                            self.assertIn(f'::{site.hook}(', site.replacement)
                            # The replacement is the original with the stored value wrapped in the host call.
                            self.assertEqual(site.replacement.replace(f'::{site.hook}(', '', 1).replace(']));', ']);', 1), site.original)
                        continue
                    with self.subTest(target=name, site=hex(site.call_site)):
                        self.assertEqual(site.return_label, site.call_site + 8)
                        self.assertIn(f'static_cast<std::int32_t>(0x{site.return_label:08X}u)', site.line)
                        self.assertIn(f'c.pc = 0x{site.callee:08X}u; return;', site.line)
                        self.assertIn(f'if (c.pc != 0x{site.return_label:08X}u) [[unlikely]] return;', site.line)
                        self.assertEqual(len(unit.sha256), 64)
                        # The unit holds the 16 KiB window of the call site (units start at 0x00100000).
                        self.assertEqual(unit.name, f'unit_{(site.call_site - 0x00100000) >> 14:04X}.cpp')
                        if site.hook == units.PEDAL_HOOK:
                            self.assertIn(units.PEDAL_SLOT, site.line)

    def test_host_table_agrees(self):
        for target, table in (('v200', 'kV200Hooks'), ('spec2', 'kSpec2Hooks')):
            sites = [site for unit in units.TARGETS[target] for site in unit.sites]
            yaw = [site.return_label for site in sites if site.hook == units.YAW_HOOK]
            pedals = [site.return_label for site in sites if site.hook == units.PEDAL_HOOK]
            host = (game_target_value('camera_yaw_call_return', table),
                    game_target_value('camera_accelerator_scale_return', table),
                    game_target_value('camera_brake_scale_return', table))
            with self.subTest(target=target):
                self.assertEqual(len(yaw), 1)
                self.assertEqual(len(pedals), 2)
                # All 0 means the target's camera is not filled in yet (the host then keeps it off).
                self.assertIn(host, ((0, 0, 0), (yaw[0], pedals[0], pedals[1])))
        self.assertEqual(game_target_value('camera_pad_stick_x', 'kV200Hooks'), 0x0086F99E)

    def test_overlay_sites_agree_with_the_host(self):
        # TASK-095: one site per ortho call, each at the return label the host checks, calling the target's ortho function.
        for target, table, ortho in (('v200', 'kV200Hooks', 0x004A5A90), ('spec2', 'kSpec2Hooks', 0x005A2600)):
            sites = [site for unit in units.TARGETS[target] for site in unit.sites if site.hook == units.OVERLAY_HOOK]
            with self.subTest(target=target):
                self.assertEqual(sorted(site.return_label for site in sites),
                                 sorted((game_target_value('menu_ortho_call_return', table),
                                         game_target_value('overlay_ortho_call_return', table))))
                self.assertTrue(all(site.callee == ortho for site in sites))
                self.assertTrue(all('c.f[17] = ' in site.line or 'c.f[16] = ' in site.line for site in sites))
                ends = [site for unit in units.TARGETS[target] for site in unit.sites if site.hook == units.OVERLAY_END_HOOK]
                self.assertEqual([site.return_label for site in ends], [game_target_value('overlay_draw_end_return', table)])
                # The end hook sits in the strobe renderer's unit, after its ortho call.
                renderer = [unit for unit in units.TARGETS[target] if ends[0] in unit.sites][0]
                ortho = [site for site in renderer.sites if site.hook == units.OVERLAY_HOOK][0]
                self.assertEqual(ortho.return_label, game_target_value('overlay_ortho_call_return', table))
                self.assertGreater(ends[0].call_site, ortho.call_site)

    def test_text_scale_stores(self):
        # Four stores per target: x and y of the 1.0 default, then of the style's percentages; the scale words are +0x24/+0x28
        # (v2.00) and +0x3C/+0x40 (Spec II).
        for target, x_offset in (('v200', 36), ('spec2', 60)):
            sites = [site for unit in units.TARGETS[target] for site in unit.sites if isinstance(site, units.StatementSite)]
            with self.subTest(target=target):
                self.assertEqual(sorted(site.hook for site in sites), sorted([units.TEXT_X_HOOK] * 2 + [units.TEXT_Y_HOOK] * 2))
                for site in sites:
                    offset = x_offset if site.hook == units.TEXT_X_HOOK else x_offset + 4
                    self.assertIn(f'(c.r[18].w[0] + {offset}u)', site.original)
                self.assertEqual(len({site.label for site in sites}), 2)

    def test_empty_delay_slot(self):
        line = units.call_line(0x003EC248, '', 'unit_00E9', 80, 0x004A5400)
        self.assertIn('static_cast<std::int32_t>(0x003EC248u);  if (depth < kMaxDepth) unit_00E9(c, 80u,', line)


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
                        if isinstance(site, units.StatementSite):
                            start, end = units.block_span(text, site.label)
                            self.assertEqual(text[start:end].count(site.original), 1)
                        else:
                            self.assertEqual(text.count(site.line), 1)
                    units.qualify_unit(text, units.hook_unit_text(text, unit), unit)


if __name__ == '__main__':
    unittest.main()
