"""TASK-090: the CoreGS 3.0 unit insertion script (scripts/coregs3_units.py), on hand-authored unit text.

The real units are private translated code; these controls use synthetic units that only contain the anchors the script
looks for, so the insertion rules, the refusals and the byte-exact removal proof are testable without any game data.
"""
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('coregs3_units', ROOT / 'scripts' / 'coregs3_units.py')
coregs3_units = importlib.util.module_from_spec(SPEC)
sys.modules['coregs3_units'] = coregs3_units
SPEC.loader.exec_module(coregs3_units)

CALL_0055A64 = ('    { c.r[31].sd[0] = static_cast<std::int32_t>(0x00455A64u);  if (depth < kMaxDepth) unit_00E7(c, 53u, '
                'depth + 1u); else { c.pc = 0x0049D308u; return; } if (c.pc != 0x00455A64u) [[unlikely]] return; }\n')

SYNTHETIC_00D5 = (
    '#include "units.hpp"\n'
    '\n'
    'namespace gen {\n'
    '\n'
    'L_00455A5C:\n'
    + CALL_0055A64 +
    'L_00455A64:\n'
    '    return;\n'
    '}\n'
)

def make_synthetic_008f():
    """Every anchor the script inserts into unit_008F, once each, wrapped as a synthetic unit."""
    lines = ['#include "units.hpp"', '', 'namespace gen {', '']
    seen = set()
    for entry in coregs3_units.INSERTS:
        if entry[0] != 'unit_008F.cpp' or entry[1] in seen:
            continue
        seen.add(entry[1])
        lines.append(entry[1].rstrip('\n'))
    lines += ['    return;', '}', '']
    return '\n'.join(lines) + '\n'


# The full unit_008F anchor set (K3/K4/K5 and every K7 pass marker). The render_after and mirror ends follow the call
# statements, because their return labels are merge points reached without the call.
SYNTHETIC_008F = make_synthetic_008f()
SYNTHETIC_008F_PASSES = SYNTHETIC_008F

SYNTHETIC_00D3 = (
    '#include "units.hpp"\n'
    '\n'
    'namespace gen {\n'
    '\n'
    + coregs3_units.CALL_44E920 +
    'L_0044E920:\n'
    '    c.budget -= 2;\n'
    + coregs3_units.CALL_44EE24 +
    'L_0044EE24:\n'
    '    return;\n'
    '}\n'
)

SYNTHETIC_00D4 = (
    '#include "units.hpp"\n'
    '\n'
    'namespace gen {\n'
    '\n'
    + coregs3_units.CALL_4507AC +
    'L_004507AC:\n'
    '    return;\n'
    '}\n'
)

SYNTHETIC_00BB = (
    '#include "units.hpp"\n'
    '\n'
    'namespace gen {\n'
    '\n'
    + coregs3_units.CALL_3EC4A0 +
    'L_003EC4A0:\n'
    '    return;\n'
    '}\n'
)

SYNTHETIC = {
    'unit_00D3.cpp': SYNTHETIC_00D3,
    'unit_00D4.cpp': SYNTHETIC_00D4,
    'unit_00BB.cpp': SYNTHETIC_00BB,
    'unit_00D5.cpp': SYNTHETIC_00D5,
    'unit_008F.cpp': SYNTHETIC_008F,
}


