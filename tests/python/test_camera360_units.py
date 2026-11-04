"""TASK-159, TASK-160, TASK-161, TASK-095: the translated units with the camera calls, the pedal statements and the 2D overlay
canvas calls (scripts/camera360_units.py).

Rules checked:
  * each host call goes after its call's delay slot and before the call, the declaration after the include (outside
    `namespace gen`), and nothing else changes;
  * removing the insertions gives the input back; a unit hooked twice, a missing or doubled call line, a wrong unit hash, an
    existing output folder and an extra change are refused;
  * each target's call lines are the translator's form of the call sites in the table, and their return labels are the ones
    native/game_target.hpp gives the host, so the units and the host hooks agree;
  * statements (TASK-161 pedal cases, TASK-095 text style parser) are replaced inside their own labelled block only, exactly
    once; a unit with an overlay hook gets the TASK-095 header, the others keep the header of the rc14 units;
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
    # Spec II: TASK-166's translation (the rc14 source; the same units as private/ee-spec2/generated for every site here), or
    # GT4_SPEC2_UNITS.
    'spec2': Path(os.environ.get('GT4_SPEC2_UNITS', ASSETS / 'private/slice405-spec2-aspect-producer/generated')),
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
        self.assertIn('namespace gt4::camera360 { void before_chase_yaw_call(ps2::Cpu &c); '
                      'void before_pad_analog_channel_call(ps2::Cpu &c); }', hooked)
        self.assertLess(hooked.index('namespace gt4::camera360'), hooked.index('namespace gen {'))
        self.assertEqual(hooked.count('::gt4::camera360::'), 2)
        self.assertEqual(units.unhook_unit_text(hooked, UNIT), TEXT)
        units.qualify_unit(TEXT, hooked, UNIT)

    def test_hooks_of_two_namespaces_get_one_declaration_line_each(self):
        projection = units.CallSite(units.PROJECTION_HOOK, 0x00100050, 0x00100058, 0x00100300,
                                    units.call_line(0x00100058, 'c.f[13] = c.f[12];', 'unit_0001', 11, 0x00100300))
        text = TEXT.replace('}\n\n}\n', projection.line + '}\n\n}\n')
        unit = units.Unit('unit_0001.cpp', units.sha256(text.encode()), (YAW, PEDAL, projection))
        hooked = units.hook_unit_text(text, unit)
        self.assertIn('namespace gt4::fov_adjust { void before_projection_scale_call(ps2::Cpu &c); }\n', hooked)
        self.assertIn('c.f[13] = c.f[12]; ::gt4::fov_adjust::before_projection_scale_call(c); if (depth', hooked)
        self.assertEqual(units.unhook_unit_text(hooked, unit), text)
        units.qualify_unit(text, hooked, unit)

    def test_overlay_hooks_get_the_task_095_header(self):
        overlay = units.CallSite(units.OVERLAY_HOOK, 0x00100050, 0x00100058, 0x00100300,
                                 units.call_line(0x00100058, 'c.f[17] = c.f[21];', 'unit_0003', 11, 0x00100300))
        end = units.CallSite(units.OVERLAY_END_HOOK, 0x00100060, 0x00100068, 0x00100400,
                             units.call_line(0x00100068, '', 'unit_0003', 12, 0x00100400))
        text = TEXT.replace('}\n\n}\n', overlay.line + 'L_00100058:\n    c.budget -= 1;\n' + end.line + '}\n\n}\n')
        unit = units.Unit('unit_0001.cpp', units.sha256(text.encode()), (YAW, PEDAL, overlay, end))
        hooked = units.hook_unit_text(text, unit)
        self.assertIn(units.HEADER_095 + '\n', hooked)
        self.assertIn('namespace gt4::display_aspect { void after_overlay_draw(ps2::Cpu &c); '
                      'void before_overlay_ortho_call(ps2::Cpu &c); }\n', hooked)
        self.assertIn('c.f[17] = c.f[21]; ::gt4::display_aspect::before_overlay_ortho_call(c); if (depth', hooked)
        self.assertIn('(0x00100068u); ::gt4::display_aspect::after_overlay_draw(c); if (depth', hooked)
        self.assertEqual(units.unhook_unit_text(hooked, unit), text)
        units.qualify_unit(text, hooked, unit)
        with self.assertRaises(ValueError):
            units.hook_unit_text(hooked, unit)  # already hooked
        # A unit without overlay hooks keeps the header of the units built before TASK-095.
        self.assertIn(units.HEADER_161 + '\n', units.hook_unit_text(TEXT, UNIT))
        self.assertNotIn('TASK-095', units.hook_unit_text(TEXT, UNIT))

    def test_empty_delay_slot(self):
        line = units.call_line(0x003EC248, '', 'unit_00E9', 80, 0x004A5400)
        self.assertIn('static_cast<std::int32_t>(0x003EC248u);  if (depth < kMaxDepth) unit_00E9(c, 80u,', line)

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


class StatementSiteTests(unittest.TestCase):
    """TASK-161: statements replaced inside their labelled block."""
    BLOCKS = ('L_00100100:\n    c.r[3].d[0] = c.r[4].d[0] & 255ull;\n    set32(c.r[3], c.r[3].w[0] << 1);\n'
              '    set32(c.r[2], c.r[3].w[0] << 1);\n    c.budget -= 3;\n'
              'L_00100120:\n    mmi::div(c, 0, c.r[2].w[0], c.r[7].w[0]);\n    c.budget -= 1;\n'
              'L_00100140:\n    set32(c.r[3], c.r[3].w[0] << 1);\n    mmi::div(c, 0, c.r[2].w[0], c.r[7].w[0]);\n')
    TEXT = (units.INCLUDE_LINE + '\nnamespace gen {\n\nusing namespace ps2;\n\nvoid unit_0001(Cpu &c, std::uint32_t entry, '
            'std::uint32_t depth) {\n' + BLOCKS + '}\n\n}\n')
    SITES = (units.doubling_site(0x00100108, 'L_00100100'), units.divide_site(0x00100120, 'L_00100120', 7))

    def unit(self, sites=SITES, text=TEXT):
        return units.Unit('unit_0001.cpp', units.sha256(text.encode()), sites)

    def test_only_the_statements_of_their_blocks_change(self):
        hooked = units.hook_unit_text(self.TEXT, self.unit())
        self.assertIn('namespace gt4::trigger_range { std::uint32_t pedal_divisor(std::uint32_t value); '
                      'std::uint32_t pedal_pressure(std::uint32_t value); }\n', hooked)
        first = hooked[hooked.index('L_00100100:'):hooked.index('L_00100120:')]
        self.assertIn('set32(c.r[3], ::gt4::trigger_range::pedal_pressure(c.r[3].w[0]));', first)
        self.assertIn('set32(c.r[2], c.r[3].w[0] << 1);', first)  # the other doubling of the case stays
        self.assertIn('mmi::div(c, 0, c.r[2].w[0], ::gt4::trigger_range::pedal_divisor(c.r[7].w[0]));', hooked)
        third = hooked[hooked.index('L_00100140:'):]
        self.assertIn('set32(c.r[3], c.r[3].w[0] << 1);', third)  # the same statement in another block stays
        self.assertIn('mmi::div(c, 0, c.r[2].w[0], c.r[7].w[0]);', third)
        self.assertEqual(units.unhook_unit_text(hooked, self.unit()), self.TEXT)
        units.qualify_unit(self.TEXT, hooked, self.unit())

    def test_refusals(self):
        with self.assertRaises(ValueError):  # the label is missing
            units.hook_unit_text(self.TEXT, self.unit((units.doubling_site(0x00100208, 'L_00100200'),)))
        with self.assertRaises(ValueError):  # the statement is not in its block
            units.hook_unit_text(self.TEXT, self.unit((units.divide_site(0x00100108, 'L_00100100', 7),)))
        twice = self.TEXT.replace('    c.budget -= 3;\n', '    set32(c.r[3], c.r[3].w[0] << 1);\n    c.budget -= 3;\n')
        with self.assertRaises(ValueError):  # twice in its block
            units.hook_unit_text(twice, self.unit(text=twice))
        hooked = units.hook_unit_text(self.TEXT, self.unit())
        with self.assertRaises(ValueError):  # an extra change
            units.qualify_unit(self.TEXT, hooked.replace('c.budget -= 1;', 'c.budget -= 2;'), self.unit())
        with self.assertRaises(ValueError):  # already hooked
            units.hook_unit_text(hooked, self.unit())

    def test_text_scale_sites_stay_in_their_blocks(self):
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
        with self.assertRaises(ValueError):  # one block's statement missing
            units.hook_unit_text(text.replace('L_00490290:\n' + stored, 'L_00490290:\n'), unit)

    def test_pedal_cases_in_the_tables(self):
        pedal = (units.PRESSURE_HOOK, units.DIVISOR_HOOK)
        for name, target in units.TARGETS.items():
            statements = [site for unit in target for site in unit.sites
                          if isinstance(site, units.StatementSite) and site.hook in pedal]
            with self.subTest(target=name):
                self.assertEqual(sorted(site.hook for site in statements), sorted([units.PRESSURE_HOOK, units.DIVISOR_HOOK] * 2))
                for site in statements:
                    self.assertTrue(site.label.startswith('L_') and int(site.label[2:], 16) <= site.address)

    def test_text_scale_stores_in_the_tables(self):
        # TASK-095: four stores per target, x and y of the 1.0 default and of the style's percentages, in two blocks; the scale
        # words are +0x24/+0x28 (v2.00) and +0x3C/+0x40 (Spec II).
        for name, x_offset in (('v200', 36), ('spec2', 60)):
            statements = [site for unit in units.TARGETS[name] for site in unit.sites
                          if isinstance(site, units.StatementSite) and site.hook in (units.TEXT_X_HOOK, units.TEXT_Y_HOOK)]
            with self.subTest(target=name):
                self.assertEqual(sorted(site.hook for site in statements), sorted([units.TEXT_X_HOOK, units.TEXT_Y_HOOK] * 2))
                self.assertEqual(len({site.label for site in statements}), 2)
                for site in statements:
                    offset = x_offset if site.hook == units.TEXT_X_HOOK else x_offset + 4
                    self.assertIn(f'(c.r[18].w[0] + {offset}u)', site.original)
                    self.assertTrue(int(site.label[2:], 16) <= site.address)


class TargetTableTests(unittest.TestCase):
    def test_call_lines_name_their_call_sites(self):
        for name, target in units.TARGETS.items():
            for unit in target:
                for site in unit.sites:
                    if isinstance(site, units.StatementSite):
                        with self.subTest(target=name, statement=site.place):
                            self.assertEqual(unit.name, f'unit_{(site.address - 0x00100000) >> 14:04X}.cpp')
                            self.assertIn(f'::{site.hook}(', site.replacement)
                        continue
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
            projection = [site.return_label for site in sites if site.hook == units.PROJECTION_HOOK]
            host = (game_target_value('camera_yaw_call_return', table), game_target_value('camera_analog_loop_return', table),
                    game_target_value('fov_projection_call_return', table))
            with self.subTest(target=target):
                self.assertEqual((len(yaw), len(loops), len(projection)), (1, 1, 1))
                self.assertEqual(host, (yaw[0], loops[0], projection[0]))
        self.assertEqual(game_target_value('camera_pad_stick_x', 'kV200Hooks'), 0x0086F99E)

    def test_overlay_sites_agree_with_the_host(self):
        # TASK-095: one hook per ortho call (the menu context and the strobe renderer) at the return labels the host checks, and
        # the end hook in the renderer's unit after its ortho call.
        for target, table, ortho in (('v200', 'kV200Hooks', 0x004A5A90), ('spec2', 'kSpec2Hooks', 0x005A2600)):
            sites = [site for unit in units.TARGETS[target] for site in unit.sites if site.hook == units.OVERLAY_HOOK]
            ends = [site for unit in units.TARGETS[target] for site in unit.sites if site.hook == units.OVERLAY_END_HOOK]
            with self.subTest(target=target):
                self.assertEqual(sorted(site.return_label for site in sites),
                                 sorted((game_target_value('menu_ortho_call_return', table),
                                         game_target_value('overlay_ortho_call_return', table))))
                self.assertTrue(all(site.callee == ortho for site in sites))
                self.assertEqual([site.return_label for site in ends], [game_target_value('overlay_draw_end_return', table)])
                renderer = [unit for unit in units.TARGETS[target] if ends[0] in unit.sites][0]
                start = [site for site in renderer.sites if site.hook == units.OVERLAY_HOOK][0]
                self.assertEqual(start.return_label, game_target_value('overlay_ortho_call_return', table))
                self.assertGreater(ends[0].call_site, start.call_site)


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