class InsertionTest(unittest.TestCase):
    def test_apply_and_remove_are_exact(self):
        for name, text in SYNTHETIC.items():
            instrumented = coregs3_units.apply_insertions(name, text)
            self.assertNotEqual(instrumented, text)
            self.assertIn('namespace gt4::coregs3 {', instrumented)
            self.assertEqual(coregs3_units.remove_insertions(name, instrumented), text,
                             f'{name}: removal must restore the input byte for byte')

    def test_k1_call_sites(self):
        instrumented = coregs3_units.apply_insertions('unit_00D5.cpp', SYNTHETIC_00D5)
        self.assertIn('::gt4::coregs3::before_shape_call(c, 0u);\n' + CALL_0055A64, instrumented)
        self.assertIn('L_00455A64:\n    ::gt4::coregs3::after_shape_call(c);\n', instrumented)
        # The direct pgluCallShape sites carry their own call-site constant. The hook is inserted after the a0
        # assignment inside the call statement, so the shape pointer is already loaded when it runs.
        for name, constant, calls in (('unit_00D3.cpp', '2u', 2), ('unit_00D4.cpp', '1u', 1), ('unit_00BB.cpp', '3u', 1)):
            text = coregs3_units.apply_insertions(name, SYNTHETIC[name])
            self.assertEqual(text.count(f'::gt4::coregs3::before_shape_call(c, {constant});'), calls)
            self.assertEqual(text.count('::gt4::coregs3::after_shape_call(c);'), calls)
        text = coregs3_units.apply_insertions('unit_00D3.cpp', SYNTHETIC_00D3)
        self.assertIn('set32(c.r[4], c.r[18].w[0] + 32000u);\n    ::gt4::coregs3::before_shape_call(c, 2u);\n',
                      text)

    def test_pass_and_camera_sites(self):
        instrumented = coregs3_units.apply_insertions('unit_008F.cpp', SYNTHETIC_008F)
        for label, call in (('L_0033EBB4', 'projection_set'), ('L_0033EBF4', 'modelview_set'),
                            ('L_0033EE28', 'car_pass_begin'), ('L_0033EE54', 'car_pass_end')):
            self.assertIn(f'{label}:\n    ::gt4::coregs3::{call}(c);\n', instrumented)

    def test_pass_markers(self):
        instrumented = coregs3_units.apply_insertions('unit_008F.cpp', SYNTHETIC_008F_PASSES)
        for pass_id, call in (('1u', 'pass_begin'), ('2u', 'pass_begin'), ('5u', 'pass_begin'), ('9u', 'pass_begin'),
                              ('15u', 'pass_begin')):
            self.assertIn(f'::gt4::coregs3::{call}(c, {pass_id});', instrumented)
        self.assertIn('L_0033E9F0:\n    ::gt4::coregs3::pass_end(c, 1u);\n', instrumented)
        self.assertIn('::gt4::coregs3::pass_begin(c, 9u);\n' + coregs3_units.CALL_33EEF8 +
                      '    ::gt4::coregs3::pass_end(c, 9u);\n', instrumented)
        self.assertIn('L_0033EC28:\n    ::gt4::coregs3::pass_end(c, 2u);\n', instrumented)
        self.assertIn('L_0033DA84:\n    ::gt4::coregs3::pass_end(c, 15u);\n', instrumented)

    def test_a_missing_anchor_is_refused(self):
        broken = SYNTHETIC_00D5.replace('0x00455A64u);  if', '0x00455A64u); if')
        with self.assertRaises(coregs3_units.UnitError):
            coregs3_units.apply_insertions('unit_00D5.cpp', broken)

    def test_an_ambiguous_anchor_is_refused(self):
        twice = SYNTHETIC_008F.replace('L_0033EE54:\n', 'L_0033EE54:\nL_0033EE54:\n')
        with self.assertRaises(coregs3_units.UnitError):
            coregs3_units.apply_insertions('unit_008F.cpp', twice)

    def test_a_file_without_the_include_is_refused(self):
        with self.assertRaises(coregs3_units.UnitError):
            coregs3_units.apply_insertions('unit_00D5.cpp', 'int main() {}\n')

    def test_instrument_writes_a_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'units'
            source.mkdir()
            for name, text in SYNTHETIC.items():
                (source / name).write_text(text, encoding='utf-8', newline='\n')
            (source / 'unit_0000.cpp').write_text('unchanged\n', encoding='utf-8', newline='\n')
            output = Path(temporary) / 'instrumented'
            receipt = coregs3_units.instrument(source, output)
            self.assertEqual(receipt['files']['instrumented'], 5)
            self.assertEqual(receipt['files']['copied'], 1)
            self.assertEqual((output / 'unit_0000.cpp').read_text(encoding='utf-8'), 'unchanged\n')
            self.assertTrue((Path(temporary) / 'instrumented.coregs3-units.json').is_file())
            with self.assertRaises(coregs3_units.UnitError):
                coregs3_units.instrument(source, output)  # a fresh folder only


if __name__ == '__main__':
    unittest.main()
